import CoreGraphics
import CoreMedia
import CoreVideo
import ScreenCaptureKit

@MainActor
final class StreamLifecycleCoordinator {
    private let displayManager: DisplayManager
    private let screenCapture: ScreenCapture
    private let captureBridge: CaptureBridge
    private let encoderPipeline: EncoderPipeline
    private let frameSender: FrameSender
    private let inputReceiver: InputReceiver
    private let inputInjector: InputInjector
    private let controlClient: ControlClient
    private let statusController: StatusController

    private lazy var lifecycle = StreamLifecycle(services: .init(
        prepare: { [unowned self] in try await self.prepare($0) },
        startCapture: { [unowned self] actual in
            try await self.screenCapture.startCapture(display: self.selectedDisplay(),
                width: actual.width, height: actual.height, fps: actual.fps)
        },
        sendStart: { [unowned self] actual in
            await withCheckedContinuation { continuation in
                self.controlClient.sendStart(actual) { continuation.resume(returning: $0) }
            }
        },
        closeDelivery: { [unowned self] in self.closeSessionImmediately() },
        cleanup: { [unowned self] in await self.teardown(restoreDisplay: true) },
        activate: { [unowned self] in self.activate($0) },
        publish: { [unowned self] in self.publish($0, configuration: $1, reason: $2) },
        sendStop: { [unowned self] in self.controlClient.sendStop(sessionID: $0.sessionID, reason: $1) }
    ))

    init(displayManager: DisplayManager, screenCapture: ScreenCapture,
         captureBridge: CaptureBridge, encoderPipeline: EncoderPipeline,
         frameSender: FrameSender, inputReceiver: InputReceiver,
         inputInjector: InputInjector, controlClient: ControlClient,
         statusController: StatusController) {
        self.displayManager = displayManager
        self.screenCapture = screenCapture
        self.captureBridge = captureBridge
        self.encoderPipeline = encoderPipeline
        self.frameSender = frameSender
        self.inputReceiver = inputReceiver
        self.inputInjector = inputInjector
        self.controlClient = controlClient
        self.statusController = statusController
    }

    func configure(_ requested: StreamConfiguration) { lifecycle.configure(requested) }
    func disconnected(reason: String) {
        statusController.state.connected = false
        lifecycle.disconnect()
    }
    func requestKeyframe(sessionID: StreamSessionID) {
        encoderPipeline.requestKeyframe(sessionID: sessionID)
    }
    func controlFailure(_ reason: String) {
        statusController.state.failureReason = reason
        statusController.state.phase = .failed
        statusController.state.streaming = false
    }
    func settle() async { await lifecycle.settle(); inputInjector.drain() }
    func shutdown() async { await lifecycle.shutdown(); inputInjector.drain() }

    private func prepare(_ requested: StreamConfiguration) async throws -> StreamConfiguration {
        guard CGPreflightScreenCaptureAccess() else { throw LifecycleError.screenRecordingPermission }
        try await displayManager.refreshDisplays()
        let actual = try resolve(requested)
        let mainSize = DisplayController.mainPointSize()
        inputInjector.setScreenSize(width: mainSize.width, height: mainSize.height)
        let sender = frameSender
        let started = await encoderPipeline.start(configuration: actual,
            output: { frame, sessionID in
                sender.sendEncodedFrame(frame.data, isKeyframe: frame.isKeyframe,
                    format: NetworkConfig.wireFormat(for: actual.codec),
                    width: frame.width, height: frame.height, sessionID: sessionID)
            },
            failure: { [weak self] sessionID, reason in
                Task { @MainActor [weak self] in
                    self?.lifecycle.runtimeFailure(sessionID: sessionID, reason: reason)
                }
            })
        guard started else { throw LifecycleError.encoderUnavailable(actual.codec) }
        captureBridge.setHandlers(
            onFrame: { [weak encoderPipeline] pixelBuffer, pts in
                encoderPipeline?.encode(pixelBuffer, pts: pts, sessionID: actual.sessionID)
            },
            onError: { [weak self] error in
                Task { @MainActor [weak self] in
                    self?.lifecycle.runtimeFailure(sessionID: actual.sessionID,
                        reason: "Screen capture stopped: \(error.localizedDescription)")
                }
            })
        return actual
    }

