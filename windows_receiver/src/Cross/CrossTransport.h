#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thunderlink {

enum class CrossRouteClass {
    Thunderbolt,
    Ethernet,
    WiFi,
    Other,
};

struct CrossEndpoint {
    std::string host;
    std::string address;
    uint16_t port = 50004;
    std::string interfaceId;
    std::string interfaceName;
    CrossRouteClass routeClass = CrossRouteClass::Other;
    bool discovered = false;
};

struct CrossPeer {
    std::string id;
    std::string name;
    bool paired = false;
    std::vector<CrossEndpoint> endpoints;
};

struct CrossTransportOptions {
    // Empty means discovery-only. A manual IPv4, IPv6 (including a scope ID),
    // or DNS hostname is resolved in addition to discovered endpoints.
    std::string manualHost;
    uint16_t port = 50004;

    // Decimal Windows interface index. Empty selects a route automatically.
    std::string interfaceId;

    // Optional certificate SHA-256 pin selected by the user. If supplied, a
    // different TLS identity is rejected before any application data is used.
    std::string peerId;
    std::string deviceName;
};

struct CrossTransportSnapshot {
    bool running = false;
    bool connected = false;
    bool paired = false;
    bool pairing = false;
    std::string verificationCode;
    std::string peerId;
    std::string peerName;
    std::string interfaceName;
    double latencyMs = -1.0;
    std::string error;
    std::vector<CrossPeer> peers;
    // Active local adapters, also available when multicast discovery is
    // blocked. Only interfaceId/interfaceName/routeClass are populated.
    std::vector<CrossEndpoint> availableInterfaces;
};

// Windows is always the TLS client. All methods are thread-safe. Protocol
// messages returned by DrainMessages have already passed TLS, identity,
// pairing, framing and pre-pairing message gating.
class CrossTransport {
public:
    CrossTransport();
    ~CrossTransport();

    CrossTransport(const CrossTransport&) = delete;
    CrossTransport& operator=(const CrossTransport&) = delete;

    bool Start(const CrossTransportOptions& options);
    void Stop();

    // Enqueues one protocol-v4 JSON message. The queue is bounded at 256;
    // consecutive compatible mouse moves may be replaced by the newest move.
    bool Send(const nlohmann::json& message);
    bool ApprovePairing();
    bool ForgetDevice(const std::string& peerFingerprint);
    // Ends only the current TLS session. The worker immediately resumes its
    // authenticated endpoint selection and pairing-pin checks.
    void AbortConnection();

    CrossTransportSnapshot Snapshot() const;
    std::vector<nlohmann::json> DrainMessages();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace thunderlink
