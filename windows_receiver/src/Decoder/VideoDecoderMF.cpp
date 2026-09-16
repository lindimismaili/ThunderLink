#include "VideoDecoder.h"
#include "NV12Copy.h"
#include "DecoderQueue.h"
#include "DecoderTestHooks.h"
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
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace thunderlink {
namespace {


std::string HResultString(const char* operation, HRESULT hr) {
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "%s failed (hr=0x%08lX)",
                  operation, static_cast<unsigned long>(hr));
    return buffer;
}

std::string FriendlyName(IMFActivate* activate) {
    wchar_t* value = nullptr;
    UINT32 length = 0;
    if (!activate || FAILED(activate->GetAllocatedString(
            MFT_FRIENDLY_NAME_Attribute, &value, &length)) || !value) {
        return {};
    }

    int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string result;
    if (needed > 1) {
        result.resize(static_cast<size_t>(needed));
        WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), needed,
                            nullptr, nullptr);
        result.pop_back();
    }
    CoTaskMemFree(value);
    return result;
}

bool CheckedNV12Size(uint32_t width, uint32_t height, size_t* ySize,
                     size_t* uvSize) {
    if (width == 0 || height == 0 || (width & 1u) || (height & 1u)) return false;
    const uint64_t y = static_cast<uint64_t>(width) * height;
    const uint64_t uv = static_cast<uint64_t>(width) * (height / 2u);
    if (y > std::numeric_limits<size_t>::max() ||
        uv > std::numeric_limits<size_t>::max() - static_cast<size_t>(y)) {
        return false;
    }
    *ySize = static_cast<size_t>(y);
    *uvSize = static_cast<size_t>(uv);
    return true;
}

} // namespace

/* Media Foundation decoder with one strict owner thread. Hardware decoder MFTs
 * are asynchronous and are driven only by their event queue. If hardware
 * processing fails, the worker tries a synchronous software MFT for the same
 * codec and waits for a fresh keyframe. */
class MediaFoundationDecoder final : public VideoDecoder {
public:
    explicit MediaFoundationDecoder(DecoderCandidateFactory factory = {}) : m_candidateFactory(std::move(factory)) {}
    ~MediaFoundationDecoder() override { Shutdown(); }

