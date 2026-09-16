import Foundation
import Network
import Security

enum CrossServicePhase: String {
    case stopped = "Cross unavailable"
    case listening = "Cross ready to pair"
    case pairing = "Pairing approval required"
    case paired = "Paired — waiting for Cross"
    case configuring = "Preparing Cross…"
    case ready = "Cross ready"
    case remote = "Windows controls this Mac"
    case failed = "Cross failed"
}

enum CrossSystemInhibition: Hashable {
    case sleeping
    case screensSleeping
    case sessionInactive
    case screenLocked

    var message: String {
        switch self {
        case .sleeping: return "this Mac is asleep"
        case .screensSleeping: return "the Mac displays are asleep"
        case .sessionInactive: return "the Mac user session is inactive"
        case .screenLocked: return "this Mac is locked"
        }
    }
}

struct CrossSystemInhibitionState {
    private(set) var reasons: Set<CrossSystemInhibition> = []

    var isSuspended: Bool { !reasons.isEmpty }
    var message: String {
        let detail = reasons.map(\.message).sorted().joined(separator: ", ")
        return detail.isEmpty ? "" : "Cross is paused because \(detail)"
    }

    mutating func update(_ reason: CrossSystemInhibition, active: Bool) {
        if active { reasons.insert(reason) }
        else { reasons.remove(reason) }
    }
}

struct CrossServiceSnapshot {
    var phase: CrossServicePhase = .stopped
    var peerName: String?
    var peerFingerprint: String?
    var verificationCode: String?
    var interfaceName: String?
    var error: String?
    var pairedPeers: [CrossPairedPeer] = []
    var configured = false
    var remoteOwnership = false
}

/// TLS-only Cross server. One accepted client owns the ordered channel; a new
/// connection never displaces a healthy session.
final class CrossServer {
    typealias PrepareHandler = (@escaping (Result<Void, Error>) -> Void) -> Void

    var onSnapshot: ((CrossServiceSnapshot) -> Void)?
    var onPrepareCross: PrepareHandler?
    var onCrossEnded: (() -> Void)?

    private let queue = DispatchQueue(label: "com.thunderlink.cross-server", qos: .userInitiated)
    private let identityStore: CrossIdentityStore
    private let topologyProvider: CrossDisplayTopologyProvider
    private let input: CrossInputController
    private var identity: CrossLocalIdentity?
    private var listener: NWListener?
    private var connection: NWConnection?
    private var connectionToken = UUID()
    private var decoder = CrossFrameDecoder()
    private var topology: CrossDisplayTopology
    private var topologyMonitor: CrossDisplayTopologyMonitor?
    private var snapshot = CrossServiceSnapshot()
    private var peerFingerprint: String?
    private var peerHello: CrossPeerHello?
    private var localPairApproved = false
    private var remotePairApproved = false
    private var mutuallyPaired = false
    private var configured: CrossConfigure?
    private var usedSessionIDs: Set<StreamSessionID> = []
    private var currentGeneration: UInt64?
    private var lastPingMilliseconds: UInt64 = 0
    private var outgoing: [Data] = []
    private var writeInProgress = false
    private var closeAfterWrites = false
    private var handshakeTimer: DispatchSourceTimer?
    private var pairingTimer: DispatchSourceTimer?
    private var leaseTimer: DispatchSourceTimer?
    private var writeTimer: DispatchSourceTimer?
    private var preparingToken: UUID?
    private var running = false
    private var systemInhibition = CrossSystemInhibitionState()

    init(identityStore: CrossIdentityStore = CrossIdentityStore(),
         topologyProvider: CrossDisplayTopologyProvider = CrossDisplayTopologyProvider(),
         input: CrossInputController = CrossInputController()) {
        self.identityStore = identityStore
        self.topologyProvider = topologyProvider
        self.input = input
        self.topology = topologyProvider.current()
    }

    func start() {
        queue.async { [weak self] in self?.startNow() }
    }

