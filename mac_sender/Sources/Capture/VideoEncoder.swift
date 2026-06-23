import Foundation
import VideoToolbox
import CoreMedia
import CoreVideo

/// Hardware H.264/HEVC encoder built on VideoToolbox (`VTCompressionSession`).
///
/// Tuned for real-time screen streaming: real-time mode, no frame reordering
/// (no B-frames → no reorder latency), on-demand IDR (no periodic keyframes),
/// and a hard data-rate cap so a single complex frame can't blow the link's
/// packet budget.
///
/// Output is delivered as **Annex-B** (start-code delimited). Parameter sets
/// (VPS/SPS/PPS) are prepended in-band before every IDR so each keyframe is
/// independently decodable after packet loss.
///
/// `encode(_:pts:)` must be called serially (it is — the capture queue is
/// serial). `requestKeyframe()` may be called from any thread.
final class VideoEncoder {
    struct EncodedFrame {
        let data: Data
        let isKeyframe: Bool
        let width: Int
        let height: Int
    }

    /// Invoked on VideoToolbox's internal output queue for each encoded frame.
    var onEncodedFrame: ((EncodedFrame) -> Void)?

    private var session: VTCompressionSession?
    private let codec: TlnkCodecSwift
    private let width: Int32
    private let height: Int32
    private let fps: Int32
    private let bitrateBps: Int32

    private let forceKeyframeLock = NSLock()
    private var forceKeyframePending = false

    init?(width: Int, height: Int, fps: Int, bitrateKbps: Int, codec: TlnkCodecSwift) {
        self.codec = codec
        self.width = Int32(width)
        self.height = Int32(height)
        self.fps = Int32(fps)
        self.bitrateBps = Int32(bitrateKbps * 1000)

        let codecType: CMVideoCodecType = (codec == .hevc) ? kCMVideoCodecType_HEVC
                                                           : kCMVideoCodecType_H264

        var created: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: self.width,
            height: self.height,
            codecType: codecType,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil,        // using the block-based encode API
            refcon: nil,
            compressionSessionOut: &created)

        guard status == noErr, let session = created else {
            logger.error("VideoEncoder: VTCompressionSessionCreate failed (\(status))")
            return nil
        }
        self.session = session

