#include "FrameBuffer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using thunderlink::DecodeFrameHeader;
using thunderlink::FrameBuffer;
using thunderlink::FrameMetadata;
using thunderlink::ReceivedFrame;

[[noreturn]] void Fail(const char* expression, int line) {
    std::cerr << "assertion failed at line " << line << ": " << expression << '\n';
    std::exit(1);
}

#define EXPECT(expr) do { if (!(expr)) Fail(#expr, __LINE__); } while (false)

FrameMetadata CompressedPacket(uint64_t sessionId, uint64_t frameId,
                               uint32_t frameBytes, uint32_t packetSeq) {
    FrameMetadata header{};
    header.magic = TLNK_MAGIC;
    header.frameId = frameId;
    header.packetSeq = packetSeq;
    header.totalPackets = (frameBytes + TLNK_MAX_PAYLOAD - 1) / TLNK_MAX_PAYLOAD;
    header.payloadSize = packetSeq + 1 == header.totalPackets
        ? frameBytes - packetSeq * TLNK_MAX_PAYLOAD
        : TLNK_MAX_PAYLOAD;
    header.frameTotalBytes = frameBytes;
    header.width = 1920;
    header.height = 1080;
    header.format = TLNK_FORMAT_H264;
    header.flags = TLNK_FLAG_KEY_FRAME;
    header.sessionId = sessionId;
    return header;
}

