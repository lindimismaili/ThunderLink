#include "CrossInputController.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

#include "Input/InputMapper.h"
#include "Utils/Logger.h"
#include "cross_input_state.h"
#include "protocol.h"

namespace thunderlink {
std::atomic<CrossInputController*> CrossInputController::s_instance{nullptr};
namespace {
constexpr UINT kWake = WM_APP + 0x320;
constexpr UINT kLeaseTimer = 0x321;
constexpr wchar_t kInputClass[] = L"ThunderLinkCrossInput";
constexpr uint8_t kLeftButtonBit = 1, kRightButtonBit = 2, kMiddleButtonBit = 4;
uint8_t ButtonBit(int button) {
    return button == TLNK_BUTTON_LEFT ? kLeftButtonBit :
           button == TLNK_BUTTON_RIGHT ? kRightButtonBit :
           button == TLNK_BUTTON_MIDDLE ? kMiddleButtonBit : 0;
}
uint16_t PhysicalKey(uint16_t virtualKey, DWORD scanCode, DWORD flags) {
    // LL hooks can report the aggregate modifier VK. Keep hook state in
    // sided slots so releasing one Ctrl/Shift/Alt cannot clear the other.
    if (virtualKey == VK_SHIFT) return scanCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    if (virtualKey == VK_CONTROL) return (flags & LLKHF_EXTENDED) ? VK_RCONTROL : VK_LCONTROL;
    if (virtualKey == VK_MENU) return (flags & LLKHF_EXTENDED) ? VK_RMENU : VK_LMENU;
    return virtualKey;
}
bool InputDesktopName(std::wstring& name) {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return false;
    DWORD bytes = 0;
    (void)GetUserObjectInformationW(desktop, UOI_NAME, nullptr, 0, &bytes);
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    const bool ok = bytes && GetUserObjectInformationW(desktop, UOI_NAME, buffer.data(), bytes, &bytes);
    CloseDesktop(desktop);
    if (ok) name.assign(buffer.data());
    return ok;
}
LONG RestoreCoordinate(double value, double origin, double extent) {
    const LONG low = static_cast<LONG>(std::ceil(origin));
    const LONG high = std::max(low, static_cast<LONG>(std::ceil(origin + extent)) - 1);
    return std::clamp(static_cast<LONG>(std::lround(value)), low, high);
}
} // namespace

CrossInputController::CrossInputController(CrossInputDelegate& delegate) : m_delegate(delegate) {}
CrossInputController::~CrossInputController() { Stop(); }

bool CrossInputController::Start() {
    if (m_running.exchange(true)) return true;
    m_captureReady = false; m_abortRequested = false; m_peerReturned = false;
    m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_ready) { m_running = false; return false; }
    m_thread = std::thread(&CrossInputController::ThreadMain, this);
    const bool ready = WaitForSingleObject(m_ready, 3000) == WAIT_OBJECT_0 && m_captureReady.load();
    if (!ready) Stop();
    return ready;
}
void CrossInputController::Stop() {
    const bool wasRunning = m_running.exchange(false);
    if (!wasRunning && !m_thread.joinable()) return;
    m_enabled = false; m_abortRequested = true;
    // On a slow or failed startup the thread may not have published its id
    // yet. Wait for its ready signal before posting, rather than closing the
    // event out from under ThreadMain or joining a message loop with no quit.
    if (!m_threadId.load() && m_ready) (void)WaitForSingleObject(m_ready, 3000);
    const DWORD threadId = m_threadId.load();
    if (threadId) { PostThreadMessageW(threadId, kWake, 0, 0); PostThreadMessageW(threadId, WM_QUIT, 0, 0); }
    if (m_thread.joinable()) m_thread.join();
    if (m_ready) { CloseHandle(m_ready); m_ready = nullptr; }
    m_threadId = 0;
}
void CrossInputController::SetEnabled(bool enabled) {
    m_enabled = enabled;
    if (!enabled) m_abortRequested = true;
    if (const DWORD threadId = m_threadId.load()) PostThreadMessageW(threadId, kWake, 0, 0);
}
void CrossInputController::SetLayout(cross::Layout layout) {
    { std::lock_guard<std::mutex> lock(m_mutex); m_layout = std::move(layout); }
    if (m_state.load() != State::Local) m_abortRequested = true;
    if (const DWORD threadId = m_threadId.load()) PostThreadMessageW(threadId, kWake, 0, 0);
}
void CrossInputController::SetEmergencyShortcut(const std::string& shortcut) {
    crossinput::EmergencyShortcut parsed{};
    if (!crossinput::ParseEmergencyShortcut(shortcut, parsed)) {
        LOG_ERROR("Cross: invalid emergency shortcut; retaining existing shortcut");
        return;
    }
    m_emergencyModifiers = parsed.modifiers; m_emergencyKey = parsed.key;
}
void CrossInputController::SetRemoteOwnership(uint64_t generation, const std::string& displayId) {
    { std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_enabled || m_state.load() != State::Entering || displayId != m_remoteDisplay) return;
      m_generation = generation; m_pendingRemote = true; m_lastPeer = std::chrono::steady_clock::now(); }
    if (const DWORD threadId = m_threadId.load()) PostThreadMessageW(threadId, kWake, 0, 0);
}
void CrossInputController::ReturnLocal() { m_abortRequested = true; if (const DWORD threadId = m_threadId.load()) PostThreadMessageW(threadId, kWake, 0, 0); }
void CrossInputController::PeerReturnedLocal() {
    m_peerReturned = true; m_abortRequested = true;
    if (const DWORD threadId = m_threadId.load()) PostThreadMessageW(threadId, kWake, 0, 0);
}
void CrossInputController::NotifyPeerLiveness() {
    std::lock_guard<std::mutex> lock(m_mutex); m_lastPeer = std::chrono::steady_clock::now();
}
void CrossInputController::Abort() { ReturnLocal(); }

