#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "cross_layout.h"
#include "Network/InputSender.h" // shared legacy input event shape used inside Cross JSON

namespace thunderlink {

struct CrossInputEvent {
    InputEvent event{};
    std::string displayId;
};

/* CrossInputController owns the Windows hook/Raw Input thread. The delegate is
 * called on that thread and must only enqueue ordered transport work; it must
 * never wait for a network acknowledgment. */
class CrossInputDelegate {
public:
    virtual ~CrossInputDelegate() = default;
    virtual void CrossEdgeEntered(const std::string& displayId, double x, double y) = 0;
    virtual void CrossInput(const CrossInputEvent& event) = 0;
    virtual void CrossReturnRequested() = 0;
    virtual void CrossEmergencyPause() = 0;
    virtual void CrossLeasePing() = 0;
    virtual void CrossLeaseExpired() = 0;
    virtual void CrossInputFault(const char* reason) = 0;
};

class CrossInputController {
public:
    explicit CrossInputController(CrossInputDelegate& delegate);
    ~CrossInputController();
    CrossInputController(const CrossInputController&) = delete;
    CrossInputController& operator=(const CrossInputController&) = delete;

    bool Start();
    void Stop();
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return m_enabled.load(); }
    void SetLayout(cross::Layout layout);
    /* Accepts the persisted form used by CrossSettings, for example
     * "Ctrl+Alt+Shift+F12". Invalid values retain the safe default. */
    void SetEmergencyShortcut(const std::string& shortcut);
    void SetRemoteOwnership(uint64_t generation, const std::string& displayId);
    void ReturnLocal();
    /* Applies a server-originated local/reset acknowledgement without sending a
     * second leave message back across the wire. */
    void PeerReturnedLocal();
    void NotifyPeerLiveness();
    void Abort();

private:
    enum class State { Local, Entering, Remote, Paused };
    enum class QueuedKind { MouseMove, Button, Key, Wheel, Emergency, Abort };
    struct Queued {
        QueuedKind kind = QueuedKind::MouseMove;
        LONG dx = 0, dy = 0;
        int button = 0, state = 0;
        uint16_t key = 0;
        uint8_t modifiers = 0;
        bool quarantineAll = false;
        int deltaX = 0, deltaY = 0;
    };

    bool Queue(Queued event);
    void ThreadMain();
    void ProcessRawInput(LPARAM parameter);
    void ProcessMouse(const RAWMOUSE& mouse);
    void DrainQueued();
    void TickLease();
    void CheckInputDesktop();
    void UpdateRemoteState();
    void RouteLocalPointer(LONG dx, LONG dy);
    void RouteRemotePointer(LONG dx, LONG dy);
    void EmitPointerMove();
    void EmitButton(int button, int state);
    void EmitKey(uint16_t key, int state, uint8_t modifiers);
    void EmitWheel(int x, int y);
    void EnterAt(const cross::Crossing& crossing);
    void ReturnLocalOnThread(bool peerAlreadyLocal = false, bool forceQuarantine = false);
    void ParkCursor();
    void RestoreCursor();
    bool HasHeldState() const;
    std::size_t LocalDisplayAt(double x, double y) const;
    std::size_t DisplayById(const std::string& id) const;
    bool QueueFromHook(Queued event);
    void FailOpenFromInputThread(const char* reason);
    bool IsEmergency(uint16_t key, bool down) const;
    void UpdatePhysicalKey(uint16_t key, bool down);
    void SeedPhysicalState();
    bool IsReleaseQuarantined(uint16_t key, bool down);
    bool IsButtonReleaseQuarantined(int button, bool down);

    static LRESULT CALLBACK InputWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wp, LPARAM lp);

    CrossInputDelegate& m_delegate;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_enabled{false};
    std::thread m_thread;
    HANDLE m_ready = nullptr;
    std::atomic<DWORD> m_threadId{0};
    HWND m_inputWindow = nullptr;
    HHOOK m_mouseHook = nullptr;
    HHOOK m_keyboardHook = nullptr;
    std::mutex m_mutex;
    cross::Layout m_layout;
    std::deque<Queued> m_queue;
    std::atomic<State> m_state{State::Local};
    std::atomic<bool> m_abortRequested{false};
    std::atomic<bool> m_peerReturned{false};
    std::atomic<bool> m_captureReady{false};
    bool m_pendingRemote = false;
    bool m_pendingLocal = false;
    uint64_t m_generation = 0;
    std::string m_remoteDisplay;
    double m_virtualX = 0, m_virtualY = 0;
    POINT m_lastLocalCursor{};
    bool m_haveLocalCursor = false;
    POINT m_pendingCursorWarp{};
    bool m_ignoreCursorWarp = false;
    POINT m_returnPoint{};
    bool m_hasReturnPoint = false;
    bool m_cursorHidden = false;
    int m_cursorHideAdjustments = 0;
    bool m_leftHeld = false, m_rightHeld = false, m_middleHeld = false;
    std::unordered_set<uint16_t> m_keysHeld;
    std::chrono::steady_clock::time_point m_lastPeer{};
    std::chrono::steady_clock::time_point m_lastPing{};
    std::chrono::steady_clock::time_point m_enterStarted{};
    std::atomic<bool> m_faulted{false};
    std::atomic<bool> m_absoluteMouseFault{false};
    /* These are written only by low-level hooks. They must not use
     * GetAsyncKeyState: Windows has not updated that state while a hook is
     * deciding whether to suppress an event. */
    std::array<std::atomic<uint8_t>, 256> m_physicalKeys{};
    std::array<std::atomic<uint8_t>, 256> m_releaseQuarantineKeys{};
    std::atomic<uint8_t> m_physicalButtons{0};
    std::atomic<uint8_t> m_releaseQuarantineButtons{0};
    std::atomic<uint8_t> m_hookModifiers{0};
    std::atomic<uint8_t> m_emergencyModifiers{0x07}; // Ctrl+Alt+Shift
    std::atomic<uint16_t> m_emergencyKey{VK_F12};
    std::wstring m_inputDesktopName;
    bool m_sessionNotifications = false;
    static std::atomic<CrossInputController*> s_instance;
};

} // namespace thunderlink
