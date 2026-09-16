#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <optional>
#include "network_utils.h"
namespace thunderlink {
struct ConnectionInfo {
    uint32_t width=0, height=0, fps=0;
    TlnkCodec codec=TLNK_CODEC_HEVC;
    uint32_t bitrateKbps=0;
    bool connected=false;
    float peerLatencyMs=0;
    uint64_t sessionId=0;
};
class NetworkManager {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    explicit NetworkManager(Clock clock = [] { return std::chrono::steady_clock::now(); })
        : m_clock(std::move(clock)) {}
    ~NetworkManager();
    using OnConnectedCallback=std::function<void(const ConnectionInfo&)>;
    using OnDisconnectedCallback=std::function<void()>;
    void SetOnConnected(OnConnectedCallback cb) { m_onConnected=std::move(cb); }
    void SetOnDisconnected(OnDisconnectedCallback cb) { m_onDisconnected=std::move(cb); }
    bool Start();
    void Stop();
    bool IsConnected() const { return m_connected.load(); }
    float GetPeerLatency() const { return m_peerLatencyMs.load(); }
    ConnectionInfo GetConnectionInfo() const;
    std::string GetError() const;
    bool RequestConfigure(uint32_t width,uint32_t height,uint32_t fps);
    void RequestKeyframe();
private:
    Clock m_clock;
    void ControlThread();
    void HandleClient(tlnk_socket_t socket);
    void Disconnect();
    void SetError(const std::string& error);
    tlnk_socket_t m_listenSocket=TLNK_INVALID_SOCKET;
    std::thread m_controlThread;
    std::atomic<bool> m_running{false},m_connected{false};
    std::atomic<float> m_peerLatencyMs{0};
    mutable std::mutex m_mutex;
    ConnectionInfo m_connInfo{};
    std::string m_error;
    std::optional<ConnectionInfo> m_requested;
    bool m_keyframeRequested=false;
    OnConnectedCallback m_onConnected;
    OnDisconnectedCallback m_onDisconnected;
};
}
