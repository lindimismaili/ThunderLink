#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace thunderlink::cross {

// Tracks serialized ping/pong liveness without trusting an unsolicited or
// replayed pong. Times are supplied by the caller's monotonic clock.
class HeartbeatTracker {
public:
    static constexpr std::size_t MaxPending = 64;

    bool Sent(uint64_t sequence, uint64_t nowMs) {
        if (m_pending.size() >= MaxPending || m_pending.count(sequence)) return false;
        m_pending.emplace(sequence, PendingPing{nowMs, ++m_nextOrder});
        return true;
    }

    std::optional<uint64_t> Acknowledge(uint64_t sequence, uint64_t nowMs) {
        const auto acknowledged = m_pending.find(sequence);
        if (acknowledged == m_pending.end()) return std::nullopt;
        const uint64_t sentAt = acknowledged->second.sentAt;
        const uint64_t acknowledgedOrder = acknowledged->second.order;
        const uint64_t roundTrip = nowMs >= sentAt ? nowMs - sentAt : 0;
        for (auto pending = m_pending.begin(); pending != m_pending.end();) {
            if (pending->second.order <= acknowledgedOrder) pending = m_pending.erase(pending);
            else ++pending;
        }
        return roundTrip;
    }

    bool Expired(uint64_t nowMs, uint64_t timeoutMs) const {
        for (const auto& pending : m_pending) {
            if (nowMs >= pending.second.sentAt &&
                nowMs - pending.second.sentAt >= timeoutMs) return true;
        }
        return false;
    }

    void Reset() { m_pending.clear(); m_nextOrder = 0; }
    std::size_t Pending() const { return m_pending.size(); }

private:
    struct PendingPing {
        uint64_t sentAt;
        uint64_t order;
    };
    std::map<uint64_t, PendingPing> m_pending;
    uint64_t m_nextOrder = 0;
};

} // namespace thunderlink::cross
