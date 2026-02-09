#pragma once

/*
 * ThunderLink Input Event Definitions
 * Structures and constants for mouse/keyboard/scroll events
 * sent from Windows receiver to Mac sender over TCP.
 */

#include <stdint.h>

/* Input event types */
typedef enum {
    TLNK_INPUT_MOUSE_MOVE   = 1,
    TLNK_INPUT_MOUSE_BUTTON = 2,
    TLNK_INPUT_KEY          = 3,
    TLNK_INPUT_SCROLL       = 4
} TlnkInputType;

/* Mouse buttons */
typedef enum {
    TLNK_BUTTON_LEFT   = 0,
    TLNK_BUTTON_RIGHT  = 1,
    TLNK_BUTTON_MIDDLE = 2
} TlnkMouseButton;

/* Button / key state */
typedef enum {
    TLNK_STATE_UP   = 0,
    TLNK_STATE_DOWN = 1
} TlnkButtonState;

/* Modifier key flags (bitmask, combinable) */
typedef enum {
    TLNK_MOD_NONE  = 0x00,
    TLNK_MOD_SHIFT = 0x01,
    TLNK_MOD_CTRL  = 0x02,
    TLNK_MOD_ALT   = 0x04,
    TLNK_MOD_META  = 0x08   /* Cmd on Mac, Win on Windows */
} TlnkModifierFlags;

/* JSON field names used in TCP serialization */
#define TLNK_JSON_TYPE       "type"
#define TLNK_JSON_TIMESTAMP  "timestamp"
#define TLNK_JSON_X          "x"
#define TLNK_JSON_Y          "y"
#define TLNK_JSON_BUTTON     "button"
#define TLNK_JSON_STATE      "state"
#define TLNK_JSON_KEY_CODE   "key_code"
#define TLNK_JSON_MODIFIERS  "modifiers"
#define TLNK_JSON_DELTA_X    "delta_x"
#define TLNK_JSON_DELTA_Y    "delta_y"

/* JSON type string values */
#define TLNK_JSON_TYPE_MOUSE_MOVE   "mouse_move"
#define TLNK_JSON_TYPE_MOUSE_BUTTON "mouse_button"
#define TLNK_JSON_TYPE_KEY          "key"
#define TLNK_JSON_TYPE_SCROLL       "scroll"

/* JSON state string values */
#define TLNK_JSON_STATE_DOWN "down"
#define TLNK_JSON_STATE_UP   "up"

/* JSON modifier string values */
#define TLNK_JSON_MOD_SHIFT "shift"
#define TLNK_JSON_MOD_CTRL  "ctrl"
#define TLNK_JSON_MOD_ALT   "alt"
#define TLNK_JSON_MOD_META  "meta"
