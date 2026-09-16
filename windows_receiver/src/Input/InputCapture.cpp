#include "InputCapture.h"
#include "InputMapper.h"
#include "Network/InputSender.h"
#include "Utils/Logger.h"

#include "input_event.h"
#include "imgui.h"

#include <vector>

namespace thunderlink {

InputCapture::InputCapture(std::function<bool(HWND)> focusCheck)
    : m_focusCheck(focusCheck ? std::move(focusCheck) : [](HWND hwnd) { return GetForegroundWindow() == hwnd; }) {
}

InputCapture::~InputCapture() {
    ReleaseAll();
    SetCursorConfined(false);
}

bool InputCapture::Initialize(HWND hwnd, InputCallback callback) {
    m_hwnd = hwnd;
    m_callback = std::move(callback);

    RAWINPUTDEVICE devices[2] = {};

    /* Mouse */
    devices[0].usUsagePage = 0x01; /* HID_USAGE_PAGE_GENERIC */
    devices[0].usUsage = 0x02;     /* HID_USAGE_GENERIC_MOUSE */
    devices[0].dwFlags = 0;
    devices[0].hwndTarget = hwnd;

    /* Keyboard */
    devices[1].usUsagePage = 0x01;
    devices[1].usUsage = 0x06;     /* HID_USAGE_GENERIC_KEYBOARD */
    devices[1].dwFlags = 0;
    devices[1].hwndTarget = hwnd;

    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        LOG_ERROR("InputCapture: RegisterRawInputDevices failed (err=%lu)", GetLastError());
        return false;
    }

    LOG_INFO("InputCapture: RawInput registered");
    m_hasFocus = GetForegroundWindow() == m_hwnd;
    return true;
}

void InputCapture::ProcessRawInput(LPARAM lParam) {
    UINT size = 0;
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size,
                    sizeof(RAWINPUTHEADER));

    if (size == 0) return;

    std::vector<BYTE> buffer(size);
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer.data(), &size,
                         sizeof(RAWINPUTHEADER)) != size) {
        return;
    }

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        ProcessMouse(raw->data.mouse);
    } else if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        ProcessKeyboard(raw->data.keyboard);
    }
}

void InputCapture::SetWindowSize(uint32_t width, uint32_t height) {
    m_windowWidth = width;
    m_windowHeight = height;
    ReapplyCursorClip();
}

void InputCapture::SetStreamSize(uint32_t width, uint32_t height) {
    m_streamWidth = width;
    m_streamHeight = height;
}

void InputCapture::SetEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    if (!enabled) ReleaseAll();
    m_enabled = enabled;
}

void InputCapture::UpdateInputPolicy() {
    bool captureMouse = false;
    bool captureKeyboard = false;
    if (ImGui::GetCurrentContext() != nullptr) {
        const ImGuiIO& io = ImGui::GetIO();
        captureMouse = io.WantCaptureMouse;
        captureKeyboard = io.WantCaptureKeyboard;
    }

    const bool mouseTookOver = captureMouse && !m_overlayCapturesMouse;
    const bool keyboardTookOver = captureKeyboard && !m_overlayCapturesKeyboard;
    if (mouseTookOver || keyboardTookOver) ReleaseAll();
    m_overlayCapturesMouse = captureMouse;
    m_overlayCapturesKeyboard = captureKeyboard;
}

void InputCapture::ReleaseAll() {
    ReleaseMouseButtons();
    ReleaseKeys();
    EmitReset();
}

void InputCapture::SetCursorConfined(bool confined) {
    if (!confined) ReleaseAll();
    m_cursorConfined = confined;
    if (confined && m_hasFocus) {
        UpdateCursorClip();
        SetCursorHidden(true);
    } else {
        ClipCursor(nullptr);
        SetCursorHidden(false);
    }
}

void InputCapture::ToggleCursorConfinement() {
    SetCursorConfined(!m_cursorConfined);
    LOG_INFO("InputCapture: cursor confinement %s",
             m_cursorConfined ? "enabled" : "disabled");
}

