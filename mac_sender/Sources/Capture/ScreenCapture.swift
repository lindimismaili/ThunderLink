import ScreenCaptureKit
import CoreMedia
import CoreVideo

/// Delegate protocol for receiving captured frames.
protocol ScreenCaptureDelegate: AnyObject {
    func screenCapture(_ capture: ScreenCapture,
                       didCaptureFrame pixelBuffer: CVPixelBuffer,
                       width: Int, height: Int, bytesPerRow: Int)
}

/// Captures the Mac screen using ScreenCaptureKit with hardware acceleration.
/// Configures the stream for BGRA32 output at the requested FPS.
final class ScreenCapture: NSObject, SCStreamDelegate, SCStreamOutput {
    weak var delegate: ScreenCaptureDelegate?

    private var stream: SCStream?
    private let frameQueue = DispatchQueue(label: "com.thunderlink.capture",
                                           qos: .userInteractive)
    private(set) var captureWidth: Int = 0
    private(set) var captureHeight: Int = 0
    private(set) var captureFPS: Int = 0

    /// Start capturing the given display.
    func startCapture(display: SCDisplay, width: Int, height: Int, fps: Int) async throws {
        captureWidth = width
        captureHeight = height
        captureFPS = fps

        let config = SCStreamConfiguration()
        config.width = width
        config.height = height
        config.pixelFormat = kCVPixelFormatType_32BGRA
        config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        config.queueDepth = 3
        config.showsCursor = true

        let filter = SCContentFilter(display: display,
                                     excludingWindows: [])

        stream = SCStream(filter: filter, configuration: config, delegate: self)

        try stream?.addStreamOutput(self, type: .screen, sampleHandlerQueue: frameQueue)
        try await stream?.startCapture()

        logger.info("ScreenCapture: started \(width)x\(height) @ \(fps) fps")
    }

    /// Stop the current capture session.
    func stopCapture() async {
        do {
            try await stream?.stopCapture()
        } catch {
            logger.warn("ScreenCapture: stopCapture error: \(error)")
        }
        stream = nil
        logger.info("ScreenCapture: stopped")
    }

    // MARK: - SCStreamOutput

    func stream(_ stream: SCStream,
                didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
                of type: SCStreamOutputType) {
        guard type == .screen else { return }
        guard let pixelBuffer = sampleBuffer.imageBuffer else { return }

        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)

        delegate?.screenCapture(self, didCaptureFrame: pixelBuffer,
                                width: width, height: height, bytesPerRow: bytesPerRow)
    }

    // MARK: - SCStreamDelegate

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        logger.error("ScreenCapture: stream stopped with error: \(error)")
    }
}
