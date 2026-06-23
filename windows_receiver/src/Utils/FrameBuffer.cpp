#include "FrameBuffer.h"
#include "PerformanceMonitor.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>

namespace thunderlink {

FrameBuffer::FrameBuffer(uint32_t poolSize, uint32_t maxWidth, uint32_t maxHeight,
                         PerformanceMonitor* perfMon)
    : m_poolSize(poolSize)
    , m_maxFrameBytes(maxWidth * maxHeight * 4) /* worst case = raw BGRA32 */
    , m_perfMon(perfMon)
{
    m_pool.resize(static_cast<size_t>(m_poolSize) * m_maxFrameBytes, 0);
    m_poolInUse.resize(m_poolSize, false);

    LOG_INFO("FrameBuffer: allocated %u slots, %u bytes each (%.1f MB total)",
             m_poolSize, m_maxFrameBytes,
             (float)(m_poolSize * m_maxFrameBytes) / (1024.0f * 1024.0f));
}

FrameBuffer::~FrameBuffer() {
}

uint8_t* FrameBuffer::GetPoolSlot(uint32_t index) {
    return m_pool.data() + static_cast<size_t>(index) * m_maxFrameBytes;
}

int32_t FrameBuffer::AllocatePoolSlot() {
    for (uint32_t i = 0; i < m_poolSize; i++) {
        if (!m_poolInUse[i]) {
            m_poolInUse[i] = true;
            return static_cast<int32_t>(i);
        }
    }
    return -1; /* No slots available */
}

void FrameBuffer::FreePoolSlot(uint32_t index) {
    if (index < m_poolSize) {
        m_poolInUse[index] = false;
    }
}

void FrameBuffer::NotifyLost() {
    if (m_onFrameLost) m_onFrameLost();
}

void FrameBuffer::InsertPacket(const TlnkFrameHeader& header, const uint8_t* payload) {
    if (header.payload_size == 0 || !payload) return;
    if (header.packet_seq >= header.total_packets) return;
    if (header.frame_total_bytes == 0 || header.frame_total_bytes > m_maxFrameBytes) return;

    auto startTime = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_assemblyMutex);

    /* Skip frames older than the most recently completed one */
    if (header.frame_id <= m_latestCompletedFrameId) return;

    auto it = m_pendingFrames.find(header.frame_id);

    if (it == m_pendingFrames.end()) {
        /* New frame - allocate a pool slot */
        int32_t slot = AllocatePoolSlot();
        if (slot < 0) {
            /* No slots available - drop the oldest pending frame to make room.
             * This is a real data loss: trigger keyframe recovery. */
            uint64_t oldestId = UINT64_MAX;
            for (auto& [id, assembly] : m_pendingFrames) {
                if (id < oldestId) oldestId = id;
            }
            if (oldestId != UINT64_MAX) {
                FreePoolSlot(m_pendingFrames[oldestId].poolIndex);
                m_pendingFrames.erase(oldestId);
                if (m_perfMon) m_perfMon->FrameDropped();
                NotifyLost();
                LOG_WARN("FrameBuffer: dropped frame %llu to free pool slot", oldestId);
            }
            slot = AllocatePoolSlot();
            if (slot < 0) return; /* Still no room, give up */
        }

        FrameAssembly assembly;
        assembly.frameId = header.frame_id;
        assembly.width = header.width;
        assembly.height = header.height;
        assembly.totalPackets = header.total_packets;
        assembly.receivedPackets = 0;
        assembly.frameTotalBytes = header.frame_total_bytes;
        assembly.format = header.format;
        assembly.flags = header.flags;
        assembly.poolIndex = static_cast<uint32_t>(slot);
        assembly.packetReceived.resize(header.total_packets, false);
        assembly.startTime = startTime;

        auto [insertIt, _] = m_pendingFrames.emplace(header.frame_id, std::move(assembly));
        it = insertIt;
    }

    FrameAssembly& frame = it->second;

    /* Skip duplicate packets */
    if (frame.packetReceived[header.packet_seq]) return;

    /* Copy payload into the correct offset in the pool buffer (contiguous
     * slicing of the encoded access unit / raw frame). */
    uint32_t offset = header.packet_seq * TLNK_MAX_PAYLOAD;
    if (offset + header.payload_size > frame.frameTotalBytes) {
        LOG_WARN("FrameBuffer: packet offset+size exceeds frame size, skipping");
        return;
    }
    if (offset + header.payload_size > m_maxFrameBytes) return;

