#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

namespace thunderlink {

struct PerformanceStats {
    float fps;
    float frameLatencyMs;
    float bandwidthMbps;
    uint64_t framesRendered;
    uint64_t framesDropped;
    uint64_t packetsReceived;
    float reassemblyTimeUs;
};

class PerformanceMonitor {
public:
    PerformanceMonitor();

    /* Called from the network thread */
    void PacketReceived(uint32_t bytes);
    void FrameDropped();
    void FrameAssembled(float assemblyTimeUs);

    /* Called from the render thread */
    void FrameRendered();

    /* Thread-safe stats snapshot */
    PerformanceStats GetStats() const;

    /* Log stats periodically (call from main loop) */
    void MaybeLogStats();

    void Reset();

private:
    /* Atomic counters for cross-thread access */
    std::atomic<uint64_t> m_packetsReceived{0};
    std::atomic<uint64_t> m_bytesReceived{0};
    std::atomic<uint64_t> m_framesRendered{0};
    std::atomic<uint64_t> m_framesDropped{0};

    /* FPS tracking (render thread only) */
    mutable std::mutex m_fpsMutex;
    std::deque<std::chrono::steady_clock::time_point> m_frameTimes;

    /* Reassembly timing (network thread only) */
    std::atomic<float> m_avgReassemblyTimeUs{0.0f};

    /* Bandwidth tracking */
    std::chrono::steady_clock::time_point m_bandwidthStart;
    uint64_t m_bandwidthBytesSnapshot{0};

    /* Periodic logging */
    std::chrono::steady_clock::time_point m_lastLogTime;

    float CalculateFPS() const;
    float CalculateBandwidth();
};

} // namespace thunderlink
