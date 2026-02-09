#pragma once

#include <thread>
#include <atomic>
#include <cstdint>

#include "network_utils.h"

namespace thunderlink {

class FrameBuffer;
class PerformanceMonitor;

class FrameReceiver {
public:
    FrameReceiver(FrameBuffer* frameBuffer, PerformanceMonitor* perfMon);
    ~FrameReceiver();

    bool Start();
    void Stop();
    bool IsRunning() const { return m_running.load(); }

private:
    void ReceiveThread();

    FrameBuffer* m_frameBuffer;
    PerformanceMonitor* m_perfMon;

    tlnk_socket_t m_socket = TLNK_INVALID_SOCKET;
    std::thread m_receiveThread;
    std::atomic<bool> m_running{false};

    /* Eviction counter - evict stale frames every N packets */
    uint32_t m_evictionCounter = 0;
    static constexpr uint32_t EVICTION_INTERVAL = 500;
};

} // namespace thunderlink