    func stop(completion: (() -> Void)? = nil) {
        queue.async { [weak self] in
            guard let self else { completion?(); return }
            self.running = false
            self.handshakeTimer?.cancel(); self.handshakeTimer = nil
            self.pairingTimer?.cancel(); self.pairingTimer = nil
            self.leaseTimer?.cancel(); self.leaseTimer = nil
            self.writeTimer?.cancel(); self.writeTimer = nil
            self.topologyMonitor?.stop(); self.topologyMonitor = nil
            self.listener?.cancel(); self.listener = nil
            self.closeConnection(notifyEnd: true)
            self.snapshot.phase = .stopped
            self.snapshot.error = nil
            self.publish()
            completion?()
        }
    }

    func approvePairing() {
        queue.async { [weak self] in
            guard let self, self.peerHello != nil, !self.mutuallyPaired else { return }
            self.localPairApproved = true
            self.send(["type": "pair_approve"])
            self.finishPairingIfReady()
        }
    }

    func rejectPairing() {
        queue.async { [weak self] in
            self?.failAndClose("Pairing was declined on this Mac")
        }
    }

    func forgetPeer(fingerprint: String) {
        queue.async { [weak self] in
            guard let self else { return }
            do {
                try self.identityStore.forgetPeer(fingerprint: fingerprint)
                if self.peerFingerprint == fingerprint { self.closeConnection(notifyEnd: true) }
                self.snapshot.pairedPeers = self.identityStore.pairedPeers()
                self.publish()
            } catch { self.snapshot.error = error.localizedDescription; self.publish() }
        }
    }

    func disconnectClient(reason: String = "Disconnected on Mac") {
        queue.async { [weak self] in
            self?.failAndClose(reason)
        }
    }

    func updateSystemInhibition(_ reason: CrossSystemInhibition, active: Bool) {
        queue.async { [weak self] in
            guard let self else { return }
            let wasSuspended = self.systemInhibition.isSuspended
            self.systemInhibition.update(reason, active: active)
            if self.systemInhibition.isSuspended {
                self.suspendConfiguredSession(reason: self.systemInhibition.message)
                return
            }
            guard wasSuspended else { return }
            self.snapshot.error = nil
            if self.configured != nil {
                self.snapshot.phase = .ready
                self.snapshot.remoteOwnership = false
            } else if self.mutuallyPaired {
                self.snapshot.phase = .paired
            } else if self.peerHello != nil {
                self.snapshot.phase = self.localPairApproved ? .paired : .pairing
            } else if self.running {
                self.snapshot.phase = .listening
            }
            self.publish()
        }
    }

    private func suspendConfiguredSession(reason: String) {
        guard configured != nil else {
            snapshot.error = reason
            publish()
            return
        }
        let generation = currentGeneration
        input.reset()
        currentGeneration = nil
        snapshot.remoteOwnership = false
        snapshot.phase = .ready
        snapshot.error = reason
        if let generation, let configured {
            send(["type": "local", "session_id": configured.sessionID.description,
                  "topology_revision": NSNumber(value: configured.topologyRevision),
                  "generation": NSNumber(value: generation)])
        }
        publish()
    }

    func refreshPermission() {
        queue.async { [weak self] in
            guard let self, self.configured != nil, !InputInjector.checkAccessibilityPermission() else { return }
            self.failAndClose("Accessibility permission was removed; Cross input stopped")
        }
    }

