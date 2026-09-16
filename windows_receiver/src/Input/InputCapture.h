#pragma once

#include <functional>
#include <cstdint>

#include "Input/ForwardedInputState.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace thunderlink {

struct InputEvent;

class InputCapture {
public:
    /* True means the sender accepted the event for the active session. */
    using InputCallback = std::function<bool(const InputEvent&)>;

    explicit InputCapture(std::function<bool(HWND)> focusCheck = {});
    ~InputCapture();

    /* Register RawInput devices for the given window */
    bool Initialize(HWND hwnd, InputCallback callback);

    /* Process a WM_INPUT message (call from WndProc) */
    void ProcessRawInput(LPARAM lParam);
    // Raw-input delivery boundary also accepts synthetic records in
    // tests, without posting input to Windows or the Mac.
    void ProcessMouse(const RAWMOUSE& mouse);
    void ProcessKeyboard(const RAWKEYBOARD& keyboard);

    /* Set the display resolution for coordinate normalization */
    void SetWindowSize(uint32_t width, uint32_t height);

    /* Set the current stream (video) resolution so the cursor is normalized
     * against the letterboxed video rectangle, not the whole window. */
    void SetStreamSize(uint32_t width, uint32_t height);

    /* Input is enabled only for an active stream session. Disabling it releases
     * every forwarded key/button before subsequent raw input is discarded. */
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled; }

    /* Refresh the overlay policy after Dear ImGui has built its UI for a
     * frame. Taking over mouse or keyboard input releases any matching remote
     * state so it cannot become stuck on the Mac. */
    void UpdateInputPolicy();

    /* Idempotently release all state previously forwarded to the Mac. */
    void ReleaseAll();

    /* Cursor confinement */
    void SetCursorConfined(bool confined);
    bool IsCursorConfined() const { return m_cursorConfined; }
    void ToggleCursorConfinement();
    void ReapplyCursorClip();

    /* Called when window gains/loses focus */
    void OnFocusGained();
    void OnFocusLost();

private:
    bool EmitEvent(const InputEvent& event);
    bool EmitReset();
    void ReleaseMouseButtons();
    void ReleaseKeys();
    void SetCursorHidden(bool hidden);
    void UpdateCursorClip();

    /* True if input should be forwarded right now: the window is focused AND the
     * ImGui overlay isn't capturing this input class. */
    bool InputAllowed(bool isMouse) const;

    /* Normalized [0,1] position of the OS cursor over the client (video) area.
     * This is the absolute mapping: where the cursor is on the screenshare
     * window translates directly to the Mac screen. */
    bool CurrentClientCursor(float& nx, float& ny) const;

    InputCallback m_callback;
    std::function<bool(HWND)> m_focusCheck;
    HWND m_hwnd = nullptr;

    uint32_t m_windowWidth = 1920;
    uint32_t m_windowHeight = 1080;

    /* Current stream/video resolution (for letterbox-aware cursor mapping). */
    uint32_t m_streamWidth = 0;
    uint32_t m_streamHeight = 0;

    bool m_cursorConfined = false;
    bool m_hasFocus = false;
    bool m_cursorHidden = false;
    bool m_enabled = false;
    bool m_overlayCapturesMouse = false;
    bool m_overlayCapturesKeyboard = false;
    ForwardedInputState m_forwardedState;
    float m_lastCursorX = 0.0f;
    float m_lastCursorY = 0.0f;
};

} // namespace thunderlink
