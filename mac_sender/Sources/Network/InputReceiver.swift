import Foundation

struct InputEventMessage: Codable, Equatable {
    let type: String
    let sessionID: StreamSessionID
    let timestamp: Int64?
    let x: Float?
    let y: Float?
    let button: Int?
    let state: String?
    let keyCode: UInt16?
    let modifiers: [String]?
    let deltaX: Int32?
    let deltaY: Int32?

    enum CodingKeys: String, CodingKey {
        case type, timestamp, x, y, button, state, modifiers
        case sessionID = "session_id"
        case keyCode = "key_code"
        case deltaX = "delta_x"
        case deltaY = "delta_y"
    }
}

/// Input TCP server. The receive worker exclusively closes the listening and
/// client sockets, while lifecycle changes only update synchronized state.
final class InputReceiver {
    typealias EventCallback = (InputEventMessage) -> Void

    private let lock = NSLock()
    private var running = false
    private var activeSession: StreamSessionID?
    private var listenSocket: Int32 = -1
    private var callback: EventCallback?
    private var disconnectCallback: (() -> Void)?
    private var thread: Thread?
    private let stopped = DispatchGroup()

    func start(callback: @escaping EventCallback,
               onDisconnect: @escaping () -> Void) -> Bool {
        lock.lock()
        let alreadyRunning = running
        lock.unlock()
        if alreadyRunning { return true }
        let socket = Darwin.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
        guard socket >= 0 else {
            logger.error("InputReceiver: socket failed (errno=\(errno))")
            return false
        }
        var reuse: Int32 = 1
        var noSignal: Int32 = 1
        guard setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse,
                         socklen_t(MemoryLayout<Int32>.size)) == 0,
              setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSignal,
                         socklen_t(MemoryLayout<Int32>.size)) == 0 else {
            logger.error("InputReceiver: socket options failed (errno=\(errno))")
            close(socket)
            return false
        }
        let flags = fcntl(socket, F_GETFL, 0)
        guard flags >= 0, fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0 else {
            close(socket)
            return false
        }

        var address = sockaddr_in()
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = NetworkConfig.inputPort.bigEndian
        address.sin_addr.s_addr = inet_addr(NetworkConfig.macIP)
        let bindResult = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(socket, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0, listen(socket, 1) == 0 else {
            logger.error("InputReceiver: bind/listen failed (errno=\(errno))")
            close(socket)
            return false
        }

        lock.lock()
        guard !running else { lock.unlock(); close(socket); return true }
        running = true
        listenSocket = socket
        self.callback = callback
        self.disconnectCallback = onDisconnect
        stopped.enter()
        lock.unlock()

        let worker = Thread { [weak self] in self?.receiveLoop() }
        worker.name = "ThunderLinkInputReceiver"
        worker.qualityOfService = .userInteractive
        thread = worker
        worker.start()
        logger.info("InputReceiver: listening on \(NetworkConfig.macIP):\(NetworkConfig.inputPort)")
        return true
    }

    func setActiveSession(_ sessionID: StreamSessionID?) {
        lock.lock()
        activeSession = sessionID
        lock.unlock()
    }

    func stop(completion: (() -> Void)? = nil) {
        lock.lock()
        running = false
        activeSession = nil
        lock.unlock()
        guard let completion else { return }
        DispatchQueue.global(qos: .userInitiated).async { [stopped] in
            stopped.wait()
            completion()
        }
    }

    private var shouldRun: Bool {
        lock.lock(); defer { lock.unlock() }
        return running
    }

    private func currentSession() -> StreamSessionID? {
        lock.lock(); defer { lock.unlock() }
        return activeSession
    }

    private func receiveLoop() {
        lock.lock()
        let listener = listenSocket
        lock.unlock()
        defer {
            if listener >= 0 { close(listener) }
            lock.lock()
            listenSocket = -1
            running = false
            lock.unlock()
            stopped.leave()
        }

        while shouldRun {
            var descriptor = pollfd(fd: listener, events: Int16(POLLIN), revents: 0)
            let result = Darwin.poll(&descriptor, 1, 100)
            if result < 0 {
                if errno == EINTR { continue }
                logger.error("InputReceiver: listener poll failed (errno=\(errno))")
                break
            }
            guard result > 0 else { continue }
            if (descriptor.revents & Int16(POLLERR | POLLHUP | POLLNVAL)) != 0 { break }

            let client = accept(listener, nil, nil)
            if client < 0 {
                if errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR {
                    logger.warn("InputReceiver: accept failed (errno=\(errno))")
                }
                continue
            }
            let flags = fcntl(client, F_GETFL, 0)
            var noDelay: Int32 = 1
            guard flags >= 0, fcntl(client, F_SETFL, flags | O_NONBLOCK) == 0,
                  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &noDelay,
                             socklen_t(MemoryLayout<Int32>.size)) == 0 else {
                close(client)
                continue
            }
            logger.info("InputReceiver: Windows connected")
            handleClient(client)
            close(client)
            disconnectCallback?()
            logger.info("InputReceiver: Windows disconnected")
        }
    }

    private func handleClient(_ socket: Int32) {
        let decoder = FramedJSONDecoder()
        let jsonDecoder = JSONDecoder()
        while shouldRun {
            do { try decoder.checkDeadline() } catch { break }
            var descriptor = pollfd(fd: socket, events: Int16(POLLIN), revents: 0)
            let result = Darwin.poll(&descriptor, 1, 100)
            if result < 0 {
                if errno == EINTR { continue }
                break
            }
            guard result > 0 else { continue }
            if (descriptor.revents & Int16(POLLERR | POLLHUP | POLLNVAL)) != 0 { break }

            var bytes = [UInt8](repeating: 0, count: 16_384)
            let count = recv(socket, &bytes, bytes.count, 0)
            if count == 0 { break }
            if count < 0 {
                if errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR { continue }
                break
            }
            do {
                for object in try decoder.append(Data(bytes.prefix(count))) {
                    let data = try JSONSerialization.data(withJSONObject: object)
                    let event = try jsonDecoder.decode(InputEventMessage.self, from: data)
                    guard event.sessionID == currentSession() else {
                        logger.debug("InputReceiver: dropped stale input for \(event.sessionID)")
                        continue
                    }
                    callback?(event)
                }
            } catch {
                logger.warn("InputReceiver: malformed input stream: \(error)")
                break
            }
        }
    }
}
