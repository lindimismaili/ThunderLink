#include "NetworkManager.h"
#include "Utils/Logger.h"
#include "App/Config.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>

using json = nlohmann::json;

namespace thunderlink {

NetworkManager::NetworkManager() {
}

NetworkManager::~NetworkManager() {
    Stop();
}

bool NetworkManager::Start() {
    m_listenSocket = tlnk_create_tcp_socket();
    if (m_listenSocket == TLNK_INVALID_SOCKET) {
        LOG_ERROR("NetworkManager: failed to create control socket");
        return false;
    }

    const auto& cfg = GetConfig();
    struct sockaddr_in addr = tlnk_make_addr(cfg.bindIP.c_str(), cfg.controlPort);
    if (bind(m_listenSocket, (struct sockaddr*)&addr, sizeof(addr)) == TLNK_SOCKET_ERROR) {
        LOG_ERROR("NetworkManager: failed to bind control socket to %s:%d (err=%d)",
                  cfg.bindIP.c_str(), cfg.controlPort, tlnk_get_error());
        tlnk_close_socket(m_listenSocket);
        m_listenSocket = TLNK_INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, 1) == TLNK_SOCKET_ERROR) {
        LOG_ERROR("NetworkManager: listen failed (err=%d)", tlnk_get_error());
        tlnk_close_socket(m_listenSocket);
        m_listenSocket = TLNK_INVALID_SOCKET;
        return false;
    }

    LOG_INFO("NetworkManager: listening on %s:%d", cfg.bindIP.c_str(), cfg.controlPort);

    m_running.store(true);
    m_controlThread = std::thread(&NetworkManager::ControlThread, this);

    return true;
}

void NetworkManager::Stop() {
    m_running.store(false);

    if (m_clientSocket != TLNK_INVALID_SOCKET) {
        /* Send BYE message */
        json bye;
        bye["type"] = TLNK_CTRL_BYE;
        SendControlMessage(m_clientSocket, bye.dump());
        tlnk_close_socket(m_clientSocket);
        m_clientSocket = TLNK_INVALID_SOCKET;
    }

    if (m_listenSocket != TLNK_INVALID_SOCKET) {
        tlnk_close_socket(m_listenSocket);
        m_listenSocket = TLNK_INVALID_SOCKET;
    }

    if (m_controlThread.joinable()) {
        m_controlThread.join();
    }

    m_connected.store(false);
}

ConnectionInfo NetworkManager::GetConnectionInfo() const {
    std::lock_guard<std::mutex> lock(m_infoMutex);
    return m_connInfo;
}

bool NetworkManager::RequestConfigure(uint32_t width, uint32_t height, uint32_t fps) {
    if (!m_connected.load() || m_clientSocket == TLNK_INVALID_SOCKET) return false;

    const auto& cfg = GetConfig();
    json configure;
    configure["type"]    = TLNK_CTRL_CONFIGURE;
    configure["width"]   = width;
    configure["height"]  = height;
    configure["fps"]     = fps;
    configure["codec"]   = (int)cfg.codec;
    configure["bitrate"] = cfg.bitrateKbps;

    return SendControlMessage(m_clientSocket, configure.dump());
}

void NetworkManager::ControlThread() {
    LOG_INFO("NetworkManager: control thread started");

    /* Set a timeout on accept so we can check m_running */
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&tv, sizeof(tv));

    while (m_running.load()) {
        struct sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);

        tlnk_socket_t client = accept(m_listenSocket,
                                       (struct sockaddr*)&clientAddr, &addrLen);

        if (client == TLNK_INVALID_SOCKET) {
            continue; /* Timeout or error, loop again */
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
        LOG_INFO("NetworkManager: Mac sender connected from %s", ip);

        m_clientSocket = client;
        HandleClient(client);

        m_clientSocket = TLNK_INVALID_SOCKET;
        m_connected.store(false);

        if (m_onDisconnected) m_onDisconnected();
        LOG_INFO("NetworkManager: Mac sender disconnected");
    }

    LOG_INFO("NetworkManager: control thread exiting");
}

