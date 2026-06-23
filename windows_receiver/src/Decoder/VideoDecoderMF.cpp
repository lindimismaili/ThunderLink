#include "VideoDecoder.h"
#include "Utils/Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <vector>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;

namespace thunderlink {

/* Synchronous Media Foundation decoder.
 *
 * Selects a hardware video-decoder MFT (preferring sync-capable ones) for the
 * negotiated codec, decodes Annex-B access units, and produces tightly-packed
 * NV12 in system memory. Hardware decode is used when a sync-capable hardware
 * MFT is present; otherwise it falls back to the OS software decoder so the
 * pipeline always runs.
 *
 * Note: the lowest-possible-latency path is an *async* hardware MFT driven by
 * its event generator, decoding straight into a DXGI surface. That is the
 * documented optimization; this synchronous, system-memory path is chosen for
 * robustness and simplicity as the first implementation. */
class MediaFoundationDecoder : public VideoDecoder {
public:
    ~MediaFoundationDecoder() override { Shutdown(); }

    bool Initialize(TlnkCodec codec, uint32_t width, uint32_t height) override {
        Shutdown();
        m_codec = codec;
        m_width = width;
        m_height = height;
        m_waitingForKeyframe = true;

        const GUID inputSubtype = (codec == TLNK_CODEC_HEVC) ? MFVideoFormat_HEVC
                                                             : MFVideoFormat_H264;

        if (!CreateDecoderMFT(inputSubtype)) {
            LOG_ERROR("MFDecoder: failed to create decoder MFT");
            return false;
        }
        if (!ConfigureTypes(inputSubtype)) {
            LOG_ERROR("MFDecoder: failed to configure media types");
            return false;
        }

        /* Start streaming */
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        LOG_INFO("MFDecoder: initialized %s %ux%u (%s)",
                 (codec == TLNK_CODEC_HEVC ? "HEVC" : "H264"), width, height,
                 m_hardware ? "hardware" : "software");
        return true;
    }

    bool Decode(const uint8_t* data, uint32_t size, bool isKeyframe,
                DecodedFrame* out) override {
        if (!m_transform || !data || size == 0) return false;

        /* After loss, ignore everything until the next IDR to avoid corruption. */
        if (m_waitingForKeyframe) {
            if (!isKeyframe) return false;
            m_waitingForKeyframe = false;
        }

        ComPtr<IMFSample> inSample;
        if (FAILED(MakeInputSample(data, size, &inSample))) return false;

        HRESULT hr = m_transform->ProcessInput(0, inSample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            /* Drain pending output first, then retry once. */
            DecodedFrame tmp;
            while (PullOutput(&tmp)) { /* keep latest */ *out = tmp; }
            hr = m_transform->ProcessInput(0, inSample.Get(), 0);
        }
        if (FAILED(hr)) {
            LOG_WARN("MFDecoder: ProcessInput failed (hr=0x%08X)", hr);
            RequireKeyframe();
            return false;
        }

        return PullOutput(out);
    }

    void Shutdown() override {
        if (m_transform) {
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            m_transform.Reset();
        }
        m_yBuf.clear();
        m_uvBuf.clear();
    }

    bool IsWaitingForKeyframe() const override { return m_waitingForKeyframe; }
    void RequireKeyframe() override { m_waitingForKeyframe = true; }

private:
    bool CreateDecoderMFT(const GUID& inputSubtype) {
        MFT_REGISTER_TYPE_INFO inInfo = { MFMediaType_Video, inputSubtype };
        MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_NV12 };

        /* Prefer a hardware MFT; fall back to software. We drive synchronously,
         * so request sync MFTs (hardware decoders that expose a sync interface,
         * plus the software decoder). */
        const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT
                           | MFT_ENUM_FLAG_LOCALMFT
                           | MFT_ENUM_FLAG_SORTANDFILTER;

