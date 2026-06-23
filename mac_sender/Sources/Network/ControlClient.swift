import Foundation

/// Notifies the application coordinator of control-channel state changes.
protocol ControlClientDelegate: AnyObject {
    /// Called after a successful HELLO → CONFIGURE → START handshake.
    /// The capture pipeline should start at the negotiated resolution, fps, codec and bitrate.
    func controlClient(_ client: ControlClient,
                       didStartStreamingAt width: Int,
                       height: Int,
                       fps: Int,
                       codec: TlnkCodecSwift,
                       bitrateKbps: Int)

    /// Called when the Windows receiver sent STOP/BYE or the connection dropped.
    /// The capture pipeline should stop. ControlClient will attempt to reconnect.
    func controlClientDidStopStreaming(_ client: ControlClient)

    /// Called when the receiver asks for an IDR (loss recovery). The encoder
    /// should force a keyframe on its next encoded frame.
    func controlClientDidRequestKeyframe(_ client: ControlClient)
}

/// TCP control channel between the Mac sender and the Windows receiver.
///
/// Topology: the **Windows receiver listens** on `TLNK_PORT_CONTROL`; the Mac is the client.
/// Wire format: 4-byte big-endian length prefix followed by a JSON object.
/// Protocol flow (matches windows_receiver/src/Network/NetworkManager.cpp):
///   1. Mac connects to Windows on TCP 50003 (retries every TLNK_RECONNECT_INTERVAL_MS on failure)
///   2. Mac sends HELLO { type, version }
///   3. Windows sends CONFIGURE { type, width, height, fps }
///   4. Mac sends START { type, width, height, fps } and begins capturing
///   5. Steady state: Windows sends PING { type, sent_time }; Mac replies PONG { type, sent_time }
///   6. Either side may send STOP or BYE to end streaming.
final class ControlClient {
    weak var delegate: ControlClientDelegate?

    private var socket: Int32 = -1
    private var running = false
    private var thread: Thread?
    private let stateQueue = DispatchQueue(label: "com.thunderlink.controlclient.state")
    private var streamingActive = false

    func start(delegate: ControlClientDelegate) {
        self.delegate = delegate
        running = true

        let t = Thread { [weak self] in
            self?.runLoop()
        }
        t.qualityOfService = .userInteractive
        t.name = "ThunderLinkControlClient"
        thread = t
        t.start()
    }

    func stop() {
        running = false
        closeSocket()
    }

    // MARK: - Reconnect loop

    private func runLoop() {
        while running {
            if connectOnce() {
                handshakeAndServe()
            }
            // After disconnect (or failed connect), wait then retry.
            closeSocket()
            notifyStoppedIfStreaming()
            if !running { break }
            Thread.sleep(forTimeInterval: Double(NetworkConfig.reconnectIntervalMs) / 1000.0)
        }
    }

