#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
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
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /* An injectable clock keeps rate-window tests deterministic. */
    explicit PerformanceMonitor(Clock clock = {});

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
    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint Now() const;
    void RefreshBandwidth() const;
    /* Atomic counters for cross-thread access */
    std::atomic<uint64_t> m_packetsReceived{0};
    std::atomic<uint64_t> m_bytesReceived{0};
    std::atomic<uint64_t> m_framesRendered{0};
    std::atomic<uint64_t> m_framesDropped{0};

    /* FPS tracking (render thread only) */
    mutable std::mutex m_fpsMutex;
    std::deque<TimePoint> m_frameTimes;

    /* Reassembly timing (network thread only) */
    std::atomic<float> m_avgReassemblyTimeUs{0.0f};

    /* Bandwidth tracking */
    mutable std::mutex m_rateMutex;
    Clock m_clock;
    mutable TimePoint m_bandwidthStart;
    mutable uint64_t m_bandwidthBytesSnapshot{0};
    mutable float m_cachedBandwidthMbps{0.0f};

    /* Periodic logging */
    TimePoint m_lastLogTime;

    float CalculateFPS() const;
};

} // namespace thunderlink