    private func activate(_ configuration: StreamConfiguration) {
        inputInjector.setActiveSession(configuration.sessionID)
        inputReceiver.setActiveSession(configuration.sessionID)
        frameSender.activate(sessionID: configuration.sessionID)
        encoderPipeline.requestKeyframe(sessionID: configuration.sessionID)
    }

    private func publish(_ phase: AppState.Phase, configuration: StreamConfiguration?, reason: String?) {
        let state = statusController.state
        state.streaming = phase == .streaming
        // A peer teardown after STOP must leave the actionable failure visible.
        if phase != .waiting || state.phase != .failed { state.phase = phase }
        if phase != .waiting { state.failureReason = reason }
        if phase == .configuring || phase == .streaming { state.connected = true }
        state.screenRecordingGranted = CGPreflightScreenCaptureAccess()
        if let configuration, phase == .streaming {
            state.width = configuration.width
            state.height = configuration.height
            state.fps = configuration.fps
            state.codec = configuration.codec.description
            state.bitrateKbps = configuration.bitrateKbps
        }
        statusController.refreshIcon()
    }

    private func closeSessionImmediately() {
        frameSender.deactivate()
        inputReceiver.setActiveSession(nil)
        inputInjector.setActiveSession(nil)
        inputInjector.releaseAll()
    }

    private func teardown(restoreDisplay: Bool) async {
        captureBridge.setHandlers(onFrame: nil, onError: nil)
        await screenCapture.stopCapture()
        await encoderPipeline.stop()
        frameSender.deactivate()
        inputReceiver.setActiveSession(nil)
        inputInjector.setActiveSession(nil)
        inputInjector.releaseAll()
        if restoreDisplay { DisplayController.restore() }
    }

    private func resolve(_ requested: StreamConfiguration) throws -> StreamConfiguration {
        let displayIndex = NetworkConfig.captureDisplayIndex
        let width: Int
        let height: Int

        if displayIndex == 0 {
            if requested.width == 0 && requested.height > 0 {
                DisplayController.restore()
                let native = DisplayController.mainPointSize()
                height = requested.height
                width = Int((Double(height) * Double(native.width) / Double(native.height)).rounded()) & ~1
            } else if requested.width > 0 {
                _ = try DisplayController.apply(requestedWidth: requested.width,
                                                requestedHeight: requested.height)
                width = requested.width
                height = requested.height
            } else {
                let native = try DisplayController.apply(requestedWidth: 0, requestedHeight: 0)
                width = native.width
                height = native.height
            }
        } else {
            DisplayController.restore()
            guard let display = displayManager.display(at: displayIndex) else {
                throw LifecycleError.displayUnavailable(displayIndex)
            }
            width = Int(display.width) & ~1
            height = Int(display.height) & ~1
        }
        return try requested.resolved(width: width, height: height)
    }

    private func selectedDisplay() throws -> SCDisplay {
        if NetworkConfig.captureDisplayIndex == 0 {
            guard let display = displayManager.display(byID: CGMainDisplayID()) ?? displayManager.mainDisplay else {
                throw LifecycleError.displayUnavailable(0)
            }
            return display
        }
        guard let display = displayManager.display(at: NetworkConfig.captureDisplayIndex) else {
            throw LifecycleError.displayUnavailable(NetworkConfig.captureDisplayIndex)
        }
        return display
    }
}

enum LifecycleError: Error, LocalizedError {
    case screenRecordingPermission
    case displayUnavailable(Int)
    case encoderUnavailable(TlnkCodecSwift)

    var errorDescription: String? {
        switch self {
        case .screenRecordingPermission:
            return "Screen Recording permission is required"
        case .displayUnavailable(let index):
            return "Capture display #\(index) is unavailable"
        case .encoderUnavailable(let codec):
            return "The Mac could not create a \(codec) hardware encoder"
        }
    }
}
