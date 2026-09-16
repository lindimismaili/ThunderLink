#include "Decoder/DecoderTestHooks.h"
#include "Utils/Logger.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
using namespace thunderlink;
using Microsoft::WRL::ComPtr;

struct Trace { std::atomic<int> rejected{0}, inputs{0}, shutdowns{0}, changes{0}; };

// A deterministic MFT: async candidates demand an unlock and emit real MF
// events. Negotiation failures and runtime failures exercise production fallback.
class FakeTransform final : public IMFTransform, public IMFMediaEventGenerator, public IMFShutdown {
public:
    FakeTransform(bool async, bool reject, bool failInput, bool change, TlnkCodec codec,
                  std::shared_ptr<Trace> trace)
        : m_async(async), m_reject(reject), m_failInput(failInput), m_change(change),
          m_codec(codec), m_trace(std::move(trace)) {
        assert(SUCCEEDED(MFCreateAttributes(&m_attributes, 4)));
        assert(SUCCEEDED(m_attributes->SetUINT32(MF_TRANSFORM_ASYNC, async)));
        assert(SUCCEEDED(MFCreateEventQueue(&m_events)));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IMFTransform)) *out = static_cast<IMFTransform*>(this);
        else if (iid == __uuidof(IMFMediaEventGenerator)) *out = static_cast<IMFMediaEventGenerator*>(this);
        else if (iid == __uuidof(IMFShutdown)) *out = static_cast<IMFShutdown*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --m_references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetStreamLimits(DWORD* a,DWORD* b,DWORD* c,DWORD* d) override { *a=*b=*c=*d=1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* in,DWORD* out) override { *in=*out=1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStreamIDs(DWORD,DWORD* in,DWORD,DWORD* out) override { *in=3; *out=7; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetInputStreamInfo(DWORD,MFT_INPUT_STREAM_INFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetOutputStreamInfo(DWORD id,MFT_OUTPUT_STREAM_INFO* info) override {
        assert(id==7); *info={}; info->dwFlags=MFT_OUTPUT_STREAM_PROVIDES_SAMPLES; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(IMFAttributes** out) override { return m_attributes.CopyTo(out); }
    HRESULT STDMETHODCALLTYPE GetInputStreamAttributes(DWORD,IMFAttributes**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetOutputStreamAttributes(DWORD,IMFAttributes**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeleteInputStream(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddInputStreams(DWORD,DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInputAvailableType(DWORD,DWORD,IMFMediaType**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetOutputAvailableType(DWORD id,DWORD index,IMFMediaType** out) override {
        assert(id==7); if (index) return MF_E_NO_MORE_TYPES;
        ComPtr<IMFMediaType> type;
        assert(SUCCEEDED(MFCreateMediaType(&type)));
        type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12);
        MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,m_changed ? 128 : 64,32);
        type->SetUINT32(MF_MT_DEFAULT_STRIDE,m_changed ? 128 : 64);
        return type.CopyTo(out);
    }
    HRESULT STDMETHODCALLTYPE SetInputType(DWORD id,IMFMediaType* type,DWORD) override {
        assert(id==3);
        if (m_async) { UINT32 unlocked=0; m_attributes->GetUINT32(MF_TRANSFORM_ASYNC_UNLOCK,&unlocked); assert(unlocked); }
        GUID subtype{}; assert(SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE,&subtype)));
        assert(subtype == (m_codec==TLNK_CODEC_HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264));
        if (m_reject) { ++m_trace->rejected; return MF_E_INVALIDMEDIATYPE; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetOutputType(DWORD id,IMFMediaType*,DWORD) override { assert(id==7); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetInputCurrentType(DWORD,IMFMediaType**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetOutputCurrentType(DWORD,IMFMediaType**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInputStatus(DWORD,DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetOutputStatus(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetOutputBounds(LONGLONG,LONGLONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessEvent(DWORD,IMFMediaEvent*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessMessage(MFT_MESSAGE_TYPE message,ULONG_PTR) override {
        if (message==MFT_MESSAGE_COMMAND_FLUSH) m_pending=false;
        if (m_async && message==MFT_MESSAGE_NOTIFY_START_OF_STREAM) Event(METransformNeedInput);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ProcessInput(DWORD id,IMFSample*,DWORD) override {
        assert(id==3); ++m_trace->inputs;
        if (m_failInput) return E_FAIL;
        m_pending=true;
        if (m_async) { Event(METransformHaveOutput); Event(METransformNeedInput); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ProcessOutput(DWORD,DWORD count,MFT_OUTPUT_DATA_BUFFER* output,DWORD* status) override {
        assert(count==1 && output->dwStreamID==7); *status=0;
        if (!m_pending) return MF_E_TRANSFORM_NEED_MORE_INPUT;
        if (m_change && !m_changed) {
            m_changed=true; ++m_trace->changes;
            if (m_async) Event(METransformHaveOutput);
            return MF_E_TRANSFORM_STREAM_CHANGE;
        }
        m_pending=false;
        ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buffer;
        const DWORD size=(m_changed ? 128 : 64)*32*3/2;
        assert(SUCCEEDED(MFCreateSample(&sample)));
        assert(SUCCEEDED(MFCreateMemoryBuffer(size,&buffer)));
        BYTE* bytes=nullptr; assert(SUCCEEDED(buffer->Lock(&bytes,nullptr,nullptr)));
        std::memset(bytes,42,size); buffer->Unlock(); buffer->SetCurrentLength(size);
        sample->AddBuffer(buffer.Get()); output->pSample=sample.Detach(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags,IMFMediaEvent** out) override { return m_events->GetEvent(flags,out); }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* cb,IUnknown* state) override { return m_events->BeginGetEvent(cb,state); }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* result,IMFMediaEvent** out) override { return m_events->EndGetEvent(result,out); }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType type,REFGUID guid,HRESULT status,const PROPVARIANT* value) override {
        return m_events->QueueEventParamVar(type,guid,status,value);
    }
    HRESULT STDMETHODCALLTYPE Shutdown() override { ++m_trace->shutdowns; return m_events->Shutdown(); }
    HRESULT STDMETHODCALLTYPE GetShutdownStatus(MFSHUTDOWN_STATUS* status) override { *status=MFSHUTDOWN_COMPLETED; return S_OK; }
private:
    void Event(MediaEventType type) { assert(SUCCEEDED(m_events->QueueEventParamVar(type,GUID_NULL,S_OK,nullptr))); }
    std::atomic<ULONG> m_references{1};
    bool m_async,m_reject,m_failInput,m_change,m_changed=false,m_pending=false;
    TlnkCodec m_codec;
    std::shared_ptr<Trace> m_trace;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFMediaEventQueue> m_events;
};

void Wait(const std::function<bool()>& predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main() {
    Logger::SetLevel(LogLevel::Fatal);
    assert(SUCCEEDED(MFStartup(MF_VERSION)));
    for (bool runtimeFailure : {false,true}) {
        const auto trace=std::make_shared<Trace>();
        auto decoder=CreateMediaFoundationDecoderForTesting([=](TlnkCodec codec,bool hardware) {
            assert(codec==TLNK_CODEC_HEVC); // Fallback must retain HEVC.
            std::vector<ComPtr<IMFTransform>> candidates;
            ComPtr<IMFTransform> rejected; rejected.Attach(new FakeTransform(hardware,true,false,false,codec,trace));
            candidates.push_back(rejected);
            if (!hardware && runtimeFailure) {
                ComPtr<IMFTransform> failsAtRuntime;
                failsAtRuntime.Attach(new FakeTransform(false,false,true,false,codec,trace));
                candidates.push_back(failsAtRuntime);
            }
            ComPtr<IMFTransform> good; good.Attach(new FakeTransform(hardware,false,hardware && runtimeFailure,true,codec,trace));
            candidates.push_back(good); return candidates;
        });
        assert(decoder->Initialize(TLNK_CODEC_HEVC,64,32,120,99));
        Wait([&]{return decoder->IsReady();});
        uint8_t key[]={0,0,0,1,1};
        assert(decoder->Submit(key,sizeof(key),true,99));
        if (runtimeFailure) {
            Wait([&]{return decoder->GetBackendName().find("software")==0 && decoder->IsWaitingForKeyframe();});
            assert(decoder->Submit(key,sizeof(key),true,99));
        }
        DecodedFrame frame;
        Wait([&]{
            if (decoder->TryGetFrame(&frame)) return true;
            if (decoder->IsWaitingForKeyframe()) decoder->Submit(key,sizeof(key),true,99);
            return false;
        });
        assert(frame.sessionId==99 && frame.width==128 && frame.height==32 && frame.YPlane()[0]==42);
        assert(trace->rejected >= 1 && trace->changes >= 1);
        if (runtimeFailure) assert(trace->inputs >= 3);
        decoder->Shutdown();
        assert(trace->shutdowns >= 1);
        assert(frame.YPlane()[0]==42); // Owned pixels outlive the transform.
    }
    MFShutdown();
}