    uint8_t* dst = GetPoolSlot(frame.poolIndex) + offset;
    std::memcpy(dst, payload, header.payload_size);

    frame.packetReceived[header.packet_seq] = true;
    frame.receivedPackets++;

    /* Check if frame is complete */
    if (frame.IsComplete()) {
        auto elapsed = std::chrono::steady_clock::now() - frame.startTime;
        float assemblyUs = std::chrono::duration<float, std::micro>(elapsed).count();
        if (m_perfMon) m_perfMon->FrameAssembled(assemblyUs);

        m_latestCompletedFrameId = frame.frameId;

        CompletedFrame completed;
        completed.frameId = frame.frameId;
        completed.width = frame.width;
        completed.height = frame.height;
        completed.size = frame.frameTotalBytes;
        completed.format = frame.format;
        completed.flags = frame.flags;
        completed.poolIndex = frame.poolIndex;

        /* Remove from pending (but keep pool slot allocated - renderer will release) */
        m_pendingFrames.erase(it);

        /* Enqueue for the render thread */
        {
            std::lock_guard<std::mutex> cLock(m_completedMutex);
            m_completedFrames.push(completed);
        }
        m_completedCV.notify_one();

        /* Drop any pending frames older than the completed one. For inter-frame
         * video these are unrecoverable gaps, so request a keyframe. */
        std::vector<uint64_t> toRemove;
        for (auto& [id, assembly] : m_pendingFrames) {
            if (id < completed.frameId) {
                toRemove.push_back(id);
            }
        }
        for (uint64_t id : toRemove) {
            FreePoolSlot(m_pendingFrames[id].poolIndex);
            m_pendingFrames.erase(id);
            if (m_perfMon) m_perfMon->FrameDropped();
            NotifyLost();
        }
    }
}

bool FrameBuffer::WaitForFrame(ReceivedFrame* out, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(m_completedMutex);

    if (m_completedFrames.empty()) {
        m_completedCV.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [this] { return !m_completedFrames.empty(); });
    }

    if (m_completedFrames.empty()) return false;

    /* Return the OLDEST completed frame (FIFO). Unlike the old raw path we do
     * NOT skip to the latest, because compressed frames depend on their
     * predecessors — dropping one would corrupt everything until the next IDR.
     * Build-up is bounded by the small pool; if we ever fall behind, pool
     * exhaustion triggers keyframe recovery instead. */
    CompletedFrame frame = m_completedFrames.front();
    m_completedFrames.pop();

    out->data = GetPoolSlot(frame.poolIndex);
    out->size = frame.size;
    out->width = frame.width;
    out->height = frame.height;
    out->frameId = frame.frameId;
    out->format = frame.format;
    out->flags = frame.flags;

    /* Track that this frame's pool slot is held by the renderer */
    {
        std::lock_guard<std::mutex> rLock(m_releaseMutex);
        m_heldFrames[frame.frameId] = frame.poolIndex;
    }

    return true;
}

void FrameBuffer::ReleaseFrame(uint64_t frameId) {
    std::lock_guard<std::mutex> lock(m_releaseMutex);

    auto it = m_heldFrames.find(frameId);
    if (it != m_heldFrames.end()) {
        FreePoolSlot(it->second);
        m_heldFrames.erase(it);
    }
}

void FrameBuffer::EvictStaleFrames(uint32_t maxAgeMs) {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_assemblyMutex);

    std::vector<uint64_t> toRemove;
    for (auto& [id, assembly] : m_pendingFrames) {
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - assembly.startTime).count();
        if (age > maxAgeMs) {
            toRemove.push_back(id);
        }
    }

    for (uint64_t id : toRemove) {
        LOG_DEBUG("FrameBuffer: evicting stale frame %llu (%u/%u packets)",
                  id, m_pendingFrames[id].receivedPackets,
                  m_pendingFrames[id].totalPackets);
        FreePoolSlot(m_pendingFrames[id].poolIndex);
        m_pendingFrames.erase(id);
        if (m_perfMon) m_perfMon->FrameDropped();
        NotifyLost();  /* lost packets -> need an IDR to resume cleanly */
    }
}

} // namespace thunderlink
