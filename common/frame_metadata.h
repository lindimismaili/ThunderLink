#pragma once

/*
 * Safe C++ view of a UDP video header.  TlnkFrameHeader is deliberately
 * packed for the wire and must never be dereferenced from an arbitrary packet
 * buffer: on some architectures its u64 fields are unaligned.  Decode this
 * representation from bytes instead.
 */

#include <cstddef>
#include <cstdint>

#include "frame_header.h"

namespace thunderlink {

struct FrameMetadata {
    uint32_t magic = 0;
    uint64_t frameId = 0;
    uint32_t packetSeq = 0;
    uint32_t totalPackets = 0;
    uint32_t payloadSize = 0;
    uint32_t frameTotalBytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t format = 0;
    uint16_t flags = 0;
    uint64_t sessionId = 0;
};

inline uint16_t ReadLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t ReadLE64(const uint8_t* p) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return value;
}

inline bool DecodeFrameHeader(const uint8_t* bytes, size_t byteCount,
                              FrameMetadata* out) {
    if (bytes == nullptr || out == nullptr || byteCount < TLNK_HEADER_SIZE) {
        return false;
    }

    FrameMetadata decoded{};
    decoded.magic = ReadLE32(bytes + 0);
    decoded.frameId = ReadLE64(bytes + 4);
    decoded.packetSeq = ReadLE32(bytes + 12);
    decoded.totalPackets = ReadLE32(bytes + 16);
    decoded.payloadSize = ReadLE32(bytes + 20);
    decoded.frameTotalBytes = ReadLE32(bytes + 24);
    decoded.width = ReadLE32(bytes + 28);
    decoded.height = ReadLE32(bytes + 32);
    decoded.format = ReadLE16(bytes + 36);
    decoded.flags = ReadLE16(bytes + 38);
    decoded.sessionId = ReadLE64(bytes + 40);
    *out = decoded;
    return true;
}

} // namespace thunderlink
