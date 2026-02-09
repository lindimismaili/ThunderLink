#include "FrameReceiver.h"
#include "Utils/FrameBuffer.h"
#include "Utils/PerformanceMonitor.h"
#include "Utils/Logger.h"

#include "frame_header.h"

namespace thunderlink {

FrameReceiver::FrameReceiver(FrameBuffer* frameBuffer, PerformanceMonitor* perfMon)
    : m_frameBuffer(frameBuffer)
    , m_perfMon(perfMon)
{
}

FrameReceiver::~FrameReceiver() {
    Stop();
}

bool FrameReceiver::Start() {
    m_socket = tlnk_create_udp_socket();
    if (m_socket == TLNK_INVALID_SOCKET) {
        LOG_ERROR("FrameReceiver: failed to create UDP socket");
        return false;
    }

    struct sockaddr_in addr = tlnk_make_addr(TLNK_WIN_IP, TLNK_PORT_VIDEO);
    if (bind(m_socket, (struct sockaddr*)&addr, sizeof(addr)) == TLNK_SOCKET_ERROR) {
        LOG_ERROR("FrameReceiver: failed to bind to %s:%d (err=%d)",
                  TLNK_WIN_IP, TLNK_PORT_VIDEO, tlnk_get_error());
        tlnk_close_socket(m_socket);
        m_socket = TLNK_INVALID_SOCKET;
        return false;
    }

    /* Set receive timeout so the thread can check m_running */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; /* 100ms */
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    LOG_INFO("FrameReceiver: listening on %s:%d (UDP)", TLNK_WIN_IP, TLNK_PORT_VIDEO);

    m_running.store(true);
    m_receiveThread = std::thread(&FrameReceiver::ReceiveThread, this);

    return true;
}

void FrameReceiver::Stop() {
    m_running.store(false);

    if (m_socket != TLNK_INVALID_SOCKET) {
        tlnk_close_socket(m_socket);
        m_socket = TLNK_INVALID_SOCKET;
    }

    if (m_receiveThread.joinable()) {
        m_receiveThread.join();
    }
}

void FrameReceiver::ReceiveThread() {
    LOG_INFO("FrameReceiver: receive thread started");

    /* Stack-allocated receive buffer - sized for jumbo frames */
    uint8_t buffer[TLNK_MTU];

    while (m_running.load()) {
        struct sockaddr_in sender;
        int senderLen = sizeof(sender);

        int received = recvfrom(m_socket, (char*)buffer, sizeof(buffer), 0,
                                (struct sockaddr*)&sender, &senderLen);

        if (received <= 0) {
            /* Timeout or error - just loop */
            continue;
        }

        if (received < TLNK_HEADER_SIZE) {
            continue; /* Packet too small for header */
        }

        const TlnkFrameHeader* header = reinterpret_cast<const TlnkFrameHeader*>(buffer);

        /* Validate magic */
        if (header->magic != TLNK_MAGIC) {
            continue;
        }

        /* Validate payload size */
        uint32_t payloadSize = static_cast<uint32_t>(received) - TLNK_HEADER_SIZE;
        if (payloadSize != header->payload_size) {
            LOG_WARN("FrameReceiver: payload size mismatch: got %u, header says %u",
                     payloadSize, header->payload_size);
            continue;
        }

        const uint8_t* payload = buffer + TLNK_HEADER_SIZE;

        /* Insert into frame buffer for reassembly */
        m_frameBuffer->InsertPacket(*header, payload);

        if (m_perfMon) {
            m_perfMon->PacketReceived(static_cast<uint32_t>(received));
        }

        /* Periodically evict stale incomplete frames */
        m_evictionCounter++;
        if (m_evictionCounter >= EVICTION_INTERVAL) {
            m_evictionCounter = 0;
            m_frameBuffer->EvictStaleFrames(TLNK_FRAME_TIMEOUT_MS);
        }
    }

    LOG_INFO("FrameReceiver: receive thread exiting");
}

} // namespace thunderlink
