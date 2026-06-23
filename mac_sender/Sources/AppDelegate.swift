import AppKit
import ScreenCaptureKit
import CoreMedia
import CoreVideo

/// Top-level coordinator for the ThunderLink Mac sender.
///
/// Lifecycle:
///   * `applicationDidFinishLaunching` — checks permissions, brings up the input
///     receiver, frame sender, and control client. Capture is **not** started here.
///   * Once the control client completes the HELLO/CONFIGURE/START handshake with
///     the Windows receiver, `controlClient(_:didStartStreamingAt:...)` builds a
///     `VideoEncoder` for the negotiated codec/bitrate and starts `ScreenCapture`,
///     wiring capture → encode → `FrameSender`.
///   * On STOP/BYE or connection loss, capture + encoder are torn down and the
///     control client retries the connection.
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let displayManager = DisplayManager()
    private let frameSender    = FrameSender()
    private let inputReceiver  = InputReceiver()
    private let inputInjector  = InputInjector()
    private let screenCapture  = ScreenCapture()
    private let controlClient  = ControlClient()
    private let captureBridge  = CaptureBridge()
    private let statusController = StatusController()
    private var videoEncoder: VideoEncoder?

    func applicationDidFinishLaunching(_ notification: Notification) {
        logger.info("ThunderLinkSender starting")

        // Menu-bar-only app (no dock icon) with a SwiftUI status panel.
        NSApp.setActivationPolicy(.accessory)
        statusController.onQuit = { NSApp.terminate(nil) }
        statusController.install()
        statusController.state.peerIP = NetworkConfig.winIP
        statusController.state.accessibilityGranted = InputInjector.checkAccessibilityPermission()

        screenCapture.delegate = captureBridge

        // Start the input receiver (TCP 50002, Mac is server).
        let injector = inputInjector
        if !inputReceiver.start(callback: { event in
            injector.processEvent(event)
        }) {
            logger.error("AppDelegate: InputReceiver failed to start")
        }

        // Start the UDP frame sender.
        if !frameSender.start() {
            logger.error("AppDelegate: FrameSender failed to start")
        }

        // Enumerate displays so we can pick one when streaming starts.
        Task {
            do {
                try await displayManager.refreshDisplays()
            } catch {
                logger.error("AppDelegate: refreshDisplays failed: \(error)")
            }

            // Kick off the control-channel client (will retry until Windows is up).
            controlClient.start(delegate: self)
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        logger.info("ThunderLinkSender terminating")
        controlClient.stop()
        Task { await screenCapture.stopCapture() }
        videoEncoder?.stop()
        videoEncoder = nil
        frameSender.stop()
        inputReceiver.stop()
    }
}

// MARK: - ControlClientDelegate

extension AppDelegate: ControlClientDelegate {
    func controlClient(_ client: ControlClient,
                       didStartStreamingAt width: Int,
                       height: Int,
                       fps: Int,
                       codec: TlnkCodecSwift,
                       bitrateKbps: Int) {
        guard let display = displayManager.mainDisplay else {
            logger.error("AppDelegate: no displays available; cannot start capture")
            return
        }

        // The receiver normalizes mouse coordinates against the streamed
        // resolution, so use that here. (Multi-display: this assumes the
        // mainDisplay is the one being captured; revisit if CONFIGURE ever
        // carries a display ID.)
        inputInjector.setScreenSize(width: width, height: height)

        // Build a fresh encoder for the negotiated parameters.
        videoEncoder?.stop()
        let encoder = VideoEncoder(width: width, height: height, fps: fps,
                                   bitrateKbps: bitrateKbps, codec: codec)
        let wireFormat = NetworkConfig.wireFormat(for: codec)
        encoder?.onEncodedFrame = { [weak self] frame in
            self?.frameSender.sendEncodedFrame(frame.data,
                                               isKeyframe: frame.isKeyframe,
                                               format: wireFormat,
                                               width: frame.width,
                                               height: frame.height)
        }
        videoEncoder = encoder

        // Route captured NV12 buffers into the encoder.
        captureBridge.onFrame = { [weak self] pixelBuffer, pts in
            self?.videoEncoder?.encode(pixelBuffer, pts: pts)
        }

        // Update the menu-bar panel.
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            let s = self.statusController.state
            s.connected = true
            s.streaming = true
            s.width = width
            s.height = height
            s.fps = fps
            s.codec = (codec == .hevc) ? "HEVC" : "H.264"
            s.bitrateKbps = bitrateKbps
            self.statusController.refreshIcon()
        }

        Task { @MainActor in
            // If reconnecting after a drop, stop any prior capture first.
            await screenCapture.stopCapture()
            do {
                try await screenCapture.startCapture(display: display,
                                                     width: width,
                                                     height: height,
                                                     fps: fps)
            } catch {
                logger.error("AppDelegate: startCapture failed: \(error)")
            }
        }
    }

    func controlClientDidStopStreaming(_ client: ControlClient) {
        captureBridge.onFrame = nil
        Task { @MainActor in
            await screenCapture.stopCapture()
        }
        videoEncoder?.stop()
        videoEncoder = nil

        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.statusController.state.connected = false
            self.statusController.state.streaming = false
            self.statusController.refreshIcon()
        }
    }

    func controlClientDidRequestKeyframe(_ client: ControlClient) {
        videoEncoder?.requestKeyframe()
    }
}

// MARK: - ScreenCapture -> VideoEncoder bridge

/// Forwards each captured NV12 CVPixelBuffer to a sink (the encoder).
/// Kept separate so FrameSender/VideoEncoder don't depend on ScreenCaptureKit types.
final class CaptureBridge: ScreenCaptureDelegate {
    var onFrame: ((CVPixelBuffer, CMTime) -> Void)?

    func screenCapture(_ capture: ScreenCapture,
                       didCaptureFrame pixelBuffer: CVPixelBuffer,
                       pts: CMTime) {
        onFrame?(pixelBuffer, pts)
    }
}
