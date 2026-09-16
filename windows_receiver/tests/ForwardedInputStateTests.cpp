#include "Input/ForwardedInputState.h"

#include "input_event.h"

#include <cassert>
#include <vector>

namespace {

void TestOnlyTrackedInputIsReleased() {
    thunderlink::ForwardedInputState state;
    state.SetButtonHeld(TLNK_BUTTON_LEFT, true);
    state.SetKeyHeld(0x41, true);

    assert(state.IsButtonHeld(TLNK_BUTTON_LEFT));
    assert(state.IsKeyHeld(0x41));
    assert(!state.IsButtonHeld(TLNK_BUTTON_RIGHT));
    assert(!state.IsKeyHeld(0x42));

    const std::vector<int> buttons = state.TakeHeldButtons();
    const std::vector<uint16_t> keys = state.TakeHeldKeys();
    assert(buttons.size() == 1 && buttons.front() == TLNK_BUTTON_LEFT);
    assert(keys.size() == 1 && keys.front() == 0x41);
    assert(!state.IsButtonHeld(TLNK_BUTTON_LEFT));
    assert(!state.IsKeyHeld(0x41));
}

void TestInvalidButtonsNeverBecomeHeld() {
    thunderlink::ForwardedInputState state;
    state.SetButtonHeld(-1, true);
    state.SetButtonHeld(3, true);
    assert(state.TakeHeldButtons().empty());
}

} // namespace

int main() {
    TestOnlyTrackedInputIsReleased();
    TestInvalidButtonsNeverBecomeHeld();
    return 0;
}
