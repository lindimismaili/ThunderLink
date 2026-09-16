#include "Decoder/DecoderQueue.h"
#include <cassert>
#include <atomic>
#include <thread>
using namespace thunderlink;
DecodedFrame Frame(uint64_t session, uint8_t value) {
    DecodedFrame frame;
    frame.sessionId = session;
    frame.storage = std::make_shared<const std::vector<uint8_t>>(6, value);
    frame.width = frame.height = frame.yStride = frame.uvStride = 2;
    frame.uvOffset = 4;
    return frame;
}
int main() {
    DecoderQueue queue;
    const uint8_t data[] = {1, 2, 3};
    queue.Start(1);
    const auto initialEpoch = queue.Epoch();
    assert(!queue.Submit(data, 3, false, 1));
    assert(!queue.Submit(data, 3, true, 2));
    assert(queue.Submit(data, 3, true, 1));
    for (unsigned i = 0; i < 3; ++i) assert(queue.Submit(data, 3, false, 1));
    assert(!queue.Submit(data, 3, false, 1)); // Overload discards dependent AUs.
    assert(queue.Waiting());
    queue.Publish(Frame(1, 7), initialEpoch); // A decoder event already in flight.
    DecodedFrame decoded;
    assert(!queue.TakeNewest(&decoded));
    assert(queue.TakeFlush().has_value());
    EncodedFrame encoded;
    assert(!queue.Pop(&encoded));
    assert(queue.Submit(data, 3, true, 1));
    assert(queue.Pop(&encoded) && encoded.bytes == std::vector<uint8_t>({1, 2, 3}));
    for (uint8_t i = 1; i <= 3; ++i) queue.Publish(Frame(1, i), queue.Epoch());
    assert(queue.TakeNewest(&decoded) && decoded.storage->at(0) == 3);
    const auto owned = decoded.storage;
    queue.Start(2);
    queue.Publish(Frame(1, 9), initialEpoch);
    assert(!queue.TakeNewest(&decoded));
    assert(owned->at(0) == 3); // Renderer ownership survives reset/new output.
    std::atomic<bool> done{false};
    std::thread consumer([&] {
        while (!done) {
            queue.TakeFlush();
            if (queue.Pop(&encoded)) {
                assert(encoded.sessionId == 2 && encoded.bytes.size() == 3);
                queue.Publish(Frame(2, encoded.bytes[0]), queue.Epoch());
            }
        }
    });
    for (unsigned i = 0; i < 2000; ++i) {
        assert(queue.Submit(data, 3, true, 2));
        queue.TakeNewest(&decoded);
    }
    done = true; consumer.join();
    queue.Stop();
    assert(!queue.Submit(data, 3, true, 2));
    assert(!queue.TakeNewest(&decoded));
}
