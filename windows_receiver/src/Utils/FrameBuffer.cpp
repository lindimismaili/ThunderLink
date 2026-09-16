#include "FrameBuffer.h"

#include "PerformanceMonitor.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace thunderlink {
namespace {

constexpr uint16_t kKnownFrameFlags = TLNK_FLAG_KEY_FRAME | TLNK_FLAG_CURSOR;

bool IsKnownFormat(uint16_t format) {
    return format == TLNK_FORMAT_BGRA32 || format == TLNK_FORMAT_H264 ||
           format == TLNK_FORMAT_HEVC;
}

} // namespace

FrameBuffer::FrameBuffer(uint32_t poolSize, uint32_t maxWidth, uint32_t maxHeight,
                         PerformanceMonitor* perfMon, Clock clock)
    : m_poolSize(poolSize)
    , m_maxWidth(maxWidth)
    , m_maxHeight(maxHeight)
    , m_maxFrameBytes(static_cast<uint32_t>(
        static_cast<uint64_t>(maxWidth) * maxHeight * 4U))
    , m_perfMon(perfMon)
    , m_clock(clock ? std::move(clock) : [] { return std::chrono::steady_clock::now(); })
{
    if (maxWidth == 0 || maxHeight == 0 ||
        static_cast<uint64_t>(maxWidth) * maxHeight * 4U >
            std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("invalid FrameBuffer dimensions");
    }

    const uint64_t poolBytes = static_cast<uint64_t>(m_poolSize) * m_maxFrameBytes;
    if (poolBytes > std::numeric_limits<size_t>::max()) {
        throw std::invalid_argument("FrameBuffer pool is too large");
    }
    m_pool.resize(static_cast<size_t>(poolBytes), 0);
    m_poolInUse.resize(m_poolSize, 0);

    LOG_INFO("FrameBuffer: allocated %u slots, %u bytes each (%.1f MB total)",
             m_poolSize, m_maxFrameBytes,
             static_cast<float>(poolBytes) / (1024.0f * 1024.0f));
}

void FrameBuffer::SetSession(uint64_t sessionId, uint32_t width, uint32_t height, uint16_t format) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sessionId == m_activeSessionId) return;

    for (const auto& pair : m_pendingFrames) {
        FreePoolSlotLocked(pair.second.poolIndex);
    }
    m_pendingFrames.clear();
    while (!m_completedFrames.empty()) {
        FreePoolSlotLocked(m_completedFrames.front().poolIndex);
        m_completedFrames.pop();
    }
    m_activeSessionId = sessionId;
    m_expectedWidth = width;
    m_expectedHeight = height;
    m_expectedFormat = format;
    m_latestCompletedFrameId = 0;
    m_completedCV.notify_all();
}

uint64_t FrameBuffer::GetSession() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeSessionId;
}

bool FrameBuffer::IsPacketValidLocked(const FrameMetadata& header,
                                      const uint8_t* payload) const {
    if (payload == nullptr || header.magic != TLNK_MAGIC || header.frameId == 0 ||
        header.sessionId == 0 || header.sessionId != m_activeSessionId ||
        header.width == 0 || header.height == 0 || header.width > m_maxWidth ||
        header.height > m_maxHeight || !IsKnownFormat(header.format) ||
        (header.flags & ~kKnownFrameFlags) != 0 || header.frameTotalBytes == 0 ||
        header.frameTotalBytes > m_maxFrameBytes || header.totalPackets == 0 ||
        header.payloadSize == 0 || header.payloadSize > TLNK_MAX_PAYLOAD ||
        header.packetSeq >= header.totalPackets) {
        return false;
    }

    if ((m_expectedWidth && header.width != m_expectedWidth) ||
        (m_expectedHeight && header.height != m_expectedHeight) ||
        (m_expectedFormat != UINT16_MAX && header.format != m_expectedFormat)) return false;

    const uint64_t requiredPackets =
        (static_cast<uint64_t>(header.frameTotalBytes) + TLNK_MAX_PAYLOAD - 1) /
        TLNK_MAX_PAYLOAD;
    if (requiredPackets != header.totalPackets) return false;

    const uint64_t offset = static_cast<uint64_t>(header.packetSeq) * TLNK_MAX_PAYLOAD;
    const uint64_t expectedPayload =
        (header.packetSeq + 1U == header.totalPackets)
            ? static_cast<uint64_t>(header.frameTotalBytes) - offset
            : TLNK_MAX_PAYLOAD;
    if (expectedPayload == 0 || header.payloadSize != expectedPayload ||
        offset + header.payloadSize > header.frameTotalBytes) {
        return false;
    }

    if (header.format == TLNK_FORMAT_BGRA32) {
        const uint64_t rawBytes = static_cast<uint64_t>(header.width) * header.height * 4U;
        if (rawBytes != header.frameTotalBytes) return false;
    } else if (header.width < 2 || header.height < 2 ||
               (header.width & 1U) != 0 || (header.height & 1U) != 0) {
        /* Media Foundation's negotiated NV12 output has even dimensions. */
        return false;
    }
    return true;
}

bool FrameBuffer::SameFrameMetadata(const FrameMetadata& left,
                                    const FrameMetadata& right) {
    return left.magic == right.magic && left.frameId == right.frameId &&
           left.totalPackets == right.totalPackets &&
           left.frameTotalBytes == right.frameTotalBytes && left.width == right.width &&
           left.height == right.height && left.format == right.format &&
           left.flags == right.flags && left.sessionId == right.sessionId;
}

uint8_t* FrameBuffer::GetPoolSlot(uint32_t index) {
    return m_pool.data() + static_cast<size_t>(index) * m_maxFrameBytes;
}