    private func startNow() {
        guard !running else { return }
        do {
            let identity = try identityStore.loadOrCreateIdentity()
            self.identity = identity
            topology = topologyProvider.current()
            let tls = NWProtocolTLS.Options()
            guard let secIdentity = sec_identity_create(identity.identity) else {
                throw CrossIdentityError.identityUnavailable
            }
            sec_protocol_options_set_local_identity(tls.securityProtocolOptions, secIdentity)
            sec_protocol_options_set_peer_authentication_required(tls.securityProtocolOptions, true)
            sec_protocol_options_set_min_tls_protocol_version(tls.securityProtocolOptions, .TLSv12)
            sec_protocol_options_set_verify_block(tls.securityProtocolOptions, { _, trust, complete in
                let securityTrust = sec_trust_copy_ref(trust).takeRetainedValue()
                complete(SecTrustGetCertificateCount(securityTrust) > 0)
            }, queue)
            let tcp = NWProtocolTCP.Options()
            tcp.noDelay = true
            tcp.enableKeepalive = true
            let parameters = NWParameters(tls: tls, tcp: tcp)
            let listener = try NWListener(using: parameters, on: NWEndpoint.Port(rawValue: NetworkConfig.crossPort)!)
            listener.service = NWListener.Service(name: Host.current().localizedName ?? "Mac",
                                                  type: NetworkConfig.crossService,
                                                  domain: "local.",
                                                  txtRecord: Self.txtRecord(fingerprint: identity.fingerprint))
            listener.stateUpdateHandler = { [weak self] state in self?.queue.async { self?.listenerState(state) } }
            listener.newConnectionHandler = { [weak self] connection in self?.queue.async { self?.accept(connection) } }
            self.listener = listener
            running = true
            snapshot.phase = .listening
            snapshot.pairedPeers = identityStore.pairedPeers()
            snapshot.error = systemInhibition.isSuspended ? systemInhibition.message : nil
            publish()
            listener.start(queue: queue)
            let monitor = CrossDisplayTopologyMonitor(queue: queue, provider: topologyProvider,
                                                      initial: topology) { [weak self] in self?.topologyChanged($0) }
            topologyMonitor = monitor
            monitor.start()
            startLeaseTimer()
            logger.info("Cross: TLS service listening on port \(NetworkConfig.crossPort)")
        } catch {
            snapshot.phase = .failed
            snapshot.error = error.localizedDescription
            publish()
            logger.error("Cross: failed to start secure service: \(error.localizedDescription)")
        }
    }

    private func listenerState(_ state: NWListener.State) {
        switch state {
        case .failed(let error):
            running = false
            listener?.cancel(); listener = nil
            closeConnection(notifyEnd: true)
            snapshot.phase = .failed
            snapshot.error = "Cross listener failed: \(error.localizedDescription)"
            publish()
        case .waiting(let error):
            snapshot.error = "Cross is waiting for the local network: \(error.localizedDescription)"
            publish()
        default: break
        }
    }

    private func accept(_ candidate: NWConnection) {
        guard running, connection == nil else { candidate.cancel(); return }
        connection = candidate
        connectionToken = UUID()
        decoder = CrossFrameDecoder()
        resetPeerState()
        let token = connectionToken
        candidate.stateUpdateHandler = { [weak self] state in
            self?.queue.async { self?.connectionState(state, token: token) }
        }
        candidate.start(queue: queue)
        startHandshakeTimer(token: token)
    }

    private func connectionState(_ state: NWConnection.State, token: UUID) {
        guard token == connectionToken else { return }
        switch state {
        case .ready:
            // TLS is ready, but the authenticated application HELLO must also
            // arrive inside the five-second handshake window.
            startHandshakeTimer(token: token)
            guard let fingerprint = peerCertificateFingerprint() else {
                failAndClose("The Windows TLS client did not present a certificate")
                return
            }
            peerFingerprint = fingerprint
            snapshot.peerFingerprint = fingerprint
            snapshot.interfaceName = activeInterfaceName()
            if let identity {
                send(CrossProtocol.hello(deviceID: identity.fingerprint,
                                         name: Host.current().localizedName ?? "Mac"))
            }
            receiveNext(token: token)
        case .failed(let error):
            logger.warn("Cross: secure connection failed: \(error.localizedDescription)")
            closeConnection(notifyEnd: true)
        case .cancelled:
            closeConnection(notifyEnd: true)
        default: break
        }
    }

