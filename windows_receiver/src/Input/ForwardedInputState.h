#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace thunderlink {

/* State that has crossed the Windows-to-Mac boundary. Keeping it independent
 * of RawInput and Win32 makes the release invariant directly testable. */
class ForwardedInputState {
public:
    bool IsButtonHeld(int button) const {
        return button >= 0 && static_cast<std::size_t>(button) < m_buttons.size() &&
               m_buttons[static_cast<std::size_t>(button)];
    }

    void SetButtonHeld(int button, bool held) {
        if (button < 0 || static_cast<std::size_t>(button) >= m_buttons.size()) return;
        m_buttons[static_cast<std::size_t>(button)] = held;
    }

    bool IsKeyHeld(uint16_t keyCode) const {
        return m_keys.find(keyCode) != m_keys.end();
    }

    void SetKeyHeld(uint16_t keyCode, bool held) {
        if (held) m_keys.insert(keyCode);
        else m_keys.erase(keyCode);
    }

    std::vector<int> TakeHeldButtons() {
        std::vector<int> buttons;
        for (std::size_t index = 0; index < m_buttons.size(); ++index) {
            if (!m_buttons[index]) continue;
            buttons.push_back(static_cast<int>(index));
            m_buttons[index] = false;
        }
        return buttons;
    }

    std::vector<uint16_t> TakeHeldKeys() {
        std::vector<uint16_t> keys;
        keys.reserve(m_keys.size());
        for (uint16_t keyCode : m_keys) keys.push_back(keyCode);
        m_keys.clear();
        return keys;
    }

private:
    std::array<bool, 3> m_buttons{};
    std::unordered_set<uint16_t> m_keys;
};

} // namespace thunderlink
