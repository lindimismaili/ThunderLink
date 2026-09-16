#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

namespace thunderlink {

/* One decoded frame as tightly-packed NV12 in system memory. The shared
 * storage keeps the pixels alive while a later frame is decoded on the worker
 * thread. */
struct DecodedFrame {
    std::shared_ptr<const std::vector<uint8_t>> storage;
    uint64_t sessionId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t yStride = 0;
    uint32_t uvStride = 0;
    std::size_t yOffset = 0;
    std::size_t uvOffset = 0;

    const uint8_t* YPlane() const {
        return storage && yOffset < storage->size() ? storage->data() + yOffset : nullptr;
    }
    const uint8_t* UVPlane() const {
        return storage && uvOffset < storage->size() ? storage->data() + uvOffset : nullptr;
    }
};

/* Hardware video decoder abstraction. The Media Foundation backend is the
 * default; a zero-copy D3D12 Video backend could implement the same interface
 * later. */
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    /* (Re)initialize for a codec and stream session. Media Foundation setup is
     * performed asynchronously on the decoder's COM-initialized worker. */
    virtual bool Initialize(TlnkCodec codec, uint32_t width, uint32_t height,
                            uint32_t fps = 60, uint64_t sessionId = 0) = 0;

    /* Queue one owned copy of an Annex-B access unit without blocking on the
     * decoder. Returns false for a stale session, invalid input, or overload. */
    virtual bool Submit(const uint8_t* data, uint32_t size, bool isKeyframe,
                        uint64_t sessionId) = 0;

    /* Take the newest available frame. Older decoded frames are discarded to
     * keep display latency bounded. */
    virtual bool TryGetFrame(DecodedFrame* out) = 0;

    virtual void Shutdown() = 0;

    /* True if a previous error/loss means we're discarding frames until the
     * next keyframe arrives. */
    virtual bool IsWaitingForKeyframe() const = 0;

    /* Mark that a gap/loss occurred: drop non-keyframe input until the next IDR. */
    virtual void RequireKeyframe() = 0;

    virtual bool IsReady() const = 0;
    virtual std::string GetBackendName() const = 0;
    virtual std::string GetError() const = 0;
};

std::unique_ptr<VideoDecoder> CreateMediaFoundationDecoder();

} // namespace thunderlink
