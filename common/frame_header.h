#pragma once

/*
 * ThunderLink Frame Packet Header
 * 40-byte binary header prepended to every UDP video packet.
 * Packed to match wire format exactly on both platforms (little-endian).
 *
 * For compressed video (H.264/HEVC) a "frame" is one encoded access unit. The
 * encoded bytes are sliced into <=TLNK_MAX_PAYLOAD contiguous chunks; chunk N
 * lives at byte offset (packet_seq * TLNK_MAX_PAYLOAD) within the access unit.
 * The receiver allocates/validates using frame_total_bytes rather than
 * width*height*4 (which only holds for the legacy BGRA32 mode).
 */

#include <stdint.h>
#include "protocol.h"

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;             /* TLNK_MAGIC (0x544C4E4B)                          off 0  */
    uint64_t frame_id;          /* Monotonically increasing frame counter           off 4  */
    uint32_t packet_seq;        /* Packet index within this frame (0-based)         off 12 */
    uint32_t total_packets;     /* Total number of packets for this frame           off 16 */
    uint32_t payload_size;      /* Bytes of payload in THIS packet (<=MAX_PAYLOAD)  off 20 */
    uint32_t frame_total_bytes; /* Total bytes of the whole frame/access unit       off 24 */
    uint32_t width;             /* Frame width in pixels                            off 28 */
    uint32_t height;            /* Frame height in pixels                           off 32 */
    uint16_t format;            /* TlnkPixelFormat (BGRA32 / H264 / HEVC)           off 36 */
    uint16_t flags;             /* TlnkFrameFlags bitmask                           off 38 */
} TlnkFrameHeader;              /*                                                  end 40 */
#pragma pack(pop)

/* Compile-time size verification */
#ifdef __cplusplus
static_assert(sizeof(TlnkFrameHeader) == TLNK_HEADER_SIZE,
              "TlnkFrameHeader must be exactly 40 bytes (TLNK_HEADER_SIZE)");
#else
_Static_assert(sizeof(TlnkFrameHeader) == TLNK_HEADER_SIZE,
               "TlnkFrameHeader must be exactly 40 bytes (TLNK_HEADER_SIZE)");
#endif
