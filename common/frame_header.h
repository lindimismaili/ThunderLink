#pragma once

/*
 * ThunderLink Frame Packet Header
 * 32-byte binary header prepended to every UDP video packet.
 * Packed to match wire format exactly on both platforms.
 */

#include <stdint.h>
#include "protocol.h"

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          /* TLNK_MAGIC (0x544C4E4B) */
    uint64_t frame_id;       /* Monotonically increasing frame counter */
    uint32_t packet_seq;     /* Packet index within this frame (0-based) */
    uint32_t total_packets;  /* Total number of packets for this frame */
    uint32_t payload_size;   /* Bytes of payload in THIS packet (<=TLNK_MAX_PAYLOAD) */
    uint32_t width;          /* Frame width in pixels */
    uint32_t height;         /* Frame height in pixels */
    uint16_t format;         /* TlnkPixelFormat */
    uint16_t flags;          /* TlnkFrameFlags bitmask */
} TlnkFrameHeader;
#pragma pack(pop)

/* Compile-time size verification */
#ifdef __cplusplus
static_assert(sizeof(TlnkFrameHeader) == TLNK_HEADER_SIZE,
              "TlnkFrameHeader must be exactly 32 bytes");
#else
_Static_assert(sizeof(TlnkFrameHeader) == TLNK_HEADER_SIZE,
               "TlnkFrameHeader must be exactly 32 bytes");
#endif
