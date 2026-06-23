#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>

#include "frame_header.h"

namespace thunderlink {

class PerformanceMonitor;

/* A reassembled video frame handed to the decoder.
 * For compressed formats `data`/`size` is one Annex-B access unit; for the
 * legacy BGRA32 mode it is the raw pixel buffer. */
struct ReceivedFrame {
    const uint8_t* data;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint64_t frameId;
    uint16_t format;   /* TlnkPixelFormat */
    uint16_t flags;    /* TlnkFrameFlags (TLNK_FLAG_KEY_FRAME) */
};

class FrameBuffer {
public:
    FrameBuffer(uint32_t poolSize, uint32_t maxWidth, uint32_t maxHeight,
                PerformanceMonitor* perfMon = nullptr);
    ~FrameBuffer();

    /* Called from network receive thread.
     * Copies packet payload into the correct position in the frame buffer. */
    void InsertPacket(const TlnkFrameHeader& header, const uint8_t* payload);

    /* Called from render thread. Blocks until a complete frame is available
     * or timeout expires. Returns false on timeout.
     *
     * Frames are returned in arrival order (oldest first) so the decoder's
     * inter-frame dependency chain stays intact — we never skip a P-frame. */
    bool WaitForFrame(ReceivedFrame* out, uint32_t timeoutMs);

    /* Called from render thread after the frame has been consumed (decoded).
     * Returns the buffer slot back to the pool. */
    void ReleaseFrame(uint64_t frameId);

    /* Called periodically to drop incomplete frames older than maxAgeMs. */
    void EvictStaleFrames(uint32_t maxAgeMs);

    /* Invoked whenever a frame is dropped/lost (incomplete eviction, pool
     * exhaustion, or an out-of-order skip). The owner wires this to a
     * (rate-limited) keyframe request so the stream can recover. */
    void SetOnFrameLost(std::function<void()> cb) { m_onFrameLost = std::move(cb); }

private:
    struct FrameAssembly {
        uint64_t frameId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t totalPackets = 0;
        uint32_t receivedPackets = 0;
        uint32_t frameTotalBytes = 0;
        uint16_t format = 0;
        uint16_t flags = 0;
        uint32_t poolIndex = 0;
        std::vector<bool> packetReceived;
        std::chrono::steady_clock::time_point startTime;

        bool IsComplete() const { return receivedPackets == totalPackets; }
    };

    struct CompletedFrame {
        uint64_t frameId;
        uint32_t width;
        uint32_t height;
        uint32_t size;     /* frameTotalBytes */
        uint16_t format;
        uint16_t flags;
        uint32_t poolIndex;
    };

    /* Pre-allocated frame data pool. Sized for the worst case (raw BGRA) so the
     * legacy test path still fits; compressed frames use a small fraction. */
    uint32_t m_poolSize;
    uint32_t m_maxFrameBytes;
    std::vector<uint8_t> m_pool;
    std::vector<bool> m_poolInUse;

    std::mutex m_assemblyMutex;
    std::unordered_map<uint64_t, FrameAssembly> m_pendingFrames;

    std::mutex m_completedMutex;
    std::condition_variable m_completedCV;
    std::queue<CompletedFrame> m_completedFrames;

    std::mutex m_releaseMutex;
    std::unordered_map<uint64_t, uint32_t> m_heldFrames;

    uint64_t m_latestCompletedFrameId = 0;

    PerformanceMonitor* m_perfMon;
    std::function<void()> m_onFrameLost;

    uint8_t* GetPoolSlot(uint32_t index);
    int32_t AllocatePoolSlot();
    void FreePoolSlot(uint32_t index);
    void NotifyLost();
};

} // namespace thunderlink
