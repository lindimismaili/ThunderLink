#include "Decoder/VideoDecoder.h"
#include "Utils/Logger.h"
#include <windows.h>
#include <mfapi.h>
#include <cassert>
#include <chrono>
#include <thread>
using namespace thunderlink;
using Clock = std::chrono::steady_clock;
int main() {
    Logger::SetLevel(LogLevel::Fatal);
    assert(SUCCEEDED(MFStartup(MF_VERSION)));
    for (const auto codec : {TLNK_CODEC_H264, TLNK_CODEC_HEVC}) {
        auto decoder = CreateMediaFoundationDecoder();
        assert(!decoder->Initialize(codec, 1921, 1080, 60, 1));
        assert(!decoder->Initialize(codec, 1920, 1080, 0, 1));
        const auto began = Clock::now();
        assert(decoder->Initialize(codec, 1920, 1080, 60, 1));
        assert(Clock::now() - began < std::chrono::milliseconds(500));
        const uint8_t invalid[] = {0, 0, 0, 1, 0};
        assert(!decoder->Submit(invalid, sizeof(invalid), true, 2));
        assert(!decoder->Submit(invalid, sizeof(invalid), false, 1));
        auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!decoder->IsReady() && decoder->GetError().empty() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(decoder->IsReady() || !decoder->GetError().empty());
        if (decoder->IsReady()) {
            const auto backend = decoder->GetBackendName();
            assert(backend.find("hardware") == 0 || backend.find("software") == 0);
            for (unsigned i = 0; i < 20; ++i)
                decoder->Submit(invalid, sizeof(invalid), i == 0, 1);
            decoder->RequireKeyframe();
            assert(decoder->IsWaitingForKeyframe());
        }
        decoder->Shutdown();
        DecodedFrame frame;
        assert(!decoder->TryGetFrame(&frame));
        assert(decoder->Initialize(codec, 1280, 720, 120, 2));
        assert(!decoder->Submit(invalid, sizeof(invalid), true, 1));
        decoder->Shutdown();
    }
    MFShutdown();
}