    private func peerCertificateFingerprint() -> String? {
        guard let connection,
              let metadata = connection.metadata(definition: NWProtocolTLS.definition) as? NWProtocolTLS.Metadata else { return nil }
        var leaf: SecCertificate?
        _ = sec_protocol_metadata_access_peer_certificate_chain(metadata.securityProtocolMetadata) { certificate in
            if leaf == nil { leaf = sec_certificate_copy_ref(certificate).takeRetainedValue() }
        }
        guard let leaf else { return nil }
        return CrossProtocol.fingerprint(SecCertificateCopyData(leaf) as Data)
    }

    private func receiveNext(token: UUID) {
        guard let connection, token == connectionToken else { return }
        connection.receive(minimumIncompleteLength: 1,
                           maximumLength: FramedJSON.maximumBodyBytes + 4) { [weak self] data, _, complete, error in
            guard let self else { return }
            self.queue.async {
                guard token == self.connectionToken else { return }
                if let data, !data.isEmpty {
                    do {
                        for object in try self.decoder.append(data) { try self.handle(object) }
                    } catch {
                        self.failAndClose(error.localizedDescription)
                        return
                    }
                }
                if complete || error != nil { self.closeConnection(notifyEnd: true); return }
                self.receiveNext(token: token)
            }
        }
    }

    private func handle(_ object: [String: Any]) throws {
        let type = try CrossProtocol.type(of: object)
        if peerHello == nil {
            guard type == "hello", let peerFingerprint else { throw CrossProtocolError.malformed("hello") }
            let hello = try CrossProtocol.parseHello(object, certificateFingerprint: peerFingerprint)
            handshakeTimer?.cancel(); handshakeTimer = nil
            peerHello = hello
            snapshot.peerName = hello.name
            let peers = identityStore.pairedPeers()
            let pinned = peers.contains { $0.fingerprint == peerFingerprint }
            if !pinned && peers.contains(where: { $0.name.caseInsensitiveCompare(hello.name) == .orderedSame }) {
                throw CrossProtocolError.malformed("This known device presented a changed identity. Forget it before pairing again")
            }
            localPairApproved = pinned
            snapshot.phase = pinned ? .paired : .pairing
            if !pinned, let identity {
                snapshot.verificationCode = CrossProtocol.verificationCode(clientFingerprint: peerFingerprint,
                                                                            serverFingerprint: identity.fingerprint)
            }
            publish()
            startPairingTimer()
            if pinned { send(["type": "pair_approve"]) }
            return
        }
        if !mutuallyPaired {
            guard CrossProtocol.allowsBeforePairing(type) else { throw CrossProtocolError.notPaired }
            remotePairApproved = true
            finishPairingIfReady()
            return
        }

        switch type {
        case "configure": try handleConfigure(try CrossProtocol.parseConfigure(object))
        case "enter":
            let parsed = try CrossProtocol.parseEnter(object)
            guard let configured,
                  parsed.0 == configured.sessionID,
                  parsed.1 == configured.topologyRevision else { return }
            guard !systemInhibition.isSuspended else { return }
            do {
                try input.enter(sessionID: parsed.0, topologyRevision: parsed.1, generation: parsed.2,
                                displayID: parsed.3, x: parsed.4, y: parsed.5)
            } catch CrossProtocolError.invalidGeneration {
                return
            }
            lastPingMilliseconds = CrossOwnershipLease.started(now: crossMonotonicMilliseconds())
            currentGeneration = parsed.2
            snapshot.remoteOwnership = true
            snapshot.phase = .remote
            publish()
            send(["type": "entered", "session_id": parsed.0.description,
                  "topology_revision": NSNumber(value: parsed.1), "generation": NSNumber(value: parsed.2)])
        case "input":
            let envelope = try CrossProtocol.parseInput(object)
            guard let configured,
                  envelope.sessionID == configured.sessionID,
                  envelope.topologyRevision == configured.topologyRevision,
                  envelope.generation == currentGeneration else { return }
            try input.process(envelope)
            if envelope.event.type == "reset" {
                currentGeneration = nil
                snapshot.remoteOwnership = false
                snapshot.phase = .ready
                publish()
                send(["type": "local", "session_id": envelope.sessionID.description,
                      "topology_revision": NSNumber(value: envelope.topologyRevision),
                      "generation": NSNumber(value: envelope.generation)])
            }
        case "leave":
            let (session, revision) = try CrossProtocol.parseSession(object)
            let generation = try CrossProtocol.parseGeneration(object)
            guard let configured,
                  session == configured.sessionID,
                  revision == configured.topologyRevision,
                  generation == currentGeneration else { return }
            try input.leave(sessionID: session, topologyRevision: revision, generation: generation)
            currentGeneration = nil
            snapshot.remoteOwnership = false
            snapshot.phase = .ready
            publish()
            send(["type": "local", "session_id": session.description,
                  "topology_revision": NSNumber(value: revision), "generation": NSNumber(value: generation)])
        case "reset":
            let (session, revision) = try CrossProtocol.parseSession(object)
            guard configured?.sessionID == session, configured?.topologyRevision == revision else { return }
            let generation = currentGeneration
            if object["generation"] != nil {
                let supplied = try CrossProtocol.parseGeneration(object)
                guard generation == supplied else { return }
            }
            input.reset(); currentGeneration = nil
            snapshot.remoteOwnership = false; snapshot.phase = .ready; publish()
            if let generation {
                send(["type": "local", "session_id": session.description,
                      "topology_revision": NSNumber(value: revision), "generation": NSNumber(value: generation)])
            }
        case "stop":
            if let configured, (object["session_id"] != nil || object["topology_revision"] != nil) {
                let (session, revision) = try CrossProtocol.parseSession(object)
                guard session == configured.sessionID, revision == configured.topologyRevision else { return }
            }
            endConfiguredSession()
        case "ping": try handlePing(object)
        case "identify":
            let displays = topology.displays
            DispatchQueue.main.async { CrossDisplayIdentifierPresenter.shared.show(displays: displays) }
        default: throw CrossProtocolError.malformed("type")
        }
    }

