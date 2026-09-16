#include "CrossController.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>

#include "Utils/Logger.h"
#include "protocol.h"

namespace thunderlink {
namespace {
std::string TypeFor(const InputEvent& event) {
    switch (event.type) {
        case TLNK_INPUT_MOUSE_MOVE: return "mouse_move";
        case TLNK_INPUT_MOUSE_BUTTON: return "mouse_button";
        case TLNK_INPUT_KEY: return "key";
        case TLNK_INPUT_SCROLL: return "scroll";
        case TLNK_INPUT_RESET: return "reset";
        default: return {};
    }
}
std::string StateFor(int state) { return state == TLNK_STATE_DOWN ? "down" : "up"; }
nlohmann::json ModifiersFor(uint8_t modifiers) {
    nlohmann::json result = nlohmann::json::array();
    if (modifiers & TLNK_MOD_SHIFT) result.push_back("shift");
    if (modifiers & TLNK_MOD_CTRL) result.push_back("ctrl");
    if (modifiers & TLNK_MOD_ALT) result.push_back("alt");
    if (modifiers & TLNK_MOD_META) result.push_back("meta");
    return result;
}
} // namespace

CrossController::CrossController() : m_input(*this) {}
CrossController::~CrossController() { Stop(); }

bool CrossController::Start(const CrossSettings& settings, const std::string& deviceName) {
    Stop();
    // Discovery and pairing stay available before a user has placed a Mac tile.
    // A layout is validated only when it contains both sides and is applied.
    const bool hasRemote = std::any_of(settings.layout.begin(), settings.layout.end(), [](const auto& display) { return display.remote; });
    const auto layoutError = settings.layout.empty() ? std::string{} : cross::ValidateLayout(settings.layout, settings.enabled || hasRemote);
    if (!layoutError.empty()) { std::lock_guard<std::mutex> lock(m_mutex); m_status = "Saved Cross arrangement needs review: " + layoutError; }
    CrossTransportOptions options{};
    options.manualHost = settings.manualHost; options.port = TLNK_PORT_CROSS;
    options.interfaceId = settings.interfaceId; options.peerId = settings.peerId; options.deviceName = deviceName;
    if (!m_transport.Start(options)) { std::lock_guard<std::mutex> lock(m_mutex); m_status = "Could not start Cross discovery and connection."; return false; }
    m_input.SetEmergencyShortcut(settings.emergencyShortcut);
    if (!m_input.Start()) { m_transport.Stop(); std::lock_guard<std::mutex> lock(m_mutex); m_status = "Could not start Windows Cross input capture."; return false; }
    { std::lock_guard<std::mutex> lock(m_mutex);
      m_settings = settings; m_layout = settings.layout; m_enabled = settings.enabled;
      m_running = true; m_ready = false; m_layoutApproved = false; m_configureInFlight = false;
      m_topologyRevision = 0; m_nextGeneration = m_activeGeneration = m_pendingGeneration = 0; m_enterAcknowledged = false;
      m_sessionId.clear(); m_status = "Discovering Macs on your local network."; }
    m_input.SetLayout(settings.layout);
    return true;
}

void CrossController::Stop() {
    bool wasRunning = false;
    { std::lock_guard<std::mutex> lock(m_mutex); wasRunning = m_running; m_running = false; m_ready = false; m_enabled = false; }
    if (wasRunning) Send({{"type", "stop"}});
    m_input.SetEnabled(false); m_input.Stop(); m_transport.Stop();
}

void CrossController::SetCurrentLocalTopology(std::vector<CrossSettings::TopologyDisplay> displays) {
    std::lock_guard<std::mutex> lock(m_mutex); m_currentLocalTopology = std::move(displays);
}

void CrossController::Tick() {
    if (!Running()) return;
    for (const auto& message : m_transport.DrainMessages()) HandleMessage(message);
    const auto snapshot = m_transport.Snapshot();
    if (!snapshot.error.empty()) { std::lock_guard<std::mutex> lock(m_mutex); m_status = snapshot.error; }
    if (!snapshot.connected) {
        bool wasReady = false;
        { std::lock_guard<std::mutex> lock(m_mutex); wasReady = m_ready || m_configureInFlight; m_ready = false; m_configureInFlight = false; m_sessionId.clear(); m_activeGeneration = m_pendingGeneration = 0; }
        if (wasReady) m_input.SetEnabled(false);
    }
    ConfigureIfReady();
}

bool CrossController::SetLayout(cross::Layout layout) {
    const auto error = cross::ValidateLayout(layout);
    const auto reset = Envelope("reset");
    bool matchesTopology = false;
    { std::lock_guard<std::mutex> lock(m_mutex);
      if (!error.empty()) { m_status = error; return false; }
      std::size_t remoteCount = 0;
      matchesTopology = !m_remoteDisplays.empty();
      for (const auto& display : layout) if (display.remote) {
          ++remoteCount;
          matchesTopology = matchesTopology && std::any_of(m_remoteDisplays.begin(), m_remoteDisplays.end(), [&](const auto& remote) { return remote.id == display.id; });
      }
      matchesTopology = matchesTopology && remoteCount == m_remoteDisplays.size();
      if (!matchesTopology) { m_status = "The Mac display list changed. Arrange every current Mac display before applying."; return false; }
      m_layout = std::move(layout); m_settings.layout = m_layout; m_ready = false; m_configureInFlight = false;
      m_sessionId.clear(); m_activeGeneration = m_pendingGeneration = 0; m_enterAcknowledged = false; m_layoutApproved = true; }
    if (reset.contains("session_id")) (void)m_transport.Send(reset);
    m_input.SetEnabled(false); m_input.SetLayout(m_layout); ConfigureIfReady(); return true;
}

void CrossController::RememberAppliedTopology(std::vector<CrossSettings::TopologyDisplay> remote,
                                               std::vector<CrossSettings::TopologyDisplay> local) {
    try {
        ValidateCrossTopology(remote);
        ValidateCrossTopology(local);
    } catch (...) {
        FailClosed("Could not retain the applied Cross display topology.");
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings.remoteTopology = std::move(remote);
    m_settings.localTopology = std::move(local);
}

void CrossController::SetEnabled(bool enabled) {
    const auto reset = enabled ? nlohmann::json{} : Envelope("reset");
    { std::lock_guard<std::mutex> lock(m_mutex);
      m_enabled = enabled; m_settings.enabled = enabled;
      if (!enabled) { m_activeGeneration = m_pendingGeneration = 0; m_enterAcknowledged = false; }
    }
    if (!enabled) { if (reset.contains("session_id")) (void)m_transport.Send(reset); m_input.SetEnabled(false); }
    else if (Ready()) m_input.SetEnabled(true);
    else ConfigureIfReady();
}

void CrossController::InvalidateLocalTopology(const char* reason) {
    const auto reset = Envelope("reset");
    { std::lock_guard<std::mutex> lock(m_mutex);
      m_ready = false; m_configureInFlight = false; m_layoutApproved = false; m_enabled = false; m_settings.enabled = false;
      m_sessionId.clear(); m_activeGeneration = m_pendingGeneration = 0;
      m_status = reason ? reason : "Windows displays changed. Review Cross arrangement."; }
    m_input.SetEnabled(false); (void)Send(reset);
}

bool CrossController::Enabled() const { std::lock_guard<std::mutex> lock(m_mutex); return m_enabled; }
bool CrossController::RemoteOwnership() const { std::lock_guard<std::mutex> lock(m_mutex); return m_activeGeneration != 0; }
bool CrossController::Ready() const { std::lock_guard<std::mutex> lock(m_mutex); return m_ready; }
bool CrossController::Running() const { std::lock_guard<std::mutex> lock(m_mutex); return m_running; }
CrossTransportSnapshot CrossController::Snapshot() const { return m_transport.Snapshot(); }
std::vector<CrossRemoteDisplay> CrossController::RemoteDisplays() const { std::lock_guard<std::mutex> lock(m_mutex); return m_remoteDisplays; }
std::vector<CrossSettings::TopologyDisplay> CrossController::RemoteTopology() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CrossSettings::TopologyDisplay> result;
    result.reserve(m_remoteDisplays.size());
    for (const auto& display : m_remoteDisplays) result.push_back({display.id, display.x, display.y, display.width, display.height, display.scale, display.rotation});
    return result;
}
uint64_t CrossController::TopologyRevision() const { std::lock_guard<std::mutex> lock(m_mutex); return m_topologyRevision; }
std::string CrossController::Status() const { std::lock_guard<std::mutex> lock(m_mutex); return m_status; }
void CrossController::ApprovePairing() { (void)m_transport.ApprovePairing(); }
void CrossController::ForgetDevice(const std::string& fingerprint) { (void)m_transport.ForgetDevice(fingerprint); }
void CrossController::IdentifyRemoteDisplays() { (void)Send({{"type", "identify"}}); }

void CrossController::ConfigureIfReady() {
    CrossTransportSnapshot snapshot = m_transport.Snapshot();
    std::string session; uint64_t revision = 0; bool configure = false;
    { std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_running || !m_layoutApproved || m_layout.empty() || !snapshot.paired || !m_topologyRevision || m_ready || m_configureInFlight) return;
      m_sessionId = NewSessionId(); if (m_sessionId.empty()) { m_status = "Could not generate a Cross session ID."; return; }
      session = m_sessionId; revision = m_topologyRevision; configure = true; m_configureInFlight = true;
      m_status = "Configuring Cross input."; }
    if (configure && !Send({{"type", "configure"}, {"session_id", session}, {"topology_revision", revision},
                             {"swap_ctrl_cmd", m_settings.swapCtrlCmd}})) ReleaseForReconnect("Cross connection was interrupted; waiting to reconnect.");
}