bool NetworkManager::HandleClient(tlnk_socket_t clientSocket) {
    /* Set receive timeout for control messages */
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&tv, sizeof(tv));

    /* Wait for HELLO from Mac */
    std::string helloMsg = ReceiveControlMessage(clientSocket);
    if (helloMsg.empty()) {
        LOG_ERROR("NetworkManager: no HELLO received from Mac");
        tlnk_close_socket(clientSocket);
        return false;
    }

    try {
        json hello = json::parse(helloMsg);
        if (hello["type"].get<int>() != TLNK_CTRL_HELLO) {
            LOG_ERROR("NetworkManager: expected HELLO, got type %d", hello["type"].get<int>());
            tlnk_close_socket(clientSocket);
            return false;
        }

        LOG_INFO("NetworkManager: received HELLO - version %d",
                 hello.value("version", 0));

        /* Send CONFIGURE with desired settings from the runtime config. The
         * codec is our PREFERRED codec; the Mac confirms the actual codec in
         * START (it may downgrade HEVC->H.264 if it cannot encode HEVC). */
        const auto& cfg = GetConfig();
        json configure;
        configure["type"]    = TLNK_CTRL_CONFIGURE;
        configure["width"]   = cfg.width;
        configure["height"]  = cfg.height;
        configure["fps"]     = cfg.fps;
        configure["codec"]   = (int)cfg.codec;       /* TlnkCodec */
        configure["bitrate"] = cfg.bitrateKbps;

        if (!SendControlMessage(clientSocket, configure.dump())) {
            LOG_ERROR("NetworkManager: failed to send CONFIGURE");
            tlnk_close_socket(clientSocket);
            return false;
        }

        /* Wait for START confirmation */
        std::string startMsg = ReceiveControlMessage(clientSocket);
        if (startMsg.empty()) {
            LOG_ERROR("NetworkManager: no START received");
            tlnk_close_socket(clientSocket);
            return false;
        }

        json start = json::parse(startMsg);
        if (start["type"].get<int>() != TLNK_CTRL_START) {
            LOG_ERROR("NetworkManager: expected START, got type %d", start["type"].get<int>());
            tlnk_close_socket(clientSocket);
            return false;
        }

        uint32_t w = start.value("width", cfg.width);
        uint32_t h = start.value("height", cfg.height);
        uint32_t fps = start.value("fps", cfg.fps);
        TlnkCodec codec = (TlnkCodec)start.value("codec", (int)cfg.codec);
        uint32_t bitrate = start.value("bitrate", cfg.bitrateKbps);

        {
            std::lock_guard<std::mutex> lock(m_infoMutex);
            m_connInfo.width = w;
            m_connInfo.height = h;
            m_connInfo.fps = fps;
            m_connInfo.codec = codec;
            m_connInfo.bitrateKbps = bitrate;
            m_connInfo.connected = true;
        }

        m_connected.store(true);
        LOG_INFO("NetworkManager: streaming started at %ux%u @ %u fps, codec=%s, %u kbps",
                 w, h, fps, (codec == TLNK_CODEC_HEVC ? "HEVC" : "H264"), bitrate);

        if (m_onConnected) {
            ConnectionInfo info;
            info.width = w;
            info.height = h;
            info.fps = fps;
            info.codec = codec;
            info.bitrateKbps = bitrate;
            info.connected = true;
            info.peerLatencyMs = 0.0f;
            m_onConnected(info);
        }

    } catch (const json::exception& e) {
        LOG_ERROR("NetworkManager: JSON parse error: %s", e.what());
        tlnk_close_socket(clientSocket);
        return false;
    }

    /* Main control loop - handle pings and other messages */
    auto lastPing = std::chrono::steady_clock::now();

    while (m_running.load() && m_connected.load()) {
        std::string msg = ReceiveControlMessage(clientSocket);

        if (!msg.empty()) {
            try {
                json j = json::parse(msg);
                int type = j["type"].get<int>();

                if (type == TLNK_CTRL_PONG) {
                    auto sent = j.value("sent_time", 0LL);
                    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    float latencyMs = static_cast<float>(now - sent) / 1000.0f;
                    m_peerLatencyMs.store(latencyMs);

                    std::lock_guard<std::mutex> lock(m_infoMutex);
                    m_connInfo.peerLatencyMs = latencyMs;
                }
                else if (type == TLNK_CTRL_BYE) {
                    LOG_INFO("NetworkManager: Mac sent BYE");
                    break;
                }
                else if (type == TLNK_CTRL_STOP) {
                    LOG_INFO("NetworkManager: Mac stopped streaming");
                    break;
                }
            } catch (...) {}
        }

        /* Send periodic pings */
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPing).count()
            >= TLNK_PING_INTERVAL_MS) {
            SendPing(clientSocket);
            lastPing = now;
        }
    }

    tlnk_close_socket(clientSocket);
    return true;
}

bool NetworkManager::SendControlMessage(tlnk_socket_t sock, const std::string& jsonStr) {
    /* 4-byte big-endian length prefix + JSON bytes.
     * Serialized because pings (control thread) and keyframe requests (render
     * thread) can both write to the client socket; interleaving would corrupt
     * the length-prefixed framing. */
    std::lock_guard<std::mutex> lock(m_sendMutex);

    uint32_t len = static_cast<uint32_t>(jsonStr.size());
    uint8_t header[4];
    header[0] = (len >> 24) & 0xFF;
    header[1] = (len >> 16) & 0xFF;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;

    if (send(sock, (const char*)header, 4, 0) != 4) return false;
    if (send(sock, jsonStr.c_str(), (int)jsonStr.size(), 0) != (int)jsonStr.size()) return false;

    return true;
}

void NetworkManager::RequestKeyframe() {
    if (!m_connected.load() || m_clientSocket == TLNK_INVALID_SOCKET) return;

    /* Rate-limit so a burst of lost packets doesn't trigger an IDR storm. */
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastKeyframeReq).count() < KEYFRAME_REQ_MIN_INTERVAL_MS) {
            return;
        }
        m_lastKeyframeReq = now;
    }

    json req;
    req["type"] = TLNK_CTRL_REQUEST_KEYFRAME;
    SendControlMessage(m_clientSocket, req.dump());
    LOG_DEBUG("NetworkManager: requested keyframe (loss recovery)");
}

std::string NetworkManager::ReceiveControlMessage(tlnk_socket_t sock) {
    /* Read 4-byte length prefix */
    uint8_t header[4];
    int received = 0;
    while (received < 4) {
        int r = recv(sock, (char*)(header + received), 4 - received, 0);
        if (r <= 0) return "";
        received += r;
    }

    uint32_t len = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
    if (len > 65536) return ""; /* Sanity check */

    std::string result(len, '\0');
    received = 0;
    while (received < (int)len) {
        int r = recv(sock, &result[received], (int)(len - received), 0);
        if (r <= 0) return "";
        received += r;
    }

    return result;
}

void NetworkManager::SendPing(tlnk_socket_t sock) {
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    json ping;
    ping["type"] = TLNK_CTRL_PING;
    ping["sent_time"] = now;

    SendControlMessage(sock, ping.dump());
}

} // namespace thunderlink
