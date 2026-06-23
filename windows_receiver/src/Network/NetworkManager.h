#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>

#include "network_utils.h"

namespace thunderlink {

struct ConnectionInfo {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    TlnkCodec codec;        /* codec the Mac confirmed it will send */
    uint32_t bitrateKbps;
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

    /* Ask the Mac to emit an IDR on its next frame (loss recovery).
     * Thread-safe and internally rate-limited; safe to call on every dropped frame. */
    void RequestKeyframe();

private:
    void ControlThread();
    bool HandleClient(tlnk_socket_t clientSocket);
    bool SendControlMessage(tlnk_socket_t sock, const std::string& json);
    std::string ReceiveControlMessage(tlnk_socket_t sock);
    void SendPing(tlnk_socket_t sock);

    tlnk_socket_t m_listenSocket = TLNK_INVALID_SOCKET;
    tlnk_socket_t m_clientSocket = TLNK_INVALID_SOCKET;

    /* Serializes writes to m_clientSocket across threads (control loop +
     * keyframe requests coming from the render/receive path). */
    std::mutex m_sendMutex;
    /* Rate-limit keyframe requests so a burst of loss doesn't trigger an IDR storm. */
    std::chrono::steady_clock::time_point m_lastKeyframeReq{};
    static constexpr int KEYFRAME_REQ_MIN_INTERVAL_MS = 75;

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