void CrossController::HandleMessage(const nlohmann::json& message) {
    try { HandleMessageValidated(message); }
    catch (...) { FailClosed("Mac sent a malformed Cross control message."); }
}

void CrossController::HandleMessageValidated(const nlohmann::json& message) {
    if (!message.is_object()) throw std::invalid_argument("not object");
    const auto type = message.value("type", std::string{});
    if (type.empty() || type.size() > 64) throw std::invalid_argument("type");
    if (type == "topology") {
        // A new topology invalidates any current ownership. Keep the old
        // envelope so the Mac can release it before we configure the revision.
        const auto reset = Envelope("reset");
        if (!message.contains("revision") || !message.at("revision").is_number_unsigned() ||
            !message.contains("displays") || !message.at("displays").is_array()) throw std::invalid_argument("topology");
        const auto revision = message.value("revision", uint64_t{0});
        std::vector<CrossRemoteDisplay> displays;
        try {
            std::unordered_set<std::string> ids;
            for (const auto& item : message.at("displays")) {
                CrossRemoteDisplay display{}; display.id = item.at("id").get<std::string>();
                display.name = item.value("name", display.id); display.x = item.at("x").get<double>(); display.y = item.at("y").get<double>();
                display.width = item.at("width").get<double>(); display.height = item.at("height").get<double>();
                display.scale = item.value("scale", 1.0); display.rotation = item.value("rotation", 0); display.primary = item.value("primary", false);
                if (display.id.empty() || display.id.size() > 256 || !ids.insert(display.id).second || display.name.size() > 256 ||
                    display.width <= 0 || display.height <= 0 || display.scale <= 0 || display.scale > 16 ||
                    !std::isfinite(display.x) || !std::isfinite(display.y) || !std::isfinite(display.width) || !std::isfinite(display.height) || !std::isfinite(display.scale) ||
                    std::abs(display.x) > 1e7 || std::abs(display.y) > 1e7 || display.rotation % 90 != 0) throw std::invalid_argument("invalid display");
                displays.push_back(std::move(display));
            }
        } catch (...) { FailClosed("Mac sent an invalid Cross display topology."); return; }
        if (!revision || displays.empty() || displays.size() > TLNK_CROSS_MAX_DISPLAYS) { FailClosed("Mac sent an invalid Cross display topology."); return; }
        const auto snapshot = m_transport.Snapshot();
        bool reuseSavedLayout = !snapshot.peerId.empty();
        { std::lock_guard<std::mutex> lock(m_mutex);
          reuseSavedLayout = reuseSavedLayout && snapshot.peerId == m_settings.peerId;
          std::size_t savedRemote = 0;
          for (const auto& tile : m_layout) if (tile.remote) {
              ++savedRemote;
              reuseSavedLayout = reuseSavedLayout && std::any_of(displays.begin(), displays.end(), [&](const auto& current) { return current.id == tile.id; });
          }
          reuseSavedLayout = reuseSavedLayout && savedRemote == displays.size() && m_settings.remoteTopology.size() == displays.size() &&
              m_settings.localTopology.size() == m_currentLocalTopology.size() && !m_layout.empty();
          for (const auto& current : displays) {
              const auto previous = std::find_if(m_settings.remoteTopology.begin(), m_settings.remoteTopology.end(), [&](const auto& saved) { return saved.id == current.id; });
              reuseSavedLayout = reuseSavedLayout && previous != m_settings.remoteTopology.end() &&
                  std::abs(previous->x - current.x) < 0.01 && std::abs(previous->y - current.y) < 0.01 &&
                  std::abs(previous->width - current.width) < 0.01 && std::abs(previous->height - current.height) < 0.01 &&
                  std::abs(previous->scale - current.scale) < 0.001 && previous->rotation == current.rotation;
          }
          for (const auto& current : m_currentLocalTopology) {
              const auto previous = std::find_if(m_settings.localTopology.begin(), m_settings.localTopology.end(), [&](const auto& saved) { return saved.id == current.id; });
              reuseSavedLayout = reuseSavedLayout && previous != m_settings.localTopology.end() &&
                  std::abs(previous->x - current.x) < 0.01 && std::abs(previous->y - current.y) < 0.01 &&
                  std::abs(previous->width - current.width) < 0.01 && std::abs(previous->height - current.height) < 0.01 &&
                  std::abs(previous->scale - current.scale) < 0.001 && previous->rotation == current.rotation;
          }
          m_topologyRevision = revision; m_remoteDisplays = std::move(displays); m_ready = false;
          m_configureInFlight = false; m_layoutApproved = reuseSavedLayout; m_sessionId.clear(); m_activeGeneration = m_pendingGeneration = 0; m_enterAcknowledged = false;
          m_status = reuseSavedLayout ? "Reusing the saved Cross display arrangement." : "Review the Cross display arrangement."; }
        m_input.SetEnabled(false);
        if (reset.contains("session_id")) (void)m_transport.Send(reset);
        return;
    }
    if (type == "ready") {
        if (!message.contains("session_id") || !message.at("session_id").is_string() || !message.contains("topology_revision") || !message.at("topology_revision").is_number_unsigned()) throw std::invalid_argument("ready");
        const auto session = message.value("session_id", std::string{}); const auto revision = message.value("topology_revision", uint64_t{0});
        bool activate = false;
        { std::lock_guard<std::mutex> lock(m_mutex); if (session == m_sessionId && revision == m_topologyRevision && m_configureInFlight) { m_ready = true; m_configureInFlight = false; activate = m_enabled; m_status = "Cross is ready. Move beyond a connected screen edge."; } }
        if (activate) m_input.SetEnabled(true); return;
    }
    if (type == "entered") {
        if (!message.contains("generation") || !message.at("generation").is_number_unsigned() || !message.contains("session_id") || !message.at("session_id").is_string() || !message.contains("topology_revision") || !message.at("topology_revision").is_number_unsigned()) throw std::invalid_argument("entered");
        const auto generation = message.value("generation", uint64_t{0}); std::string display;
        const auto session = message.value("session_id", std::string{}); const auto revision = message.value("topology_revision", uint64_t{0});
        { std::lock_guard<std::mutex> lock(m_mutex); if (!m_ready || !m_enabled || !generation || m_enterAcknowledged || session != m_sessionId || revision != m_topologyRevision || !m_pendingGeneration || generation != m_pendingGeneration) return; m_activeGeneration = generation; m_enterAcknowledged = true; display = m_pendingDisplay; m_status = "Windows mouse and keyboard are controlling your Mac."; }
        m_input.SetRemoteOwnership(generation, display); return;
    }
    if (type == "local") {
        if (!message.contains("generation") || !message.at("generation").is_number_unsigned() || !message.contains("session_id") || !message.at("session_id").is_string() || !message.contains("topology_revision") || !message.at("topology_revision").is_number_unsigned()) throw std::invalid_argument("local");
        const auto generation = message.value("generation", uint64_t{0}); const auto session = message.value("session_id", std::string{}); const auto revision = message.value("topology_revision", uint64_t{0});
        { std::lock_guard<std::mutex> lock(m_mutex); const auto expected = m_activeGeneration ? m_activeGeneration : m_pendingGeneration; if (session != m_sessionId || revision != m_topologyRevision || !expected || generation != expected) return; m_activeGeneration = m_pendingGeneration = 0; m_enterAcknowledged = false; m_status = "Windows input is local."; }
        m_input.PeerReturnedLocal(); return;
    }
    if (type == "pong") {
        if (!message.contains("sequence") || !message.at("sequence").is_number_unsigned()) throw std::invalid_argument("pong");
        const auto sequence = message.value("sequence", uint64_t{0});
        bool current = false;
        { std::lock_guard<std::mutex> lock(m_mutex); current = m_ready && sequence && sequence == m_lastPingSequence; }
        if (current) m_input.NotifyPeerLiveness();
        return;
    }
    if (type == "error") { FailClosed(message.value("reason", std::string{"Mac rejected Cross input."}).c_str()); return; }
}

