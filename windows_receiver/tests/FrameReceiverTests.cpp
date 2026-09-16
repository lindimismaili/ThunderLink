#include "Network/FrameReceiver.h"
#include "Network/TcpTransport.h"
#include "Utils/FrameBuffer.h"
#include "Utils/PerformanceMonitor.h"
#include "Utils/Logger.h"
#include "App/Config.h"
#include "frame_header.h"
#include <cassert>
#include <cstring>
using namespace thunderlink;
int main() {
    assert(tlnk_init_sockets() == 0);
    Logger::SetLevel(LogLevel::Fatal);
    auto sender = tlnk_create_udp_socket();
    auto address = tlnk_make_addr("127.0.0.1", 0);
    assert(bind(sender, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    SocketLength length = sizeof(address);
    assert(getsockname(sender, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    GetConfig().bindIP = GetConfig().peerIP = "127.0.0.1";
    GetConfig().videoPort = ntohs(address.sin_port);
    tlnk_close_socket(sender);
    sender = tlnk_create_udp_socket();
    std::atomic<int64_t> milliseconds{0};
    FrameBuffer pool(2, 32, 32, nullptr, [&] {
        return NetClock::time_point{} + std::chrono::milliseconds(milliseconds.load());
    });
    PerformanceMonitor stats;
    FrameReceiver receiver(&pool, &stats);
    pool.SetSession(1);
    std::atomic<unsigned> lost{0};
    pool.SetOnFrameLost([&] { ++lost; });
    assert(receiver.Start());
    // Golden little-endian datagram with only the first of two chunks.
    std::vector<uint8_t> packet(TLNK_HEADER_SIZE + TLNK_MAX_PAYLOAD, 0);
    auto write = [&](size_t offset, uint64_t value, size_t size) {
        for (size_t i = 0; i < size; ++i) packet[offset + i] = uint8_t(value >> (8*i));
    };
    write(0, TLNK_MAGIC, 4); write(4, 1, 8); write(16, 2, 4);
    write(20, 1400, 4); write(24, 1401, 4); write(28, 32, 4); write(32, 32, 4);
    write(36, TLNK_FORMAT_H264, 2); write(40, 1, 8);
    assert(sendto(sender, reinterpret_cast<char*>(packet.data()), int(packet.size()), 0,
                  reinterpret_cast<sockaddr*>(&address), sizeof(address)) == int(packet.size()));
    auto deadline = NetClock::now() + std::chrono::seconds(2);
    while (!stats.GetStats().packetsReceived && NetClock::now() < deadline) std::this_thread::yield();
    assert(stats.GetStats().packetsReceived == 1);
    milliseconds = 50; // No further datagrams: the receive worker must evict on its idle poll.
    while (!lost && NetClock::now() < deadline) std::this_thread::yield();
    assert(lost == 1);
    ReceivedFrame frame;
    assert(!pool.WaitForFrame(&frame, 0));
    receiver.Stop();
    tlnk_close_socket(sender); tlnk_cleanup_sockets();
}
