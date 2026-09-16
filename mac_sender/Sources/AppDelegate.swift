import AppKit
import CoreGraphics
import CoreMedia
import CoreVideo
import Combine

final class AppDelegate: NSObject, NSApplicationDelegate {
    private let displayManager = DisplayManager()
    private let frameSender = FrameSender()
    private let inputReceiver = InputReceiver()
    private let inputInjector = InputInjector()
    private let screenCapture = ScreenCapture()
    private let controlClient = ControlClient()
    private let captureBridge = CaptureBridge()
    private let encoderPipeline = EncoderPipeline()
    private let statusController = StatusController()
    private let crossServer = CrossServer()
    private let sessionPower = SessionPowerController()
    private let lidSleep = LidSleepController()
    private var powerStateSubscription: AnyCancellable?
    @MainActor private lazy var lifecycle = StreamLifecycleCoordinator(
        displayManager: displayManager, screenCapture: screenCapture,
        captureBridge: captureBridge, encoderPipeline: encoderPipeline,
        frameSender: frameSender, inputReceiver: inputReceiver,
        inputInjector: inputInjector, controlClient: controlClient,
        statusController: statusController)
    private var restartRequested = false
    private var connectionPaused = false
    private var screenShareSuppressedByCross = false
    private var crossPreparationToken: UUID?
    private var terminationInProgress = false
    @MainActor private var networkRetryTimer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        logger.info("ThunderLinkSender starting")


        NSApp.setActivationPolicy(.regular)
        statusController.onQuit = { NSApp.terminate(nil) }
        statusController.onToggleConnection = { [weak self] in self?.toggleConnection() }
        statusController.onRestart = { [weak self] in
            self?.restartRequested = true
            NSApp.terminate(nil)
        }
        statusController.onRefreshPermissions = { [weak self] in self?.refreshPermissions() }
        statusController.onKeepAwakeChanged = { [weak self] enabled in self?.setKeepAwake(enabled) }
        statusController.onAuthorizeLidSleep = { [weak self] in self?.lidSleep.authorize() }
        lidSleep.onStatus = { [weak self] message, busy in
            self?.statusController.state.lidSleepStatus = message
            self?.statusController.state.lidSleepAuthorizedOrPending = busy
        }
        sessionPower.onRequested = { [weak self] in self?.lidSleep.setRequested($0) }
        statusController.state.keepAwakeOnPower = NetworkConfig.keepAwakeOnPower
        sessionPower.setEnabled(NetworkConfig.keepAwakeOnPower)
        sessionPower.onStatus = { [weak self] held in self?.statusController.state.keepingAwake = held }
        powerStateSubscription = Publishers.CombineLatest3(statusController.state.$streaming,
            statusController.state.$crossConfigured, statusController.state.$paused)
            .sink { [weak self] streaming, cross, paused in
                self?.sessionPower.setSessionActive(SessionPowerController.hasActiveSession(
                    streaming: streaming, crossConfigured: cross, paused: paused))
            }
        sessionPower.start()
        statusController.onApprovePairing = { [weak self] in self?.crossServer.approvePairing() }
        statusController.onRejectPairing = { [weak self] in self?.crossServer.rejectPairing() }
        statusController.onForgetPeer = { [weak self] fingerprint in self?.crossServer.forgetPeer(fingerprint: fingerprint) }
        statusController.state.configurationError = DesktopSettings.startupError
        statusController.install()

        statusController.state.peerIP = NetworkConfig.winIP
        refreshPermissions()

