#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "CrossInputController.h"
#include "CrossSettings.h"
#include "CrossTransport.h"

namespace thunderlink {

struct CrossRemoteDisplay {
    std::string id;
    std::string name;
    double x = 0, y = 0, width = 0, height = 0;
    double scale = 1;
    int rotation = 0;
    bool primary = false;
};

/* Owns the protocol-v4 Cross lifecycle. It contains no video/capture/decoder
 * dependencies, so Cross can run on Windows N systems where Screen Share is
 * unavailable. */
class CrossController final : private CrossInputDelegate {
public:
    CrossController();
    ~CrossController();
    CrossController(const CrossController&) = delete;
    CrossController& operator=(const CrossController&) = delete;

    bool Start(const CrossSettings& settings, const std::string& deviceName);
    void SetCurrentLocalTopology(std::vector<CrossSettings::TopologyDisplay> displays);
    void Stop();
    void Tick();
    bool SetLayout(cross::Layout layout);
    // Retain the physical signatures that produced an applied virtual layout.
    // Exact matching signatures can be reused after a reconnect; anything else
    // requires a visible arrangement review.
    void RememberAppliedTopology(std::vector<CrossSettings::TopologyDisplay> remote,
                                std::vector<CrossSettings::TopologyDisplay> local);
    void SetEnabled(bool enabled);
    void InvalidateLocalTopology(const char* reason);
    bool Enabled() const;
    bool RemoteOwnership() const;
    bool Ready() const;
    bool Running() const;
    void ApprovePairing();
    void ForgetDevice(const std::string& fingerprint);
    void IdentifyRemoteDisplays();
    CrossTransportSnapshot Snapshot() const;
    std::vector<CrossRemoteDisplay> RemoteDisplays() const;
    std::vector<CrossSettings::TopologyDisplay> RemoteTopology() const;
    uint64_t TopologyRevision() const;
    std::string Status() const;

private:
    void ConfigureIfReady();
    void HandleMessage(const nlohmann::json& message);
    void HandleMessageValidated(const nlohmann::json& message);
    bool Send(const nlohmann::json& message);
    nlohmann::json Envelope(const char* type) const;
    std::string NewSessionId();
    void FailClosed(const char* reason);
    // A lost/black-holed transport gives input back immediately but preserves
    // the user's enabled preference for a fresh authenticated reconnect.
    void ReleaseForReconnect(const char* reason);

    // CrossInputDelegate: these are called from the input worker and enqueue
    // only; CrossTransport performs all network work away from hooks.
    void CrossEdgeEntered(const std::string& displayId, double x, double y) override;
    void CrossInput(const CrossInputEvent& event) override;
    void CrossReturnRequested() override;
    void CrossEmergencyPause() override;
    void CrossLeasePing() override;
    void CrossLeaseExpired() override;
    void CrossInputFault(const char* reason) override;

    mutable std::mutex m_mutex;
    CrossTransport m_transport;
    CrossInputController m_input;
    CrossSettings m_settings;
    cross::Layout m_layout;
    std::vector<CrossRemoteDisplay> m_remoteDisplays;
    std::vector<CrossSettings::TopologyDisplay> m_currentLocalTopology;
    uint64_t m_topologyRevision = 0;
    uint64_t m_nextGeneration = 0;
    uint64_t m_activeGeneration = 0;
    uint64_t m_pendingGeneration = 0;
    uint64_t m_lastPingSequence = 0;
    std::string m_sessionId;
    std::string m_pendingDisplay;
    bool m_running = false;
    bool m_ready = false;
    bool m_enabled = false;
    bool m_layoutApproved = false;
    bool m_configureInFlight = false;
    bool m_enterAcknowledged = false;
    std::string m_status;
};

} // namespace thunderlink
