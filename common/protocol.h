#pragma once

/*
 * ThunderLink Protocol Definitions
 * Shared constants and types for Mac sender and Windows receiver.
 */

#include <stdint.h>

/* Protocol identification */
#define TLNK_MAGIC          0x544C4E4B  /* "TLNK" in ASCII */
#define TLNK_VERSION        4           /* v4: independent, authenticated Cross sessions */

/* Network configuration.
 * These are DEFAULTS for a direct link-local link; both sides accept overrides
 * (peer/bind IP + ports) via command-line arguments. See AppConfig (Windows) and
 * RuntimeConfig (Mac). */
#define TLNK_MAC_IP         "169.254.1.1"
#define TLNK_WIN_IP         "169.254.1.2"
#define TLNK_PORT_VIDEO     50001   /* UDP - video frames */
#define TLNK_PORT_INPUT     50002   /* TCP - input events */
#define TLNK_PORT_CONTROL   50003   /* TCP - control messages */
#define TLNK_PORT_CROSS     50004   /* TLS TCP - Cross control and ordered input */
#define TLNK_CROSS_SERVICE  "_thunderlink-cross._tcp"
#define TLNK_CROSS_LEASE_MS 1000
#define TLNK_CROSS_PING_MS  250
#define TLNK_CROSS_QUEUE_LIMIT 256
#define TLNK_CROSS_MAX_DISPLAYS 32

/* Packet limits.
 * Sized for the STANDARD 1500-byte Ethernet MTU (jumbo frames are usually OFF
 * on USB-C->Ethernet adapters). Max UDP payload over 1500 MTU = 1472 bytes
 * (1500 - 20 IP - 8 UDP); minus the 48-byte TlnkFrameHeader leaves 1424, so a
 * 1400-byte chunk has margin. Oversized datagrams would be silently dropped by
 * a non-jumbo link, which manifests as a black screen while TCP control/input
 * (small packets) still work. */
#define TLNK_MTU            1500    /* Standard Ethernet MTU */
#define TLNK_HEADER_SIZE    48      /* sizeof(TlnkFrameHeader); enforced by _Static_assert */
#define TLNK_MAX_PAYLOAD    1400    /* Payload per UDP packet (fits 1500 MTU with the header) */

/* Socket buffer sizes */
#define TLNK_RECV_BUFFER_SIZE  (4 * 1024 * 1024)  /* 4 MB */
#define TLNK_SEND_BUFFER_SIZE  (4 * 1024 * 1024)  /* 4 MB */

/* Frame buffer pool */
#define TLNK_FRAME_POOL_SIZE   4
#define TLNK_MAX_WIDTH         3840
#define TLNK_MAX_HEIGHT        2160
#define TLNK_MIN_FPS            1
#define TLNK_MAX_FPS            240
#define TLNK_MIN_BITRATE_KBPS   1000
#define TLNK_MAX_BITRATE_KBPS   200000

/* Timeouts */
#define TLNK_FRAME_TIMEOUT_MS       50   /* Drop incomplete frames after this */
#define TLNK_RECONNECT_INTERVAL_MS  1000
#define TLNK_PING_INTERVAL_MS       1000
#define TLNK_CONNECT_TIMEOUT_MS      2000
#define TLNK_WRITE_TIMEOUT_MS        2000
#define TLNK_HANDSHAKE_TIMEOUT_MS    5000
#define TLNK_PEER_TIMEOUT_MS         5000

/* Video payload formats (carried in TlnkFrameHeader.format).
 * BGRA32 is the legacy uncompressed mode kept for testing/regression; the
 * normal path is a hardware-encoded H.264/HEVC Annex-B access unit. */
typedef enum {
    TLNK_FORMAT_BGRA32 = 0,  /* raw uncompressed BGRA (legacy/test) */
    TLNK_FORMAT_H264   = 1,  /* H.264 Annex-B access unit */
    TLNK_FORMAT_HEVC   = 2   /* HEVC (H.265) Annex-B access unit */
} TlnkPixelFormat;

/* Frame flags (bitmask) */
typedef enum {
    TLNK_FLAG_NONE      = 0x0000,
    TLNK_FLAG_KEY_FRAME = 0x0001,
    TLNK_FLAG_CURSOR    = 0x0002
} TlnkFrameFlags;

/* Control message types */
typedef enum {
    TLNK_CTRL_HELLO           = 1,
    TLNK_CTRL_START           = 2,
    TLNK_CTRL_STOP            = 3,
    TLNK_CTRL_CONFIGURE       = 4,
    TLNK_CTRL_PING            = 5,
    TLNK_CTRL_PONG            = 6,
    TLNK_CTRL_BYE             = 7,
    TLNK_CTRL_REQUEST_KEYFRAME = 8  /* Win -> Mac: force next frame to be an IDR (loss recovery) */
} TlnkControlType;

/* Video codecs negotiated in the CONFIGURE/START handshake. */
typedef enum {
    TLNK_CODEC_H264 = 0,
    TLNK_CODEC_HEVC = 1
} TlnkCodec;

/* Resolution presets */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
} TlnkResolution;

static const TlnkResolution TLNK_RES_1080P_60  = {1920, 1080, 60};
static const TlnkResolution TLNK_RES_1440P_60  = {2560, 1440, 60};
static const TlnkResolution TLNK_RES_4K_60     = {3840, 2160, 60};
static const TlnkResolution TLNK_RES_1080P_120 = {1920, 1080, 120};
static const TlnkResolution TLNK_RES_4K_120    = {3840, 2160, 120};
