#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

#include "network_utils.h"

namespace thunderlink {

struct ConnectionInfo {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    bool connected;
    float peerLatencyMs;
};

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    using OnConnectedCallback = std::function<void(const ConnectionInfo&)>;
    using OnDisconnectedCallback = std::function<void()>;

    void SetOnConnected(OnConnectedCallback cb) { m_onConnected = std::move(cb); }
    void SetOnDisconnected(OnDisconnectedCallback cb) { m_onDisconnected = std::move(cb); }

    /* Start listening for Mac sender connection on control port */
    bool Start();
    void Stop();

    bool IsConnected() const { return m_connected.load(); }
    float GetPeerLatency() const { return m_peerLatencyMs.load(); }
    ConnectionInfo GetConnectionInfo() const;

    /* Request the Mac to change resolution/fps */
    bool RequestConfigure(uint32_t width, uint32_t height, uint32_t fps);

private:
    void ControlThread();
    bool HandleClient(tlnk_socket_t clientSocket);
    bool SendControlMessage(tlnk_socket_t sock, const std::string& json);
    std::string ReceiveControlMessage(tlnk_socket_t sock);
    void SendPing(tlnk_socket_t sock);

    tlnk_socket_t m_listenSocket = TLNK_INVALID_SOCKET;
    tlnk_socket_t m_clientSocket = TLNK_INVALID_SOCKET;

    std::thread m_controlThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<float> m_peerLatencyMs{0.0f};

    mutable std::mutex m_infoMutex;
    ConnectionInfo m_connInfo{};

    OnConnectedCallback m_onConnected;
    OnDisconnectedCallback m_onDisconnected;
};

} // namespace thunderlink
