#include "cross_heartbeat.h"

#include <cassert>
#include <iostream>

using thunderlink::cross::HeartbeatTracker;

int main() {
    HeartbeatTracker heartbeat;
    assert(heartbeat.Sent(1, 100));
    assert(heartbeat.Sent(2, 350));
    assert(heartbeat.Sent(3, 600));
    assert(!heartbeat.Expired(1599, 1500));
    assert(heartbeat.Expired(1600, 1500));

    // A pong for the newest transmitted ping proves all older serialized
    // pings made progress too, so they cannot cause a later false timeout.
    const auto latest = heartbeat.Acknowledge(3, 725);
    assert(latest && *latest == 125);
    assert(heartbeat.Pending() == 0);
    assert(!heartbeat.Expired(10'000, 1500));

    heartbeat.Reset();
    assert(heartbeat.Sent(20, 900));
    assert(heartbeat.Sent(21, 900));
    assert(heartbeat.Acknowledge(20, 950));
    assert(heartbeat.Pending() == 1); // equal clock ticks still retain newer sends

    heartbeat.Reset();
    assert(heartbeat.Sent(4, 1'000));
    assert(heartbeat.Sent(5, 1'250));
    // Unknown/stale pongs never renew liveness or discard a pending deadline.
    assert(!heartbeat.Acknowledge(999, 2'000));
    assert(heartbeat.Pending() == 2);
    assert(heartbeat.Expired(2'500, 1500));

    heartbeat.Reset();
    assert(heartbeat.Sent(6, 3'000));
    // The deadline still fires if ping production stops after an unanswered
    // ping, as it does when the controller fails closed on a blackholed link.
    assert(!heartbeat.Expired(4'499, 1500));
    assert(heartbeat.Expired(4'500, 1500));

    heartbeat.Reset();
    for (std::size_t index = 0; index < HeartbeatTracker::MaxPending; ++index)
        assert(heartbeat.Sent(index, index));
    assert(!heartbeat.Sent(HeartbeatTracker::MaxPending, HeartbeatTracker::MaxPending));
    assert(!heartbeat.Sent(0, 10'000));

    std::cout << "Cross heartbeat tests passed\n";
}
