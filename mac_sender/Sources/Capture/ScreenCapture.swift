import ScreenCaptureKit
import CoreMedia
import CoreVideo

/// Delegate protocol for receiving captured frames.
///
/// Frames are delivered as IOSurface-backed NV12 `CVPixelBuffer`s ready to feed
/// straight into the VideoToolbox encoder (no CPU copy). The presentation
/// timestamp is forwarded so the encoder can stamp the bitstream.
protocol ScreenCaptureDelegate: AnyObject {
    func screenCapture(_ capture: ScreenCapture,
                       didCaptureFrame pixelBuffer: CVPixelBuffer,
                       pts: CMTime)
}

/// Captures the Mac screen using ScreenCaptureKit with hardware acceleration.
/// Configures the stream for NV12 (4:2:0) output at the requested FPS — the
/// native input format for the VideoToolbox H.264/HEVC encoder.
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
        // NV12 (bi-planar 4:2:0, video range) — fed directly to VTCompressionSession.
        config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        config.colorSpaceName = CGColorSpace.sRGB
        config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        // Lower queue depth than the old uncompressed path to minimize latency.
        config.queueDepth = 2
        config.showsCursor = true

        let filter = SCContentFilter(display: display,
                                     excludingWindows: [])

        stream = SCStream(filter: filter, configuration: config, delegate: self)

        try stream?.addStreamOutput(self, type: .screen, sampleHandlerQueue: frameQueue)
        try await stream?.startCapture()

        logger.info("ScreenCapture: started \(width)x\(height) @ \(fps) fps (NV12)")
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

        // Only forward frames whose contents actually changed (.complete).
        // ScreenCaptureKit also emits .idle frames when the screen is static;
        // skipping them saves bandwidth (the receiver holds the last frame).
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer,
                                                                        createIfNecessary: false)
                as? [[SCStreamFrameInfo: Any]],
              let info = attachments.first,
              let statusRaw = info[.status] as? Int,
              let status = SCFrameStatus(rawValue: statusRaw),
              status == .complete else {
            return
        }

        guard let pixelBuffer = sampleBuffer.imageBuffer else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)

        delegate?.screenCapture(self, didCaptureFrame: pixelBuffer, pts: pts)
    }

    // MARK: - SCStreamDelegate

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        logger.error("ScreenCapture: stream stopped with error: \(error)")
    }
}