        screenCapture.delegate = captureBridge
        crossServer.onSnapshot = { [weak self] snapshot in self?.applyCrossSnapshot(snapshot) }
        crossServer.onPrepareCross = { [weak self] completion in
            DispatchQueue.main.async {
                guard let self else {
                    completion(.failure(CrossLifecycleError.unavailable))
                    return
                }
                Task { @MainActor in await self.prepareForCross(completion: completion) }
            }
        }
        crossServer.onCrossEnded = { [weak self] in
            DispatchQueue.main.async { self?.resumeScreenShareAfterCross() }
        }
        initializeCrossSystemInhibitions()
        crossServer.start()
        let workspace = NSWorkspace.shared.notificationCenter
        workspace.addObserver(self, selector: #selector(updateCrossSystemState(_:)),
                              name: NSWorkspace.willSleepNotification, object: nil)
        workspace.addObserver(self, selector: #selector(updateCrossSystemState(_:)),
                              name: NSWorkspace.sessionDidResignActiveNotification, object: nil)
        workspace.addObserver(self, selector: #selector(updateCrossSystemState(_:)),
                              name: NSWorkspace.screensDidSleepNotification, object: nil)
        workspace.addObserver(self, selector: #selector(updateCrossSystemState(_:)),
                              name: NSWorkspace.didWakeNotification, object: nil)
        workspace.addObserver(self, selector: #selector(updateCrossSystemState(_:)),
                              name: NSWorkspace.sessionDidBecomeActiveNotification, object: nil)
        workspace.addObserver(self, selector: #selector(updateCrossSystemState(_:)),
                              name: NSWorkspace.screensDidWakeNotification, object: nil)
        DistributedNotificationCenter.default().addObserver(
            self, selector: #selector(updateCrossSystemState(_:)),
            name: Notification.Name("com.apple.screenIsLocked"), object: nil)
        DistributedNotificationCenter.default().addObserver(
            self, selector: #selector(updateCrossSystemState(_:)),
            name: Notification.Name("com.apple.screenIsUnlocked"), object: nil)
        if DesktopSettings.startupError == nil { startNetworkServices() }
    }

    @MainActor private func startNetworkServices() {
        guard !terminationInProgress, !connectionPaused, !screenShareSuppressedByCross else { return }
        let injector = inputInjector
        if !inputReceiver.start(callback: { injector.processEvent($0) },
                                onDisconnect: { injector.releaseAll() }) {
            statusController.state.phase = .failed
            statusController.state.failureReason = "The input listener could not start"
            logger.error("AppDelegate: InputReceiver failed to start")
            retryNetworkStartup()
            return
        }
        if !frameSender.start() {
            statusController.state.phase = .failed
            statusController.state.failureReason = "The video socket could not start"
            logger.error("AppDelegate: FrameSender failed to start")
            inputReceiver.stop()
            retryNetworkStartup()
            return
        }
        statusController.state.phase = .waiting
        statusController.state.failureReason = nil
        controlClient.start(delegate: self)
    }

    @MainActor private func retryNetworkStartup() {
        networkRetryTimer?.invalidate()
        networkRetryTimer = Timer.scheduledTimer(withTimeInterval: Double(NetworkConfig.reconnectIntervalMs) / 1000,
                                                 repeats: false) { [weak self] _ in
            Task { @MainActor [weak self] in self?.startNetworkServices() }
        }
        statusController.refreshIcon()
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard !terminationInProgress else { return .terminateLater }
        terminationInProgress = true
        powerStateSubscription?.cancel()
        sessionPower.stop()
        lidSleep.stop()
        networkRetryTimer?.invalidate()
        statusController.state.phase = .stopping

        Task { @MainActor [weak self] in
            guard let self else {
                sender.reply(toApplicationShouldTerminate: true)
                return
            }
            await self.lifecycle.shutdown()
            await withCheckedContinuation { continuation in
                self.crossServer.stop { continuation.resume() }
            }
            await withCheckedContinuation { continuation in
                self.controlClient.stop(sendBye: true) { continuation.resume() }
            }
            await withCheckedContinuation { continuation in
                self.inputReceiver.stop { continuation.resume() }
            }
            self.inputInjector.releaseAll()
            self.inputInjector.drain()
            self.frameSender.stop()
            DisplayController.restore()
            if self.restartRequested {
                DesktopSettings.releaseInstance()
                let process = Process()
                process.executableURL = Bundle.main.executableURL
                do { try process.run() }
                catch { logger.error("Restart failed: \(error.localizedDescription)") }
            }
            sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }

    func applicationWillTerminate(_ notification: Notification) {
        powerStateSubscription?.cancel()
        sessionPower.stop()
        lidSleep.stop()
        logger.info("ThunderLinkSender terminating")
        controlClient.stop(sendBye: false)
        crossServer.stop()
        inputReceiver.stop()
        frameSender.stop()
        inputInjector.releaseAll()
        DisplayController.restore()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        statusController.show(); return true
    }
    func applicationDidBecomeActive(_ notification: Notification) {
        refreshPermissions()
        crossServer.refreshPermission()
    }

    private func initializeCrossSystemInhibitions() {
        if let session = CGSessionCopyCurrentDictionary() as? [String: Any] {
            let locked = session["CGSSessionScreenIsLocked"] as? Bool ?? false
            let onConsole = session[kCGSessionOnConsoleKey as String] as? Bool ?? true
            let loginDone = session[kCGSessionLoginDoneKey as String] as? Bool ?? true
            crossServer.updateSystemInhibition(.screenLocked, active: locked)
            crossServer.updateSystemInhibition(.sessionInactive, active: !onConsole || !loginDone)
        }
        crossServer.updateSystemInhibition(.screensSleeping,
                                           active: CGDisplayIsAsleep(CGMainDisplayID()) != 0)
    }

    @objc private func updateCrossSystemState(_ notification: Notification) {
        switch notification.name {
        case NSWorkspace.willSleepNotification:
            crossServer.updateSystemInhibition(.sleeping, active: true)
        case NSWorkspace.didWakeNotification:
            crossServer.updateSystemInhibition(.sleeping, active: false)
        case NSWorkspace.screensDidSleepNotification:
            crossServer.updateSystemInhibition(.screensSleeping, active: true)
        case NSWorkspace.screensDidWakeNotification:
            crossServer.updateSystemInhibition(.screensSleeping, active: false)
        case NSWorkspace.sessionDidResignActiveNotification:
            crossServer.updateSystemInhibition(.sessionInactive, active: true)
        case NSWorkspace.sessionDidBecomeActiveNotification:
            crossServer.updateSystemInhibition(.sessionInactive, active: false)
        case Notification.Name("com.apple.screenIsLocked"):
            crossServer.updateSystemInhibition(.screenLocked, active: true)
        case Notification.Name("com.apple.screenIsUnlocked"):
            crossServer.updateSystemInhibition(.screenLocked, active: false)
        default:
            break
        }
    }

    @MainActor private func toggleConnection() {
        guard !terminationInProgress, !statusController.state.busy else { return }
        if connectionPaused {
            connectionPaused = false
            statusController.state.paused = false
            crossServer.start()
            startNetworkServices()
            return
        }
        connectionPaused = true
        statusController.state.paused = true
        statusController.state.busy = true
        networkRetryTimer?.invalidate()
        crossServer.stop()
        Task { @MainActor in
            await withCheckedContinuation { continuation in
                controlClient.stop(sendBye: true) { continuation.resume() }
            }
            lifecycle.disconnected(reason: "Disconnected")
            await lifecycle.settle()
            await withCheckedContinuation { continuation in
                inputReceiver.stop { continuation.resume() }
            }
            inputInjector.releaseAll()
            inputInjector.drain()
            frameSender.stop()
            statusController.state.failureReason = nil
            statusController.state.phase = .waiting
            statusController.state.busy = false
            statusController.refreshIcon()
        }
    }

    private func refreshPermissions() {
        statusController.state.accessibilityGranted = InputInjector.checkAccessibilityPermission()
        statusController.state.screenRecordingGranted = CGPreflightScreenCaptureAccess()
    }

    private func setKeepAwake(_ enabled: Bool) {
        do {
            let arguments = DesktopSettings.argumentsSettingKeepAwake(enabled, in: DesktopSettings.savedArguments)
            try DesktopSettings.save(arguments)
            DesktopSettings.savedArguments = arguments
            NetworkConfig.keepAwakeOnPower = enabled
            statusController.state.keepAwakeOnPower = enabled
            statusController.state.powerSettingsError = nil
            sessionPower.setEnabled(enabled)
        } catch { statusController.state.powerSettingsError = error.localizedDescription }
    }

    @MainActor private func prepareForCross(completion: @escaping (Result<Void, Error>) -> Void) async {
        guard !terminationInProgress, !connectionPaused else {
            completion(.failure(CrossLifecycleError.unavailable))
            return
        }
        if screenShareSuppressedByCross {
            completion(.failure(CrossLifecycleError.unavailable))
            return
        }
        let preparation = UUID()
        crossPreparationToken = preparation
        screenShareSuppressedByCross = true
        networkRetryTimer?.invalidate()
        statusController.state.busy = true
        await withCheckedContinuation { continuation in
            controlClient.stop(sendBye: true) { continuation.resume() }
        }
        lifecycle.disconnected(reason: "Switching to Cross")
        await lifecycle.settle()
        await withCheckedContinuation { continuation in
            inputReceiver.stop { continuation.resume() }
        }
        inputInjector.releaseAll()
        inputInjector.drain()
        frameSender.stop()
        guard crossPreparationToken == preparation else {
            screenShareSuppressedByCross = false
            statusController.state.mode = .screenShare
            statusController.state.busy = false
            statusController.state.connected = false
            statusController.state.phase = .waiting
            statusController.refreshIcon()
            if !connectionPaused, !terminationInProgress, DesktopSettings.startupError == nil {
                startNetworkServices()
            }
            completion(.failure(CrossLifecycleError.unavailable))
            return
        }
        crossPreparationToken = nil
        statusController.state.streaming = false
        statusController.state.connected = true
        statusController.state.mode = .cross
        statusController.state.failureReason = nil
        statusController.state.busy = false
        statusController.refreshIcon()
        completion(.success(()))
    }

    @MainActor private func resumeScreenShareAfterCross() {
        guard screenShareSuppressedByCross else { return }
        if crossPreparationToken != nil {
            // The retained preparation task owns teardown. Mark it cancelled;
            // it will restart Screen Share only after every old worker drains.
            crossPreparationToken = nil
            return
        }
        screenShareSuppressedByCross = false
        statusController.state.mode = .screenShare
        statusController.state.connected = false
        statusController.state.streaming = false
        statusController.state.phase = .waiting
        statusController.refreshIcon()
        guard !connectionPaused, !terminationInProgress, DesktopSettings.startupError == nil else { return }
        startNetworkServices()
    }

    @MainActor private func applyCrossSnapshot(_ snapshot: CrossServiceSnapshot) {
        let state = statusController.state
        state.crossPhase = snapshot.phase
        state.crossPeerName = snapshot.peerName
        state.crossInterface = snapshot.interfaceName
        state.crossVerificationCode = snapshot.verificationCode
        state.crossError = snapshot.error
        state.crossConfigured = snapshot.configured
        state.crossRemoteOwnership = snapshot.remoteOwnership
        state.pairedPeers = snapshot.pairedPeers
        if snapshot.configured { state.mode = .cross }
        statusController.refreshIcon()
    }
}

