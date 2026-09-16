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
    func screenCapture(_ capture: ScreenCapture, didStopWithError error: Error)
}

/// Captures the Mac screen using ScreenCaptureKit with hardware acceleration.
/// Configures the stream for NV12 (4:2:0) output at the requested FPS — the
/// native input format for the VideoToolbox H.264/HEVC encoder.
final class ScreenCapture: NSObject, SCStreamDelegate, SCStreamOutput {
    weak var delegate: ScreenCaptureDelegate?

    private var stream: SCStream?
    private let deliveryLock = NSLock()
    private var deliveryStream: SCStream?

    private func setDeliveryStream(_ stream: SCStream?) {
        deliveryLock.lock(); deliveryStream = stream; deliveryLock.unlock()
    }
    private func accepts(_ stream: SCStream) -> Bool {
        deliveryLock.lock(); defer { deliveryLock.unlock() }
        return deliveryStream === stream
    }
    private let frameQueue = DispatchQueue(label: "com.thunderlink.capture",
                                           qos: .userInteractive)
    private(set) var captureWidth: Int = 0
    private(set) var captureHeight: Int = 0
    private(set) var captureFPS: Int = 0

    /// Start capturing the given display.
    func startCapture(display: SCDisplay, width: Int, height: Int, fps: Int) async throws {
        guard width >= 2, width <= NetworkConfig.maxWidth, width.isMultiple(of: 2),
              height >= 2, height <= NetworkConfig.maxHeight, height.isMultiple(of: 2),
              (NetworkConfig.minFPS...NetworkConfig.maxFPS).contains(fps) else {
            throw StreamConfigurationError.invalidResolvedResolution(width, height)
        }
        await stopCapture()
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

        let newStream = SCStream(filter: filter, configuration: config, delegate: self)
        setDeliveryStream(newStream)
        do {
            try newStream.addStreamOutput(self, type: .screen, sampleHandlerQueue: frameQueue)
            try await newStream.startCapture()
            stream = newStream
        } catch {
            setDeliveryStream(nil)
            try? newStream.removeStreamOutput(self, type: .screen)
            throw error
        }

        logger.info("ScreenCapture: started \(width)x\(height) @ \(fps) fps (NV12)")
    }

    /// Stop the current capture session.
    func stopCapture() async {
        setDeliveryStream(nil)
        guard let current = stream else { return }
        stream = nil
        do {
            try await current.stopCapture()
        } catch {
            logger.warn("ScreenCapture: stopCapture error: \(error)")
        }
        try? current.removeStreamOutput(self, type: .screen)
        logger.info("ScreenCapture: stopped")
    }

    // MARK: - SCStreamOutput

    func stream(_ stream: SCStream,
                didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
                of type: SCStreamOutputType) {
        guard type == .screen, accepts(stream) else { return }

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
        guard accepts(stream) else { return }
        logger.error("ScreenCapture: stream stopped with error: \(error)")
        delegate?.screenCapture(self, didStopWithError: error)
    }
}
