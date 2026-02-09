#pragma once

#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

#include "network_utils.h"

namespace thunderlink {

struct InputEvent {
    int type;           /* TlnkInputType */
    int64_t timestamp;
    float x, y;         /* Normalized [0,1] */
    int button;          /* TlnkMouseButton */
    int state;           /* TlnkButtonState */
    uint16_t keyCode;    /* Windows virtual key code */
    uint8_t modifiers;   /* TlnkModifierFlags bitmask */
    int32_t deltaX;      /* Scroll delta */
    int32_t deltaY;      /* Scroll delta */
};

class InputSender {
public:
    InputSender();
    ~InputSender();

    bool Start();
    void Stop();
    bool IsConnected() const { return m_connected.load(); }

    /* Thread-safe: enqueue an input event for sending */
    void SendEvent(const InputEvent& event);

private:
    void SendThread();
    bool Connect();
    bool SendMessage(const std::string& json);
    std::string SerializeEvent(const InputEvent& event);

    tlnk_socket_t m_socket = TLNK_INVALID_SOCKET;
    std::thread m_sendThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};

    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;
    std::queue<InputEvent> m_eventQueue;
};

} // namespace thunderlink