void InputCapture::OnFocusGained() {
    m_hasFocus = true;
    if (m_cursorConfined) {
        UpdateCursorClip();
        SetCursorHidden(true);
    }
}

void InputCapture::OnFocusLost() {
    ReleaseAll();
    m_hasFocus = false;
    ClipCursor(nullptr);
    SetCursorHidden(false);
}

void InputCapture::ReapplyCursorClip() {
    if (m_cursorConfined && m_hasFocus) UpdateCursorClip();
}

void InputCapture::UpdateCursorClip() {
    if (!m_hwnd) return;

    RECT rect;
    if (GetClientRect(m_hwnd, &rect)) {
        POINT topLeft = {rect.left, rect.top};
        POINT bottomRight = {rect.right, rect.bottom};
        ClientToScreen(m_hwnd, &topLeft);
        ClientToScreen(m_hwnd, &bottomRight);

        RECT screenRect;
        screenRect.left = topLeft.x;
        screenRect.top = topLeft.y;
        screenRect.right = bottomRight.x;
        screenRect.bottom = bottomRight.y;

        ClipCursor(&screenRect);
    }
}

void InputCapture::ProcessMouse(const RAWMOUSE& mouse) {
    /* Absolute mapping: where the OS cursor sits over the client area. */
    float nx = 0.0f, ny = 0.0f;
    const bool havePos = CurrentClientCursor(nx, ny);
    if (havePos) {
        m_lastCursorX = nx;
        m_lastCursorY = ny;
    } else {
        nx = m_lastCursorX;
        ny = m_lastCursorY;
    }

    const bool allow = InputAllowed(true);

    auto emitButton = [&](int button, int state) {
        const bool isRelease = state == TLNK_STATE_UP;
        if (!allow && !(isRelease && m_forwardedState.IsButtonHeld(button))) {
            return;
        }
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_BUTTON;
        event.button = button;
        event.state = state;
        event.x = nx;
        event.y = ny;
        const bool accepted = EmitEvent(event);
        /* A rejected down never reached the input connection, so it must not
         * create local held state. Releases clear prior state even if the
         * connection has already gone away. */
        if (isRelease) m_forwardedState.SetButtonHeld(button, false);
        else if (accepted) m_forwardedState.SetButtonHeld(button, true);
    };

    /* Movement (relative deltas from RawInput just tell us it moved; the
     * position itself comes from the real cursor over the window). */
    if (allow && havePos && (mouse.lLastX != 0 || mouse.lLastY != 0)) {
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_MOVE;
        event.x = nx;
        event.y = ny;
        EmitEvent(event);
    }

    if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)   emitButton(TLNK_BUTTON_LEFT,   TLNK_STATE_DOWN);
    if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)     emitButton(TLNK_BUTTON_LEFT,   TLNK_STATE_UP);
    if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)  emitButton(TLNK_BUTTON_RIGHT,  TLNK_STATE_DOWN);
    if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)    emitButton(TLNK_BUTTON_RIGHT,  TLNK_STATE_UP);
    if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) emitButton(TLNK_BUTTON_MIDDLE, TLNK_STATE_DOWN);
    if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)   emitButton(TLNK_BUTTON_MIDDLE, TLNK_STATE_UP);

    /* Scroll wheel */
    if (allow && (mouse.usButtonFlags & RI_MOUSE_WHEEL)) {
        InputEvent event = {};
        event.type = TLNK_INPUT_SCROLL;
        event.deltaX = 0;
        event.deltaY = static_cast<int32_t>(static_cast<SHORT>(mouse.usButtonData));
        EmitEvent(event);
    }
    if (allow && (mouse.usButtonFlags & RI_MOUSE_HWHEEL)) {
        InputEvent event = {};
        event.type = TLNK_INPUT_SCROLL;
        event.deltaX = static_cast<int32_t>(static_cast<SHORT>(mouse.usButtonData));
        event.deltaY = 0;
        EmitEvent(event);
    }
}

