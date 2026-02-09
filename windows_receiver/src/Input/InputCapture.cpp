#include "InputCapture.h"
#include "InputMapper.h"
#include "Network/InputSender.h"
#include "Utils/Logger.h"

#include "input_event.h"

#include <vector>

namespace thunderlink {

InputCapture::InputCapture() {
}

InputCapture::~InputCapture() {
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

    /* Initialize cursor to center of window */
    RECT rect;
    if (GetClientRect(hwnd, &rect)) {
        m_cursorX = (rect.right - rect.left) / 2;
        m_cursorY = (rect.bottom - rect.top) / 2;
    }

    LOG_INFO("InputCapture: RawInput registered");
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
}

void InputCapture::SetCursorConfined(bool confined) {
    m_cursorConfined = confined;
    if (confined && m_hasFocus) {
        UpdateCursorClip();
        ShowCursor(FALSE);
    } else {
        ClipCursor(nullptr);
        ShowCursor(TRUE);
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
        ShowCursor(FALSE);
    }
}

void InputCapture::OnFocusLost() {
    m_hasFocus = false;
    ClipCursor(nullptr);
    ShowCursor(TRUE);
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
    /* Handle movement */
    if (mouse.usFlags == MOUSE_MOVE_RELATIVE) {
        m_cursorX += mouse.lLastX;
        m_cursorY += mouse.lLastY;

        /* Clamp to window bounds */
        if (m_cursorX < 0) m_cursorX = 0;
        if (m_cursorY < 0) m_cursorY = 0;
        if (m_cursorX > (int)m_windowWidth) m_cursorX = (int)m_windowWidth;
        if (m_cursorY > (int)m_windowHeight) m_cursorY = (int)m_windowHeight;

        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_MOVE;
        InputMapper::NormalizeMouseCoords(m_cursorX, m_cursorY,
                                          m_windowWidth, m_windowHeight,
                                          event.x, event.y);
        EmitEvent(event);
    }

    /* Handle buttons */
    if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_BUTTON;
        event.button = TLNK_BUTTON_LEFT;
        event.state = TLNK_STATE_DOWN;
        InputMapper::NormalizeMouseCoords(m_cursorX, m_cursorY,
                                          m_windowWidth, m_windowHeight,
                                          event.x, event.y);
        EmitEvent(event);
    }
    if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) {
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_BUTTON;
        event.button = TLNK_BUTTON_LEFT;
        event.state = TLNK_STATE_UP;
        InputMapper::NormalizeMouseCoords(m_cursorX, m_cursorY,
                                          m_windowWidth, m_windowHeight,
                                          event.x, event.y);
        EmitEvent(event);
    }
    if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_BUTTON;
        event.button = TLNK_BUTTON_RIGHT;
        event.state = TLNK_STATE_DOWN;
        InputMapper::NormalizeMouseCoords(m_cursorX, m_cursorY,
                                          m_windowWidth, m_windowHeight,
                                          event.x, event.y);
        EmitEvent(event);
    }
    if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) {
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_BUTTON;
        event.button = TLNK_BUTTON_RIGHT;
        event.state = TLNK_STATE_UP;
        InputMapper::NormalizeMouseCoords(m_cursorX, m_cursorY,
                                          m_windowWidth, m_windowHeight,
                                          event.x, event.y);
        EmitEvent(event);
    }
    if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_BUTTON;
        event.button = TLNK_BUTTON_MIDDLE;
        event.state = TLNK_STATE_DOWN;
        InputMapper::NormalizeMouseCoords(m_cursorX, m_cursorY,
                                          m_windowWidth, m_windowHeight,
                                          event.x, event.y);
        EmitEvent(event);
    }
    if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) {
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_BUTTON;
        event.button = TLNK_BUTTON_MIDDLE;
        event.state = TLNK_STATE_UP;
        InputMapper::NormalizeMouseCoords(m_cursorX, m_cursorY,
                                          m_windowWidth, m_windowHeight,
                                          event.x, event.y);
        EmitEvent(event);
    }

    /* Handle scroll wheel */
    if (mouse.usButtonFlags & RI_MOUSE_WHEEL) {
        InputEvent event = {};
        event.type = TLNK_INPUT_SCROLL;
        event.deltaX = 0;
        event.deltaY = static_cast<int32_t>(static_cast<SHORT>(mouse.usButtonData));
        EmitEvent(event);
    }
    if (mouse.usButtonFlags & RI_MOUSE_HWHEEL) {
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

    EmitEvent(event);
}

void InputCapture::EmitEvent(const InputEvent& event) {
    if (m_callback) {
        m_callback(event);
    }
}

} // namespace thunderlink
