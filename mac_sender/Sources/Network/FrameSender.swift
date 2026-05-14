import Foundation

/// Splits captured frames into UDP packets and sends them to the Windows receiver.
/// Uses POSIX sockets for maximum control over packet construction.
final class FrameSender {
    private var socket: Int32 = -1
    private var destAddr: sockaddr_in
    private var frameID: UInt64 = 0
    private let sendQueue = DispatchQueue(label: "com.thunderlink.framesender",
                                          qos: .userInteractive)

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

        // Set send buffer size
        var bufSize = Int32(NetworkConfig.sendBufferSize)
        setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &bufSize, socklen_t(MemoryLayout<Int32>.size))

        // Bind to our local IP
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

    /// Send a captured frame by splitting it into packets.
    /// Called from the capture callback on the frame queue.
    func sendFrame(pixelData: UnsafeRawPointer, size: Int,
                   width: Int, height: Int, bytesPerRow: Int) {
        guard socket >= 0 else { return }

        frameID += 1
        let currentFrameID = frameID

        // Calculate total data to send (using actual pixel rows, not bytesPerRow padding)
        let rowBytes = width * 4  // BGRA32
        let frameSize = rowBytes * height
        let totalPackets = (frameSize + NetworkConfig.maxPayload - 1) / NetworkConfig.maxPayload

        // Build and send each packet
        for seq in 0..<totalPackets {
            let dataOffset = seq * NetworkConfig.maxPayload
            let payloadSize = min(NetworkConfig.maxPayload, frameSize - dataOffset)

            // Construct header (must match TlnkFrameHeader in common/frame_header.h).
            //   0  u32  magic
            //   4  u64  frame_id
            //  12  u32  packet_seq
            //  16  u32  total_packets
            //  20  u32  payload_size
            //  24  u32  width
            //  28  u32  height
            //  32  u16  format
            //  34  u16  flags
            //  36  end
            precondition(NetworkConfig.headerSize == 36)
            var header = Data(count: NetworkConfig.headerSize)
            header.withUnsafeMutableBytes { buf in
                let ptr = buf.baseAddress!

                var magic = NetworkConfig.magic.littleEndian
                memcpy(ptr, &magic, 4)

                var fid = currentFrameID.littleEndian
                memcpy(ptr + 4, &fid, 8)

                var pseq = UInt32(seq).littleEndian
                memcpy(ptr + 12, &pseq, 4)

                var total = UInt32(totalPackets).littleEndian
                memcpy(ptr + 16, &total, 4)

                var psize = UInt32(payloadSize).littleEndian
                memcpy(ptr + 20, &psize, 4)

                var w = UInt32(width).littleEndian
                memcpy(ptr + 24, &w, 4)

                var h = UInt32(height).littleEndian
                memcpy(ptr + 28, &h, 4)

                var fmt = NetworkConfig.formatBGRA32.littleEndian
                memcpy(ptr + 32, &fmt, 2)

                var flags: UInt16 = 0
                memcpy(ptr + 34, &flags, 2)
            }

            // Calculate source pointer accounting for bytesPerRow padding
            // dataOffset is in terms of tightly-packed data, so we need to
            // convert to the padded source buffer
            let startRow = dataOffset / rowBytes
            let rowOffset = dataOffset % rowBytes
            let sourcePtr = pixelData + (startRow * bytesPerRow) + rowOffset

            // Build packet: header + payload
            var packet = header
            packet.append(Data(bytes: sourcePtr, count: payloadSize))

            // Send
            packet.withUnsafeBytes { buf in
                withUnsafePointer(to: destAddr) { addrPtr in
                    addrPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockPtr in
                        sendto(socket, buf.baseAddress, buf.count, 0,
                               sockPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
            }
        }
    }
}