bool CrossInputController::Queue(Queued event) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (event.kind == QueuedKind::MouseMove && !m_queue.empty() && m_queue.back().kind == QueuedKind::MouseMove) {
        m_queue.back().dx += event.dx; m_queue.back().dy += event.dy; return true;
    }
    if (m_queue.size() >= TLNK_CROSS_QUEUE_LIMIT) return false;
    m_queue.push_back(event); return true;
}
bool CrossInputController::QueueFromHook(Queued event) {
    // A low-level hook never invokes a delegate or waits for UI/network work.
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (lock.owns_lock() && m_queue.size() < TLNK_CROSS_QUEUE_LIMIT) {
        m_queue.push_back(event); return true;
    }
    m_faulted = true; m_abortRequested = true;
    if (const DWORD threadId = m_threadId.load()) PostThreadMessageW(threadId, kWake, 0, 0);
    // Suppress this event as part of the remote sequence. Passing only the
    // overflow event through would create an unexpected local click/key before
    // the queued fail-open transition restores Windows ownership.
    return true;
}

void CrossInputController::ThreadMain() {
    m_threadId = GetCurrentThreadId();
    WNDCLASSW wc{}; wc.lpfnWndProc = InputWindowProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kInputClass;
    (void)RegisterClassW(&wc);
    m_inputWindow = CreateWindowExW(0, kInputClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
    bool rawReady = false;
    if (m_inputWindow) {
        RAWINPUTDEVICE device{};
        // Pointer motion comes only from Raw Input. Buttons, wheel, and all
        // keys come only from the low-level hooks, avoiding duplicate delivery.
        device.usUsagePage = 0x01; device.usUsage = 0x02;
        device.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY; device.hwndTarget = m_inputWindow;
        rawReady = RegisterRawInputDevices(&device, 1, sizeof(RAWINPUTDEVICE)) == TRUE;
        if (!rawReady) LOG_ERROR("Cross: Raw Input registration failed (%lu)", GetLastError());
    }
    s_instance = this;
    if (rawReady) {
        m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
        m_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandleW(nullptr), 0);
    }
    if (!m_mouseHook || !m_keyboardHook) LOG_ERROR("Cross: low-level input hook installation failed (%lu)", GetLastError());
    m_captureReady = rawReady && m_mouseHook && m_keyboardHook;
    if (m_inputWindow) {
        SetTimer(m_inputWindow, kLeaseTimer, TLNK_CROSS_PING_MS, nullptr);
        m_sessionNotifications = WTSRegisterSessionNotification(m_inputWindow, NOTIFY_FOR_THIS_SESSION) == TRUE;
        (void)InputDesktopName(m_inputDesktopName);
    }
    SeedPhysicalState();
    if (m_ready) SetEvent(m_ready);
    MSG message{};
    while (m_running.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message); DrainQueued(); UpdateRemoteState();
    }
    ReturnLocalOnThread();
    if (m_sessionNotifications && m_inputWindow) WTSUnRegisterSessionNotification(m_inputWindow);
    if (m_mouseHook) UnhookWindowsHookEx(m_mouseHook);
    if (m_keyboardHook) UnhookWindowsHookEx(m_keyboardHook);
    s_instance = nullptr;
    if (m_inputWindow) DestroyWindow(m_inputWindow);
    m_inputWindow = nullptr; m_mouseHook = nullptr; m_keyboardHook = nullptr; m_captureReady = false;
}