void InputCapture::ProcessKeyboard(const RAWKEYBOARD& keyboard) {
    InputEvent event = {};
    event.type = TLNK_INPUT_KEY;
    event.keyCode = InputMapper::MapKeyCode(keyboard.VKey);
    event.state = (keyboard.Flags & RI_KEY_BREAK) ? TLNK_STATE_UP : TLNK_STATE_DOWN;
    event.modifiers = InputMapper::GetModifierState();

    const bool isRelease = event.state == TLNK_STATE_UP;
    const bool wasForwarded = m_forwardedState.IsKeyHeld(event.keyCode);
    if (!InputAllowed(false) && !(isRelease && wasForwarded)) return;

    const bool accepted = EmitEvent(event);
    if (isRelease) m_forwardedState.SetKeyHeld(event.keyCode, false);
    else if (accepted) m_forwardedState.SetKeyHeld(event.keyCode, true);
}

bool InputCapture::InputAllowed(bool isMouse) const {
    /* Only forward input while the screenshare window is the active (foreground)
     * window. Checked live via GetForegroundWindow rather than a cached focus
     * flag, because the window's first WM_SETFOCUS fires during creation before
     * InputCapture is wired up, which would otherwise leave the flag stuck. */
    if (!m_enabled || !m_focusCheck(m_hwnd)) return false;

    /* Policy is sampled after the UI is built, so a UI interaction cannot also
     * drive the Mac through the raw-input path. */
    return isMouse ? !m_overlayCapturesMouse : !m_overlayCapturesKeyboard;
}

bool InputCapture::CurrentClientCursor(float& nx, float& ny) const {
    POINT p;
    if (!m_hwnd || !GetCursorPos(&p)) return false;
    if (!ScreenToClient(m_hwnd, &p)) return false;

    RECT rc;
    if (!GetClientRect(m_hwnd, &rc)) return false;
    const float clientW = static_cast<float>(rc.right - rc.left);
    const float clientH = static_cast<float>(rc.bottom - rc.top);
    if (clientW <= 0.0f || clientH <= 0.0f) return false;

    /* Normalize against the letterboxed VIDEO rectangle (matches the renderer's
     * aspect-preserving viewport), not the whole window. Without this the cursor
     * is offset/scaled whenever there are black bars. */
    float videoX = 0.0f, videoY = 0.0f, videoW = clientW, videoH = clientH;
    if (m_streamWidth > 0 && m_streamHeight > 0) {
        const float winAspect = clientW / clientH;
        const float strAspect = static_cast<float>(m_streamWidth) /
                                static_cast<float>(m_streamHeight);
        if (strAspect > winAspect) {
            videoW = clientW;
            videoH = clientW / strAspect;
        } else {
            videoH = clientH;
            videoW = clientH * strAspect;
        }
        videoX = (clientW - videoW) * 0.5f;
        videoY = (clientH - videoH) * 0.5f;
    }

    float x = (static_cast<float>(p.x) - videoX) / videoW;
    float y = (static_cast<float>(p.y) - videoY) / videoH;
    nx = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    ny = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
    return true;
}

bool InputCapture::EmitEvent(const InputEvent& event) {
    return m_callback && m_callback(event);
}

bool InputCapture::EmitReset() {
    InputEvent event = {};
    event.type = TLNK_INPUT_RESET;
    return EmitEvent(event);
}

void InputCapture::ReleaseMouseButtons() {
    for (int button : m_forwardedState.TakeHeldButtons()) {
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_BUTTON;
        event.button = button;
        event.state = TLNK_STATE_UP;
        event.x = m_lastCursorX;
        event.y = m_lastCursorY;
        EmitEvent(event);
    }
}

void InputCapture::ReleaseKeys() {
    for (uint16_t keyCode : m_forwardedState.TakeHeldKeys()) {
        InputEvent event = {};
        event.type = TLNK_INPUT_KEY;
        event.keyCode = keyCode;
        event.state = TLNK_STATE_UP;
        EmitEvent(event);
    }
}

void InputCapture::SetCursorHidden(bool hidden) {
    if (m_cursorHidden == hidden) return;
    ShowCursor(hidden ? FALSE : TRUE);
    m_cursorHidden = hidden;
}

} // namespace thunderlink
