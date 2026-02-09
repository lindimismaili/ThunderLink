#include "InputMapper.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>

namespace thunderlink {

void InputMapper::NormalizeMouseCoords(int pixelX, int pixelY,
                                        uint32_t windowWidth, uint32_t windowHeight,
                                        float& outX, float& outY) {
    if (windowWidth == 0 || windowHeight == 0) {
        outX = 0.0f;
        outY = 0.0f;
        return;
    }

    outX = static_cast<float>(pixelX) / static_cast<float>(windowWidth);
    outY = static_cast<float>(pixelY) / static_cast<float>(windowHeight);

    outX = std::clamp(outX, 0.0f, 1.0f);
    outY = std::clamp(outY, 0.0f, 1.0f);
}

uint8_t InputMapper::GetModifierState() {
    uint8_t mods = TLNK_MOD_NONE;

    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= TLNK_MOD_SHIFT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)  mods |= TLNK_MOD_CTRL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)     mods |= TLNK_MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000))   mods |= TLNK_MOD_META;

    return mods;
}

uint16_t InputMapper::MapKeyCode(uint16_t windowsVK) {
    /* Pass-through: send Windows VK codes directly.
     * The Mac side will map them to CGKeyCode values. */
    return windowsVK;
}

} // namespace thunderlink
