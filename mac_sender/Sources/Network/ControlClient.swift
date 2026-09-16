import Foundation

protocol ControlClientDelegate: AnyObject {
    func controlClient(_ client: ControlClient, didConnect token: UInt64)
    func controlClient(_ client: ControlClient, didReceive configuration: StreamConfiguration)
    func controlClient(_ client: ControlClient, didRequestKeyframeFor sessionID: StreamSessionID)
    func controlClient(_ client: ControlClient, didStop sessionID: StreamSessionID?, reason: String?)
    func controlClient(_ client: ControlClient, didFail reason: String)
    func controlClient(_ client: ControlClient, didDisconnect token: UInt64, reason: String)
}

/// Protocol-v3 control client. Its worker thread exclusively owns and closes
/// the socket. Other threads enqueue bounded commands and observe callbacks.
final class ControlClient {
    weak var delegate: ControlClientDelegate?

    private enum Command {
        case start(StreamConfiguration, (Bool) -> Void)
        case stop(StreamSessionID, String, (Bool) -> Void)
    }

    private let lock = NSLock()
    private var commands: [Command] = []
    private var running = false
    private var stopRequested = false
    private var shouldSendBye = false
    private var thread: Thread?
    private let stopped = DispatchGroup()
    private var connectionCounter: UInt64 = 0

    func start(delegate: ControlClientDelegate) {
        lock.lock()
        guard !running else { lock.unlock(); return }
        self.delegate = delegate
        running = true
        stopRequested = false
        shouldSendBye = false
        stopped.enter()
        lock.unlock()

        let worker = Thread { [weak self] in self?.runLoop() }
        worker.qualityOfService = .userInteractive
        worker.name = "ThunderLinkControlClient"
        thread = worker
        worker.start()
    }

    func sendStart(_ configuration: StreamConfiguration, completion: @escaping (Bool) -> Void) {
        enqueue(.start(configuration, completion))
    }

    func sendStop(sessionID: StreamSessionID, reason: String,
                  completion: @escaping (Bool) -> Void = { _ in }) {
        enqueue(.stop(sessionID, reason, completion))
    }

    func stop(sendBye: Bool = true, completion: (() -> Void)? = nil) {
        lock.lock()
        guard running else {
            lock.unlock()
            completion?()
            return
        }
        stopRequested = true
        shouldSendBye = sendBye
        lock.unlock()

        guard let completion else { return }
        DispatchQueue.global(qos: .userInitiated).async { [stopped] in
            stopped.wait()
            completion()
        }
    }

    private func enqueue(_ command: Command) {
        lock.lock()
        if running && !stopRequested {
            commands.append(command)
            lock.unlock()
        } else {
            lock.unlock()
            complete(command, success: false)
        }
    }

    private func runLoop() {
        defer {
            failAllCommands()
            lock.lock()
            running = false
            lock.unlock()
            stopped.leave()
        }

        while !isStopping {
            connectionCounter &+= 1
            if connectionCounter == 0 { connectionCounter = 1 }
            let token = connectionCounter

            guard let socket = connectOnce() else {
                waitForReconnect()
                continue
            }
            delegate?.controlClient(self, didConnect: token)
            let reason = serve(socket: socket, token: token)
            close(socket)
            failCommands(for: token)
            delegate?.controlClient(self, didDisconnect: token, reason: reason)
            if !isStopping { waitForReconnect() }
        }
    }

    private var isStopping: Bool {
        lock.lock(); defer { lock.unlock() }
        return stopRequested
    }