bool CrossController::Send(const nlohmann::json& message) {
    if (m_transport.Send(message)) return true;
    m_input.Abort(); return false;
}

nlohmann::json CrossController::Envelope(const char* type) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    nlohmann::json result{{"type", type}};
    if (!m_sessionId.empty()) { result["session_id"] = m_sessionId; result["topology_revision"] = m_topologyRevision; }
    return result;
}

std::string CrossController::NewSessionId() {
    std::array<unsigned char, 8> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    if (std::all_of(random.begin(), random.end(), [](unsigned char value) { return value == 0; })) return {};
    char text[17]{}; for (size_t i = 0; i < random.size(); ++i) snprintf(text + i * 2, 3, "%02x", random[i]); return text;
}

void CrossController::FailClosed(const char* reason) {
    const auto reset = Envelope("reset");
    // Reset must retain the old session envelope so the Mac can release held
    // keys/buttons before this controller invalidates local ownership.
    if (reset.contains("session_id")) (void)m_transport.Send(reset);
    m_input.SetEnabled(false);
    { std::lock_guard<std::mutex> lock(m_mutex);
      // A rejected/malformed/permission failure needs an explicit Apply (or a
      // reviewed new topology), not an unbounded configure loop in Tick.
      m_ready = false; m_configureInFlight = false; m_layoutApproved = false;
      m_enabled = false; m_settings.enabled = false; m_sessionId.clear();
      m_activeGeneration = m_pendingGeneration = 0; m_enterAcknowledged = false;
      m_status = reason ? reason : "Cross returned input to Windows."; }
}