void TestGoldenWireHeader() {
    std::ifstream file(std::string(TLNK_TEST_SOURCE_DIR) +
                       "/common/fixtures/frame_header_v3.hex");
    EXPECT(file.good());
    std::string hex((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string digits;
    for (unsigned char c : hex) {
        if (std::isxdigit(c)) digits.push_back(static_cast<char>(c));
    }
    EXPECT(digits.size() == TLNK_HEADER_SIZE * 2U);
    std::vector<uint8_t> fixture;
    fixture.reserve(TLNK_HEADER_SIZE);
    for (size_t i = 0; i < digits.size(); i += 2) {
        fixture.push_back(static_cast<uint8_t>(std::stoi(digits.substr(i, 2), nullptr, 16)));
    }
    FrameMetadata header{};
    EXPECT(DecodeFrameHeader(fixture.data(), fixture.size(), &header));
    EXPECT(!DecodeFrameHeader(fixture.data(), fixture.size() - 1, &header));
    EXPECT(header.magic == TLNK_MAGIC);
    EXPECT(header.frameId == 0x0102030405060708ULL);
    EXPECT(header.packetSeq == 2);
    EXPECT(header.totalPackets == 3);
    EXPECT(header.payloadSize == 1400);
    EXPECT(header.frameTotalBytes == 4200);
    EXPECT(header.width == 1920 && header.height == 1080);
    EXPECT(header.format == TLNK_FORMAT_H264 && header.flags == TLNK_FLAG_KEY_FRAME);
    EXPECT(header.sessionId == 0x1020304050607080ULL);
    std::vector<uint8_t> unaligned(fixture.size() + 1);
    std::memcpy(unaligned.data() + 1, fixture.data(), fixture.size());
    EXPECT(DecodeFrameHeader(unaligned.data() + 1, fixture.size(), &header));
    EXPECT(header.frameId == 0x0102030405060708ULL);
}

void TestStrictPacketValidation() {
    FrameBuffer buffer(2, TLNK_MAX_WIDTH, TLNK_MAX_HEIGHT);
    buffer.SetSession(0x11);
    std::array<uint8_t, TLNK_MAX_PAYLOAD> bytes{};
    auto first = CompressedPacket(0x11, 1, TLNK_MAX_PAYLOAD + 1, 0);
    auto last = CompressedPacket(0x11, 1, TLNK_MAX_PAYLOAD + 1, 1);

    auto mismatchedCount = first;
    mismatchedCount.totalPackets = 3;
    EXPECT(!buffer.InsertPacket(mismatchedCount, bytes.data()));

    auto shortIntermediate = first;
    shortIntermediate.payloadSize = 1;
    EXPECT(!buffer.InsertPacket(shortIntermediate, bytes.data()));

    auto oddCompressed = first;
    oddCompressed.width = 1919;
    EXPECT(!buffer.InsertPacket(oddCompressed, bytes.data()));

    auto inconsistent = first;
    EXPECT(buffer.InsertPacket(first, bytes.data()));
    inconsistent.width = 1280;
    EXPECT(!buffer.InsertPacket(inconsistent, bytes.data()));
    bytes[0] = 0xa5;
    EXPECT(buffer.InsertPacket(last, bytes.data()));

    ReceivedFrame frame{};
    EXPECT(buffer.WaitForFrame(&frame, 0));
    EXPECT(frame.size == TLNK_MAX_PAYLOAD + 1 && frame.data[0] == 0);
    EXPECT(frame.data[TLNK_MAX_PAYLOAD] == 0xa5);
    buffer.ReleaseFrame(frame.sessionId, frame.frameId);

    auto bogusRaw = CompressedPacket(0x11, 2, 16, 0);
    bogusRaw.format = TLNK_FORMAT_BGRA32;
    bogusRaw.width = 2;
    bogusRaw.height = 2;
    EXPECT(buffer.InsertPacket(bogusRaw, bytes.data()));
    EXPECT(buffer.WaitForFrame(&frame, 0));
    buffer.ReleaseFrame(frame.sessionId, frame.frameId);
    bogusRaw.frameId = 3;
    bogusRaw.frameTotalBytes = 15;
    bogusRaw.payloadSize = 15;
    EXPECT(!buffer.InsertPacket(bogusRaw, bytes.data()));

    buffer.SetSession(0x12, 1920, 1080, TLNK_FORMAT_HEVC);
    auto negotiated = CompressedPacket(0x12, 1, 1, 0);
    EXPECT(!buffer.InsertPacket(negotiated, bytes.data()));
    negotiated.format = TLNK_FORMAT_HEVC;
    negotiated.width = 1280;
    EXPECT(!buffer.InsertPacket(negotiated, bytes.data()));
    negotiated.width = 1920;
    EXPECT(buffer.InsertPacket(negotiated, bytes.data()));
}

void TestSessionRestartAndStaleRelease() {
    FrameBuffer buffer(1, TLNK_MAX_WIDTH, TLNK_MAX_HEIGHT);
    std::array<uint8_t, 1> byte{{0x7f}};
    buffer.SetSession(1);
    auto old = CompressedPacket(1, 100, 1, 0);
    EXPECT(buffer.InsertPacket(old, byte.data()));
    ReceivedFrame held{};
    EXPECT(buffer.WaitForFrame(&held, 0));

    buffer.SetSession(2);
    auto restarted = CompressedPacket(2, 1, 1, 0);
    EXPECT(!buffer.InsertPacket(restarted, byte.data()));
    buffer.ReleaseFrame(1, 100);
    EXPECT(buffer.InsertPacket(restarted, byte.data()));
    ReceivedFrame frame{};
    EXPECT(buffer.WaitForFrame(&frame, 0));
    EXPECT(frame.sessionId == 2 && frame.frameId == 1);
    buffer.ReleaseFrame(1, 100); // Delayed duplicate release cannot free session 2.
    EXPECT(!buffer.InsertPacket(CompressedPacket(2, 2, 1, 0), byte.data()));
    buffer.ReleaseFrame(frame.sessionId, frame.frameId);
}

void TestIdleExpiryWithFakeClock() {
    FrameBuffer::TimePoint now{};
    FrameBuffer buffer(2, TLNK_MAX_WIDTH, TLNK_MAX_HEIGHT, nullptr, [&now] { return now; });
    std::atomic<uint32_t> losses{0};
    buffer.SetOnFrameLost([&losses] { ++losses; });
    buffer.SetSession(9);
    std::array<uint8_t, TLNK_MAX_PAYLOAD> payload{};
    EXPECT(buffer.InsertPacket(CompressedPacket(9, 1, TLNK_MAX_PAYLOAD + 1, 0),
                               payload.data()));
    now += std::chrono::milliseconds(TLNK_FRAME_TIMEOUT_MS);
    buffer.EvictStaleFrames();
    EXPECT(losses == 1);
    ReceivedFrame frame{};
    EXPECT(!buffer.WaitForFrame(&frame, 0));
}

void TestAdversarialBoundsAndReordering() {
    FrameBuffer buffer(2, TLNK_MAX_WIDTH, TLNK_MAX_HEIGHT);
    buffer.SetSession(1);
    std::array<uint8_t, TLNK_MAX_PAYLOAD> payload{};
    auto valid = CompressedPacket(1, 1, 2805, 0);
    for (unsigned fault = 0; fault < 10; ++fault) {
        auto bad = valid;
        switch (fault) {
        case 0: bad.totalPackets = UINT32_MAX; break;
        case 1: bad.packetSeq = UINT32_MAX; break;
        case 2: bad.frameTotalBytes = TLNK_MAX_WIDTH * TLNK_MAX_HEIGHT * 4 + 1; break;
        case 3: bad.width = TLNK_MAX_WIDTH + 1; break;
        case 4: bad.flags = 0x8000; break;
        case 5: bad.magic = 0; break;
        case 6: bad.sessionId = 2; break;
        case 7: bad.frameId = 0; break;
        case 8: bad.format = UINT16_MAX; break;
        case 9: bad.payloadSize = 0; break;
        }
        EXPECT(!buffer.InsertPacket(bad, payload.data()));
    }
    auto last = CompressedPacket(1, 1, 2805, 2);
    auto shortLast = last; --shortLast.payloadSize;
    EXPECT(!buffer.InsertPacket(shortLast, payload.data()));
    EXPECT(buffer.InsertPacket(last, payload.data()));
    EXPECT(buffer.InsertPacket(last, payload.data()));
    EXPECT(buffer.InsertPacket(valid, payload.data()));
    ReceivedFrame frame{};
    EXPECT(!buffer.WaitForFrame(&frame, 0));
    EXPECT(buffer.InsertPacket(CompressedPacket(1, 1, 2805, 1), payload.data()));
    EXPECT(buffer.WaitForFrame(&frame, 0));
    EXPECT(frame.size == 2805);
    buffer.ReleaseFrame(1, 1);
    EXPECT(!buffer.InsertPacket(last, payload.data()));

    // Validate the largest allocation and its exact final chunk under ASan.
    const uint32_t maximum = TLNK_MAX_WIDTH * TLNK_MAX_HEIGHT * 4;
    auto header = CompressedPacket(1, 2, maximum, 0);
    for (uint32_t sequence = 0; sequence < header.totalPackets; ++sequence) {
        auto packet = CompressedPacket(1, 2, maximum, sequence);
        EXPECT(buffer.InsertPacket(packet, payload.data()));
    }
    EXPECT(buffer.WaitForFrame(&frame, 0) && frame.size == maximum);
    buffer.ReleaseFrame(1, 2);
    uint32_t lost = 0;
    buffer.SetOnFrameLost([&] { EXPECT(buffer.GetSession() == 1); ++lost; });
    EXPECT(buffer.InsertPacket(CompressedPacket(1, 3, 1401, 0), payload.data()));
    EXPECT(buffer.InsertPacket(CompressedPacket(1, 4, 1401, 0), payload.data()));
    EXPECT(buffer.InsertPacket(CompressedPacket(1, 5, 1401, 0), payload.data()));
    EXPECT(lost == 1); // Exhausted pending slots are released before the callback.
}

void TestConcurrentAllocationAndRelease() {
    FrameBuffer buffer(4, TLNK_MAX_WIDTH, TLNK_MAX_HEIGHT);
    buffer.SetSession(77);
    std::atomic<bool> producerDone{false};
    std::atomic<uint32_t> received{0};

    std::thread consumer([&] {
        while (!producerDone || received.load() < 200) {
            ReceivedFrame frame{};
            if (buffer.WaitForFrame(&frame, 5)) {
                EXPECT(frame.sessionId == 77);
                buffer.ReleaseFrame(frame.sessionId, frame.frameId);
                ++received;
            }
        }
    });
    std::thread producer([&] {
        std::array<uint8_t, 1> payload{{0x3c}};
        for (uint64_t frameId = 1; frameId <= 200; ++frameId) {
            const auto header = CompressedPacket(77, frameId, 1, 0);
            while (!buffer.InsertPacket(header, payload.data())) {
                std::this_thread::yield();
            }
        }
        producerDone = true;
    });
    producer.join();
    consumer.join();
    EXPECT(received == 200);
}

} // namespace

int main() {
    TestGoldenWireHeader();
    TestStrictPacketValidation();
    TestSessionRestartAndStaleRelease();
    TestIdleExpiryWithFakeClock();
    TestAdversarialBoundsAndReordering();
    TestConcurrentAllocationAndRelease();
    std::cout << "FrameBufferTests passed\n";
    return 0;
}