    private func connectOnce() -> Bool {
        let s = Darwin.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
        guard s >= 0 else {
            logger.error("ControlClient: failed to create socket (errno=\(errno))")
            return false
        }

        var nodelay: Int32 = 1
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                   socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = NetworkConfig.controlPort.bigEndian
        addr.sin_addr.s_addr = inet_addr(NetworkConfig.winIP)

        let result = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockPtr in
                Darwin.connect(s, sockPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        if result < 0 {
            // Expected on cold start before the Windows app is up — log at debug.
            logger.debug("ControlClient: connect to \(NetworkConfig.winIP):\(NetworkConfig.controlPort) failed (errno=\(errno))")
            close(s)
            return false
        }

        socket = s
        logger.info("ControlClient: connected to \(NetworkConfig.winIP):\(NetworkConfig.controlPort)")
        return true
    }

    private func closeSocket() {
        if socket >= 0 {
            close(socket)
            socket = -1
        }
    }

    // MARK: - Handshake + message loop

    private func handshakeAndServe() {
        // 1. Send HELLO
        let hello: [String: Any] = [
            ControlField.type: TlnkControlType.hello.rawValue,
            ControlField.version: Int(NetworkConfig.protocolVersion)
        ]
        guard sendJSON(hello) else {
            logger.error("ControlClient: failed to send HELLO")
            return
        }

        // 2. Wait for CONFIGURE
        guard let configure = receiveJSON() else {
            logger.error("ControlClient: no CONFIGURE received")
            return
        }
        guard (configure[ControlField.type] as? Int) == TlnkControlType.configure.rawValue else {
            logger.error("ControlClient: expected CONFIGURE, got type=\(configure[ControlField.type] ?? "nil")")
            return
        }
        let width  = (configure[ControlField.width]  as? Int) ?? NetworkConfig.defaultWidth
        let height = (configure[ControlField.height] as? Int) ?? NetworkConfig.defaultHeight
        let fps    = (configure[ControlField.fps]    as? Int) ?? NetworkConfig.defaultFPS
        // TlnkCodec: 0 = H.264, 1 = HEVC. VideoToolbox can encode both on modern
        // Macs, so we honor the receiver's request as-is.
        let codecRaw = (configure[ControlField.codec] as? Int) ?? 1
        let codec: TlnkCodecSwift = (codecRaw == 0) ? .h264 : .hevc
        let bitrate = (configure[ControlField.bitrate] as? Int) ?? (NetworkConfig.defaultBitrateMbps * 1000)
        logger.info("ControlClient: CONFIGURE \(width)x\(height) @ \(fps) fps, codec=\(codec), \(bitrate) kbps")

        // 3. Send START echoing the negotiated config (incl. the codec we'll send)
        let start: [String: Any] = [
            ControlField.type:    TlnkControlType.start.rawValue,
            ControlField.width:   width,
            ControlField.height:  height,
            ControlField.fps:     fps,
            ControlField.codec:   (codec == .hevc) ? 1 : 0,
            ControlField.bitrate: bitrate
        ]
        guard sendJSON(start) else {
            logger.error("ControlClient: failed to send START")
            return
        }

        stateQueue.sync { streamingActive = true }
        delegate?.controlClient(self,
                                didStartStreamingAt: width,
                                height: height,
                                fps: fps,
                                codec: codec,
                                bitrateKbps: bitrate)

        // 4. Steady state: handle PING/STOP/BYE
        while running {
            guard let msg = receiveJSON() else {
                logger.info("ControlClient: control connection closed")
                return
            }
            let type = (msg[ControlField.type] as? Int) ?? -1
            switch TlnkControlType(rawValue: type) {
            case .ping:
                var pong: [String: Any] = [ControlField.type: TlnkControlType.pong.rawValue]
                if let sentTime = msg[ControlField.sentTime] {
                    pong[ControlField.sentTime] = sentTime
                }
                _ = sendJSON(pong)
            case .requestKeyframe:
                delegate?.controlClientDidRequestKeyframe(self)
            case .stop, .bye:
                logger.info("ControlClient: received STOP/BYE")
                return
            default:
                logger.debug("ControlClient: ignoring control type=\(type)")
            }
        }
    }

    private func notifyStoppedIfStreaming() {
        let wasActive = stateQueue.sync { () -> Bool in
            let prev = streamingActive
            streamingActive = false
            return prev
        }
        if wasActive {
            delegate?.controlClientDidStopStreaming(self)
        }
    }

    // MARK: - Framed JSON I/O

    private func sendJSON(_ object: [String: Any]) -> Bool {
        guard socket >= 0 else { return false }
        guard let data = try? JSONSerialization.data(withJSONObject: object, options: []) else {
            logger.error("ControlClient: JSON encode failed")
            return false
        }

        var lenBE = UInt32(data.count).bigEndian
        let lenOk: Bool = withUnsafeBytes(of: &lenBE) { raw in
            sendAll(raw.baseAddress!, 4)
        }
        if !lenOk { return false }

        return data.withUnsafeBytes { raw -> Bool in
            sendAll(raw.baseAddress!, data.count)
        }
    }

    private func sendAll(_ buf: UnsafeRawPointer, _ count: Int) -> Bool {
        var sent = 0
        while sent < count {
            let n = send(socket, buf.advanced(by: sent), count - sent, 0)
            if n <= 0 { return false }
            sent += n
        }
        return true
    }

    private func receiveJSON() -> [String: Any]? {
        var lenBuf = [UInt8](repeating: 0, count: 4)
        guard recvAll(&lenBuf, 4) else { return nil }
        let len = (UInt32(lenBuf[0]) << 24) |
                  (UInt32(lenBuf[1]) << 16) |
                  (UInt32(lenBuf[2]) <<  8) |
                   UInt32(lenBuf[3])
        guard len > 0 && len <= 65536 else {
            logger.warn("ControlClient: invalid length prefix \(len)")
            return nil
        }
        var body = [UInt8](repeating: 0, count: Int(len))
        guard recvAll(&body, Int(len)) else { return nil }
        guard let obj = try? JSONSerialization.jsonObject(with: Data(body), options: []),
              let dict = obj as? [String: Any] else {
            logger.warn("ControlClient: JSON parse failed")
            return nil
        }
        return dict
    }

    private func recvAll(_ buffer: UnsafeMutablePointer<UInt8>, _ count: Int) -> Bool {
        var received = 0
        while received < count {
            let n = recv(socket, buffer.advanced(by: received), count - received, 0)
            if n <= 0 { return false }
            received += n
        }
        return true
    }
}
