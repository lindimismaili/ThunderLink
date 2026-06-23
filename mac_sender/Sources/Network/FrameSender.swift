import Foundation

/// Splits frames into UDP packets and sends them to the Windows receiver.
/// Uses POSIX sockets for maximum control over packet construction.
///
/// The normal path is `sendEncodedFrame` (a compressed H.264/HEVC Annex-B
/// access unit). `sendFrame` (raw BGRA) is retained as a legacy/test path.
final class FrameSender {
    private var socket: Int32 = -1
    private var destAddr: sockaddr_in
    private var frameID: UInt64 = 0

    init() {
        destAddr = sockaddr_in()
        destAddr.sin_family = sa_family_t(AF_INET)
        destAddr.sin_port = UInt16(NetworkConfig.videoPort).bigEndian
        destAddr.sin_addr.s_addr = inet_addr(NetworkConfig.winIP)
    }

    deinit {
        stop()
    }

    func start() -> Bool {
        socket = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard socket >= 0 else {
            logger.error("FrameSender: failed to create UDP socket")
            return false
        }

        var bufSize = Int32(NetworkConfig.sendBufferSize)
        setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &bufSize, socklen_t(MemoryLayout<Int32>.size))

        var bindAddr = sockaddr_in()
        bindAddr.sin_family = sa_family_t(AF_INET)
        bindAddr.sin_port = 0  // Any port
        bindAddr.sin_addr.s_addr = inet_addr(NetworkConfig.macIP)

        let bindResult = withUnsafePointer(to: &bindAddr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockPtr in
                bind(socket, sockPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        if bindResult < 0 {
            logger.error("FrameSender: bind failed (errno=\(errno))")
            close(socket)
            socket = -1
            return false
        }

        logger.info("FrameSender: ready to send to \(NetworkConfig.winIP):\(NetworkConfig.videoPort)")
        return true
    }

    func stop() {
        if socket >= 0 {
            close(socket)
            socket = -1
        }
    }

    /// Send a compressed (H.264/HEVC Annex-B) access unit.
    /// Called from the encoder's output callback (serial).
    func sendEncodedFrame(_ data: Data, isKeyframe: Bool, format: UInt16,
                          width: Int, height: Int) {
        guard socket >= 0, !data.isEmpty else { return }

        frameID += 1
        let currentFrameID = frameID
        let frameSize = data.count
        let totalPackets = (frameSize + NetworkConfig.maxPayload - 1) / NetworkConfig.maxPayload
        let flags: UInt16 = isKeyframe ? 0x0001 : 0x0000  // TLNK_FLAG_KEY_FRAME

        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            guard let srcBase = raw.baseAddress else { return }
            for seq in 0..<totalPackets {
                let dataOffset = seq * NetworkConfig.maxPayload
                let payloadSize = min(NetworkConfig.maxPayload, frameSize - dataOffset)

                var packet = makeHeader(frameID: currentFrameID,
                                        seq: seq,
                                        totalPackets: totalPackets,
                                        payloadSize: payloadSize,
                                        frameTotalBytes: frameSize,
                                        width: width,
                                        height: height,
                                        format: format,
                                        flags: flags)
                packet.append(Data(bytes: srcBase + dataOffset, count: payloadSize))
                send(packet)
            }
        }
    }

    /// Legacy/test path: send a raw BGRA32 frame (uncompressed).
    func sendFrame(pixelData: UnsafeRawPointer, size: Int,
                   width: Int, height: Int, bytesPerRow: Int) {
        guard socket >= 0 else { return }

        frameID += 1
        let currentFrameID = frameID

        let rowBytes = width * 4  // BGRA32
        let frameSize = rowBytes * height
        let totalPackets = (frameSize + NetworkConfig.maxPayload - 1) / NetworkConfig.maxPayload

        for seq in 0..<totalPackets {
            let dataOffset = seq * NetworkConfig.maxPayload
            let payloadSize = min(NetworkConfig.maxPayload, frameSize - dataOffset)

            var packet = makeHeader(frameID: currentFrameID,
                                    seq: seq,
                                    totalPackets: totalPackets,
                                    payloadSize: payloadSize,
                                    frameTotalBytes: frameSize,
                                    width: width,
                                    height: height,
                                    format: NetworkConfig.formatBGRA32,
                                    flags: 0)

            // Account for bytesPerRow padding in the source buffer.
            let startRow = dataOffset / rowBytes
            let rowOffset = dataOffset % rowBytes
            let sourcePtr = pixelData + (startRow * bytesPerRow) + rowOffset

            packet.append(Data(bytes: sourcePtr, count: payloadSize))
            send(packet)
        }
    }

    // MARK: - Header / send helpers

    /// Build a 40-byte TlnkFrameHeader (must match common/frame_header.h, little-endian).
    ///   0  u32 magic   4 u64 frame_id   12 u32 packet_seq   16 u32 total_packets
    ///  20  u32 payload_size  24 u32 frame_total_bytes  28 u32 width  32 u32 height
    ///  36  u16 format  38 u16 flags   (40 end)
    private func makeHeader(frameID: UInt64, seq: Int, totalPackets: Int,
                            payloadSize: Int, frameTotalBytes: Int,
                            width: Int, height: Int, format: UInt16, flags: UInt16) -> Data {
        precondition(NetworkConfig.headerSize == 40)
        var header = Data(count: NetworkConfig.headerSize)
        header.withUnsafeMutableBytes { buf in
            let ptr = buf.baseAddress!

            var magic = NetworkConfig.magic.littleEndian
            memcpy(ptr, &magic, 4)

            var fid = frameID.littleEndian
            memcpy(ptr + 4, &fid, 8)

            var pseq = UInt32(seq).littleEndian
            memcpy(ptr + 12, &pseq, 4)

            var total = UInt32(totalPackets).littleEndian
            memcpy(ptr + 16, &total, 4)

            var psize = UInt32(payloadSize).littleEndian
            memcpy(ptr + 20, &psize, 4)

            var ftotal = UInt32(frameTotalBytes).littleEndian
            memcpy(ptr + 24, &ftotal, 4)

            var w = UInt32(width).littleEndian
            memcpy(ptr + 28, &w, 4)

            var h = UInt32(height).littleEndian
            memcpy(ptr + 32, &h, 4)

            var fmt = format.littleEndian
            memcpy(ptr + 36, &fmt, 2)

            var fl = flags.littleEndian
            memcpy(ptr + 38, &fl, 2)
        }
        return header
    }

    private func send(_ packet: Data) {
        packet.withUnsafeBytes { buf in
            withUnsafePointer(to: destAddr) { addrPtr in
                addrPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockPtr in
                    _ = sendto(socket, buf.baseAddress, buf.count, 0,
                               sockPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }
}
