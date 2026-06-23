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

    /* True if input should be forwarded right now: the window is focused AND the
     * ImGui overlay isn't capturing this input class. */
    bool InputAllowed(bool isMouse) const;

    /* Normalized [0,1] position of the OS cursor over the client (video) area.
     * This is the absolute mapping: where the cursor is on the screenshare
     * window translates directly to the Mac screen. */
    bool CurrentClientCursor(float& nx, float& ny) const;

    InputCallback m_callback;
    HWND m_hwnd = nullptr;

    uint32_t m_windowWidth = 1920;
    uint32_t m_windowHeight = 1080;

    bool m_cursorConfined = false;
    bool m_hasFocus = false;
};

} // namespace thunderlink