    private func handleConfigure(_ request: CrossConfigure) throws {
        guard !systemInhibition.isSuspended else {
            send(["type": "error", "reason": systemInhibition.message])
            return
        }
        guard request.topologyRevision == topology.revision else { throw CrossProtocolError.invalidTopology }
        if let identityError = topology.identityError {
            snapshot.phase = .failed
            snapshot.error = identityError
            publish()
            send(["type": "error", "reason": identityError])
            return
        }
        guard InputInjector.checkAccessibilityPermission() else {
            send(["type": "error", "reason": "Accessibility permission is required for Cross input"])
            return
        }
        guard usedSessionIDs.count < 4_096 else {
            throw CrossProtocolError.malformed("Cross session limit reached; reconnect")
        }
        guard !usedSessionIDs.contains(request.sessionID) else { throw CrossProtocolError.invalidSession }
        usedSessionIDs.insert(request.sessionID)
        guard preparingToken == nil else { throw CrossProtocolError.malformed("configure already in progress") }
        if configured != nil {
            input.invalidateSession()
            configured = request
            currentGeneration = nil
            input.configure(request, topology: topology)
            lastPingMilliseconds = crossMonotonicMilliseconds()
            snapshot.configured = true
            snapshot.remoteOwnership = false
            snapshot.phase = .ready
            snapshot.error = nil
            publish()
            send(["type": "ready", "session_id": request.sessionID.description,
                  "topology_revision": NSNumber(value: request.topologyRevision)])
            return
        }
        snapshot.phase = .configuring
        publish()
        let token = connectionToken
        let preparation = UUID()
        preparingToken = preparation
        guard let prepare = onPrepareCross else { throw CrossProtocolError.malformed("Cross lifecycle unavailable") }
        prepare { [weak self] result in
            guard let self else { return }
            self.queue.async {
                guard self.connectionToken == token, self.connection != nil,
                      self.preparingToken == preparation,
                      self.topology.revision == request.topologyRevision else { return }
                self.preparingToken = nil
                guard !self.systemInhibition.isSuspended else {
                    self.snapshot.phase = .paired
                    self.snapshot.error = self.systemInhibition.message
                    self.publish()
                    self.send(["type": "error", "reason": self.systemInhibition.message])
                    self.onCrossEnded?()
                    return
                }
                switch result {
                case .success:
                    self.configured = request
                    self.input.configure(request, topology: self.topology)
                    self.lastPingMilliseconds = crossMonotonicMilliseconds()
                    self.snapshot.configured = true
                    self.snapshot.phase = .ready
                    self.snapshot.error = nil
                    self.publish()
                    self.send(["type": "ready", "session_id": request.sessionID.description,
                               "topology_revision": NSNumber(value: request.topologyRevision)])
                case .failure(let error): self.failAndClose(error.localizedDescription)
                }
            }
        }
    }

