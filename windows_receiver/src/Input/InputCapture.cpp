#include "InputCapture.h"
#include "InputMapper.h"
#include "Network/InputSender.h"
#include "Utils/Logger.h"

#include "input_event.h"
#include "imgui.h"

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
    if (!InputAllowed(true)) return;

    /* Absolute mapping: where the OS cursor sits over the client area. */
    float nx = 0.0f, ny = 0.0f;
    const bool havePos = CurrentClientCursor(nx, ny);

    auto emitButton = [&](int button, int state) {
        InputEvent event = {};
        event.type = TLNK_INPUT_MOUSE_BUTTON;
        event.button = button;
        event.state = state;
        event.x = nx;
        event.y = ny;
        EmitEvent(event);
    };

    /* Movement (relative deltas from RawInput just tell us it moved; the
     * position itself comes from the real cursor over the window). */
    if (havePos && (mouse.lLastX != 0 || mouse.lLastY != 0)) {
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
    if (!InputAllowed(false)) return;

    InputEvent event = {};
    event.type = TLNK_INPUT_KEY;
    event.keyCode = InputMapper::MapKeyCode(keyboard.VKey);
    event.state = (keyboard.Flags & RI_KEY_BREAK) ? TLNK_STATE_UP : TLNK_STATE_DOWN;
    event.modifiers = InputMapper::GetModifierState();

    EmitEvent(event);
}

bool InputCapture::InputAllowed(bool isMouse) const {
    /* Only forward input while the screenshare window is the active window. */
    if (!m_hasFocus) return false;

    /* Don't forward input the ImGui overlay is consuming (e.g. clicking the
     * stats panel or typing in it) — that would also drive the Mac. */
    if (ImGui::GetCurrentContext() != nullptr) {
        const ImGuiIO& io = ImGui::GetIO();
        if (isMouse && io.WantCaptureMouse) return false;
        if (!isMouse && io.WantCaptureKeyboard) return false;
    }
    return true;
}

bool InputCapture::CurrentClientCursor(float& nx, float& ny) const {
    POINT p;
    if (!m_hwnd || !GetCursorPos(&p)) return false;
    if (!ScreenToClient(m_hwnd, &p)) return false;

    RECT rc;
    if (!GetClientRect(m_hwnd, &rc)) return false;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    float x = static_cast<float>(p.x) / static_cast<float>(w);
    float y = static_cast<float>(p.y) / static_cast<float>(h);
    nx = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    ny = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
    return true;
}

void InputCapture::EmitEvent(const InputEvent& event) {
    if (m_callback) {
        m_callback(event);
    }
}

} // namespace thunderlink
