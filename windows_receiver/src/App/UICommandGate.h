#pragma once

#include <deque>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace thunderlink::ui {

struct CommandDecision {
    enum class Kind { Accepted, Rejected, DuplicatePending, DuplicateResult };
    Kind kind = Kind::Rejected;
    std::string code;
    std::string requestId;
    std::string action;
    nlohmann::json payload = nlohmann::json::object();
    nlohmann::json cachedResult = nlohmann::json::object();
};

// UI-local admission control. The native action adapter must also check
// capability and current service state before mutating the application.
class UICommandGate {
public:
    void StartInstance(std::string id, std::unordered_set<std::string> capabilities) {
        m_instance = std::move(id);
        m_capabilities = std::move(capabilities);
        m_pending.clear(); m_recent.clear(); m_order.clear();
    }
    void SetRevisions(std::string settings, std::string peer, std::string topology,
                      std::string pairingAttempt = {}) {
        m_settingsRevision = std::move(settings);
        m_peerId = std::move(peer);
        m_topologyRevision = std::move(topology);
        m_pairingAttempt = std::move(pairingAttempt);
    }
    CommandDecision Admit(const std::string& raw) {
        CommandDecision d;
        auto reject = [&](const char* code) { d.kind = CommandDecision::Kind::Rejected; d.code = code; return d; };
        if (raw.size() > 64 * 1024) return reject("too_large");
        auto j = nlohmann::json::parse(raw, nullptr, false);
        if (j.is_discarded() || !j.is_object() || j.size() != 6) return reject("malformed");
        if (!j.contains("type") || j["type"] != "command" ||
            !j.contains("version") || j["version"] != 1 ||
            !j.contains("instanceId") || !BoundedString(j["instanceId"], 128) ||
            !j.contains("requestId") || !BoundedString(j["requestId"], 128) ||
            !j.contains("action") || !BoundedString(j["action"], 80) ||
            !j.contains("payload") || !j["payload"].is_object() || j["payload"].size() > 64)
            return reject("malformed");
        d.requestId = j["requestId"].get<std::string>();
        d.action = j["action"].get<std::string>();
        d.payload = j["payload"];
        if (j["instanceId"].get<std::string>() != m_instance) return reject("stale_instance");
        if (!m_capabilities.count(d.action)) return reject("not_supported");
        if (m_pending.count(d.requestId)) { d.kind = CommandDecision::Kind::DuplicatePending; d.code = "pending"; return d; }
        auto cached = m_recent.find(d.requestId);
        if (cached != m_recent.end()) { d.kind = CommandDecision::Kind::DuplicateResult; d.cachedResult = cached->second; return d; }
        if (m_pending.size() >= 32) return reject("busy");
        if (!ValidPayload(d.action, d.payload)) return reject("invalid_payload");
        if (d.action == "settings.saveRestart" && d.payload["revision"].get<std::string>() != m_settingsRevision)
            return reject("stale_settings");
        if (d.action == "cross.layoutApply" || d.action == "cross.layoutReset" || d.action == "cross.layoutRemoveMissing") {
            if (d.payload["peerId"].get<std::string>() != m_peerId ||
                d.payload["topologyRevision"].get<std::string>() != m_topologyRevision)
                return reject("stale_topology");
        }
        if (d.action == "cross.approvePairing" || d.action == "cross.declinePairing") {
            if (d.payload["peerId"].get<std::string>() != m_peerId ||
                d.payload["attempt"].get<std::string>() != m_pairingAttempt || m_pairingAttempt.empty())
                return reject("expired_pairing");
        }
        m_pending.insert(d.requestId);
        d.kind = CommandDecision::Kind::Accepted;
        return d;
    }
    void Finish(const std::string& requestId, const nlohmann::json& result) {
        if (!m_pending.erase(requestId)) return;
        m_recent[requestId] = result;
        m_order.push_back(requestId);
        while (m_order.size() > 256) { m_recent.erase(m_order.front()); m_order.pop_front(); }
    }
    size_t PendingCount() const { return m_pending.size(); }

private:
    static bool BoundedString(const nlohmann::json& value, size_t max) {
        return value.is_string() && !value.get_ref<const std::string&>().empty() && value.get_ref<const std::string&>().size() <= max;
    }
    static bool DecimalRevision(const nlohmann::json& value) {
        if (!BoundedString(value, 20)) return false;
        const auto& s = value.get_ref<const std::string&>();
        if (s.size() > 1 && s[0] == '0') return false;
        for (char c : s) if (c < '0' || c > '9') return false;
        return true;
    }
    static bool NumberInRange(const nlohmann::json& value, double low, double high) {
        if (!value.is_number()) return false;
        const double number = value.get<double>();
        return std::isfinite(number) && number >= low && number <= high;
    }
    static bool ValidPayload(const std::string& action, const nlohmann::json& p) {
        if (action == "settings.saveRestart") {
            if (!p.contains("revision") || !DecimalRevision(p["revision"]) || !p.contains("values") ||
                !p["values"].is_object() || p["values"].size() > 64) return false;
            for (auto it = p["values"].begin(); it != p["values"].end(); ++it) {
                if (it.key().empty() || it.key().size() > 64) return false;
                const auto& value = it.value();
                if (value.is_string()) { if (value.get_ref<const std::string&>().size() > 1024) return false; }
                else if (value.is_number()) { if (!NumberInRange(value, -1000000000, 1000000000)) return false; }
                else if (!value.is_boolean()) return false;
            }
            return true;
        }
        if (action == "cross.layoutApply") {
            if (!p.contains("tiles") || !p["tiles"].is_array() || p["tiles"].size() > 64) return false;
            for (const auto& tile : p["tiles"]) {
                if (!tile.is_object() || !tile.contains("id") || !BoundedString(tile["id"], 256) ||
                    !tile.contains("owner") || !tile["owner"].is_string() ||
                    (tile["owner"] != "mac" && tile["owner"] != "windows") ||
                    !tile.contains("x") || !NumberInRange(tile["x"], -10000000, 10000000) ||
                    !tile.contains("y") || !NumberInRange(tile["y"], -10000000, 10000000) ||
                    !tile.contains("width") || !NumberInRange(tile["width"], 1, 1000000) ||
                    !tile.contains("height") || !NumberInRange(tile["height"], 1, 1000000)) return false;
            }
        }
        if (action == "cross.layoutApply" || action == "cross.layoutReset" || action == "cross.layoutRemoveMissing")
            return p.contains("peerId") && BoundedString(p["peerId"], 256) && p.contains("topologyRevision") && DecimalRevision(p["topologyRevision"]);
        if (action == "cross.approvePairing" || action == "cross.declinePairing")
            return p.contains("peerId") && BoundedString(p["peerId"], 256) && p.contains("attempt") && BoundedString(p["attempt"], 128);
        if (action == "mode.set") return p.contains("mode") && p["mode"].is_string() && (p["mode"] == "screen" || p["mode"] == "cross");
        if (action == "cross.manualAddress") return p.contains("address") && BoundedString(p["address"], 255);
        if (action == "cross.shortcut") return p.contains("shortcut") && BoundedString(p["shortcut"], 48);
        if (action == "cross.interface") return p.contains("interfaceId") && p["interfaceId"].is_string() && p["interfaceId"].get_ref<const std::string&>().size() <= 256;
        if (action == "cross.modifiers") return p.contains("mode") && p["mode"].is_string() && (p["mode"] == "native" || p["mode"] == "windows");
        if (action == "cross.selectPeer" || action == "cross.forgetPeer") return p.contains("peerId") && BoundedString(p["peerId"], 256);
        if (action == "power.keepAwake") return p.contains("enabled") && p["enabled"].is_boolean();
        return p.empty();
    }
    std::string m_instance, m_settingsRevision, m_peerId, m_topologyRevision, m_pairingAttempt;
    std::unordered_set<std::string> m_capabilities, m_pending;
    std::unordered_map<std::string, nlohmann::json> m_recent;
    std::deque<std::string> m_order;
};
} // namespace thunderlink::ui
