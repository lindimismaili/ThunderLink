#include "Network/TcpTransport.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

using namespace thunderlink;

namespace {

std::atomic<bool> running{true};

struct ConnectedPair {
    ConnectedPair() = default;
    ConnectedPair(const ConnectedPair&) = delete;
    ConnectedPair(ConnectedPair&& other) noexcept
        : listener(std::exchange(other.listener, TLNK_INVALID_SOCKET)),
          client(std::exchange(other.client, TLNK_INVALID_SOCKET)),
          server(std::exchange(other.server, TLNK_INVALID_SOCKET)) {}
    tlnk_socket_t listener = TLNK_INVALID_SOCKET;
    tlnk_socket_t client = TLNK_INVALID_SOCKET;
    tlnk_socket_t server = TLNK_INVALID_SOCKET;

    ~ConnectedPair() {
        if (client != TLNK_INVALID_SOCKET) tlnk_close_socket(client);
        if (server != TLNK_INVALID_SOCKET) tlnk_close_socket(server);
        if (listener != TLNK_INVALID_SOCKET) tlnk_close_socket(listener);
    }
};

ConnectedPair MakePair() {
    ConnectedPair pair;
    pair.listener = tlnk_create_tcp_socket();
    assert(pair.listener != TLNK_INVALID_SOCKET);
    auto address = tlnk_make_addr("127.0.0.1", 0);
    assert(bind(pair.listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(pair.listener, 1) == 0);
    SocketLength length = sizeof(address);
    assert(getsockname(pair.listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);

    pair.client = tlnk_create_tcp_socket();
    assert(pair.client != TLNK_INVALID_SOCKET);
    assert(ConnectWithDeadline(pair.client, address, running));
    pair.server = accept(pair.listener, nullptr, nullptr);
    assert(pair.server != TLNK_INVALID_SOCKET);
    assert(tlnk_set_nonblocking(pair.server) == 0);
    return pair;
}

std::string ReceiveMessage(tlnk_socket_t socket) {
    FramedReader reader;
    const auto deadline = NetClock::now() + std::chrono::seconds(2);
    while (NetClock::now() < deadline) {
        std::string message;
        const auto status = reader.Poll(socket, message);
        if (status == ReadStatus::Message) return message;
        assert(status == ReadStatus::Idle);
    }
    assert(false && "timed out receiving framed message");
    return {};
}

void TestLargeMessageAndBackpressure() {
    auto pair = MakePair();
    int sendBuffer = 1024;
    (void)setsockopt(pair.client, SOL_SOCKET, SO_SNDBUF,
                     reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));
    const std::string source(64000, 'x');
    std::atomic<bool> sent{false};
    std::thread writer([&] {
        sent = SendFramed(pair.client, source, [] { return true; }, 2000);
    });

    /* Consume slowly enough that the sender must tolerate readiness changes
     * and may use more than one send call on small socket buffers. */
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto received = ReceiveMessage(pair.server);
    writer.join();
    assert(sent && received == source);
}

void TestCancelledWriteSendsNothing() {
    auto pair = MakePair();
    assert(!SendFramed(pair.client, "never-send", [] { return false; }, 50));
    assert(SocketReady(pair.server, false, 20) == 0);
}

void TestClosedAndMalformedReads() {
    auto pair = MakePair();
    const std::array<uint8_t, 2> partial{{0, 0}};
    assert(send(pair.client, reinterpret_cast<const char*>(partial.data()),
                static_cast<int>(partial.size()), 0) == static_cast<int>(partial.size()));
    tlnk_close_socket(pair.client);
    pair.client = TLNK_INVALID_SOCKET;

    FramedReader reader;
    std::string ignored;
    ReadStatus status = ReadStatus::Idle;
    const auto deadline = NetClock::now() + std::chrono::seconds(1);
    while (status == ReadStatus::Idle && NetClock::now() < deadline) {
        status = reader.Poll(pair.server, ignored);
    }
    assert(status == ReadStatus::Closed);

    FramedReader malformed;
    const std::array<uint8_t, 4> oversized{{0, 1, 0, 1}}; // 65,537 bytes
    assert(malformed.Feed(oversized.data(), oversized.size()) == ReadStatus::Malformed);
}

} // namespace

int main() {
    assert(tlnk_init_sockets() == 0);
    TestLargeMessageAndBackpressure();
    TestCancelledWriteSendsNothing();
    TestClosedAndMalformedReads();
    tlnk_cleanup_sockets();
    return 0;
}
