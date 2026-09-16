#include "Utils/PerformanceMonitor.h"

#include <cassert>
#include <chrono>
#include <cmath>

namespace {

using Clock = std::chrono::steady_clock;

void ExpectNear(float actual, float expected, float epsilon = 0.001f) {
    assert(std::fabs(actual - expected) <= epsilon);
}

void TestCachedBandwidthAndIdleDecay() {
    Clock::time_point now{};
    thunderlink::PerformanceMonitor monitor([&now] { return now; });

    monitor.PacketReceived(125000); // one megabit
    now += std::chrono::seconds(1);
    ExpectNear(monitor.GetStats().bandwidthMbps, 1.0f);

    /* A second sample window with no packets must replace, not preserve, the
     * prior rate. The overlay and periodic logging both use this same cache. */
    now += std::chrono::seconds(1);
    ExpectNear(monitor.GetStats().bandwidthMbps, 0.0f);
}

void TestFpsDecaysAfterPresentationStops() {
    Clock::time_point now{};
    thunderlink::PerformanceMonitor monitor([&now] { return now; });

    monitor.FrameRendered();
    now += std::chrono::milliseconds(100);
    monitor.FrameRendered();
    assert(monitor.GetStats().fps > 9.0f);

    now += std::chrono::seconds(1);
    ExpectNear(monitor.GetStats().fps, 0.0f);
}

void TestResetStartsANewSession() {
    Clock::time_point now{};
    thunderlink::PerformanceMonitor monitor([&now] { return now; });
    monitor.PacketReceived(1000);
    monitor.FrameRendered();
    monitor.FrameDropped();
    now += std::chrono::seconds(1);
    (void)monitor.GetStats();

    monitor.Reset();
    const thunderlink::PerformanceStats stats = monitor.GetStats();
    assert(stats.packetsReceived == 0);
    assert(stats.framesRendered == 0);
    assert(stats.framesDropped == 0);
    ExpectNear(stats.bandwidthMbps, 0.0f);
}

} // namespace

int main() {
    TestCachedBandwidthAndIdleDecay();
    TestFpsDecaysAfterPresentationStops();
    TestResetStartsANewSession();
    return 0;
}
