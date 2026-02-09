#include "PerformanceMonitor.h"
#include "Logger.h"

namespace thunderlink {

PerformanceMonitor::PerformanceMonitor() {
    auto now = std::chrono::steady_clock::now();
    m_bandwidthStart = now;
    m_lastLogTime = now;
}

void PerformanceMonitor::PacketReceived(uint32_t bytes) {
    m_packetsReceived.fetch_add(1, std::memory_order_relaxed);
    m_bytesReceived.fetch_add(bytes, std::memory_order_relaxed);
}

void PerformanceMonitor::FrameDropped() {
    m_framesDropped.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceMonitor::FrameAssembled(float assemblyTimeUs) {
    /* Simple exponential moving average */
    float prev = m_avgReassemblyTimeUs.load(std::memory_order_relaxed);
    float alpha = 0.1f;
    m_avgReassemblyTimeUs.store(prev * (1.0f - alpha) + assemblyTimeUs * alpha,
                                 std::memory_order_relaxed);
}

void PerformanceMonitor::FrameRendered() {
    m_framesRendered.fetch_add(1, std::memory_order_relaxed);

    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_fpsMutex);
    m_frameTimes.push_back(now);

    /* Keep last 120 samples for FPS calculation */
    while (m_frameTimes.size() > 120) {
        m_frameTimes.pop_front();
    }
}

float PerformanceMonitor::CalculateFPS() const {
    std::lock_guard<std::mutex> lock(m_fpsMutex);
    if (m_frameTimes.size() < 2) return 0.0f;

    auto duration = m_frameTimes.back() - m_frameTimes.front();
    float seconds = std::chrono::duration<float>(duration).count();
    if (seconds <= 0.0f) return 0.0f;

    return static_cast<float>(m_frameTimes.size() - 1) / seconds;
}

float PerformanceMonitor::CalculateBandwidth() {
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - m_bandwidthStart).count();

    if (elapsed < 0.001f) return 0.0f;

    uint64_t currentBytes = m_bytesReceived.load(std::memory_order_relaxed);
    uint64_t deltaBytes = currentBytes - m_bandwidthBytesSnapshot;

    float mbps = (static_cast<float>(deltaBytes) * 8.0f) / (elapsed * 1000000.0f);

    m_bandwidthBytesSnapshot = currentBytes;
    m_bandwidthStart = now;

    return mbps;
}

PerformanceStats PerformanceMonitor::GetStats() const {
    PerformanceStats stats{};
    stats.fps = CalculateFPS();
    stats.frameLatencyMs = 0.0f; /* Requires timestamp in frame header - future work */
    stats.bandwidthMbps = 0.0f;  /* Updated in MaybeLogStats */
    stats.framesRendered = m_framesRendered.load(std::memory_order_relaxed);
    stats.framesDropped = m_framesDropped.load(std::memory_order_relaxed);
    stats.packetsReceived = m_packetsReceived.load(std::memory_order_relaxed);
    stats.reassemblyTimeUs = m_avgReassemblyTimeUs.load(std::memory_order_relaxed);
    return stats;
}

void PerformanceMonitor::MaybeLogStats() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - m_lastLogTime).count();
    if (elapsed < 1.0f) return;

    m_lastLogTime = now;

    float fps = CalculateFPS();
    float bw = CalculateBandwidth();
    uint64_t rendered = m_framesRendered.load(std::memory_order_relaxed);
    uint64_t dropped = m_framesDropped.load(std::memory_order_relaxed);
    uint64_t packets = m_packetsReceived.load(std::memory_order_relaxed);
    float reassembly = m_avgReassemblyTimeUs.load(std::memory_order_relaxed);

    LOG_INFO("Stats: FPS=%.1f  BW=%.1f Mbps  Rendered=%llu  Dropped=%llu  "
             "Packets=%llu  Reassembly=%.0f us",
             fps, bw, rendered, dropped, packets, reassembly);
}

void PerformanceMonitor::Reset() {
    m_packetsReceived.store(0, std::memory_order_relaxed);
    m_bytesReceived.store(0, std::memory_order_relaxed);
    m_framesRendered.store(0, std::memory_order_relaxed);
    m_framesDropped.store(0, std::memory_order_relaxed);
    m_avgReassemblyTimeUs.store(0.0f, std::memory_order_relaxed);
    m_bandwidthBytesSnapshot = 0;

    auto now = std::chrono::steady_clock::now();
    m_bandwidthStart = now;
    m_lastLogTime = now;

    std::lock_guard<std::mutex> lock(m_fpsMutex);
    m_frameTimes.clear();
}

} // namespace thunderlink
