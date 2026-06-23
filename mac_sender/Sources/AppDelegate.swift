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

        // Make sure the direct-link adapter actually carries our bind IP before
        // we open any sockets; otherwise bind() fails with EADDRNOTAVAIL (49).
        // Prompts for admin only when the address isn't already assigned.
        LinkSetup.ensureLinkAddress()

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
        DisplayController.restore()  // don't leave the Mac on a switched resolution
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
        restartStreaming(requestedWidth: width, requestedHeight: height,
                         fps: fps, codec: codec, bitrateKbps: bitrateKbps)
    }

    func controlClientDidReconfigure(_ client: ControlClient,
                                     width: Int, height: Int, fps: Int,
                                     codec: TlnkCodecSwift, bitrateKbps: Int) {
        logger.info("AppDelegate: reconfiguring stream to \(width)x\(height) (0 = native)")
        restartStreaming(requestedWidth: width, requestedHeight: height,
                         fps: fps, codec: codec, bitrateKbps: bitrateKbps)
    }

    func controlClientDidStopStreaming(_ client: ControlClient) {
        captureBridge.onFrame = nil
        Task { @MainActor in
            await screenCapture.stopCapture()
        }
        videoEncoder?.stop()
        videoEncoder = nil
        DisplayController.restore()  // put the Mac display back if we changed it

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

    /// (Re)start capture + encode at the requested resolution.
    ///
    /// For a fixed size this switches the Mac's main display to a matching mode
    /// (so the capture is native/crisp); for width/height == 0 it restores the
    /// native mode and captures at that. Capture size and the input coordinate
    /// space are both in display POINTS, so the Windows cursor maps 1:1.
    private func restartStreaming(requestedWidth: Int, requestedHeight: Int,
                                  fps: Int, codec: TlnkCodecSwift, bitrateKbps: Int) {
        let dispIndex = NetworkConfig.captureDisplayIndex
        let captureMain = (dispIndex == 0)

        var capW: Int
        var capH: Int
        if captureMain {
            if requestedWidth == 0 && requestedHeight > 0 {
                // Aspect-correct: derive width from the Mac's native display aspect
                // for the requested height, so the stream matches the Mac's shape
                // (no distortion, no ScreenCaptureKit letterbox, accurate cursor).
                // The display itself stays native; we just scale the capture.
                DisplayController.restore()
                let (nativeW, nativeH) = DisplayController.mainPointSize()
                let aspect = Double(nativeW) / Double(max(nativeH, 1))
                capH = requestedHeight & ~1
                capW = Int((Double(capH) * aspect).rounded()) & ~1
            } else if requestedWidth > 0 && requestedHeight > 0 {
                // Explicit WxH: best-effort display switch, capture at that size.
                DisplayController.apply(requestedWidth: requestedWidth,
                                        requestedHeight: requestedHeight)
                capW = requestedWidth & ~1
                capH = requestedHeight & ~1
            } else {
                // "Source (native)": restore the display and capture native points.
                (capW, capH) = DisplayController.apply(requestedWidth: 0, requestedHeight: 0)
            }
        } else if let secondary = displayManager.display(at: dispIndex) {
            // Capturing a secondary/virtual display (e.g. BetterDisplay): leave
            // the main display alone, capture the chosen display at its native size.
            DisplayController.restore()
            capW = Int(secondary.width) & ~1
            capH = Int(secondary.height) & ~1
            logger.info("AppDelegate: capturing display #\(dispIndex) at \(capW)x\(capH)")
        } else {
            logger.error("AppDelegate: display #\(dispIndex) not found; using main")
            (capW, capH) = DisplayController.apply(requestedWidth: requestedWidth,
                                                   requestedHeight: requestedHeight)
        }
        guard capW >= 2, capH >= 2 else {
            logger.error("AppDelegate: invalid capture size \(capW)x\(capH)")
            return
        }

        // CGEvent positions live in MAIN-display points (where the real cursor
        // is). Windows sends normalized [0,1] over the video; for a mirrored
        // virtual display that still lands on the right spot of the main screen.
        let (screenW, screenH) = DisplayController.mainPointSize()
        inputInjector.setScreenSize(width: screenW, height: screenH)

        // Fresh encoder for the (possibly new) parameters.
        videoEncoder?.stop()
        let encoder = VideoEncoder(width: capW, height: capH, fps: fps,
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

        captureBridge.onFrame = { [weak self] pixelBuffer, pts in
            self?.videoEncoder?.encode(pixelBuffer, pts: pts)
        }

        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            let s = self.statusController.state
            s.connected = true
            s.streaming = true
            s.width = capW
            s.height = capH
            s.fps = fps
            s.codec = (codec == .hevc) ? "HEVC" : "H.264"
            s.bitrateKbps = bitrateKbps
            self.statusController.refreshIcon()
        }

        Task { @MainActor in
            await screenCapture.stopCapture()
            try? await displayManager.refreshDisplays()
            guard let display = displayManager.display(at: dispIndex)
                    ?? displayManager.mainDisplay else {
                logger.error("AppDelegate: no display available to capture")
                return
            }
            do {
                try await screenCapture.startCapture(display: display,
                                                     width: capW, height: capH, fps: fps)
            } catch {
                logger.error("AppDelegate: startCapture failed: \(error)")
            }
        }
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
