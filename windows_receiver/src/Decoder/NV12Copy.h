#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "protocol.h"

namespace thunderlink {
// NV12 is top-down. Validate the entire source layout before reading any row.
inline bool CopyNV12Rows(const uint8_t* source, size_t length, uint32_t width,
                        uint32_t height, int64_t pitch, std::vector<uint8_t>& output) {
    if (!source || width < 2 || height < 2 || (width & 1) || (height & 1) ||
        width > TLNK_MAX_WIDTH || height > TLNK_MAX_HEIGHT || pitch < width ||
        pitch > UINT32_MAX) return false;
    const uint64_t rows = uint64_t(height) + height / 2;
    const uint64_t required = uint64_t(pitch) * rows;
    if (required > length) return false;
    output.resize(size_t(width) * size_t(rows));
    for (uint32_t row = 0; row < rows; ++row)
        std::memcpy(output.data() + size_t(row) * width,
                    source + size_t(row) * size_t(pitch), width);
    return true;
}
}
