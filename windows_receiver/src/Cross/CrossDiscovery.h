#pragma once

#include "CrossTransport.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace thunderlink {

class CrossDiscovery {
public:
    struct State;

    CrossDiscovery();
    ~CrossDiscovery();
    CrossDiscovery(const CrossDiscovery&) = delete;
    CrossDiscovery& operator=(const CrossDiscovery&) = delete;

    bool Start(const std::string& interfaceId, std::string& error);
    void Stop();
    std::vector<CrossPeer> Peers() const;

private:
    mutable std::mutex m_lifecycleMutex;
    std::shared_ptr<State> m_state;
};

} // namespace thunderlink