    bool Initialize(TlnkCodec codec, uint32_t width, uint32_t height,
                    uint32_t fps, uint64_t sessionId) override {
        Shutdown();
        if ((codec != TLNK_CODEC_H264 && codec != TLNK_CODEC_HEVC) ||
            width == 0 || height == 0 || width > TLNK_MAX_WIDTH ||
            height > TLNK_MAX_HEIGHT || (width & 1u) || (height & 1u) ||
            fps < TLNK_MIN_FPS || fps > TLNK_MAX_FPS) {
            SetError("invalid decoder configuration");
            return false;
        }

        m_codec = codec;
        m_nominalWidth = m_outputWidth = width;
        m_nominalHeight = m_outputHeight = height;
        m_fps = fps;
        m_sessionId = sessionId;
        m_nextSoftwareCandidate = 0;
        m_queue.Start(sessionId);
        m_workerGeneration = m_queue.Epoch();
        m_ready.store(false, std::memory_order_release);
        m_failed.store(false, std::memory_order_release);

        SetBackend("initializing");
        SetError({});

        try {
            m_worker = std::thread(&MediaFoundationDecoder::WorkerMain, this);
        } catch (const std::system_error& e) {
            SetError(std::string("failed to start decoder worker: ") + e.what());
            m_failed.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    bool Submit(const uint8_t* data, uint32_t size, bool keyframe, uint64_t session) override {
        return !m_failed.load(std::memory_order_acquire) && m_queue.Submit(data, size, keyframe, session);
    }
    bool TryGetFrame(DecodedFrame* frame) override { return m_queue.TakeNewest(frame); }
    void Shutdown() override {
        m_queue.Stop();
        if (m_worker.joinable()) m_worker.join();
        m_ready.store(false, std::memory_order_release);
        SetBackend("stopped");
    }
    bool IsWaitingForKeyframe() const override { return m_queue.Waiting(); }
    void RequireKeyframe() override { m_queue.RequireKeyframe(); }

    bool IsReady() const override { return m_ready.load(std::memory_order_acquire); }

    std::string GetBackendName() const override {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        return m_backendName;
    }

    std::string GetError() const override {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        return m_error;
    }

private:
    enum class OutputResult { Produced, NeedMoreInput, StreamChange, Error };

    void WorkerMain() {
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitializeCOM = SUCCEEDED(coHr);
        if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
            Fail(HResultString("CoInitializeEx", coHr));
            return;
        }

        const GUID inputSubtype = m_codec == TLNK_CODEC_HEVC
                                      ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        if (!TryDecoderCandidates(inputSubtype, true) &&
            !TryDecoderCandidates(inputSubtype, false)) {
            Fail(std::string("no usable ") +
                 (m_codec == TLNK_CODEC_HEVC ? "HEVC" : "H.264") +
                 " decoder found");
            if (uninitializeCOM) CoUninitialize();
            return;
        }

        m_ready.store(true, std::memory_order_release);
        LOG_INFO("MFDecoder: ready for %s %ux%u @ %u fps using %s",
                 m_codec == TLNK_CODEC_HEVC ? "HEVC" : "H264",
                 m_nominalWidth, m_nominalHeight, m_fps, GetBackendName().c_str());

        while (!StopRequested()) {
            if (TakeFlushRequest() && !FlushTransform()) {
                if (!HandleRuntimeFailure(E_FAIL, "flush")) break;
            }
            if (m_async && !PumpAsyncEvents()) break;

            EncodedFrame frame;
            if (TakeInput(&frame)) {
                if (!ProcessInputFrame(frame)) break;
                continue;
            }

            m_queue.Wait(!m_async || m_needInput > 0, m_async ? 2 : 20);
        }

        ResetTransform();
        m_ready.store(false, std::memory_order_release);
        if (uninitializeCOM) CoUninitialize();
    }

    bool TryDecoderCandidates(const GUID& inputSubtype, bool hardware) {
        MFT_REGISTER_TYPE_INFO inputInfo = {MFMediaType_Video, inputSubtype};
        MFT_REGISTER_TYPE_INFO outputInfo = {MFMediaType_Video, MFVideoFormat_NV12};
        const UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER |
            (hardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT)
                      : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT));
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = S_OK;
        std::vector<ComPtr<IMFTransform>> injected;
        if (m_candidateFactory) {
            injected = m_candidateFactory(m_codec, hardware);
            count = static_cast<UINT32>(injected.size());
        } else {
            hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags,
                          &inputInfo, &outputInfo, &activates, &count);
        }
        if (FAILED(hr)) {
            LOG_WARN("MFDecoder: enumeration failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }

        bool selected = false;
        for (UINT32 i = hardware ? 0 : m_nextSoftwareCandidate;
             i < count && !selected && !StopRequested(); ++i) {
            IMFActivate* activate = activates ? activates[i] : nullptr;
            const std::string name = FriendlyName(activate);
            ComPtr<IMFTransform> transform;
            if (activate) hr = activate->ActivateObject(IID_PPV_ARGS(&transform));
            else { transform = injected[i]; hr = transform ? S_OK : E_FAIL; }
            if (FAILED(hr) || !transform) continue;

            ComPtr<IMFAttributes> attributes;
            hr = transform->GetAttributes(&attributes);
            UINT32 isAsync = FALSE;
            if (attributes) attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
            if (hardware && (FAILED(hr) || !attributes ||
                             FAILED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) ||
                             !isAsync)) {
                transform.Reset();
                if (activate) activate->ShutdownObject();
                continue;
            }
            if (!hardware && isAsync) {
                transform.Reset();
                if (activate) activate->ShutdownObject();
                continue;
            }

            ComPtr<IMFMediaEventGenerator> eventGenerator;
            if (hardware) {
                hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                if (FAILED(hr) || FAILED(transform.As(&eventGenerator))) {
                    transform.Reset();
                    if (activate) activate->ShutdownObject();
                    continue;
                }
            }
            if (attributes) attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

            uint32_t outputWidth = m_nominalWidth;
            uint32_t outputHeight = m_nominalHeight;
            LONG outputStride = static_cast<LONG>(m_nominalWidth);
            DWORD inputCount = 0, outputCount = 0;
            m_inputID = m_outputID = 0;
            if (FAILED(transform->GetStreamCount(&inputCount, &outputCount)) ||
                inputCount != 1 || outputCount != 1) {
                transform.Reset();
                if (activate) activate->ShutdownObject();
                continue;
            }
            hr = transform->GetStreamIDs(1, &m_inputID, 1, &m_outputID);
            if (FAILED(hr) && hr != E_NOTIMPL) {
                transform.Reset();
                if (activate) activate->ShutdownObject();
                continue;
            }
            if (!ConfigureTypes(transform.Get(), inputSubtype,
                                &outputWidth, &outputHeight, &outputStride)) {
                LOG_WARN("MFDecoder: candidate '%s' rejected media types", name.c_str());
                transform.Reset();
                if (activate) activate->ShutdownObject();
                continue;
            }
            hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            if (SUCCEEDED(hr)) {
                hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            }
            if (FAILED(hr)) {
                transform.Reset();
                if (activate) activate->ShutdownObject();
                continue;
            }

            m_transform = std::move(transform);
            m_eventGenerator = std::move(eventGenerator);
            m_activation = activate;
            m_async = hardware;
            m_hardware = hardware;
            if (!hardware) m_nextSoftwareCandidate = i + 1;
            m_needInput = 0;
            m_outputWidth = outputWidth;
            m_outputHeight = outputHeight;
            m_outputStride = outputStride;
            m_sampleTime = 0;
            SetBackend(std::string(hardware ? "hardware" : "software") +
                       (name.empty() ? std::string() : ": " + name));
            selected = true;
        }

        if (activates) for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        return selected;
    }

    bool ConfigureTypes(IMFTransform* transform, const GUID& inputSubtype,
                        uint32_t* outputWidth, uint32_t* outputHeight,
                        LONG* outputStride) {
        ComPtr<IMFMediaType> inputType;
        if (FAILED(MFCreateMediaType(&inputType)) ||
            FAILED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(inputType->SetGUID(MF_MT_SUBTYPE, inputSubtype)) ||
            FAILED(inputType->SetUINT32(MF_MT_INTERLACE_MODE,
                                        MFVideoInterlace_Progressive)) ||
            FAILED(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE,
                                      m_nominalWidth, m_nominalHeight)) ||
            FAILED(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, m_fps, 1)) ||
            FAILED(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(transform->SetInputType(m_inputID, inputType.Get(), 0))) return false;

        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> outputType;
            HRESULT hr = transform->GetOutputAvailableType(m_outputID, i, &outputType);
            if (hr == MF_E_NO_MORE_TYPES) break;
            if (FAILED(hr)) return false;
            GUID subtype = GUID_NULL;
            if (SUCCEEDED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                subtype == MFVideoFormat_NV12 &&
                SUCCEEDED(transform->SetOutputType(m_outputID, outputType.Get(), 0))) {
                if (CacheOutputType(outputType.Get(), outputWidth,
                                    outputHeight, outputStride)) return true;
            }
        }

        ComPtr<IMFMediaType> outputType;
        if (FAILED(MFCreateMediaType(&outputType)) ||
            FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
            FAILED(outputType->SetUINT32(MF_MT_INTERLACE_MODE,
                                         MFVideoInterlace_Progressive)) ||
            FAILED(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE,
                                      m_nominalWidth, m_nominalHeight)) ||
            FAILED(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, m_fps, 1)) ||
            FAILED(transform->SetOutputType(m_outputID, outputType.Get(), 0))) return false;
        return CacheOutputType(outputType.Get(), outputWidth, outputHeight, outputStride);
    }

    static bool CacheOutputType(IMFMediaType* type, uint32_t* width,
                                uint32_t* height, LONG* stride) {
        UINT32 w = *width;
        UINT32 h = *height;
        if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h))) {
            *width = w;
            *height = h;
        }
        if (*width == 0 || *height == 0 || *width > TLNK_MAX_WIDTH ||
            *height > TLNK_MAX_HEIGHT || (*width & 1u) || (*height & 1u)) {
            return false;
        }
        UINT32 rawStride = 0;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) {
            *stride = static_cast<LONG>(rawStride);
        } else if (FAILED(MFGetStrideForBitmapInfoHeader(
                       MFVideoFormat_NV12.Data1, *width, stride))) {
            *stride = static_cast<LONG>(*width);
        }
        return *stride >= static_cast<LONG>(*width);
    }

    bool PumpAsyncEvents() {
        while (m_eventGenerator && !StopRequested()) {
            ComPtr<IMFMediaEvent> event;
            HRESULT hr = m_eventGenerator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) return true;
            if (FAILED(hr)) return HandleRuntimeFailure(hr, "GetEvent");
            HRESULT eventStatus = S_OK;
            event->GetStatus(&eventStatus);
            if (FAILED(eventStatus)) {
                return HandleRuntimeFailure(eventStatus, "decoder event");
            }
            MediaEventType type = MEUnknown;
            if (FAILED(event->GetType(&type))) continue;
            if (type == METransformNeedInput) {
                m_needInput = std::min<uint32_t>(m_needInput + 1, 16);
            } else if (type == METransformHaveOutput) {
                if (PullOutput() == OutputResult::Error) {
                    return HandleRuntimeFailure(E_FAIL, "ProcessOutput");
                }
            } else if (type == MEError) {
                return HandleRuntimeFailure(eventStatus, "asynchronous decoder");
            }
        }
        return true;
    }

    bool ProcessInputFrame(const EncodedFrame& frame) {
        if (frame.sessionId != m_sessionId) return true;
        if (m_queue.Waiting() && !frame.keyframe) {
            return true;
        }
        ComPtr<IMFSample> sample;
        HRESULT hr = MakeInputSample(frame, &sample);
        if (FAILED(hr)) return HandleRuntimeFailure(hr, "creating input sample");
        hr = m_transform->ProcessInput(m_inputID, sample.Get(), 0);
        if (!m_async && hr == MF_E_NOTACCEPTING) {
            OutputResult output;
            do { output = PullOutput(); }
            while (output == OutputResult::Produced || output == OutputResult::StreamChange);
            if (output == OutputResult::Error) return HandleRuntimeFailure(E_FAIL, "ProcessOutput");
            hr = m_transform->ProcessInput(m_inputID, sample.Get(), 0);
        }
        if (FAILED(hr)) return HandleRuntimeFailure(hr, "ProcessInput");
        if (m_async && m_needInput > 0) --m_needInput;
        if (!m_async) {
            OutputResult result;
            do {
                result = PullOutput();
            } while (result == OutputResult::Produced || result == OutputResult::StreamChange);
            if (result == OutputResult::Error) {
                return HandleRuntimeFailure(E_FAIL, "ProcessOutput");
            }
        }
        return true;
    }

    HRESULT MakeInputSample(const EncodedFrame& frame, IMFSample** output) {
        if (frame.bytes.size() > std::numeric_limits<DWORD>::max()) return E_INVALIDARG;
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(frame.bytes.size()), &buffer);
        if (FAILED(hr)) return hr;
        BYTE* destination = nullptr;
        DWORD capacity = 0;
        hr = buffer->Lock(&destination, &capacity, nullptr);
        if (FAILED(hr)) return hr;
        if (capacity < frame.bytes.size()) {
            buffer->Unlock();
            return E_UNEXPECTED;
        }
        std::memcpy(destination, frame.bytes.data(), frame.bytes.size());
        buffer->Unlock();
        hr = buffer->SetCurrentLength(static_cast<DWORD>(frame.bytes.size()));
        if (FAILED(hr)) return hr;
        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) {
            return E_FAIL;
        }
        const LONGLONG duration = std::max<LONGLONG>(1, 10000000LL / m_fps);
        sample->SetSampleTime(m_sampleTime);
        sample->SetSampleDuration(duration);
        m_sampleTime += duration;
        if (frame.keyframe) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        *output = sample.Detach();
        return S_OK;
    }

    OutputResult PullOutput() {
        MFT_OUTPUT_STREAM_INFO streamInfo = {};
        if (FAILED(m_transform->GetOutputStreamInfo(m_outputID, &streamInfo))) {
            return OutputResult::Error;
        }
        MFT_OUTPUT_DATA_BUFFER output = {};
        output.dwStreamID = m_outputID;
        ComPtr<IMFSample> suppliedSample;
        const bool providesSample =
            (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                   MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        if (!providesSample) {
            DWORD bufferSize = streamInfo.cbSize;
            if (bufferSize == 0) {
                size_t ySize = 0;
                size_t uvSize = 0;
                if (!CheckedNV12Size(m_outputWidth, m_outputHeight, &ySize, &uvSize) ||
                    ySize + uvSize > std::numeric_limits<DWORD>::max()) {
                    return OutputResult::Error;
                }
                bufferSize = static_cast<DWORD>(ySize + uvSize);
            }
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateAlignedMemoryBuffer(bufferSize, streamInfo.cbAlignment, &buffer)) ||
                FAILED(MFCreateSample(&suppliedSample)) ||
                FAILED(suppliedSample->AddBuffer(buffer.Get()))) {
                return OutputResult::Error;
            }
            output.pSample = suppliedSample.Get();
        }
        DWORD status = 0;
        HRESULT hr = m_transform->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        auto releaseReturnedSample = [&] {
            if (output.pSample && output.pSample != suppliedSample.Get()) {
                output.pSample->Release();
                output.pSample = nullptr;
            }
        };
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            releaseReturnedSample();
            return OutputResult::NeedMoreInput;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            releaseReturnedSample();
            return ReconfigureOutput() ? OutputResult::StreamChange : OutputResult::Error;
        }
        if (FAILED(hr) || !output.pSample) {
            releaseReturnedSample();
            return OutputResult::Error;
        }

        IMFSample* produced = output.pSample;
        DecodedFrame frame;
        const bool copied = CopyNV12(produced, &frame);
        releaseReturnedSample();
        if (!copied) return OutputResult::Error;
        PushDecoded(std::move(frame));
        return OutputResult::Produced;
    }

    bool ReconfigureOutput() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> outputType;
            HRESULT hr = m_transform->GetOutputAvailableType(m_outputID, i, &outputType);
            if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) return false;
            GUID subtype = GUID_NULL;
            if (SUCCEEDED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                subtype == MFVideoFormat_NV12 &&
                SUCCEEDED(m_transform->SetOutputType(m_outputID, outputType.Get(), 0))) {
                if (CacheOutputType(outputType.Get(), &m_outputWidth,
                                    &m_outputHeight, &m_outputStride)) return true;
            }
        }
    }

    bool CopyNV12(IMFSample* sample, DecodedFrame* output) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
        size_t ySize = 0;
        size_t uvSize = 0;
        if (!CheckedNV12Size(m_outputWidth, m_outputHeight, &ySize, &uvSize)) return false;
        std::shared_ptr<std::vector<uint8_t>> pixels;
        try {
            pixels = std::make_shared<std::vector<uint8_t>>(ySize + uvSize);
        } catch (const std::bad_alloc&) {
            return false;
        }

        ComPtr<IMF2DBuffer> twoDimensional;
        if (SUCCEEDED(buffer.As(&twoDimensional))) {
            // MF removes surface padding and supplies the minimum contiguous
            // stride. Do not interpret IMFMediaBuffer::Lock using surface pitch.
            DWORD contiguousLength = 0;
            if (FAILED(twoDimensional->GetContiguousLength(&contiguousLength))) return false;
            if (contiguousLength == pixels->size()) {
                if (FAILED(twoDimensional->ContiguousCopyTo(pixels->data(), contiguousLength))) return false;
            } else {
                const uint32_t rows = m_outputHeight + m_outputHeight / 2;
                if (contiguousLength % rows || contiguousLength / rows < m_outputWidth ||
                    contiguousLength > uint64_t(TLNK_MAX_WIDTH) * TLNK_MAX_HEIGHT * 4) return false;
                try {
                    std::vector<uint8_t> contiguous(contiguousLength);
                    if (FAILED(twoDimensional->ContiguousCopyTo(contiguous.data(), contiguousLength)) ||
                        !CopyNV12Rows(contiguous.data(), contiguous.size(), m_outputWidth, m_outputHeight,
                                      contiguousLength / rows, *pixels)) return false;
                } catch (const std::bad_alloc&) { return false; }
            }
        } else {
            BYTE* base = nullptr;
            DWORD capacity = 0, length = 0;
            if (FAILED(buffer->Lock(&base, &capacity, &length))) return false;
            bool copied = false;
            try {
                copied = length <= capacity && CopyNV12Rows(base, length, m_outputWidth,
                                                            m_outputHeight, m_outputStride, *pixels);
            } catch (const std::bad_alloc&) { copied = false; }
            buffer->Unlock();
            if (!copied) return false;
        }

        output->storage = std::move(pixels);
        output->sessionId = m_sessionId;
        output->width = m_outputWidth;
        output->height = m_outputHeight;
        output->yStride = m_outputWidth;
        output->uvStride = m_outputWidth;
        output->yOffset = 0;
        output->uvOffset = ySize;
        return true;
    }

    void PushDecoded(DecodedFrame frame) { m_queue.Publish(std::move(frame), m_workerGeneration); }
    bool TakeInput(EncodedFrame* frame) {
        return (!m_async || m_needInput > 0) && m_queue.Pop(frame);
    }
    bool TakeFlushRequest() {
        const auto epoch = m_queue.TakeFlush();
        if (epoch) m_workerGeneration = *epoch;
        return epoch.has_value();
    }

    bool FlushTransform() {
        if (!m_transform ||
            FAILED(m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0))) return false;
        m_needInput = 0;
        m_sampleTime = 0;
        if (m_eventGenerator) {
            ComPtr<IMFMediaEvent> stale;
            while (SUCCEEDED(m_eventGenerator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &stale))) {
                stale.Reset();
                if (StopRequested()) return false;
            }
        }
        return SUCCEEDED(m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }

    bool HandleRuntimeFailure(HRESULT hr, const char* operation) {
        const std::string message = HResultString(operation, hr);
        if (m_hardware || m_nextSoftwareCandidate > 0) {
            LOG_WARN("MFDecoder: %s; trying software fallback", message.c_str());
            const GUID subtype = m_codec == TLNK_CODEC_HEVC
                                     ? MFVideoFormat_HEVC : MFVideoFormat_H264;
            m_ready.store(false, std::memory_order_release);
            SetBackend("initializing software fallback");
            ResetTransform();
            if (TryDecoderCandidates(subtype, false)) {
                m_queue.RequireKeyframe();
                m_ready.store(true, std::memory_order_release);
                LOG_WARN("MFDecoder: software fallback active; waiting for keyframe");
                return true;
            }
        }
        Fail(message);
        return false;
    }

    void ResetTransform() {
        if (m_transform) {
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        ComPtr<IMFShutdown> shutdown;
        if (m_transform && SUCCEEDED(m_transform.As(&shutdown))) shutdown->Shutdown();
        m_eventGenerator.Reset();
        m_transform.Reset();
        if (m_activation) m_activation->ShutdownObject();
        m_activation.Reset();
        m_needInput = 0;
        m_async = false;
        m_hardware = false;
    }

    bool StopRequested() const { return m_queue.Stopped(); }

    void Fail(std::string error) {
        LOG_ERROR("MFDecoder: %s", error.c_str());
        SetError(std::move(error));
        m_failed.store(true, std::memory_order_release);
        m_ready.store(false, std::memory_order_release);
        m_queue.Stop();
    }

    void SetBackend(std::string value) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_backendName = std::move(value);
    }
    void SetError(std::string value) {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_error = std::move(value);
    }

    DecoderCandidateFactory m_candidateFactory;
    DecoderQueue m_queue;
    std::thread m_worker;
    uint64_t m_workerGeneration = 0;

    mutable std::mutex m_statusMutex;
    std::string m_backendName = "stopped";
    std::string m_error;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};

    TlnkCodec m_codec = TLNK_CODEC_HEVC;
    uint32_t m_nominalWidth = 0;
    uint32_t m_nominalHeight = 0;
    uint32_t m_outputWidth = 0;
    uint32_t m_outputHeight = 0;
    uint32_t m_fps = 60;
    uint64_t m_sessionId = 0;
    LONG m_outputStride = 0;
    LONGLONG m_sampleTime = 0;

    ComPtr<IMFActivate> m_activation;
    ComPtr<IMFTransform> m_transform;
    ComPtr<IMFMediaEventGenerator> m_eventGenerator;
    bool m_async = false;
    bool m_hardware = false;
    UINT32 m_nextSoftwareCandidate = 0;
    uint32_t m_needInput = 0;
    DWORD m_inputID = 0;
    DWORD m_outputID = 0;
};

std::unique_ptr<VideoDecoder> CreateMediaFoundationDecoder() {
    return std::make_unique<MediaFoundationDecoder>();
}

std::unique_ptr<VideoDecoder> CreateMediaFoundationDecoderForTesting(DecoderCandidateFactory factory) {
    return std::make_unique<MediaFoundationDecoder>(std::move(factory));
}

} // namespace thunderlink
