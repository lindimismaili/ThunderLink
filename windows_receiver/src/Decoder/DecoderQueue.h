#pragma once
#include "VideoDecoder.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <chrono>

namespace thunderlink {
struct EncodedFrame {
    std::vector<uint8_t> bytes;
    uint64_t sessionId = 0;
    bool keyframe = false;
};

// Shared by the UI and decoder worker. An epoch also invalidates output already
// in flight when packet loss or overload flushes a stream within one session.
class DecoderQueue {
public:
    void Start(uint64_t session) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_session = session; m_stopped = false; m_waiting = true;
        m_flush = false; ++m_epoch; m_encoded.clear(); m_decoded.clear();
    }
    void Stop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopped = true; ++m_epoch; m_encoded.clear(); m_decoded.clear();
        m_cv.notify_all();
    }
    bool Submit(const uint8_t* data, uint32_t size, bool keyframe, uint64_t session) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped || !data || !size || size > uint64_t(TLNK_MAX_WIDTH) * TLNK_MAX_HEIGHT * 4 ||
            session != m_session || (m_waiting && !keyframe)) return false;
        if (m_encoded.size() >= 4) RequireKeyframeLocked();
        if (m_waiting && !keyframe) return false;
        try { m_encoded.push_back(EncodedFrame{{data, data + size}, session, keyframe}); }
        catch (const std::bad_alloc&) { RequireKeyframeLocked(); return false; }
        if (keyframe) m_waiting = false;
        m_cv.notify_one();
        return true;
    }
    bool Pop(EncodedFrame* frame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped || m_encoded.empty() || m_flush) return false;
        *frame = std::move(m_encoded.front()); m_encoded.pop_front(); return true;
    }
    void Publish(DecodedFrame frame, uint64_t epoch) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped || m_flush || epoch != m_epoch || frame.sessionId != m_session) return;
        while (m_decoded.size() >= 2) m_decoded.pop_front();
        m_decoded.push_back(std::move(frame));
    }
    bool TakeNewest(DecodedFrame* frame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!frame || m_stopped || m_decoded.empty()) return false;
        *frame = std::move(m_decoded.back()); m_decoded.clear();
        return frame->storage != nullptr;
    }
    void RequireKeyframe() {
        std::lock_guard<std::mutex> lock(m_mutex); RequireKeyframeLocked();
    }
    std::optional<uint64_t> TakeFlush() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_flush) return {};
        m_flush = false; return m_epoch;
    }
    uint64_t Epoch() const { std::lock_guard<std::mutex> lock(m_mutex); return m_epoch; }
    bool Waiting() const { std::lock_guard<std::mutex> lock(m_mutex); return m_waiting; }
    bool Stopped() const { std::lock_guard<std::mutex> lock(m_mutex); return m_stopped; }
    void Wait(bool canTakeInput, unsigned milliseconds) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] {
            return m_stopped || m_flush || (canTakeInput && !m_encoded.empty());
        });
    }
private:
    void RequireKeyframeLocked() {
        ++m_epoch; m_encoded.clear(); m_decoded.clear(); m_waiting = true; m_flush = true;
        m_cv.notify_one();
    }
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stopped = true, m_waiting = true, m_flush = false;
    uint64_t m_session = 0, m_epoch = 0;
    std::deque<EncodedFrame> m_encoded;
    std::deque<DecodedFrame> m_decoded;
};
}