    private func consumeByeRequest() -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard stopRequested, shouldSendBye else { return false }
        shouldSendBye = false
        return true
    }

    private func connectOnce() -> Int32? {
        let socket = Darwin.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
        guard socket >= 0 else {
            logger.error("ControlClient: socket failed (errno=\(errno))")
            return nil
        }
        var noSignal: Int32 = 1
        var noDelay: Int32 = 1
        guard setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSignal,
                         socklen_t(MemoryLayout<Int32>.size)) == 0,
              setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &noDelay,
                         socklen_t(MemoryLayout<Int32>.size)) == 0 else {
            logger.error("ControlClient: socket options failed (errno=\(errno))")
            close(socket)
            return nil
        }
        let oldFlags = fcntl(socket, F_GETFL, 0)
        guard oldFlags >= 0, fcntl(socket, F_SETFL, oldFlags | O_NONBLOCK) == 0 else {
            close(socket)
            return nil
        }

        var address = sockaddr_in()
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = NetworkConfig.controlPort.bigEndian
        address.sin_addr.s_addr = inet_addr(NetworkConfig.winIP)
        let result = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(socket, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if result == 0 { return socket }
        guard errno == EINPROGRESS else {
            logger.debug("ControlClient: connect failed (errno=\(errno))")
            close(socket)
            return nil
        }

        let deadline = monotonicMilliseconds() + UInt64(NetworkConfig.connectTimeoutMs)
        while !isStopping && monotonicMilliseconds() < deadline {
            let now = monotonicMilliseconds()
            if now >= deadline { break }
            let wait = Int32(min(100, deadline - now))
            var descriptor = pollfd(fd: socket, events: Int16(POLLOUT), revents: 0)
            let polled = Darwin.poll(&descriptor, 1, wait)
            if polled < 0 && errno == EINTR { continue }
            if polled > 0 {
                var errorCode: Int32 = 0
                var size = socklen_t(MemoryLayout<Int32>.size)
                guard getsockopt(socket, SOL_SOCKET, SO_ERROR, &errorCode, &size) == 0,
                      errorCode == 0 else {
                    logger.debug("ControlClient: connect completion failed (errno=\(errorCode))")
                    close(socket)
                    return nil
                }
                logger.info("ControlClient: connected to \(NetworkConfig.winIP):\(NetworkConfig.controlPort)")
                return socket
            }
        }
        close(socket)
        return nil
    }

    private func serve(socket: Int32, token: UInt64) -> String {
        let hello: [String: Any] = [
            ControlField.type: TlnkControlType.hello.rawValue,
            ControlField.version: Int(NetworkConfig.protocolVersion)
        ]
        guard sendJSON(hello, socket: socket) else { return "failed to send HELLO" }

        let decoder = FramedJSONDecoder()
        var lastActivity = monotonicMilliseconds()
        let handshakeDeadline = lastActivity + UInt64(NetworkConfig.handshakeTimeoutMs)
        var receivedConfiguration = false
        var latestSession: StreamSessionID?
        var lastPing = lastActivity

        while true {
            do { try decoder.checkDeadline() } catch { return "partial control message timeout" }
            if isStopping {
                if consumeByeRequest() {
                    _ = sendJSON([ControlField.type: TlnkControlType.bye.rawValue], socket: socket, orderly: true)
                }
                return "sender stopped"
            }

            switch processCommands(socket: socket, token: token, latestSession: latestSession) {
            case .disconnect(let reason): return reason
            case .continueServing: break
            }

            let now = monotonicMilliseconds()
            if receivedConfiguration && now - lastPing >= UInt64(NetworkConfig.pingIntervalMs) {
                guard sendJSON([ControlField.type: TlnkControlType.ping.rawValue,
                                ControlField.sentTime: now], socket: socket) else { return "failed to send PING" }
                lastPing = now
            }
            if !receivedConfiguration && now >= handshakeDeadline { return "CONFIGURE timeout" }
            if receivedConfiguration && now - lastActivity >= UInt64(NetworkConfig.peerSilenceTimeoutMs) {
                return "peer silence timeout"
            }

            var descriptor = pollfd(fd: socket,
                                    events: Int16(POLLIN), revents: 0)
            let result = Darwin.poll(&descriptor, 1, 100)
            if result < 0 {
                if errno == EINTR { continue }
                return "poll failed (errno=\(errno))"
            }
            if result == 0 { continue }
            if (descriptor.revents & Int16(POLLERR | POLLHUP | POLLNVAL)) != 0 {
                return "control connection closed"
            }

            var bytes = [UInt8](repeating: 0, count: 16_384)
            let count = recv(socket, &bytes, bytes.count, 0)
            if count == 0 { return "control EOF" }
            if count < 0 {
                if errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR { continue }
                return "receive failed (errno=\(errno))"
            }
            lastActivity = monotonicMilliseconds()

            let messages: [[String: Any]]
            do {
                messages = try decoder.append(Data(bytes.prefix(count)))
            } catch {
                return "malformed control framing: \(error)"
            }
            for message in messages {
                guard let typeRaw = try? ControlProtocol.integer(ControlField.type, in: message),
                      let type = TlnkControlType(rawValue: typeRaw) else {
                    return "invalid control message type"
                }
                switch type {
                case .configure:
                    do {
                        let configuration = try ControlProtocol.configuration(from: message,
                                                                              connectionToken: token)
                        guard configuration.sessionID != latestSession else { return "CONFIGURE reused a session_id" }
                        latestSession = configuration.sessionID
                        receivedConfiguration = true
                        delegate?.controlClient(self, didReceive: configuration)
                    } catch {
                        let reason = "Invalid CONFIGURE: \(error.localizedDescription)"
                        delegate?.controlClient(self, didFail: reason)
                        var stop: [String: Any] = [ControlField.type: TlnkControlType.stop.rawValue,
                                                  ControlField.reason: reason]
                        if let text = message[ControlField.sessionID] as? String {
                            stop[ControlField.sessionID] = text
                        }
                        _ = sendJSON(stop, socket: socket)
                        return reason
                    }
                case .ping:
                    var pong: [String: Any] = [ControlField.type: TlnkControlType.pong.rawValue]
                    if let sentTime = message[ControlField.sentTime] { pong[ControlField.sentTime] = sentTime }
                    guard sendJSON(pong, socket: socket) else { return "failed to send PONG" }
                case .requestKeyframe:
                    guard let text = message[ControlField.sessionID] as? String,
                          let session = StreamSessionID(hex: text) else {
                        return "invalid keyframe session_id"
                    }
                    delegate?.controlClient(self, didRequestKeyframeFor: session)
                case .stop:
                    let session = (message[ControlField.sessionID] as? String).flatMap(StreamSessionID.init(hex:))
                    guard session != nil else { return "invalid STOP session_id" }
                    guard session == latestSession else { continue }
                    delegate?.controlClient(self, didStop: session,
                                            reason: message[ControlField.reason] as? String)
                    return "receiver STOP"
                case .bye:
                    let reason = message[ControlField.reason] as? String ?? "receiver closed"
                    delegate?.controlClient(self, didFail: reason)
                    return "receiver BYE"
                case .pong:
                    guard (try? ControlProtocol.integer(ControlField.sentTime, in: message)) != nil else {
                        return "invalid PONG timestamp"
                    }
                case .hello, .start:
                    return "unexpected control message type \(typeRaw)"
                }
            }
        }
    }

    private enum CommandResult { case continueServing, disconnect(String) }

    private func processCommands(socket: Int32, token: UInt64,
                                 latestSession: StreamSessionID?) -> CommandResult {
        lock.lock()
        let pending = commands
        commands.removeAll(keepingCapacity: true)
        lock.unlock()

        var processed = 0
        defer { pending.dropFirst(processed).forEach { complete($0, success: false) } }
        for command in pending {
            processed += 1
            switch command {
            case .start(let configuration, let completion):
                guard configuration.connectionToken == token,
                      configuration.sessionID == latestSession else {
                    completion(false)
                    continue
                }
                var object = configuration.controlFields
                object[ControlField.type] = TlnkControlType.start.rawValue
                let success = sendJSON(object, socket: socket)
                completion(success)
                if !success { return .disconnect("failed to send START") }
            case .stop(let sessionID, let reason, let completion):
                guard sessionID == latestSession else {
                    completion(false)
                    continue
                }
                let success = sendJSON([
                    ControlField.type: TlnkControlType.stop.rawValue,
                    ControlField.sessionID: sessionID.description,
                    ControlField.reason: reason
                ], socket: socket)
                completion(success)
                return .disconnect(reason)
            }
        }
        return .continueServing
    }

    private func sendJSON(_ object: [String: Any], socket: Int32, orderly: Bool = false) -> Bool {
        guard let data = try? FramedJSON.encode(object) else { return false }
        return data.withUnsafeBytes { bytes in
            guard let base = bytes.baseAddress else { return false }
            var sent = 0
            let deadline = monotonicMilliseconds() + UInt64(orderly ? 50 : NetworkConfig.writeTimeoutMs)
            while sent < bytes.count && monotonicMilliseconds() < deadline {
                if !orderly && isStopping { return false }
                let count = send(socket, base.advanced(by: sent), bytes.count - sent, 0)
                if count > 0 { sent += count; continue }
                if count < 0 && errno == EINTR { continue }
                if count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) {
                    var descriptor = pollfd(fd: socket, events: Int16(POLLOUT), revents: 0)
                    if Darwin.poll(&descriptor, 1, 10) >= 0 { continue }
                }
                return false
            }
            return sent == bytes.count
        }
    }

    private func failCommands(for token: UInt64) {
        lock.lock()
        let (failed, retained) = commands.reduce(into: ([Command](), [Command]())) { result, command in
            if case .start(let config, _) = command, config.connectionToken != token {
                result.1.append(command)
            } else {
                result.0.append(command)
            }
        }
        commands = retained
        lock.unlock()
        failed.forEach { complete($0, success: false) }
    }

    private func failAllCommands() {
        lock.lock()
        let failed = commands
        commands.removeAll()
        lock.unlock()
        failed.forEach { complete($0, success: false) }
    }

    private func complete(_ command: Command, success: Bool) {
        switch command {
        case .start(_, let completion), .stop(_, _, let completion): completion(success)
        }
    }

    private func waitForReconnect() {
        let deadline = monotonicMilliseconds() + UInt64(NetworkConfig.reconnectIntervalMs)
        while !isStopping && monotonicMilliseconds() < deadline {
            Thread.sleep(forTimeInterval: 0.05)
        }
    }

    private func monotonicMilliseconds() -> UInt64 {
        DispatchTime.now().uptimeNanoseconds / 1_000_000
    }
}
