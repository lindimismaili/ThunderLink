import Foundation

enum FrameHeaderEncoder {
    /// Protocol-v3 header. Existing v2 fields retain their offsets; the stream
    /// session is the final little-endian UInt64 at offset 40.
    static func makeHeader(frameID: UInt64, sequence: UInt32, totalPackets: UInt32,
                           payloadSize: UInt32, frameTotalBytes: UInt32,
                           width: UInt32, height: UInt32, format: UInt16,
                           flags: UInt16, sessionID: StreamSessionID) -> Data {
        precondition(NetworkConfig.headerSize == 48)
        var header = Data(count: NetworkConfig.headerSize)
        header.withUnsafeMutableBytes { bytes in
            guard let base = bytes.baseAddress else { return }
            write(NetworkConfig.magic, at: 0, to: base)
            write(frameID, at: 4, to: base)
            write(sequence, at: 12, to: base)
            write(totalPackets, at: 16, to: base)
            write(payloadSize, at: 20, to: base)
            write(frameTotalBytes, at: 24, to: base)
            write(width, at: 28, to: base)
            write(height, at: 32, to: base)
            write(format, at: 36, to: base)
            write(flags, at: 38, to: base)
            write(sessionID.rawValue, at: 40, to: base)
        }
        return header
    }

    private static func write<T: FixedWidthInteger>(_ value: T, at offset: Int, to base: UnsafeMutableRawPointer) {
        var littleEndian = value.littleEndian
        withUnsafeBytes(of: &littleEndian) { source in
            base.advanced(by: offset).copyMemory(from: source.baseAddress!, byteCount: source.count)
        }
    }
}

/// Packetizes encoded access units. A sender remains closed to video until a
/// matching START has actually been written on the control channel.
final class FrameSender {
    private let lock = NSLock()
    private var socket: Int32 = -1
    private var destination = sockaddr_in()
    private var frameID: UInt64 = 0
    private var activeSession: StreamSessionID?

    init() {
        destination.sin_family = sa_family_t(AF_INET)
        destination.sin_port = NetworkConfig.videoPort.bigEndian
        destination.sin_addr.s_addr = inet_addr(NetworkConfig.winIP)
    }

    deinit { stop() }

    func start() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard socket < 0 else { return true }

        let candidate = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard candidate >= 0 else {
            logger.error("FrameSender: failed to create UDP socket (errno=\(errno))")
            return false
        }
        var bufferSize = Int32(NetworkConfig.sendBufferSize)
        if setsockopt(candidate, SOL_SOCKET, SO_SNDBUF, &bufferSize,
                      socklen_t(MemoryLayout<Int32>.size)) != 0 {
            bufferSize = 65_536
            guard setsockopt(candidate, SOL_SOCKET, SO_SNDBUF, &bufferSize,
                             socklen_t(MemoryLayout<Int32>.size)) == 0 else {
            logger.error("FrameSender: SO_SNDBUF failed (errno=\(errno))")
            close(candidate)
            return false
            }
        }
        let flags = fcntl(candidate, F_GETFL, 0)
        guard flags >= 0, fcntl(candidate, F_SETFL, flags | O_NONBLOCK) == 0 else {
            close(candidate)
            return false
        }

        var bindAddress = sockaddr_in()
        bindAddress.sin_family = sa_family_t(AF_INET)
        bindAddress.sin_port = 0
        bindAddress.sin_addr.s_addr = inet_addr(NetworkConfig.macIP)
        let result = withUnsafePointer(to: &bindAddress) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(candidate, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard result == 0 else {
            logger.error("FrameSender: bind failed (errno=\(errno))")
            close(candidate)
            return false
        }
        socket = candidate
        logger.info("FrameSender: ready for \(NetworkConfig.winIP):\(NetworkConfig.videoPort)")
        return true
    }

    func activate(sessionID: StreamSessionID) {
        lock.lock()
        activeSession = sessionID
        frameID = 0
        lock.unlock()
    }

    func deactivate(sessionID: StreamSessionID? = nil) {
        lock.lock()
        if sessionID == nil || activeSession == sessionID { activeSession = nil }
        lock.unlock()
    }

    func stop() {
        lock.lock()
        activeSession = nil
        if socket >= 0 {
            close(socket)
            socket = -1
        }
        lock.unlock()
    }

    func sendEncodedFrame(_ data: Data, isKeyframe: Bool, format: UInt16,
                          width: Int, height: Int, sessionID expectedSession: StreamSessionID) {
        lock.lock()
        defer { lock.unlock() }
        guard socket >= 0, activeSession == expectedSession, !data.isEmpty,
              data.count <= NetworkConfig.maxWidth * NetworkConfig.maxHeight * 4,
              width >= 2, width <= NetworkConfig.maxWidth, width.isMultiple(of: 2),
              height >= 2, height <= NetworkConfig.maxHeight, height.isMultiple(of: 2),
              format == NetworkConfig.formatH264 || format == NetworkConfig.formatHEVC else { return }

        let packetCount = (data.count + NetworkConfig.maxPayload - 1) / NetworkConfig.maxPayload
        guard packetCount > 0, packetCount <= Int(UInt32.max) else { return }
        frameID &+= 1
        if frameID == 0 { frameID = 1 }
        let currentFrameID = frameID
        let flags: UInt16 = isKeyframe ? 0x0001 : 0

        data.withUnsafeBytes { raw in
            guard let source = raw.baseAddress else { return }
            for sequence in 0..<packetCount {
                let offset = sequence * NetworkConfig.maxPayload
                let payloadSize = min(NetworkConfig.maxPayload, data.count - offset)
                var packet = FrameHeaderEncoder.makeHeader(
                    frameID: currentFrameID,
                    sequence: UInt32(sequence),
                    totalPackets: UInt32(packetCount),
                    payloadSize: UInt32(payloadSize),
                    frameTotalBytes: UInt32(data.count),
                    width: UInt32(width), height: UInt32(height),
                    format: format, flags: flags, sessionID: expectedSession)
                packet.append(Data(bytes: source.advanced(by: offset), count: payloadSize))
                sendPacket(packet)
            }
        }
    }

    private func sendPacket(_ packet: Data) {
        let sent = packet.withUnsafeBytes { bytes in
            withUnsafePointer(to: &destination) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(socket, bytes.baseAddress, bytes.count, 0, $0,
                           socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
        if sent != packet.count {
            logger.warn("FrameSender: UDP send failed (sent=\(sent), errno=\(errno))")
        }
    }
}