LRESULT CALLBACK CrossInputController::InputWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<CrossInputController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<CrossInputController*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_INPUT) {
        self->ProcessRawInput(lp);
        // GetRawInputData consumes the record; DefWindowProc completes the
        // documented WM_INPUT cleanup for an input sink window.
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_TIMER && wp == kLeaseTimer) { self->TickLease(); return 0; }
    if (msg == kWake) { self->DrainQueued(); self->UpdateRemoteState(); return 0; }
    if (msg == WM_WTSSESSION_CHANGE) {
        if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_LOGOFF || wp == WTS_CONSOLE_DISCONNECT ||
            wp == WTS_REMOTE_DISCONNECT || wp == WTS_SESSION_TERMINATE)
            self->FailOpenFromInputThread("Windows session changed; Cross input was returned locally.");
        return 0;
    }
    if (msg == WM_POWERBROADCAST && (wp == PBT_APMSUSPEND || wp == PBT_APMPOWERSTATUSCHANGE ||
        wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)) {
        self->FailOpenFromInputThread("Windows power state changed; Cross input was returned locally."); return TRUE;
    }
    if (msg == WM_DISPLAYCHANGE) {
        self->FailOpenFromInputThread("Windows display topology changed; review the Cross arrangement."); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK CrossInputController::MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    auto* self = s_instance.load();
    if (code < 0 || !self) return CallNextHookEx(nullptr, code, wp, lp);
    const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    if (event->flags & LLMHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);
    Queued queued{}; bool button = false; bool down = false;
    switch (wp) {
        case WM_LBUTTONDOWN: queued = {QueuedKind::Button,0,0,TLNK_BUTTON_LEFT,TLNK_STATE_DOWN}; button=true; down=true; break;
        case WM_LBUTTONUP: queued = {QueuedKind::Button,0,0,TLNK_BUTTON_LEFT,TLNK_STATE_UP}; button=true; break;
        case WM_RBUTTONDOWN: queued = {QueuedKind::Button,0,0,TLNK_BUTTON_RIGHT,TLNK_STATE_DOWN}; button=true; down=true; break;
        case WM_RBUTTONUP: queued = {QueuedKind::Button,0,0,TLNK_BUTTON_RIGHT,TLNK_STATE_UP}; button=true; break;
        case WM_MBUTTONDOWN: queued = {QueuedKind::Button,0,0,TLNK_BUTTON_MIDDLE,TLNK_STATE_DOWN}; button=true; down=true; break;
        case WM_MBUTTONUP: queued = {QueuedKind::Button,0,0,TLNK_BUTTON_MIDDLE,TLNK_STATE_UP}; button=true; break;
        case WM_MOUSEWHEEL: queued.kind=QueuedKind::Wheel; queued.deltaY=static_cast<SHORT>(HIWORD(event->mouseData)); break;
        case WM_MOUSEHWHEEL: queued.kind=QueuedKind::Wheel; queued.deltaX=static_cast<SHORT>(HIWORD(event->mouseData)); break;
        case WM_MOUSEMOVE: {
            const auto state = self->m_state.load();
            return self->m_enabled.load() && (state == State::Remote || state == State::Entering)
                ? 1 : CallNextHookEx(nullptr, code, wp, lp);
        }
        default: return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (button) {
        const uint8_t bit = ButtonBit(queued.button), before = self->m_physicalButtons.load();
        self->m_physicalButtons = down ? static_cast<uint8_t>(before | bit) : static_cast<uint8_t>(before & ~bit);
        if (self->IsButtonReleaseQuarantined(queued.button, down)) return 1;
    }
    const auto state = self->m_state.load();
    if (!self->m_enabled.load() || state == State::Local || state == State::Paused)
        return CallNextHookEx(nullptr, code, wp, lp);
    if (state == State::Entering) {
        if (button && down) self->m_releaseQuarantineButtons.fetch_or(ButtonBit(queued.button));
        return 1;
    }
    return self->QueueFromHook(queued) ? 1 : CallNextHookEx(nullptr, code, wp, lp);
}
LRESULT CALLBACK CrossInputController::KeyboardHookProc(int code, WPARAM wp, LPARAM lp) {
    auto* self = s_instance.load();
    if (code < 0 || !self) return CallNextHookEx(nullptr, code, wp, lp);
    const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    if (event->flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);
    const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN, up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
    if (!down && !up) return CallNextHookEx(nullptr, code, wp, lp);
    const uint16_t wireKey = static_cast<uint16_t>(event->vkCode);
    const uint16_t key = PhysicalKey(wireKey, event->scanCode, event->flags);
    self->UpdatePhysicalKey(key, down);
    if (self->IsReleaseQuarantined(key, down)) return 1;
    const auto state = self->m_state.load();
    // The local emergency chord remains available during a pending edge
    // transition as well as remote ownership. It is handled before normal
    // state gating so a stalled acknowledgement can always be cancelled.
    if (self->m_enabled.load() && self->IsEmergency(wireKey, down)) {
        Queued emergency{};
        emergency.kind = QueuedKind::Emergency;
        emergency.quarantineAll = state != State::Local;
        // In Local state the modifiers already reached Windows. Only consume
        // the activating F-key's later release, so Ctrl/Alt/Shift cannot stay
        // stuck in the foreground application.
        if (state == State::Local) self->m_releaseQuarantineKeys[key] = 1;
        return self->QueueFromHook(emergency) ? 1 : CallNextHookEx(nullptr, code, wp, lp);
    }
    if (!self->m_enabled.load() || state == State::Local || state == State::Paused)
        return CallNextHookEx(nullptr, code, wp, lp);
    if (state == State::Entering) {
        if (down) self->m_releaseQuarantineKeys[key] = 1;
        return 1;
    }
    Queued queued{}; queued.kind=QueuedKind::Key; queued.key=wireKey;
    queued.state=down ? TLNK_STATE_DOWN : TLNK_STATE_UP;
    queued.modifiers=self->m_hookModifiers.load();
    return self->QueueFromHook(queued) ? 1 : CallNextHookEx(nullptr, code, wp, lp);
}

void CrossInputController::ProcessRawInput(LPARAM parameter) {
    UINT bytes = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(parameter), RID_INPUT, nullptr, &bytes, sizeof(RAWINPUTHEADER)) != 0 || !bytes) return;
    std::vector<BYTE> storage(bytes);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(parameter), RID_INPUT, storage.data(), &bytes, sizeof(RAWINPUTHEADER)) != bytes) return;
    const auto* raw = reinterpret_cast<const RAWINPUT*>(storage.data());
    if (raw->header.dwType == RIM_TYPEMOUSE) ProcessMouse(raw->data.mouse);
}
void CrossInputController::ProcessMouse(const RAWMOUSE& mouse) {
    if (!m_enabled.load()) return;
    if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // A tablet/absolute HID report is not a relative motion delta. Do not
        // turn its 0..65535 coordinates into a huge Cross jump; return local
        // ownership and report the unsupported source instead.
        m_absoluteMouseFault = true;
        m_faulted = true;
        m_abortRequested = true;
        return;
    }
    if (mouse.lLastX == 0 && mouse.lLastY == 0) return;
    if (!Queue({QueuedKind::MouseMove, mouse.lLastX, mouse.lLastY})) { m_faulted = true; m_abortRequested = true; }
}
void CrossInputController::DrainQueued() {
    std::deque<Queued> work;
    { std::lock_guard<std::mutex> lock(m_mutex); work.swap(m_queue); }
    if (m_faulted.exchange(false)) {
        const char* reason = m_absoluteMouseFault.exchange(false)
            ? "An absolute pointing device is not supported by Cross; Windows input was restored."
            : "Cross input queue could not preserve an event; Windows input was restored.";
        FailOpenFromInputThread(reason);
    }
    if (m_abortRequested.exchange(false)) ReturnLocalOnThread(m_peerReturned.exchange(false));
    for (const auto& event : work) {
        if (!m_enabled.load()) continue;
        if (event.kind == QueuedKind::Emergency) { m_delegate.CrossEmergencyPause(); ReturnLocalOnThread(false, event.quarantineAll); continue; }
        if (event.kind == QueuedKind::Abort) { ReturnLocalOnThread(m_peerReturned.exchange(false)); continue; }
        if (event.kind == QueuedKind::MouseMove) {
            if (m_state.load() == State::Remote) RouteRemotePointer(event.dx, event.dy);
            else if (m_state.load() == State::Local) RouteLocalPointer(event.dx, event.dy);
        } else if (m_state.load() == State::Remote && event.kind == QueuedKind::Button) EmitButton(event.button,event.state);
        else if (m_state.load() == State::Remote && event.kind == QueuedKind::Key) EmitKey(event.key,event.state,event.modifiers);
        else if (m_state.load() == State::Remote && event.kind == QueuedKind::Wheel) EmitWheel(event.deltaX,event.deltaY);
    }
}