int32_t FrameBuffer::AllocatePoolSlotLocked() {
    for (uint32_t i = 0; i < m_poolSize; ++i) {
        if (m_poolInUse[i] == 0) {
            m_poolInUse[i] = 1;
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void FrameBuffer::FreePoolSlotLocked(uint32_t index) {
    if (index < m_poolSize) m_poolInUse[index] = 0;
}

std::function<void()> FrameBuffer::LossCallbackLocked() const {
    return m_onFrameLost;
}

void FrameBuffer::RecordDropLocked() {
    if (m_perfMon) m_perfMon->FrameDropped();
}

bool FrameBuffer::InsertPacket(const FrameMetadata& header, const uint8_t* payload) {
    std::function<void()> onLost;
    uint32_t losses = 0;
    bool accepted = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsPacketValidLocked(header, payload) ||
            header.frameId <= m_latestCompletedFrameId) {
            return false;
        }

        auto found = m_pendingFrames.find(header.frameId);
        if (found == m_pendingFrames.end()) {
            int32_t slot = AllocatePoolSlotLocked();
            if (slot < 0 && !m_pendingFrames.empty()) {
                auto oldest = std::min_element(m_pendingFrames.begin(), m_pendingFrames.end(),
                    [](const auto& left, const auto& right) {
                        return left.second.startTime < right.second.startTime;
                    });
                FreePoolSlotLocked(oldest->second.poolIndex);
                m_pendingFrames.erase(oldest);
                RecordDropLocked();
                ++losses;
                slot = AllocatePoolSlotLocked();
            }
            if (slot < 0) {
                RecordDropLocked();
                ++losses;
                onLost = LossCallbackLocked();
            } else {
                FrameAssembly assembly{};
                assembly.metadata = header;
                assembly.poolIndex = static_cast<uint32_t>(slot);
                assembly.packetReceived.resize(header.totalPackets, 0);
                assembly.startTime = m_clock();
                found = m_pendingFrames.emplace(header.frameId, std::move(assembly)).first;
            }
        }

        if (found != m_pendingFrames.end()) {
            FrameAssembly& frame = found->second;
            if (!SameFrameMetadata(frame.metadata, header)) return false;
            if (frame.packetReceived[header.packetSeq] != 0) return true;

            const uint64_t offset = static_cast<uint64_t>(header.packetSeq) * TLNK_MAX_PAYLOAD;
            std::memcpy(GetPoolSlot(frame.poolIndex) + offset, payload, header.payloadSize);
            frame.packetReceived[header.packetSeq] = 1;
            ++frame.receivedPackets;
            accepted = true;

            if (frame.receivedPackets == frame.metadata.totalPackets) {
                const auto elapsed = m_clock() - frame.startTime;
                if (m_perfMon) {
                    m_perfMon->FrameAssembled(
                        std::chrono::duration<float, std::micro>(elapsed).count());
                }
                m_latestCompletedFrameId = frame.metadata.frameId;
                m_completedFrames.push(CompletedFrame{frame.metadata, frame.poolIndex});
                m_pendingFrames.erase(found);
                m_completedCV.notify_one();

                for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end();) {
                    if (it->first < m_latestCompletedFrameId) {
                        FreePoolSlotLocked(it->second.poolIndex);
                        it = m_pendingFrames.erase(it);
                        RecordDropLocked();
                        ++losses;
                    } else {
                        ++it;
                    }
                }
            }
            if (losses != 0) onLost = LossCallbackLocked();
        }
    }

    while (losses-- != 0 && onLost) onLost();
    return accepted;
}

bool FrameBuffer::WaitForFrame(ReceivedFrame* out, uint32_t timeoutMs) {
    if (out == nullptr) return false;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_completedFrames.empty()) {
        m_completedCV.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
            return !m_completedFrames.empty();
        });
    }
    if (m_completedFrames.empty()) return false;

    CompletedFrame frame = m_completedFrames.front();
    m_completedFrames.pop();
    const FrameKey key{frame.metadata.sessionId, frame.metadata.frameId};
    m_heldFrames.emplace(key, frame.poolIndex);

    out->data = GetPoolSlot(frame.poolIndex);
    out->size = frame.metadata.frameTotalBytes;
    out->width = frame.metadata.width;
    out->height = frame.metadata.height;
    out->sessionId = frame.metadata.sessionId;
    out->frameId = frame.metadata.frameId;
    out->format = frame.metadata.format;
    out->flags = frame.metadata.flags;
    return true;
}

void FrameBuffer::ReleaseFrame(uint64_t sessionId, uint64_t frameId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const FrameKey key{sessionId, frameId};
    auto held = m_heldFrames.find(key);
    if (held == m_heldFrames.end()) return;
    FreePoolSlotLocked(held->second);
    m_heldFrames.erase(held);
}

void FrameBuffer::EvictStaleFrames(uint32_t maxAgeMs) {
    std::function<void()> onLost;
    uint32_t losses = 0;
    const TimePoint now = m_clock();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end();) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.startTime).count();
            if (age >= maxAgeMs) {
                LOG_DEBUG("FrameBuffer: evicting stale frame %llu (%u/%u packets)",
                          static_cast<unsigned long long>(it->first),
                          it->second.receivedPackets, it->second.metadata.totalPackets);
                FreePoolSlotLocked(it->second.poolIndex);
                it = m_pendingFrames.erase(it);
                RecordDropLocked();
                ++losses;
            } else {
                ++it;
            }
        }
        if (losses != 0) onLost = LossCallbackLocked();
    }
    while (losses-- != 0 && onLost) onLost();
}

void FrameBuffer::SetOnFrameLost(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_onFrameLost = std::move(cb);
}

} // namespace thunderlink
