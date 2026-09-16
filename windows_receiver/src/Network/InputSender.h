#pragma once
#include <string>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include "network_utils.h"
namespace thunderlink {
struct InputEvent {
    int type=0;
    int64_t timestamp=0;
    float x=0,y=0;
    int button=0,state=0;
    uint16_t keyCode=0;
    uint8_t modifiers=0;
    int32_t deltaX=0,deltaY=0;
    uint64_t sessionId=0;
};
class InputSender {
public:
    ~InputSender();
    bool Start();
    void Stop();
    void SetSession(uint64_t sessionId);
    bool IsConnected() const { return m_connected.load(); }
    bool SendEvent(const InputEvent& event);
private:
    void SendThread();
    std::string SerializeEvent(const InputEvent& event);
    std::thread m_sendThread;
    std::atomic<bool> m_running{false},m_connected{false};
    std::atomic<uint64_t> m_sessionId{0};
    std::mutex m_queueMutex;
    std::condition_variable m_queueCV;
    std::deque<InputEvent> m_eventQueue;
    bool m_resetRequested=false;
};
}