    private func handlePing(_ object: [String: Any]) throws {
        guard let sequence = object["sequence"] as? NSNumber,
              CFGetTypeID(sequence) != CFBooleanGetTypeID(), sequence.int64Value >= 0,
              NSNumber(value: sequence.int64Value) == sequence else { throw CrossProtocolError.malformed("sequence") }
        let renewOwnership = try CrossProtocol.pingRenewsOwnership(object)
        if let configured {
            let session = try CrossProtocol.parseSessionID(object)
            guard session == configured.sessionID else { throw CrossProtocolError.invalidSession }
            lastPingMilliseconds = CrossOwnershipLease.lastRenewal(
                previous: lastPingMilliseconds,
                now: crossMonotonicMilliseconds(),
                renewOwnership: renewOwnership)
        }
        send(["type": "pong", "sequence": sequence])
    }

    private func finishPairingIfReady() {
        guard CrossProtocol.pairingComplete(localApproved: localPairApproved,
                                            remoteApproved: remotePairApproved), !mutuallyPaired,
              let peerFingerprint, let peerHello else { return }
        do {
            try identityStore.rememberPeer(fingerprint: peerFingerprint, name: peerHello.name)
            mutuallyPaired = true
            pairingTimer?.cancel(); pairingTimer = nil
            snapshot.phase = .paired
            snapshot.verificationCode = nil
            snapshot.pairedPeers = identityStore.pairedPeers()
            send(["type": "paired"])
            if let identityError = topology.identityError {
                snapshot.phase = .failed
                snapshot.error = identityError
                send(["type": "error", "reason": identityError])
            } else {
                send(topology.wireObject)
            }
            publish()
            logger.info("Cross: mutually authenticated peer paired")
        } catch { failAndClose(error.localizedDescription) }
    }

    private func topologyChanged(_ next: CrossDisplayTopology) {
        topology = next
        let hadSession = configured != nil || preparingToken != nil
        preparingToken = nil
        configured = nil; currentGeneration = nil
        snapshot.configured = false; snapshot.remoteOwnership = false
        if let identityError = next.identityError {
            input.invalidateSession()
            snapshot.phase = .failed
            snapshot.error = identityError
            if mutuallyPaired { send(["type": "error", "reason": identityError]) }
        } else {
            input.updateTopology(next)
            snapshot.error = nil
            if mutuallyPaired {
                snapshot.phase = .paired
                send(next.wireObject)
            } else if peerHello != nil {
                snapshot.phase = localPairApproved ? .paired : .pairing
            } else if running {
                snapshot.phase = .listening
            }
        }
        publish()
        if hadSession { onCrossEnded?() }
    }

