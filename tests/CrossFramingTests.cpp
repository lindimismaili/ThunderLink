#include "cross_framing.h"
#include <cassert>
#include <iostream>
#include <limits>

using thunderlink::cross::JsonFramer;
using Bytes = std::vector<unsigned char>;
static Bytes frame(const std::string& text) {
    const auto n = static_cast<uint32_t>(text.size());
    Bytes bytes{static_cast<unsigned char>(n >> 24), static_cast<unsigned char>(n >> 16),
                static_cast<unsigned char>(n >> 8), static_cast<unsigned char>(n)};
    bytes.insert(bytes.end(), text.begin(), text.end());
    return bytes;
}
int main() {
    const auto enter = frame("{\"type\":\"enter\"}");
    const auto input = frame("{\"type\":\"input\"}");
    const auto leave = frame("{\"type\":\"leave\"}");
    Bytes stream = enter;
    stream.insert(stream.end(), input.begin(), input.end());
    stream.insert(stream.end(), leave.begin(), leave.end());
    // Every split point, including the middle of a length prefix, preserves
    // enter/input/leave ordering without prematurely exposing partial frames.
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        JsonFramer decoder;
        std::vector<std::string> messages;
        std::string error;
        assert(decoder.Append(stream.data(), split, messages, error, 100));
        assert(decoder.Append(stream.data() + split, stream.size() - split, messages, error, 200));
        assert((messages == std::vector<std::string>{"{\"type\":\"enter\"}", "{\"type\":\"input\"}", "{\"type\":\"leave\"}"}));
        assert(decoder.CheckDeadline(10000, error));
    }
    for (const Bytes& malformed : {Bytes{0, 0, 0, 0}, Bytes{0, 1, 0, 1}, Bytes{255, 255, 255, 255}}) {
        JsonFramer decoder; std::vector<std::string> messages; std::string error;
        assert(!decoder.Append(malformed.data(), malformed.size(), messages, error, 0));
        assert(!error.empty() && messages.empty());
    }
    JsonFramer slow; std::vector<std::string> messages; std::string error;
    assert(slow.Append(enter.data(), 1, messages, error, 0));
    assert(slow.Append(enter.data() + 1, 1, messages, error, 4999));
    assert(!slow.CheckDeadline(5000, error)); // a trickle does not extend the deadline
    assert(!slow.Append(enter.data() + 2, enter.size() - 2, messages, error, 5000));
    assert(messages.empty());
    JsonFramer progress;
    assert(progress.Append(enter.data(), 1, messages, error, 0));
    Bytes tail(enter.begin() + 1, enter.end()); tail.push_back(0);
    assert(progress.Append(tail.data(), tail.size(), messages, error, 4999));
    assert(progress.CheckDeadline(5000, error)); // new partial frame gets a fresh deadline
    assert(!progress.CheckDeadline(9999, error));
    JsonFramer maximum; messages.clear();
    const std::string payload(JsonFramer::MaxJson, 'x'); const auto maxFrame = frame(payload);
    assert(maximum.Append(maxFrame.data(), maxFrame.size(), messages, error, 0));
    assert(messages.size() == 1 && messages[0] == payload);
    JsonFramer bounded;
    assert(!bounded.Append(nullptr, std::numeric_limits<std::size_t>::max(), messages, error, 0));
    std::cout << "Cross framing tests passed\n";
}
