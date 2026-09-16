#include "cross_input_state.h"

#include <array>
#include <cassert>
#include <iostream>

int main() {
    using namespace thunderlink::crossinput;
    EmergencyShortcut shortcut{};
    assert(ParseEmergencyShortcut("Ctrl+Alt+Shift+F12", shortcut));
    assert(shortcut.modifiers == (TLNK_MOD_CTRL | TLNK_MOD_ALT | TLNK_MOD_SHIFT));
    assert(shortcut.key == VkF1 + 11);
    assert(ParseEmergencyShortcut("Win+Escape", shortcut));
    assert(shortcut.modifiers == TLNK_MOD_META && shortcut.key == VkEscape);
    assert(!ParseEmergencyShortcut("Ctrl+F0", shortcut));
    assert(!ParseEmergencyShortcut("Ctrl+F1+F2", shortcut));
    assert(!ParseEmergencyShortcut("F12", shortcut));

    std::array<bool, 256> keys{};
    keys[VkLControl] = true;
    assert(ModifierMask([&](uint16_t key) { return keys[key]; }) == TLNK_MOD_CTRL);
    keys[VkRControl] = true;
    keys[VkLControl] = false; // releasing left Ctrl must retain right Ctrl.
    assert(ModifierMask([&](uint16_t key) { return keys[key]; }) == TLNK_MOD_CTRL);
    keys[VkRControl] = false;
    keys[VkLShift] = keys[VkRMenu] = keys[VkLWin] = true;
    assert(ModifierMask([&](uint16_t key) { return keys[key]; }) ==
           (TLNK_MOD_SHIFT | TLNK_MOD_ALT | TLNK_MOD_META));
    std::cout << "Cross shortcut parsing and physical modifier aggregation passed\n";
}