    private func endConfiguredSession() {
        guard configured != nil || preparingToken != nil else { return }
        preparingToken = nil
        input.invalidateSession()
        configured = nil; currentGeneration = nil
        snapshot.configured = false; snapshot.remoteOwnership = false; snapshot.phase = .paired
        publish()
        onCrossEnded?()
    }

    private func startLeaseTimer() {
        leaseTimer?.cancel()
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + .milliseconds(NetworkConfig.crossPingMs),
                       repeating: .milliseconds(NetworkConfig.crossPingMs))
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            do { try self.decoder.checkDeadline(now: crossMonotonicMilliseconds()) }
            catch { self.failAndClose(error.localizedDescription); return }
            guard self.configured != nil else { return }
            if !InputInjector.checkAccessibilityPermission() {
                self.failAndClose("Accessibility permission was removed; Cross input stopped")
                return
            }
            guard CrossOwnershipLease.expired(now: crossMonotonicMilliseconds(),
                                              lastPing: self.lastPingMilliseconds,
                                              ownsInput: self.input.hasOwnership) else { return }
            let generation = self.currentGeneration
            self.input.reset(); self.currentGeneration = nil
            self.snapshot.remoteOwnership = false; self.snapshot.phase = .ready
            self.snapshot.error = "Cross returned input to Windows after its safety lease expired"
            self.publish()
            if let generation, let configured = self.configured {
                self.send(["type": "local", "session_id": configured.sessionID.description,
                           "topology_revision": NSNumber(value: configured.topologyRevision),
                           "generation": NSNumber(value: generation)])
            }
        }
        leaseTimer = timer
        timer.resume()
    }

    private func startHandshakeTimer(token: UUID) {
        handshakeTimer?.cancel()
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + CrossProtocol.writeTimeoutSeconds)
        timer.setEventHandler { [weak self] in
            guard let self, self.connectionToken == token else { return }
            self.failAndClose("Secure Cross handshake timed out")
        }
        handshakeTimer = timer; timer.resume()
    }

    private func startPairingTimer() {
        pairingTimer?.cancel()
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + CrossProtocol.pairingTimeoutSeconds)
        timer.setEventHandler { [weak self] in self?.failAndClose("Pairing approval timed out") }
        pairingTimer = timer; timer.resume()
    }

    private func send(_ object: [String: Any], closeAfter: Bool = false) {
        guard connection != nil else { return }
        do {
            let data = try FramedJSON.encode(object)
            guard outgoing.count < NetworkConfig.crossQueueLimit else {
                closeConnection(notifyEnd: true)
                return
            }
            outgoing.append(data)
            if closeAfter { closeAfterWrites = true }
            sendNext()
        } catch { closeConnection(notifyEnd: true) }
    }

    private func sendNext() {
        guard !writeInProgress, !outgoing.isEmpty, let connection else {
            if closeAfterWrites && outgoing.isEmpty { closeConnection(notifyEnd: true) }
            return
        }
        writeInProgress = true
        let data = outgoing.removeFirst()
        let token = connectionToken
        startWriteTimer(token: token)
        connection.send(content: data, completion: .contentProcessed { [weak self] error in
            self?.queue.async {
                guard let self, self.connectionToken == token else { return }
                self.writeTimer?.cancel(); self.writeTimer = nil
                self.writeInProgress = false
                if error != nil { self.closeConnection(notifyEnd: true) }
                else { self.sendNext() }
            }
        })
    }

    private func failAndClose(_ reason: String) {
        let hadSession = configured != nil || preparingToken != nil
        preparingToken = nil
        input.invalidateSession()
        configured = nil
        currentGeneration = nil
        snapshot.configured = false
        snapshot.remoteOwnership = false
        snapshot.error = reason
        publish()
        logger.warn("Cross: \(reason)")
        if hadSession { onCrossEnded?() }
        if connection != nil { send(["type": "error", "reason": reason], closeAfter: true) }
        else { closeConnection(notifyEnd: true) }
    }

    private func closeConnection(notifyEnd: Bool) {
        let wasConfigured = configured != nil || preparingToken != nil
        connection?.stateUpdateHandler = nil
        connection?.cancel()
        connection = nil
        connectionToken = UUID()
        handshakeTimer?.cancel(); handshakeTimer = nil
        pairingTimer?.cancel(); pairingTimer = nil
        writeTimer?.cancel(); writeTimer = nil
        outgoing.removeAll(); writeInProgress = false; closeAfterWrites = false
        input.invalidateSession()
        preparingToken = nil
        configured = nil; currentGeneration = nil
        resetPeerState()
        snapshot = CrossServiceSnapshot(phase: running ? .listening : .stopped,
                                        error: snapshot.error,
                                        pairedPeers: identityStore.pairedPeers())
        publish()
        if wasConfigured && notifyEnd { onCrossEnded?() }
    }

    private func resetPeerState() {
        peerFingerprint = nil; peerHello = nil
        localPairApproved = false; remotePairApproved = false; mutuallyPaired = false
        usedSessionIDs.removeAll()
        configured = nil; currentGeneration = nil
    }

    private func activeInterfaceName() -> String? {
        guard let path = connection?.currentPath else { return nil }
        let interface = path.availableInterfaces.first { path.usesInterfaceType($0.type) }
        guard let interface else { return nil }
        let kind: String
        switch interface.type {
        case .wiredEthernet: kind = "Ethernet / Thunderbolt network"
        case .wifi: kind = "Wi-Fi / LAN"
        case .loopback: kind = "Loopback"
        case .cellular: kind = "Cellular"
        default: kind = "Local network"
        }
        return "\(kind) · \(interface.name)"
    }

    private func publish() {
        let value = snapshot
        DispatchQueue.main.async { [weak self] in self?.onSnapshot?(value) }
    }

    private static func txtRecord(fingerprint: String) -> Data {
        var result = Data()
        for item in ["version=4", "role=mac", "id=\(fingerprint)"] {
            let bytes = Data(item.utf8)
            result.append(UInt8(bytes.count)); result.append(bytes)
        }
        return result
    }

    private func startWriteTimer(token: UUID) {
        writeTimer?.cancel()
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + CrossProtocol.writeTimeoutSeconds)
        timer.setEventHandler { [weak self] in
            guard let self, self.connectionToken == token, self.writeInProgress else { return }
            self.closeConnection(notifyEnd: true)
        }
        writeTimer = timer
        timer.resume()
    }
}