        for (int pass = 0; pass < 2; ++pass) {
            /* pass 0: hardware sync MFTs; pass 1: software fallback. */
            UINT32 passFlags = flags | (pass == 0 ? MFT_ENUM_FLAG_HARDWARE : 0u);
            IMFActivate** activates = nullptr;
            UINT32 count = 0;
            HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, passFlags,
                                   &inInfo, &outInfo, &activates, &count);
            if (SUCCEEDED(hr) && count > 0) {
                for (UINT32 i = 0; i < count; ++i) {
                    ComPtr<IMFTransform> t;
                    if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&t)))) {
                        m_transform = t;
                        m_hardware = (pass == 0);
                        break;
                    }
                }
                for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
                CoTaskMemFree(activates);
            }
            if (m_transform) break;
        }

        if (!m_transform) return false;

        /* Low-latency hint (no extra reordering/buffering). */
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(m_transform->GetAttributes(&attrs)) && attrs) {
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        return true;
    }

    bool ConfigureTypes(const GUID& inputSubtype) {
        /* Input type: codec + frame size. */
        ComPtr<IMFMediaType> inType;
        if (FAILED(MFCreateMediaType(&inType))) return false;
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, inputSubtype);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(m_transform->SetInputType(0, inType.Get(), 0))) {
            LOG_ERROR("MFDecoder: SetInputType failed");
            return false;
        }

        /* Output type: walk the available types and pick NV12. */
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> outType;
            HRESULT hr = m_transform->GetOutputAvailableType(0, i, &outType);
            if (hr == MF_E_NO_MORE_TYPES) break;
            if (FAILED(hr)) break;

            GUID subtype{};
            if (SUCCEEDED(outType->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                subtype == MFVideoFormat_NV12) {
                if (SUCCEEDED(m_transform->SetOutputType(0, outType.Get(), 0))) {
                    CacheOutputDims(outType.Get());
                    return true;
                }
            }
        }

        /* Fall back: construct an NV12 output type explicitly. */
        ComPtr<IMFMediaType> outType;
        if (FAILED(MFCreateMediaType(&outType))) return false;
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
        if (FAILED(m_transform->SetOutputType(0, outType.Get(), 0))) {
            LOG_ERROR("MFDecoder: SetOutputType(NV12) failed");
            return false;
        }
        CacheOutputDims(outType.Get());
        return true;
    }

    void CacheOutputDims(IMFMediaType* outType) {
        UINT32 w = m_width, h = m_height;
        MFGetAttributeSize(outType, MF_MT_FRAME_SIZE, &w, &h);
        m_width = w;
        m_height = h;
    }

    HRESULT MakeInputSample(const uint8_t* data, uint32_t size, IMFSample** outSample) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
        if (FAILED(hr)) return hr;

        BYTE* dst = nullptr;
        DWORD maxLen = 0;
        hr = buffer->Lock(&dst, &maxLen, nullptr);
        if (FAILED(hr)) return hr;
        std::memcpy(dst, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength(size);

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return hr;
        sample->AddBuffer(buffer.Get());

        /* Provide a monotonic timestamp; some MFTs require valid sample times. */
        const LONGLONG hundredNsPerFrame = 166667; /* ~60 fps */
        sample->SetSampleTime(m_sampleTime);
        sample->SetSampleDuration(hundredNsPerFrame);
        m_sampleTime += hundredNsPerFrame;

        *outSample = sample.Detach();
        return S_OK;
    }

    /* Pull one decoded frame if available. Handles stream/format changes. */
    bool PullOutput(DecodedFrame* out) {
        MFT_OUTPUT_STREAM_INFO si{};
        m_transform->GetOutputStreamInfo(0, &si);

        const bool providesSamples =
            (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                           MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        MFT_OUTPUT_DATA_BUFFER outBuf{};
        outBuf.dwStreamID = 0;

        ComPtr<IMFSample> preAlloc;
        if (!providesSamples) {
            /* Allocate a buffer for the MFT to write into. */
            ComPtr<IMFMediaBuffer> mb;
            DWORD cb = si.cbSize ? si.cbSize : (m_width * m_height * 3); /* NV12 = w*h*1.5, pad */
            if (FAILED(MFCreateMemoryBuffer(cb, &mb))) return false;
            if (FAILED(MFCreateSample(&preAlloc))) return false;
            preAlloc->AddBuffer(mb.Get());
            outBuf.pSample = preAlloc.Get();
        }

        DWORD status = 0;
        HRESULT hr = m_transform->ProcessOutput(0, 1, &outBuf, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (outBuf.pEvents) outBuf.pEvents->Release();
            return false;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            /* Renegotiate output type (size may have changed) and retry. */
            if (outBuf.pEvents) outBuf.pEvents->Release();
            ReconfigureOutput();
            return false;
        }
        if (FAILED(hr) || !outBuf.pSample) {
            if (outBuf.pEvents) outBuf.pEvents->Release();
            return false;
        }

        /* Ownership: when the MFT allocates its own sample (providesSamples) we
         * own a reference and must Release it. When we supplied `preAlloc`, the
         * MFT wrote into it and `preAlloc` (a ComPtr) still owns the only ref —
         * do NOT take a second one. */
        IMFSample* produced = outBuf.pSample;
        bool ok = CopyNV12(produced, out);
        if (outBuf.pEvents) outBuf.pEvents->Release();
        if (providesSamples) produced->Release();
        return ok;
    }

    void ReconfigureOutput() {
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> outType;
            HRESULT hr = m_transform->GetOutputAvailableType(0, i, &outType);
            if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
            GUID subtype{};
            if (SUCCEEDED(outType->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                subtype == MFVideoFormat_NV12) {
                if (SUCCEEDED(m_transform->SetOutputType(0, outType.Get(), 0))) {
                    CacheOutputDims(outType.Get());
                    break;
                }
            }
        }
    }

    /* Copy decoded NV12 into tightly-packed Y / UV member buffers. */
    bool CopyNV12(IMFSample* sample, DecodedFrame* out) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;

        const uint32_t w = m_width;
        const uint32_t h = m_height;
        const uint32_t chromaH = h / 2;

        m_yBuf.resize(static_cast<size_t>(w) * h);
        m_uvBuf.resize(static_cast<size_t>(w) * chromaH);

        /* Prefer IMF2DBuffer for correct source pitch. */
        ComPtr<IMF2DBuffer> buf2d;
        if (SUCCEEDED(buffer.As(&buf2d))) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (FAILED(buf2d->Lock2D(&scan0, &pitch))) return false;

            const BYTE* ySrc = scan0;
            const BYTE* uvSrc = scan0 + static_cast<size_t>(pitch) * h;
            for (uint32_t r = 0; r < h; ++r)
                std::memcpy(&m_yBuf[static_cast<size_t>(r) * w], ySrc + static_cast<size_t>(r) * pitch, w);
            for (uint32_t r = 0; r < chromaH; ++r)
                std::memcpy(&m_uvBuf[static_cast<size_t>(r) * w], uvSrc + static_cast<size_t>(r) * pitch, w);

            buf2d->Unlock2D();
        } else {
            /* 1D buffer: assume contiguous NV12 with pitch == width. */
            BYTE* base = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (FAILED(buffer->Lock(&base, &maxLen, &curLen))) return false;
            const size_t ySize = static_cast<size_t>(w) * h;
            const size_t uvSize = static_cast<size_t>(w) * chromaH;
            if (curLen < ySize + uvSize) { buffer->Unlock(); return false; }
            std::memcpy(m_yBuf.data(), base, ySize);
            std::memcpy(m_uvBuf.data(), base + ySize, uvSize);
            buffer->Unlock();
        }

        out->yPlane = m_yBuf.data();
        out->uvPlane = m_uvBuf.data();
        out->width = w;
        out->height = h;
        out->yStride = w;
        out->uvStride = w;
        return true;
    }

    ComPtr<IMFTransform> m_transform;
    bool m_hardware = false;
    TlnkCodec m_codec = TLNK_CODEC_HEVC;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    LONGLONG m_sampleTime = 0;
    bool m_waitingForKeyframe = true;

    std::vector<uint8_t> m_yBuf;
    std::vector<uint8_t> m_uvBuf;
};

std::unique_ptr<VideoDecoder> CreateMediaFoundationDecoder() {
    return std::make_unique<MediaFoundationDecoder>();
}

} // namespace thunderlink
