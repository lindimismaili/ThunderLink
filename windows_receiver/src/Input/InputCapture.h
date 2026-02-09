#pragma once

#include <functional>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace thunderlink {

struct InputEvent;

class InputCapture {
public:
    using InputCallback = std::function<void(const InputEvent&)>;

    InputCapture();
    ~InputCapture();

    /* Register RawInput devices for the given window */
    bool Initialize(HWND hwnd, InputCallback callback);

    /* Process a WM_INPUT message (call from WndProc) */
    void ProcessRawInput(LPARAM lParam);

    /* Set the display resolution for coordinate normalization */
    void SetWindowSize(uint32_t width, uint32_t height);

    /* Cursor confinement */
    void SetCursorConfined(bool confined);
    bool IsCursorConfined() const { return m_cursorConfined; }
    void ToggleCursorConfinement();

    /* Called when window gains/loses focus */
    void OnFocusGained();
    void OnFocusLost();

private:
    void ProcessMouse(const RAWMOUSE& mouse);
    void ProcessKeyboard(const RAWKEYBOARD& keyboard);
    void EmitEvent(const InputEvent& event);
    void UpdateCursorClip();

    InputCallback m_callback;
    HWND m_hwnd = nullptr;

    /* Accumulated cursor position (for relative mouse input) */
    int m_cursorX = 0;
    int m_cursorY = 0;

    uint32_t m_windowWidth = 1920;
    uint32_t m_windowHeight = 1080;

    bool m_cursorConfined = false;
    bool m_hasFocus = false;
};

} // namespace thunderlink
