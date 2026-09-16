#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace thunderlink::cross {

// The same bounded, incremental decoder is used by the Windows TLS transport
// and portable tests. It decodes lengths only; JSON is validated by the caller.
class JsonFramer {
public:
    static constexpr std::size_t MaxJson = 65536;
    static constexpr uint64_t PartialDeadlineMs = 5000;

    bool CheckDeadline(uint64_t nowMs, std::string& error) const {
        if (!m_bytes.empty() && nowMs >= m_partialSince &&
            nowMs - m_partialSince >= PartialDeadlineMs) {
            error = "Cross received an incomplete frame that timed out";
            return false;
        }
        return true;
    }

    bool Append(const unsigned char* bytes, std::size_t size,
                std::vector<std::string>& messages, std::string& error, uint64_t nowMs) {
        if (!CheckDeadline(nowMs, error)) return false;
        constexpr std::size_t capacity = 2 * (MaxJson + 4);
        if (size > capacity - m_bytes.size()) {
            error = "Cross received an oversized JSON frame buffer";
            return false;
        }
        if (!size) return true;
        if (m_bytes.empty()) m_partialSince = nowMs;
        m_bytes.insert(m_bytes.end(), bytes, bytes + size);
        while (m_bytes.size() >= 4) {
            const uint32_t length = (uint32_t(m_bytes[0]) << 24) |
                (uint32_t(m_bytes[1]) << 16) | (uint32_t(m_bytes[2]) << 8) | uint32_t(m_bytes[3]);
            if (!length || length > MaxJson) {
                error = "Cross received an invalid JSON frame length";
                return false;
            }
            if (m_bytes.size() < std::size_t(length) + 4) return true;
            messages.emplace_back(m_bytes.begin() + 4, m_bytes.begin() + 4 + length);
            m_bytes.erase(m_bytes.begin(), m_bytes.begin() + 4 + length);
            // Progress on an earlier frame must not leave its deadline attached
            // to the next frame in the stream.
            m_partialSince = nowMs;
        }
        return true;
    }

private:
    std::vector<unsigned char> m_bytes;
    uint64_t m_partialSince = 0;
};

} // namespace thunderlink::cross
