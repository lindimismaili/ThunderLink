#pragma once

#include <cstdint>
#include <memory>

#include "protocol.h"

namespace thunderlink {

/* One decoded frame as tightly-packed NV12 in system memory.
 *  - yPlane:  width x height,           yStride bytes/row  (== width)
 *  - uvPlane: (width/2) x (height/2),   uvStride bytes/row (== width, interleaved CbCr)
 * Pointers are owned by the decoder and valid until the next Decode() call. */
struct DecodedFrame {
    const uint8_t* yPlane = nullptr;
    const uint8_t* uvPlane = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t yStride = 0;
    uint32_t uvStride = 0;
};

/* Hardware video decoder abstraction. The Media Foundation backend is the
 * default; a zero-copy D3D12 Video backend could implement the same interface
 * later. */
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    /* (Re)initialize for a codec + nominal frame size. */
    virtual bool Initialize(TlnkCodec codec, uint32_t width, uint32_t height) = 0;

    /* Decode one Annex-B access unit. Returns true and fills `out` if a frame
     * was produced this call (decoders may buffer; with no B-frames this is
     * 1-in/1-out in steady state). Returns false on "need more input" or error. */
    virtual bool Decode(const uint8_t* data, uint32_t size, bool isKeyframe,
                        DecodedFrame* out) = 0;

    virtual void Shutdown() = 0;

    /* True if a previous error/loss means we're discarding frames until the
     * next keyframe arrives. */
    virtual bool IsWaitingForKeyframe() const = 0;

    /* Mark that a gap/loss occurred: drop non-keyframe input until the next IDR. */
    virtual void RequireKeyframe() = 0;
};

std::unique_ptr<VideoDecoder> CreateMediaFoundationDecoder();

} // namespace thunderlink
