#pragma once
#include "network_utils.h"
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>

namespace thunderlink {
using NetClock = std::chrono::steady_clock;
#ifdef _WIN32
using SocketLength = int;
#else
using SocketLength = socklen_t;
#endif

// select uses timeval on both platforms; SO_RCVTIMEO does not.
inline int SocketReady(tlnk_socket_t s, bool writing, int waitMs = 10) {
    fd_set set; FD_ZERO(&set); FD_SET(s, &set);
    timeval timeout{waitMs / 1000, (waitMs % 1000) * 1000};
    return select(static_cast<int>(s + 1), writing ? nullptr : &set,
                  writing ? &set : nullptr, nullptr, &timeout);
}

inline bool ConnectWithDeadline(tlnk_socket_t s, const sockaddr_in& addr,
                                const std::atomic<bool>& running, int timeoutMs = TLNK_CONNECT_TIMEOUT_MS) {
    if (tlnk_set_nonblocking(s) != 0) return false;
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) return true;
    if (!tlnk_would_block()) return false;
    const auto deadline = NetClock::now() + std::chrono::milliseconds(timeoutMs);
    while (running.load() && NetClock::now() < deadline) {
        int ready = SocketReady(s, true);
        if (ready < 0) return false;
        if (!ready) continue;
        int error = 0; SocketLength length = sizeof(error);
        return getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
    }
    return false;
}

inline std::vector<uint8_t> FrameJSON(const std::string& json) {
    if (json.empty() || json.size() > 65536) return {};
    const auto n = static_cast<uint32_t>(json.size());
    std::vector<uint8_t> bytes{uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
    bytes.insert(bytes.end(), json.begin(), json.end());
    return bytes;
}

// All writes and closure are owned by the channel worker. shouldContinue also
// cancels a send when its session changes, preventing replay after reconnect.
inline bool SendFramed(tlnk_socket_t s, const std::string& json,
                       const std::function<bool()>& shouldContinue, int timeoutMs = TLNK_WRITE_TIMEOUT_MS) {
    auto bytes = FrameJSON(json);
    if (bytes.empty()) return false;
    const auto deadline = NetClock::now() + std::chrono::milliseconds(timeoutMs);
    size_t sent = 0;
    while (sent < bytes.size() && shouldContinue() && NetClock::now() < deadline) {
        int ready = SocketReady(s, true);
        if (ready < 0) return false;
        if (!ready) continue;
        if (!shouldContinue() || NetClock::now() >= deadline) return false;
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        int n = static_cast<int>(send(s, reinterpret_cast<const char*>(bytes.data() + sent),
                                     static_cast<int>(bytes.size() - sent), flags));
        if (n > 0) sent += static_cast<size_t>(n);
        else if (n == 0 || !tlnk_would_block()) return false;
    }
    return sent == bytes.size();
}

enum class ReadStatus { Idle, Message, Closed, Malformed, IOError, TimedOut };

// Incremental framing retains partial prefixes/bodies across readiness polls.
class FramedReader {
public:
    using TimePoint = NetClock::time_point;
    ReadStatus Feed(const uint8_t* data, size_t size, TimePoint now = NetClock::now()) {
        if (m_bytes.empty() && size) m_started = now;
        if (size > 65540 - m_bytes.size()) return ReadStatus::Malformed;
        m_bytes.insert(m_bytes.end(), data, data + size);
        if (m_bytes.size() < 4) return ReadStatus::Idle;
        uint32_t length = (uint32_t(m_bytes[0]) << 24) | (uint32_t(m_bytes[1]) << 16) |
                          (uint32_t(m_bytes[2]) << 8) | uint32_t(m_bytes[3]);
        if (!length || length > 65536 || m_bytes.size() > size_t(length) + 4) return ReadStatus::Malformed;
        return m_bytes.size() == size_t(length) + 4 ? ReadStatus::Message : ReadStatus::Idle;
    }
    ReadStatus CheckDeadline(TimePoint now = NetClock::now()) const {
        return !m_bytes.empty() && now - m_started >= std::chrono::milliseconds(TLNK_WRITE_TIMEOUT_MS)
            ? ReadStatus::TimedOut : ReadStatus::Idle;
    }
    std::string TakeMessage() {
        std::string result(m_bytes.begin() + 4, m_bytes.end()); m_bytes.clear(); return result;
    }
    ReadStatus Poll(tlnk_socket_t s, std::string& message) {
        if (CheckDeadline() == ReadStatus::TimedOut) return ReadStatus::TimedOut;
        int ready = SocketReady(s, false);
        if (ready < 0) return ReadStatus::IOError;
        if (!ready) return ReadStatus::Idle;
        // Do not consume any bytes from the next message.
        size_t need = 4 - std::min<size_t>(4, m_bytes.size());
        if (!need) {
            uint32_t n = (uint32_t(m_bytes[0]) << 24) | (uint32_t(m_bytes[1]) << 16) |
                         (uint32_t(m_bytes[2]) << 8) | uint32_t(m_bytes[3]);
            need = size_t(n) + 4 - m_bytes.size();
        }
        std::array<uint8_t, 8192> chunk{};
        int n = static_cast<int>(recv(s, reinterpret_cast<char*>(chunk.data()),
                                     static_cast<int>(std::min(need, chunk.size())), 0));
        if (n == 0) return ReadStatus::Closed;
        if (n < 0) return tlnk_would_block() ? ReadStatus::Idle : ReadStatus::IOError;
        auto status = Feed(chunk.data(), size_t(n));
        if (status == ReadStatus::Message) message = TakeMessage();
        return status;
    }
private:
    std::vector<uint8_t> m_bytes;
    TimePoint m_started{};
};
} // namespace thunderlink
