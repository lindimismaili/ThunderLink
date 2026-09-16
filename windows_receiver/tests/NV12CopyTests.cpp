#include "Decoder/NV12Copy.h"
#include <cassert>
#include <limits>
using namespace thunderlink;
int main() {
    const uint8_t padded[] = {1,2,99,99,3,4,99,99,5,6,99,99};
    std::vector<uint8_t> output;
    assert(CopyNV12Rows(padded, sizeof(padded), 2, 2, 4, output));
    assert((output == std::vector<uint8_t>{1,2,3,4,5,6}));
    assert(!CopyNV12Rows(padded, 11, 2, 2, 4, output));
    assert(!CopyNV12Rows(padded, sizeof(padded), 2, 2, -4, output));
    assert(!CopyNV12Rows(padded, sizeof(padded), 2, 2, INT64_MIN, output));
    assert(!CopyNV12Rows(padded, sizeof(padded), 3, 2, 4, output));
    assert(!CopyNV12Rows(padded, sizeof(padded), 2, 2, UINT32_MAX, output));
    std::vector<uint8_t> maximum(size_t(TLNK_MAX_WIDTH) * TLNK_MAX_HEIGHT * 3 / 2, 42);
    assert(CopyNV12Rows(maximum.data(), maximum.size(), TLNK_MAX_WIDTH, TLNK_MAX_HEIGHT,
                        TLNK_MAX_WIDTH, output));
    assert(output == maximum);
}