final class CrossFrameDecoder {
    private var buffer = Data()
    private var partialSince: UInt64?
    private let clock: () -> UInt64

    init(clock: @escaping () -> UInt64 = crossMonotonicMilliseconds) { self.clock = clock }

    func append(_ data: Data) throws -> [[String: Any]] {
        try checkDeadline(now: clock())
        if buffer.isEmpty && !data.isEmpty { partialSince = clock() }
        buffer.append(data)
        var result: [[String: Any]] = []
        while buffer.count >= 4 {
            let length = buffer.prefix(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
            guard length > 0, length <= FramedJSON.maximumBodyBytes else {
                throw CrossProtocolError.malformed("frame length")
            }
            let total = 4 + Int(length)
            guard buffer.count >= total else { break }
            let body = buffer.subdata(in: 4..<total)
            buffer.removeSubrange(0..<total)
            guard let object = try JSONSerialization.jsonObject(with: body) as? [String: Any] else {
                throw CrossProtocolError.malformed("JSON object")
            }
            result.append(object)
        }
        if buffer.isEmpty { partialSince = nil }
        return result
    }

    func checkDeadline(now: UInt64) throws {
        if let partialSince, now - partialSince >= UInt64(CrossProtocol.writeTimeoutSeconds * 1_000) {
            throw CrossProtocolError.malformed("partial frame timed out")
        }
    }
}

private func crossMonotonicMilliseconds() -> UInt64 {
    DispatchTime.now().uptimeNanoseconds / 1_000_000
}