void CrossController::ReleaseForReconnect(const char* reason) {
    const auto reset = Envelope("reset");
    // The worker owns fail-open cursor restoration and runs without the
    // dashboard, so stop forwarding before the old TLS session is discarded.
    m_input.SetEnabled(false);
    { std::lock_guard<std::mutex> lock(m_mutex);
      m_ready = false; m_configureInFlight = false; m_sessionId.clear();
      m_activeGeneration = m_pendingGeneration = 0; m_enterAcknowledged = false;
      m_status = reason ? reason : "Cross connection was interrupted; waiting to reconnect."; }
    if (reset.contains("session_id")) (void)m_transport.Send(reset);
    m_transport.AbortConnection();
}

void CrossController::CrossEdgeEntered(const std::string& displayId, double x, double y) {
    uint64_t generation = 0;
    nlohmann::json message;
    { std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_ready || !m_enabled || m_sessionId.empty()) return;
      generation = ++m_nextGeneration; if (!generation) generation = ++m_nextGeneration;
      m_pendingGeneration = generation; m_activeGeneration = 0; m_enterAcknowledged = false; m_pendingDisplay = displayId; m_status = "Moving input onto your Mac.";
      message = {{"type", "enter"}, {"session_id", m_sessionId}, {"topology_revision", m_topologyRevision},
                 {"generation", generation}, {"display_id", displayId}, {"x", x}, {"y", y}}; }
    if (!Send(message)) ReleaseForReconnect("Could not enter the Mac display; waiting to reconnect.");
}

