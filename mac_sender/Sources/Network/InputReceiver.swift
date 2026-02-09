import Foundation

/// Codable structure matching the JSON input protocol.
struct InputEventMessage: Codable {
    let type: String
    let timestamp: Int64?
    let x: Float?
    let y: Float?
    let button: Int?
    let state: String?
    let key_code: UInt16?
    let modifiers: [String]?
    let delta_x: Int32?
    let delta_y: Int32?
}

/// Receives input events from the Windows receiver over TCP.
/// Listens on the input port and delivers deserialized events via a callback.
final class InputReceiver {
    typealias EventCallback = (InputEventMessage) -> Void

    private var listenSocket: Int32 = -1
    private var clientSocket: Int32 = -1
    private var running = false
    private var receiveThread: Thread?
    private var callback: EventCallback?
    private let decoder = JSONDecoder()

    func start(callback: @escaping EventCallback) -> Bool {
        self.callback = callback

        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
        guard listenSocket >= 0 else {
            logger.error("InputReceiver: failed to create TCP socket")
            return false
        }

        var reuse: Int32 = 1
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   socklen_t(MemoryLayout<Int32>.size))

        var nodelay: Int32 = 1
        setsockopt(listenSocket, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                   socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = UInt16(NetworkConfig.inputPort).bigEndian
        addr.sin_addr.s_addr = inet_addr(NetworkConfig.macIP)

        let bindResult = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockPtr in
                bind(listenSocket, sockPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        guard bindResult >= 0 else {
            logger.error("InputReceiver: bind failed (errno=\(errno))")
            close(listenSocket)
            listenSocket = -1
            return false
        }

        guard listen(listenSocket, 1) >= 0 else {
            logger.error("InputReceiver: listen failed (errno=\(errno))")
            close(listenSocket)
            listenSocket = -1
            return false
        }

        running = true
        receiveThread = Thread {
            self.receiveLoop()
        }
        receiveThread?.qualityOfService = .userInteractive
        receiveThread?.start()

        logger.info("InputReceiver: listening on \(NetworkConfig.macIP):\(NetworkConfig.inputPort)")
        return true
    }

    func stop() {
        running = false
        if clientSocket >= 0 {
            close(clientSocket)
            clientSocket = -1
        }
        if listenSocket >= 0 {
            close(listenSocket)
            listenSocket = -1
        }
    }

    private func receiveLoop() {
        while running {
            // Accept connection from Windows
            var clientAddr = sockaddr_in()
            var addrLen = socklen_t(MemoryLayout<sockaddr_in>.size)

            let client = withUnsafeMutablePointer(to: &clientAddr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockPtr in
                    accept(listenSocket, sockPtr, &addrLen)
                }
            }

            guard client >= 0 else {
                if running { logger.debug("InputReceiver: accept returned \(client)") }
                continue
            }

            logger.info("InputReceiver: Windows connected")
            clientSocket = client

            // Read messages from this client
            handleClient(client)

            close(client)
            clientSocket = -1
            logger.info("InputReceiver: Windows disconnected")
        }
    }

    private func handleClient(_ sock: Int32) {
        while running {
            // Read 4-byte length prefix (big-endian)
            var lenBuf = [UInt8](repeating: 0, count: 4)
            guard readExact(sock, buffer: &lenBuf, count: 4) else { break }

            let len = (UInt32(lenBuf[0]) << 24) | (UInt32(lenBuf[1]) << 16) |
                      (UInt32(lenBuf[2]) << 8) | UInt32(lenBuf[3])

            guard len > 0 && len < 65536 else {
                logger.warn("InputReceiver: invalid message length \(len)")
                break
            }

            // Read JSON payload
            var jsonBuf = [UInt8](repeating: 0, count: Int(len))
            guard readExact(sock, buffer: &jsonBuf, count: Int(len)) else { break }

            // Decode and deliver
            let data = Data(jsonBuf)
            do {
                let event = try decoder.decode(InputEventMessage.self, from: data)
                callback?(event)
            } catch {
                logger.warn("InputReceiver: JSON decode error: \(error)")
            }
        }
    }

    private func readExact(_ sock: Int32, buffer: inout [UInt8], count: Int) -> Bool {
        var received = 0
        while received < count {
            let r = recv(sock, &buffer + received, count - received, 0)
            if r <= 0 { return false }
            received += r
        }
        return true
    }
}
