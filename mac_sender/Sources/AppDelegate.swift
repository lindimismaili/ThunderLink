import AppKit
import ScreenCaptureKit

/// Top-level coordinator for the ThunderLink Mac sender.
///
/// Lifecycle:
///   * `applicationDidFinishLaunching` — checks permissions, brings up the input
///     receiver, frame sender, and control client. Capture is **not** started here.
///   * Once the control client completes the HELLO/CONFIGURE/START handshake with
///     the Windows receiver, it calls back into `controlClient(_:didStartStreamingAt:...)`
///     which starts `ScreenCapture` with `FrameSender` wired as its delegate.
///   * On STOP/BYE or connection loss, `ScreenCapture` is stopped and the control
///     client retries the connection.
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let displayManager = DisplayManager()
    private let frameSender    = FrameSender()
    private let inputReceiver  = InputReceiver()
    private let inputInjector  = InputInjector()
    private let screenCapture  = ScreenCapture()
    private let controlClient  = ControlClient()
    private var captureBridge: CaptureBridge?

    func applicationDidFinishLaunching(_ notification: Notification) {
        logger.info("ThunderLinkSender starting")
        _ = InputInjector.checkAccessibilityPermission()

        // Bridge ScreenCapture frames -> FrameSender (locks the CVPixelBuffer).
        captureBridge = CaptureBridge(sender: frameSender)
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
        frameSender.stop()
        inputReceiver.stop()
    }
}

// MARK: - ControlClientDelegate

extension AppDelegate: ControlClientDelegate {
    func controlClient(_ client: ControlClient,
                       didStartStreamingAt width: Int,
                       height: Int,
                       fps: Int) {
        guard let display = displayManager.mainDisplay else {
            logger.error("AppDelegate: no displays available; cannot start capture")
            return
        }

        // The receiver normalizes mouse coordinates against the streamed
        // resolution, so use that here. (Multi-display: this assumes the
        // mainDisplay is the one being captured; revisit if CONFIGURE ever
        // carries a display ID.)
        inputInjector.setScreenSize(width: width, height: height)

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
        Task { @MainActor in
            await screenCapture.stopCapture()
        }
    }
}

// MARK: - ScreenCapture -> FrameSender bridge

/// Locks each captured CVPixelBuffer and forwards the BGRA bytes to FrameSender.
/// Kept separate so FrameSender doesn't depend on ScreenCaptureKit types.
final class CaptureBridge: ScreenCaptureDelegate {
    private let sender: FrameSender

    init(sender: FrameSender) {
        self.sender = sender
    }

    func screenCapture(_ capture: ScreenCapture,
                       didCaptureFrame pixelBuffer: CVPixelBuffer,
                       width: Int, height: Int, bytesPerRow: Int) {
        _ = PixelBufferUtils.withLockedBaseAddress(pixelBuffer) { base, w, h, bpr in
            sender.sendFrame(pixelData: base,
                             size: h * bpr,
                             width: w,
                             height: h,
                             bytesPerRow: bpr)
        }
    }
}
