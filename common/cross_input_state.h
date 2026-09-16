#pragma once

// Portable Cross input-state helpers.  Windows hooks use these to preserve the
// exact modifier state attached to a physical key event; tests use the same
// parser and left/right modifier aggregation without injecting native input.

#include <cctype>
#include <cstdint>
#include <string>

#include "input_event.h"

namespace thunderlink::crossinput {

constexpr uint16_t VkShift = 0x10, VkControl = 0x11, VkMenu = 0x12;
constexpr uint16_t VkLShift = 0xA0, VkRShift = 0xA1;
constexpr uint16_t VkLControl = 0xA2, VkRControl = 0xA3;
constexpr uint16_t VkLMenu = 0xA4, VkRMenu = 0xA5;
constexpr uint16_t VkLWin = 0x5B, VkRWin = 0x5C;
constexpr uint16_t VkEscape = 0x1B, VkF1 = 0x70;

struct EmergencyShortcut {
    uint8_t modifiers = TLNK_MOD_CTRL | TLNK_MOD_ALT | TLNK_MOD_SHIFT;
    uint16_t key = VkF1 + 11; // F12
};

inline std::string Uppercase(std::string value) {
    for (char& character : value)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return value;
}

inline bool ParseEmergencyShortcut(const std::string& value, EmergencyShortcut& shortcut) {
    uint8_t modifiers = 0;
    uint16_t key = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('+', start);
        const auto part = Uppercase(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (part == "CTRL" || part == "CONTROL") modifiers |= TLNK_MOD_CTRL;
        else if (part == "ALT") modifiers |= TLNK_MOD_ALT;
        else if (part == "SHIFT") modifiers |= TLNK_MOD_SHIFT;
        else if (part == "WIN" || part == "WINDOWS" || part == "META") modifiers |= TLNK_MOD_META;
        else {
            uint16_t candidate = 0;
            if (part == "ESC" || part == "ESCAPE") candidate = VkEscape;
            else if (part.size() >= 2 && part[0] == 'F') {
                unsigned number = 0;
                for (std::size_t i = 1; i < part.size(); ++i) {
                    if (part[i] < '0' || part[i] > '9') return false;
                    const unsigned digit = static_cast<unsigned>(part[i] - '0');
                    if (number > (24U - digit) / 10U) return false;
                    number = number * 10U + digit;
                }
                if (number < 1 || number > 24) return false;
                candidate = static_cast<uint16_t>(VkF1 + number - 1);
            } else if (part.size() == 1 && std::isalnum(static_cast<unsigned char>(part[0]))) {
                candidate = static_cast<uint16_t>(part[0]);
            } else return false;
            if (key) return false; // one activating key, never a silent overwrite
            key = candidate;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!key || !modifiers) return false;
    shortcut = {modifiers, key};
    return true;
}

template <typename IsDown>
inline uint8_t ModifierMask(IsDown isDown) {
    uint8_t modifiers = TLNK_MOD_NONE;
    if (isDown(VkShift) || isDown(VkLShift) || isDown(VkRShift)) modifiers |= TLNK_MOD_SHIFT;
    if (isDown(VkControl) || isDown(VkLControl) || isDown(VkRControl)) modifiers |= TLNK_MOD_CTRL;
    if (isDown(VkMenu) || isDown(VkLMenu) || isDown(VkRMenu)) modifiers |= TLNK_MOD_ALT;
    if (isDown(VkLWin) || isDown(VkRWin)) modifiers |= TLNK_MOD_META;
    return modifiers;
}

} // namespace thunderlink::crossinput
