#pragma once

#include <cstdint>

#include "input_event.h"

namespace thunderlink {

class InputMapper {
public:
    /* Normalize mouse coordinates from pixel space to [0, 1] */
    static void NormalizeMouseCoords(int pixelX, int pixelY,
                                      uint32_t windowWidth, uint32_t windowHeight,
                                      float& outX, float& outY);

    /* Get current modifier key state as TlnkModifierFlags bitmask */
    static uint8_t GetModifierState();

    /* Map Windows virtual key code to the key code we send over the wire.
     * Currently pass-through (Mac side converts), but this provides
     * a central point for future remapping. */
    static uint16_t MapKeyCode(uint16_t windowsVK);
};

} // namespace thunderlink