        configure(session)
        VTCompressionSessionPrepareToEncodeFrames(session)
        logger.info("VideoEncoder: ready \(width)x\(height) @ \(fps) fps, " +
                    "codec=\(codec), \(bitrateKbps) kbps")
    }

    deinit {
        stop()
    }

    private func configure(_ session: VTCompressionSession) {
        func setProp(_ key: CFString, _ value: CFTypeRef) {
            VTSessionSetProperty(session, key: key, value: value)
        }

        setProp(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        setProp(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)

        let profile: CFString = (codec == .hevc)
            ? kVTProfileLevel_HEVC_Main_AutoLevel
            : kVTProfileLevel_H264_High_AutoLevel
        setProp(kVTCompressionPropertyKey_ProfileLevel, profile)

        setProp(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: bitrateBps))
        setProp(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))

        // Rely on on-demand IDR (see requestKeyframe); avoid frequent keyframes
        // that spike the bitrate. ~10 s safety net at the configured fps.
        setProp(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: fps * 10))

        // Hard cap: ~1.4x average bitrate worth of bytes over a 1-second window.
        let capBytesPerSec = Int(Double(bitrateBps) * 1.4 / 8.0)
        let dataRateLimits = [NSNumber(value: capBytesPerSec), NSNumber(value: 1)] as CFArray
        setProp(kVTCompressionPropertyKey_DataRateLimits, dataRateLimits)

        // BT.709 color so the Windows YUV->RGB shader matches.
        setProp(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
        setProp(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2)
        setProp(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
    }

    /// Force the next encoded frame to be an IDR (loss recovery).
    func requestKeyframe() {
        forceKeyframeLock.lock()
        forceKeyframePending = true
        forceKeyframeLock.unlock()
    }

    /// Encode one captured frame. Call serially.
    func encode(_ pixelBuffer: CVPixelBuffer, pts: CMTime) {
        guard let session = session else { return }

        forceKeyframeLock.lock()
        let force = forceKeyframePending
        forceKeyframePending = false
        forceKeyframeLock.unlock()

        var frameProps: CFDictionary? = nil
        if force {
            frameProps = [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary
        }

        let status = VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: pixelBuffer,
            presentationTimeStamp: pts,
            duration: .invalid,
            frameProperties: frameProps,
            infoFlagsOut: nil
        ) { [weak self] status, _, sampleBuffer in
            guard let self = self else { return }
            guard status == noErr, let sb = sampleBuffer else {
                if status != noErr { logger.warn("VideoEncoder: encode callback status \(status)") }
                return
            }
            self.handleEncoded(sb)
        }

        if status != noErr {
            logger.warn("VideoEncoder: VTCompressionSessionEncodeFrame failed (\(status))")
        }
    }

    func stop() {
        guard let session = session else { return }
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(session)
        self.session = nil
    }

    // MARK: - CMSampleBuffer -> Annex-B

    private func handleEncoded(_ sb: CMSampleBuffer) {
        guard let dataBuffer = CMSampleBufferGetDataBuffer(sb) else { return }

        let isKeyframe = Self.isKeyframe(sb)

        var out = Data()

        // Prepend parameter sets (Annex-B) ahead of every IDR for self-contained recovery.
        if isKeyframe, let fmt = CMSampleBufferGetFormatDescription(sb) {
            out.append(parameterSetsAnnexB(fmt))
        }

        var totalLength = 0
        var dataPointer: UnsafeMutablePointer<Int8>?
        let st = CMBlockBufferGetDataPointer(dataBuffer,
                                             atOffset: 0,
                                             lengthAtOffsetOut: nil,
                                             totalLengthOut: &totalLength,
                                             dataPointerOut: &dataPointer)
        guard st == kCMBlockBufferNoErr, let base = dataPointer else { return }

        // VideoToolbox emits length-prefixed (AVCC/HVCC) NAL units: each is a
        // 4-byte big-endian length followed by the NAL payload. Convert each to
        // an Annex-B start code (00 00 00 01).
        let startCode: [UInt8] = [0, 0, 0, 1]
        let raw = UnsafeRawPointer(base)
        var offset = 0
        while offset + 4 <= totalLength {
            var nalLength: UInt32 = 0
            memcpy(&nalLength, raw + offset, 4)
            nalLength = UInt32(bigEndian: nalLength)
            offset += 4
            if nalLength == 0 || offset + Int(nalLength) > totalLength { break }
            out.append(contentsOf: startCode)
            out.append(Data(bytes: raw + offset, count: Int(nalLength)))
            offset += Int(nalLength)
        }

        onEncodedFrame?(EncodedFrame(data: out,
                                     isKeyframe: isKeyframe,
                                     width: Int(width),
                                     height: Int(height)))
    }

    private static func isKeyframe(_ sb: CMSampleBuffer) -> Bool {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false)
                as? [[CFString: Any]],
              let first = attachments.first else {
            return true  // no attachments → treat as sync sample
        }
        // Keyframe == NOT a "not-sync" sample.
        if let notSync = first[kCMSampleAttachmentKey_NotSync] as? Bool {
            return !notSync
        }
        return true
    }

    private func parameterSetsAnnexB(_ fmt: CMFormatDescription) -> Data {
        var data = Data()
        let startCode: [UInt8] = [0, 0, 0, 1]

        // First query the parameter-set count, then emit each one.
        var count = 0
        if codec == .hevc {
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                fmt, parameterSetIndex: 0,
                parameterSetPointerOut: nil, parameterSetSizeOut: nil,
                parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
        } else {
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                fmt, parameterSetIndex: 0,
                parameterSetPointerOut: nil, parameterSetSizeOut: nil,
                parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
        }

        for i in 0..<count {
            var ptr: UnsafePointer<UInt8>?
            var size = 0
            let st: OSStatus
            if codec == .hevc {
                st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    fmt, parameterSetIndex: i,
                    parameterSetPointerOut: &ptr, parameterSetSizeOut: &size,
                    parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
            } else {
                st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    fmt, parameterSetIndex: i,
                    parameterSetPointerOut: &ptr, parameterSetSizeOut: &size,
                    parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
            }
            if st == noErr, let p = ptr {
                data.append(contentsOf: startCode)
                data.append(Data(bytes: p, count: size))
            }
        }
        return data
    }
}