void CrossInputController::TickLease() {
    const auto state=m_state.load();
    if (!m_enabled.load()) return;
    const auto now=std::chrono::steady_clock::now();
    // The configured Cross session stays alive while the cursor is local too;
    // the ownership deadline below applies only after the Mac has accepted it.
    if (m_lastPing.time_since_epoch().count()==0 || now-m_lastPing >= std::chrono::milliseconds(TLNK_CROSS_PING_MS)) {
        m_lastPing=now;
        m_delegate.CrossLeasePing();
    }
    if (state != State::Remote && state != State::Entering) return;
    CheckInputDesktop();
    if (m_state.load()==State::Entering && now-m_enterStarted > std::chrono::milliseconds(TLNK_CROSS_LEASE_MS)) {
        FailOpenFromInputThread("Mac did not acknowledge the Cross edge transition; Windows input was restored."); return;
    }
    if (m_state.load()==State::Remote) {
        std::chrono::steady_clock::time_point peer; { std::lock_guard<std::mutex> lock(m_mutex); peer=m_lastPeer; }
        if (peer.time_since_epoch().count()==0 || now-peer > std::chrono::milliseconds(TLNK_CROSS_LEASE_MS)) {
            m_delegate.CrossLeaseExpired(); ReturnLocalOnThread();
        }
    }
}
void CrossInputController::CheckInputDesktop() {
    if (m_state.load()!=State::Remote && m_state.load()!=State::Entering) return;
    std::wstring current;
    if (!InputDesktopName(current) || (!m_inputDesktopName.empty() && _wcsicmp(current.c_str(),m_inputDesktopName.c_str()) != 0))
        FailOpenFromInputThread("Windows input desktop changed; Cross input was returned locally.");
}
void CrossInputController::UpdateRemoteState() {
    bool enter=false; { std::lock_guard<std::mutex> lock(m_mutex); enter=m_pendingRemote; m_pendingRemote=false; }
    if (enter && m_state.load()==State::Entering && m_enabled.load()) { m_state=State::Remote; ParkCursor(); }
    if (m_state.load()==State::Local && m_enabled.load() && !m_haveLocalCursor) {
        POINT point{};
        if (GetCursorPos(&point)) {
            m_virtualX=point.x; m_virtualY=point.y;
            m_lastLocalCursor=point; m_haveLocalCursor=true;
        }
    }
}