private enum CrossLifecycleError: Error, LocalizedError {
    case unavailable
    var errorDescription: String? { "ThunderLink is disconnected or shutting down" }
}

extension AppDelegate: ControlClientDelegate {
    func controlClient(_ client: ControlClient, didConnect token: UInt64) {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            guard !self.connectionPaused, !self.terminationInProgress, !self.screenShareSuppressedByCross else { return }
            self.statusController.state.connected = true
            if self.statusController.state.phase != .failed {
                self.statusController.state.phase = .configuring
            }
            self.statusController.refreshIcon()
        }
    }

    func controlClient(_ client: ControlClient, didReceive configuration: StreamConfiguration) {
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.connectionPaused, !self.terminationInProgress, !self.screenShareSuppressedByCross else { return }
            self.refreshPermissions()
            self.lifecycle.configure(configuration)
        }
    }

    func controlClient(_ client: ControlClient, didRequestKeyframeFor sessionID: StreamSessionID) {
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.screenShareSuppressedByCross else { return }
            self.lifecycle.requestKeyframe(sessionID: sessionID)
        }
    }

    func controlClient(_ client: ControlClient, didStop sessionID: StreamSessionID?, reason: String?) {
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.screenShareSuppressedByCross else { return }
            self.lifecycle.disconnected(reason: reason ?? "receiver stopped")
        }
    }

    func controlClient(_ client: ControlClient, didFail reason: String) {
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.screenShareSuppressedByCross else { return }
            self.lifecycle.controlFailure(reason)
        }
    }

    func controlClient(_ client: ControlClient, didDisconnect token: UInt64, reason: String) {
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.screenShareSuppressedByCross else { return }
            self.lifecycle.disconnected(reason: reason)
        }
    }
}

/// Thread-safe bridge from ScreenCaptureKit's serial output queue to the
/// current generation's encoder pipeline.
final class CaptureBridge: ScreenCaptureDelegate {
    private let lock = NSLock()
    private var frameHandler: ((CVPixelBuffer, CMTime) -> Void)?
    private var errorHandler: ((Error) -> Void)?

    func setHandlers(onFrame: ((CVPixelBuffer, CMTime) -> Void)?,
                     onError: ((Error) -> Void)?) {
        lock.lock()
        frameHandler = onFrame
        errorHandler = onError
        lock.unlock()
    }

    func screenCapture(_ capture: ScreenCapture, didCaptureFrame pixelBuffer: CVPixelBuffer,
                       pts: CMTime) {
        lock.lock()
        let handler = frameHandler
        lock.unlock()
        handler?(pixelBuffer, pts)
    }

    func screenCapture(_ capture: ScreenCapture, didStopWithError error: Error) {
        lock.lock()
        let handler = errorHandler
        lock.unlock()
        handler?(error)
    }
}
