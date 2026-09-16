import CoreMedia
import CoreVideo

/// Owns encoder creation, submission, keyframe requests, and destruction on one
/// serial context. Generation/session checks discard delayed VideoToolbox output.
final class EncoderPipeline {
    typealias Output = (VideoEncoder.EncodedFrame, StreamSessionID) -> Void

    private let queue = DispatchQueue(label: "com.thunderlink.encoder", qos: .userInteractive)
    private var encoder: VideoEncoder?
    private var sessionID: StreamSessionID?
    private var output: Output?
    private var failure: ((StreamSessionID, String) -> Void)?
    private var lastBuffer: CVPixelBuffer?
    private var lastPTS = CMTime.invalid
    private var frameDuration = CMTime(value: 1, timescale: 60)

    func start(configuration: StreamConfiguration,
               output: @escaping Output,
               failure: @escaping (StreamSessionID, String) -> Void) async -> Bool {
        await withCheckedContinuation { continuation in
            queue.async {
                continuation.resume(returning: self.startNow(configuration: configuration,
                                                              output: output, failure: failure))
            }
        }
    }

    private func startNow(configuration: StreamConfiguration,
                          output: @escaping Output,
                          failure: @escaping (StreamSessionID, String) -> Void) -> Bool {
        stopNow()
        guard let created = VideoEncoder(width: configuration.width,
                                         height: configuration.height,
                                         fps: configuration.fps,
                                         bitrateKbps: configuration.bitrateKbps,
                                         codec: configuration.codec) else { return false }
        sessionID = configuration.sessionID
        frameDuration = CMTime(value: 1, timescale: CMTimeScale(configuration.fps))
        self.output = output
        self.failure = failure
        let expectedSession = configuration.sessionID
        created.onEncodedFrame = { [weak self] frame in
            self?.queue.async { [weak self] in
                guard let self, self.sessionID == expectedSession else { return }
                self.output?(frame, expectedSession)
            }
        }
        created.onError = { [weak self] message in
            self?.queue.async { [weak self] in
                guard let self, self.sessionID == expectedSession else { return }
                self.failure?(expectedSession, message)
            }
        }
        encoder = created
        return true
    }

    func encode(_ pixelBuffer: CVPixelBuffer, pts: CMTime, sessionID: StreamSessionID) {
        queue.async { [weak self] in
            guard let self, self.sessionID == sessionID else { return }
            self.lastBuffer = pixelBuffer
            self.lastPTS = self.lastPTS.isValid && CMTimeCompare(pts, self.lastPTS) <= 0
                ? CMTimeAdd(self.lastPTS, self.frameDuration) : pts
            self.encoder?.encode(pixelBuffer, pts: self.lastPTS)
        }
    }

    func requestKeyframe(sessionID: StreamSessionID) {
        queue.async { [weak self] in
            guard self?.sessionID == sessionID else { return }
            self?.encoder?.requestKeyframe()
            if let self, let buffer = self.lastBuffer {
                self.lastPTS = CMTimeAdd(self.lastPTS, self.frameDuration)
                self.encoder?.encode(buffer, pts: self.lastPTS)
            }
        }
    }

    func stop() async {
        await withCheckedContinuation { continuation in
            queue.async { self.stopNow(); continuation.resume() }
        }
    }


    private func stopNow() {
        sessionID = nil
        output = nil
        failure = nil
        encoder?.stop()
        encoder = nil
        lastBuffer = nil
        lastPTS = .invalid
    }
}
