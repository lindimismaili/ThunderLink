#pragma once

/*
 * ThunderLink Protocol Definitions
 * Shared constants and types for Mac sender and Windows receiver.
 */

#include <stdint.h>

/* Protocol identification */
#define TLNK_MAGIC          0x544C4E4B  /* "TLNK" in ASCII */
#define TLNK_VERSION        1

/* Network configuration */
#define TLNK_MAC_IP         "169.254.1.1"
#define TLNK_WIN_IP         "169.254.1.2"
#define TLNK_PORT_VIDEO     50001   /* UDP - video frames */
#define TLNK_PORT_INPUT     50002   /* TCP - input events */
#define TLNK_PORT_CONTROL   50003   /* TCP - control messages */

/* Packet limits */
#define TLNK_MTU            9000    /* Jumbo frames */
#define TLNK_HEADER_SIZE    36      /* sizeof(TlnkFrameHeader); enforced by _Static_assert */
#define TLNK_MAX_PAYLOAD    8192    /* Payload per UDP packet */

/* Socket buffer sizes */
#define TLNK_RECV_BUFFER_SIZE  (4 * 1024 * 1024)  /* 4 MB */
#define TLNK_SEND_BUFFER_SIZE  (4 * 1024 * 1024)  /* 4 MB */

/* Frame buffer pool */
#define TLNK_FRAME_POOL_SIZE   4
#define TLNK_MAX_WIDTH         3840
#define TLNK_MAX_HEIGHT        2160

/* Timeouts */
#define TLNK_FRAME_TIMEOUT_MS       50   /* Drop incomplete frames after this */
#define TLNK_RECONNECT_INTERVAL_MS  1000
#define TLNK_PING_INTERVAL_MS       1000

/* Pixel formats */
typedef enum {
    TLNK_FORMAT_BGRA32 = 0
} TlnkPixelFormat;

/* Frame flags (bitmask) */
typedef enum {
    TLNK_FLAG_NONE      = 0x0000,
    TLNK_FLAG_KEY_FRAME = 0x0001,
    TLNK_FLAG_CURSOR    = 0x0002
} TlnkFrameFlags;

/* Control message types */
typedef enum {
    TLNK_CTRL_HELLO     = 1,
    TLNK_CTRL_START     = 2,
    TLNK_CTRL_STOP      = 3,
    TLNK_CTRL_CONFIGURE = 4,
    TLNK_CTRL_PING      = 5,
    TLNK_CTRL_PONG      = 6,
    TLNK_CTRL_BYE       = 7
} TlnkControlType;

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
