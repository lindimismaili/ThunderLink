#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include "frame_metadata.h"

namespace thunderlink {

class PerformanceMonitor;

/* A reassembled video frame handed to the decoder. The slot remains owned by
 * FrameBuffer until ReleaseFrame(sessionId, frameId) is called. */
struct ReceivedFrame {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t sessionId = 0;
    uint64_t frameId = 0;
    uint16_t format = 0;
    uint16_t flags = 0;
};

class FrameBuffer {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Clock = std::function<TimePoint()>;

    FrameBuffer(uint32_t poolSize, uint32_t maxWidth, uint32_t maxHeight,
                PerformanceMonitor* perfMon = nullptr, Clock clock = Clock{});
    ~FrameBuffer() = default;

    /* A session change discards queued/incomplete frames from the old stream.
     * Frames already returned to a decoder retain their slots until their
     * session-qualified release arrives. */
    void SetSession(uint64_t sessionId, uint32_t width = 0, uint32_t height = 0,
                    uint16_t format = UINT16_MAX);
    uint64_t GetSession() const;

    /* Called by the UDP receive thread after DecodeFrameHeader has parsed the
     * complete datagram header. Returns false when the packet is invalid or
     * belongs to another session. */
    bool InsertPacket(const FrameMetadata& header, const uint8_t* payload);

    bool WaitForFrame(ReceivedFrame* out, uint32_t timeoutMs);
    void ReleaseFrame(uint64_t sessionId, uint64_t frameId);

    /* Called from the receiver's periodic clock tick, including while UDP is
     * idle. A zero argument uses the protocol's 50 ms deadline. */
    void EvictStaleFrames(uint32_t maxAgeMs = TLNK_FRAME_TIMEOUT_MS);

    /* The callback is copied while the state lock is held and invoked after it
     * is released, so it may enqueue control work without lock inversion. */
    void SetOnFrameLost(std::function<void()> cb);

private:
    struct FrameKey {
        uint64_t sessionId = 0;
        uint64_t frameId = 0;

        bool operator==(const FrameKey& other) const {
            return sessionId == other.sessionId && frameId == other.frameId;
        }
    };

    struct FrameKeyHash {
        size_t operator()(const FrameKey& key) const {
            return static_cast<size_t>(key.sessionId ^
                (key.frameId + 0x9e3779b97f4a7c15ULL + (key.sessionId << 6) +
                 (key.sessionId >> 2)));
        }
    };

    struct FrameAssembly {
        FrameMetadata metadata{};
        uint32_t receivedPackets = 0;
        uint32_t poolIndex = 0;
        std::vector<uint8_t> packetReceived;
        TimePoint startTime{};
    };

    struct CompletedFrame {
        FrameMetadata metadata{};
        uint32_t poolIndex = 0;
    };

    bool IsPacketValidLocked(const FrameMetadata& header, const uint8_t* payload) const;
    static bool SameFrameMetadata(const FrameMetadata& left,
                                  const FrameMetadata& right);
    uint8_t* GetPoolSlot(uint32_t index);
    int32_t AllocatePoolSlotLocked();
    void FreePoolSlotLocked(uint32_t index);
    std::function<void()> LossCallbackLocked() const;
    void RecordDropLocked();

    const uint32_t m_poolSize;
    const uint32_t m_maxWidth;
    const uint32_t m_maxHeight;
    const uint32_t m_maxFrameBytes;
    std::vector<uint8_t> m_pool;
    std::vector<uint8_t> m_poolInUse;

    mutable std::mutex m_mutex;
    std::condition_variable m_completedCV;
    uint64_t m_activeSessionId = 0;
    uint32_t m_expectedWidth = 0, m_expectedHeight = 0;
    uint16_t m_expectedFormat = UINT16_MAX;
    uint64_t m_latestCompletedFrameId = 0;
    std::unordered_map<uint64_t, FrameAssembly> m_pendingFrames;
    std::queue<CompletedFrame> m_completedFrames;
    std::unordered_map<FrameKey, uint32_t, FrameKeyHash> m_heldFrames;

    PerformanceMonitor* m_perfMon;
    Clock m_clock;
    std::function<void()> m_onFrameLost;
};

} // namespace thunderlink