void CrossInputController::RouteLocalPointer(LONG dx, LONG dy) {
    POINT actual{}; if (!GetCursorPos(&actual)) return;
    if (m_ignoreCursorWarp && actual.x == m_pendingCursorWarp.x && actual.y == m_pendingCursorWarp.y) {
        // SetCursorPos may result in an input notification on some drivers.
        // It establishes our new local baseline and is never routed as user
        // motion across an edge.
        m_virtualX=actual.x; m_virtualY=actual.y;
        m_lastLocalCursor=actual; m_haveLocalCursor=true; m_ignoreCursorWarp=false;
        return;
    }
    m_ignoreCursorWarp=false;
    if (!m_haveLocalCursor) { m_virtualX=actual.x; m_virtualY=actual.y; m_lastLocalCursor=actual; m_haveLocalCursor=true; return; }
    const bool actualMoved=actual.x != m_lastLocalCursor.x || actual.y != m_lastLocalCursor.y;
    const double nextX=actualMoved ? static_cast<double>(actual.x) : m_virtualX+dx;
    const double nextY=actualMoved ? static_cast<double>(actual.y) : m_virtualY+dy;
    const auto current=LocalDisplayAt(m_virtualX,m_virtualY);
    cross::Layout layout; { std::lock_guard<std::mutex> lock(m_mutex); layout=m_layout; }
    m_lastLocalCursor=actual;
    if (current==SIZE_MAX || current>=layout.size()) { m_virtualX=actual.x; m_virtualY=actual.y; return; }
    const auto crossing=cross::CrossEdge(layout,current,m_virtualX,m_virtualY,nextX-m_virtualX,nextY-m_virtualY);
    if (crossing && layout[crossing->target].remote && !HasHeldState()) {
        m_returnPoint=actual; m_hasReturnPoint=true; EnterAt(*crossing); return;
    }
    if (crossing && !layout[crossing->target].remote) { m_virtualX=actual.x; m_virtualY=actual.y; return; }
    const auto clamped=cross::ClampTo(layout[current].bounds,nextX,nextY);
    m_virtualX=clamped.first; m_virtualY=clamped.second;
}
void CrossInputController::RouteRemotePointer(LONG dx, LONG dy) {
    cross::Layout layout; std::string id;
    { std::lock_guard<std::mutex> lock(m_mutex); layout=m_layout; id=m_remoteDisplay; }
    const auto current=DisplayById(id);
    if (current==SIZE_MAX || current>=layout.size() || !layout[current].remote) {
        FailOpenFromInputThread("The active Mac display disappeared; Windows input was restored."); return;
    }
    const auto crossing=cross::CrossEdge(layout,current,m_virtualX,m_virtualY,dx,dy);
    if (crossing) {
        const auto& target=layout[crossing->target];
        if (!target.remote) {
            if (HasHeldState()) {
                const auto p=cross::ClampTo(layout[current].bounds,m_virtualX+dx,m_virtualY+dy);
                m_virtualX=p.first; m_virtualY=p.second; EmitPointerMove(); return;
            }
            m_returnPoint={RestoreCoordinate(crossing->x, target.bounds.x, target.bounds.width),
                           RestoreCoordinate(crossing->y, target.bounds.y, target.bounds.height)};
            m_hasReturnPoint=true; m_virtualX=crossing->x; m_virtualY=crossing->y; ReturnLocalOnThread(); return;
        }
        { std::lock_guard<std::mutex> lock(m_mutex); m_remoteDisplay=target.id; }
        m_virtualX=crossing->x; m_virtualY=crossing->y; EmitPointerMove(); return;
    }
    const auto p=cross::ClampTo(layout[current].bounds,m_virtualX+dx,m_virtualY+dy);
    m_virtualX=p.first; m_virtualY=p.second; EmitPointerMove();
}
void CrossInputController::EmitPointerMove() {
    cross::Layout layout; std::string id; { std::lock_guard<std::mutex> lock(m_mutex); layout=m_layout; id=m_remoteDisplay; }
    const auto it=std::find_if(layout.begin(),layout.end(),[&](const auto& d){return d.id==id;});
    if (it==layout.end() || !it->remote) { FailOpenFromInputThread("The active Mac display disappeared; Windows input was restored."); return; }
    const auto p=cross::ClampTo(it->bounds,m_virtualX,m_virtualY); m_virtualX=p.first; m_virtualY=p.second;
    const auto n=cross::Normalized(it->bounds,m_virtualX,m_virtualY);
    CrossInputEvent output{}; output.displayId=id; output.event.type=TLNK_INPUT_MOUSE_MOVE;
    output.event.x=static_cast<float>(n.first); output.event.y=static_cast<float>(n.second); m_delegate.CrossInput(output);
}
void CrossInputController::EmitButton(int button,int state) {
    if(button==TLNK_BUTTON_LEFT)m_leftHeld=state==TLNK_STATE_DOWN;
    if(button==TLNK_BUTTON_RIGHT)m_rightHeld=state==TLNK_STATE_DOWN;
    if(button==TLNK_BUTTON_MIDDLE)m_middleHeld=state==TLNK_STATE_DOWN;
    cross::Layout layout; std::string id; { std::lock_guard<std::mutex> lock(m_mutex); layout=m_layout; id=m_remoteDisplay; }
    const auto it=std::find_if(layout.begin(),layout.end(),[&](const auto& d){return d.id==id;});
    if(it==layout.end() || !it->remote) { FailOpenFromInputThread("The active Mac display disappeared; Windows input was restored."); return; }
    const auto n=cross::Normalized(it->bounds,m_virtualX,m_virtualY);
    CrossInputEvent output{}; output.displayId=id; output.event.type=TLNK_INPUT_MOUSE_BUTTON; output.event.button=button; output.event.state=state;
    output.event.x=static_cast<float>(n.first); output.event.y=static_cast<float>(n.second); m_delegate.CrossInput(output);
}
void CrossInputController::EmitKey(uint16_t key,int state,uint8_t modifiers) {
    if(state==TLNK_STATE_DOWN)m_keysHeld.insert(key); else m_keysHeld.erase(key);
    CrossInputEvent output{}; { std::lock_guard<std::mutex> lock(m_mutex); output.displayId=m_remoteDisplay; }
    output.event.type=TLNK_INPUT_KEY; output.event.keyCode=InputMapper::MapKeyCode(key); output.event.state=state; output.event.modifiers=modifiers;
    m_delegate.CrossInput(output);
}
void CrossInputController::EmitWheel(int x,int y) {
    CrossInputEvent output{}; { std::lock_guard<std::mutex> lock(m_mutex); output.displayId=m_remoteDisplay; }
    output.event.type=TLNK_INPUT_SCROLL; output.event.deltaX=x; output.event.deltaY=y; m_delegate.CrossInput(output);
}
void CrossInputController::EnterAt(const cross::Crossing& crossing) {
    cross::Layout layout; { std::lock_guard<std::mutex> lock(m_mutex); layout=m_layout; }
    if(crossing.target>=layout.size() || !layout[crossing.target].remote || HasHeldState()) return;
    const auto n=cross::Normalized(layout[crossing.target].bounds,crossing.x,crossing.y);
    m_virtualX=crossing.x; m_virtualY=crossing.y;
    { std::lock_guard<std::mutex> lock(m_mutex); m_remoteDisplay=layout[crossing.target].id; }
    m_enterStarted=std::chrono::steady_clock::now(); m_state=State::Entering;
    m_delegate.CrossEdgeEntered(layout[crossing.target].id,n.first,n.second);
}
void CrossInputController::ReturnLocalOnThread(bool peerAlreadyLocal, bool forceQuarantine) {
    const State previous=m_state.exchange(State::Paused);
    const bool quarantine = forceQuarantine || previous==State::Remote || previous==State::Entering || m_cursorHidden;
    if((previous==State::Remote || previous==State::Entering) && !peerAlreadyLocal) m_delegate.CrossReturnRequested();
    if (quarantine) {
        for(std::size_t i=0;i<m_physicalKeys.size();++i) if(m_physicalKeys[i].load())m_releaseQuarantineKeys[i]=1;
        const uint8_t physical=m_physicalButtons.load();
        m_releaseQuarantineButtons.fetch_or(physical);
    }
    m_leftHeld=m_rightHeld=m_middleHeld=false; m_keysHeld.clear(); m_generation=0;
    if (quarantine && m_hasReturnPoint) {
        m_virtualX=m_returnPoint.x; m_virtualY=m_returnPoint.y;
        m_lastLocalCursor=m_returnPoint; m_haveLocalCursor=true;
    }
    if (quarantine) RestoreCursor();
    m_hasReturnPoint=false;
    m_state=State::Local;
}
void CrossInputController::ParkCursor() {
    if(!m_hasReturnPoint && GetCursorPos(&m_returnPoint))m_hasReturnPoint=true;
    if(m_hasReturnPoint && SetCursorPos(m_returnPoint.x,m_returnPoint.y)) {
        m_pendingCursorWarp=m_returnPoint; m_ignoreCursorWarp=true;
    }
    if(m_cursorHidden)return;
    int changes=0; while(ShowCursor(FALSE)>=0)++changes; ++changes;
    m_cursorHideAdjustments=changes; m_cursorHidden=true;
}
void CrossInputController::RestoreCursor() {
    if(m_cursorHidden) { for(int i=0;i<m_cursorHideAdjustments;++i)(void)ShowCursor(TRUE); m_cursorHideAdjustments=0; m_cursorHidden=false; }
    if(m_hasReturnPoint && SetCursorPos(m_returnPoint.x,m_returnPoint.y)) {
        m_pendingCursorWarp=m_returnPoint; m_ignoreCursorWarp=true;
    }
}
bool CrossInputController::HasHeldState() const {
    if(m_leftHeld||m_rightHeld||m_middleHeld||!m_keysHeld.empty()||m_physicalButtons.load())return true;
    for(const auto& held:m_physicalKeys)if(held.load())return true;
    return false;
}
std::size_t CrossInputController::LocalDisplayAt(double x,double y) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for(std::size_t i=0;i<m_layout.size();++i)if(!m_layout[i].remote&&cross::Contains(m_layout[i].bounds,x,y))return i;
    return SIZE_MAX;
}
std::size_t CrossInputController::DisplayById(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for(std::size_t i=0;i<m_layout.size();++i)if(m_layout[i].id==id)return i;
    return SIZE_MAX;
}
void CrossInputController::FailOpenFromInputThread(const char* reason) {
    if(m_state.load()==State::Remote || m_state.load()==State::Entering || m_cursorHidden)ReturnLocalOnThread();
    m_enabled=false; m_abortRequested=false; m_delegate.CrossInputFault(reason);
}
bool CrossInputController::IsEmergency(uint16_t key,bool down) const {
    return down && key==m_emergencyKey.load() &&
        (m_hookModifiers.load()&m_emergencyModifiers.load())==m_emergencyModifiers.load();
}
void CrossInputController::UpdatePhysicalKey(uint16_t key,bool down) {
    if(key<m_physicalKeys.size())m_physicalKeys[key]=down?1:0;
    m_hookModifiers = crossinput::ModifierMask([this](uint16_t virtualKey) {
        return virtualKey < m_physicalKeys.size() && m_physicalKeys[virtualKey].load() != 0;
    });
}
void CrossInputController::SeedPhysicalState() {
    for (std::size_t key=0; key<m_physicalKeys.size(); ++key) {
        m_physicalKeys[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) ? 1 : 0;
        m_releaseQuarantineKeys[key] = 0;
    }
    // These are aggregate aliases or mouse virtual keys. They are never
    // cleared by the keyboard hook / m_physicalButtons paths, so do not seed
    // them into the all-key crossing gate.
    m_physicalKeys[VK_SHIFT] = 0;
    m_physicalKeys[VK_CONTROL] = 0;
    m_physicalKeys[VK_MENU] = 0;
    m_physicalKeys[VK_LBUTTON] = 0;
    m_physicalKeys[VK_RBUTTON] = 0;
    m_physicalKeys[VK_MBUTTON] = 0;
    m_physicalKeys[VK_XBUTTON1] = 0;
    m_physicalKeys[VK_XBUTTON2] = 0;
    uint8_t buttons = 0;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) buttons |= kLeftButtonBit;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) buttons |= kRightButtonBit;
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) buttons |= kMiddleButtonBit;
    m_physicalButtons = buttons;
    m_releaseQuarantineButtons = 0;
    m_hookModifiers = crossinput::ModifierMask([this](uint16_t virtualKey) {
        return virtualKey < m_physicalKeys.size() && m_physicalKeys[virtualKey].load() != 0;
    });
}
bool CrossInputController::IsReleaseQuarantined(uint16_t key,bool down) {
    if(key>=m_releaseQuarantineKeys.size()||!m_releaseQuarantineKeys[key].load())return false;
    if(!down)m_releaseQuarantineKeys[key]=0; return true;
}
bool CrossInputController::IsButtonReleaseQuarantined(int button,bool down) {
    const uint8_t bit=ButtonBit(button);
    if(!bit||!(m_releaseQuarantineButtons.load()&bit))return false;
    if(!down)m_releaseQuarantineButtons.fetch_and(static_cast<uint8_t>(~bit));
    return true;
}
} // namespace thunderlink
