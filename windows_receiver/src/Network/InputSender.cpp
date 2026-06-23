#include "InputSender.h"
#include "Utils/Logger.h"
#include "App/Config.h"

#include "input_event.h"

#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace thunderlink {

InputSender::InputSender() {
}

InputSender::~InputSender() {
    Stop();
}

bool InputSender::Start() {
    m_running.store(true);
    m_sendThread = std::thread(&InputSender::SendThread, this);
    return true;
}

void InputSender::Stop() {
    m_running.store(false);
    m_queueCV.notify_all();

    if (m_socket != TLNK_INVALID_SOCKET) {
        tlnk_close_socket(m_socket);
        m_socket = TLNK_INVALID_SOCKET;
    }

    if (m_sendThread.joinable()) {
        m_sendThread.join();
    }

    m_connected.store(false);
}

void InputSender::SendEvent(const InputEvent& event) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        /* Limit queue size to prevent unbounded growth */
        if (m_eventQueue.size() < 1000) {
            m_eventQueue.push(event);
        }
    }
    m_queueCV.notify_one();
}

bool InputSender::Connect() {
    if (m_socket != TLNK_INVALID_SOCKET) {
        tlnk_close_socket(m_socket);
    }

    m_socket = tlnk_create_tcp_socket();
    if (m_socket == TLNK_INVALID_SOCKET) {
        LOG_ERROR("InputSender: failed to create TCP socket");
        return false;
    }

    const auto& cfg = GetConfig();
    struct sockaddr_in addr = tlnk_make_addr(cfg.peerIP.c_str(), cfg.inputPort);
    if (connect(m_socket, (struct sockaddr*)&addr, sizeof(addr)) == TLNK_SOCKET_ERROR) {
        LOG_DEBUG("InputSender: failed to connect to %s:%d (err=%d)",
                  cfg.peerIP.c_str(), cfg.inputPort, tlnk_get_error());
        tlnk_close_socket(m_socket);
        m_socket = TLNK_INVALID_SOCKET;
        return false;
    }

    LOG_INFO("InputSender: connected to %s:%d", cfg.peerIP.c_str(), cfg.inputPort);
    m_connected.store(true);
    return true;
}

void InputSender::SendThread() {
    LOG_INFO("InputSender: send thread started");

    while (m_running.load()) {
        /* Try to connect if not connected */
        if (!m_connected.load()) {
            if (!Connect()) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(TLNK_RECONNECT_INTERVAL_MS));
                continue;
            }
        }

        /* Wait for events */
        InputEvent event;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCV.wait_for(lock, std::chrono::milliseconds(100),
                               [this] { return !m_eventQueue.empty() || !m_running.load(); });

            if (!m_running.load()) break;
            if (m_eventQueue.empty()) continue;

            event = m_eventQueue.front();
            m_eventQueue.pop();
        }

        /* Serialize and send */
        std::string jsonStr = SerializeEvent(event);
        if (!SendMessage(jsonStr)) {
            LOG_WARN("InputSender: send failed, disconnecting");
            tlnk_close_socket(m_socket);
            m_socket = TLNK_INVALID_SOCKET;
            m_connected.store(false);
        }
    }

    LOG_INFO("InputSender: send thread exiting");
}

bool InputSender::SendMessage(const std::string& jsonStr) {
    /* 4-byte big-endian length prefix + JSON */
    uint32_t len = static_cast<uint32_t>(jsonStr.size());
    uint8_t header[4];
    header[0] = (len >> 24) & 0xFF;
    header[1] = (len >> 16) & 0xFF;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;

    if (send(m_socket, (const char*)header, 4, 0) != 4) return false;
    if (send(m_socket, jsonStr.c_str(), (int)jsonStr.size(), 0) != (int)jsonStr.size())
        return false;

    return true;
}

std::string InputSender::SerializeEvent(const InputEvent& event) {
    json j;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    j[TLNK_JSON_TIMESTAMP] = now;

    switch (event.type) {
        case TLNK_INPUT_MOUSE_MOVE:
            j[TLNK_JSON_TYPE] = TLNK_JSON_TYPE_MOUSE_MOVE;
            j[TLNK_JSON_X] = event.x;
            j[TLNK_JSON_Y] = event.y;
            break;

        case TLNK_INPUT_MOUSE_BUTTON:
            j[TLNK_JSON_TYPE] = TLNK_JSON_TYPE_MOUSE_BUTTON;
            j[TLNK_JSON_X] = event.x;
            j[TLNK_JSON_Y] = event.y;
            j[TLNK_JSON_BUTTON] = event.button;
            j[TLNK_JSON_STATE] = (event.state == TLNK_STATE_DOWN)
                                    ? TLNK_JSON_STATE_DOWN : TLNK_JSON_STATE_UP;
            break;

        case TLNK_INPUT_KEY: {
            j[TLNK_JSON_TYPE] = TLNK_JSON_TYPE_KEY;
            j[TLNK_JSON_KEY_CODE] = event.keyCode;
            j[TLNK_JSON_STATE] = (event.state == TLNK_STATE_DOWN)
                                    ? TLNK_JSON_STATE_DOWN : TLNK_JSON_STATE_UP;

            json mods = json::array();
            if (event.modifiers & TLNK_MOD_SHIFT) mods.push_back(TLNK_JSON_MOD_SHIFT);
            if (event.modifiers & TLNK_MOD_CTRL)  mods.push_back(TLNK_JSON_MOD_CTRL);
            if (event.modifiers & TLNK_MOD_ALT)   mods.push_back(TLNK_JSON_MOD_ALT);
            if (event.modifiers & TLNK_MOD_META)  mods.push_back(TLNK_JSON_MOD_META);
            j[TLNK_JSON_MODIFIERS] = mods;
            break;
        }

        case TLNK_INPUT_SCROLL:
            j[TLNK_JSON_TYPE] = TLNK_JSON_TYPE_SCROLL;
            j[TLNK_JSON_DELTA_X] = event.deltaX;
            j[TLNK_JSON_DELTA_Y] = event.deltaY;
            break;

        default:
            j[TLNK_JSON_TYPE] = "unknown";
            break;
    }

    return j.dump();
}

} // namespace thunderlink