void CrossController::CrossInput(const CrossInputEvent& crossEvent) {
    nlohmann::json message; std::string session; uint64_t revision = 0, generation = 0;
    { std::lock_guard<std::mutex> lock(m_mutex); if (!m_ready || !m_enabled || !m_activeGeneration || m_activeGeneration != m_pendingGeneration) return;
      session = m_sessionId; revision = m_topologyRevision; generation = m_activeGeneration; }
    const auto& event = crossEvent.event; const auto type = TypeFor(event); if (type.empty()) return;
    nlohmann::json payload{{"type", type}, {"session_id", session}};
    if (event.type == TLNK_INPUT_MOUSE_MOVE || event.type == TLNK_INPUT_MOUSE_BUTTON) { payload["x"] = event.x; payload["y"] = event.y; }
    if (event.type == TLNK_INPUT_MOUSE_BUTTON) { payload["button"] = event.button; payload["state"] = StateFor(event.state); }
    if (event.type == TLNK_INPUT_KEY) { payload["key_code"] = event.keyCode; payload["state"] = StateFor(event.state); payload["modifiers"] = ModifiersFor(event.modifiers); }
    if (event.type == TLNK_INPUT_SCROLL) { payload["delta_x"] = event.deltaX; payload["delta_y"] = event.deltaY; }
    message = {{"type", "input"}, {"session_id", session}, {"topology_revision", revision}, {"generation", generation}, {"event", std::move(payload)}};
    if (event.type == TLNK_INPUT_MOUSE_MOVE || event.type == TLNK_INPUT_MOUSE_BUTTON) message["display_id"] = crossEvent.displayId;
    if (!Send(message)) ReleaseForReconnect("Cross input connection closed; Windows input restored while reconnecting.");
}

void CrossController::CrossReturnRequested() {
    auto message = Envelope("leave"); uint64_t generation = 0;
    { std::lock_guard<std::mutex> lock(m_mutex); generation = m_activeGeneration ? m_activeGeneration : m_pendingGeneration; m_status = "Returning input to Windows."; }
    if (generation) {
        message["generation"] = generation;
        if (!Send(message)) ReleaseForReconnect("Cross connection was interrupted; waiting to reconnect.");
    }
}
void CrossController::CrossEmergencyPause() { SetEnabled(false); { std::lock_guard<std::mutex> lock(m_mutex); m_status = "Cross paused by " + m_settings.emergencyShortcut + "."; } }
void CrossController::CrossLeasePing() {
    auto message = Envelope("ping");
    { std::lock_guard<std::mutex> lock(m_mutex); if (!m_ready || m_sessionId.empty()) return; message["sequence"] = ++m_lastPingSequence; }
    if (!Send(message)) ReleaseForReconnect("Cross connection was interrupted; waiting to reconnect.");
}
void CrossController::CrossLeaseExpired() { ReleaseForReconnect("Mac did not respond; Windows input restored while reconnecting."); }
void CrossController::CrossInputFault(const char* reason) { FailClosed(reason); }

} // namespace thunderlink
