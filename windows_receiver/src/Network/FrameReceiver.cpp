#include "FrameReceiver.h"
#include "Utils/FrameBuffer.h"
#include "Utils/PerformanceMonitor.h"
#include "Utils/Logger.h"
#include "App/Config.h"

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

    const auto& cfg = GetConfig();
    struct sockaddr_in addr = tlnk_make_addr(cfg.bindIP.c_str(), cfg.videoPort);
    if (bind(m_socket, (struct sockaddr*)&addr, sizeof(addr)) == TLNK_SOCKET_ERROR) {
        LOG_ERROR("FrameReceiver: failed to bind to %s:%d (err=%d)",
                  cfg.bindIP.c_str(), cfg.videoPort, tlnk_get_error());
        tlnk_close_socket(m_socket);
        m_socket = TLNK_INVALID_SOCKET;
        return false;
    }

    /* Set receive timeout so the thread can check m_running */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; /* 100ms */
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    LOG_INFO("FrameReceiver: listening on %s:%d (UDP)", cfg.bindIP.c_str(), cfg.videoPort);

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

    /* Receive buffer sized to the max UDP datagram so a sender/receiver chunk-
     * size mismatch can never silently truncate a packet (recvfrom would drop
     * an oversized datagram with WSAEMSGSIZE). The sender chunks to
     * TLNK_MAX_PAYLOAD; this is just headroom. */
    uint8_t buffer[65536];

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

        if (!m_firstPacketLogged) {
            m_firstPacketLogged = true;
            LOG_INFO("FrameReceiver: first video packet received "
                     "(frame_id=%llu, total_packets=%u, %d bytes on wire)",
                     (unsigned long long)header->frame_id, header->total_packets, received);
        }

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
