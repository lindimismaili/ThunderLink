#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "frame_header.h"

namespace thunderlink {

class PerformanceMonitor;

struct CompletedFrame {
    uint64_t frameId;
    uint32_t width;
    uint32_t height;
    uint32_t poolIndex;  /* Index into pre-allocated pool */
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
     * or timeout expires. Returns false on timeout. */
    bool WaitForFrame(const uint8_t** outData, uint32_t* outWidth, uint32_t* outHeight,
                      uint64_t* outFrameId, uint32_t timeoutMs);

    /* Called from render thread after the frame has been consumed (uploaded to GPU).
     * Returns the buffer slot back to the pool. */
    void ReleaseFrame(uint64_t frameId);

    /* Called periodically to drop incomplete frames older than maxAgeMs. */
    void EvictStaleFrames(uint32_t maxAgeMs);

private:
    struct FrameAssembly {
        uint64_t frameId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t totalPackets = 0;
        uint32_t receivedPackets = 0;
        uint32_t poolIndex = 0;
        std::vector<bool> packetReceived;
        std::chrono::steady_clock::time_point startTime;

        bool IsComplete() const { return receivedPackets == totalPackets; }
    };

    /* Pre-allocated frame data pool */
    uint32_t m_poolSize;
    uint32_t m_maxFrameBytes;
    std::vector<uint8_t> m_pool;             /* Contiguous block: poolSize * maxFrameBytes */
    std::vector<bool> m_poolInUse;

    /* In-progress frames being assembled */
    std::mutex m_assemblyMutex;
    std::unordered_map<uint64_t, FrameAssembly> m_pendingFrames;

    /* Completed frames ready for rendering */
    std::mutex m_completedMutex;
    std::condition_variable m_completedCV;
    std::queue<CompletedFrame> m_completedFrames;

    /* Track which pool slots are held by the renderer */
    std::mutex m_releaseMutex;
    std::unordered_map<uint64_t, uint32_t> m_heldFrames; /* frameId -> poolIndex */

    /* Most recent frame ID we've completed (to skip old frames) */
    uint64_t m_latestCompletedFrameId = 0;

    PerformanceMonitor* m_perfMon;

    uint8_t* GetPoolSlot(uint32_t index);
    int32_t AllocatePoolSlot();
    void FreePoolSlot(uint32_t index);
};

} // namespace thunderlink
