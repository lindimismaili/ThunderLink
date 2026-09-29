import Foundation

struct UIAdmission {
    enum Kind { case accepted, rejected, duplicatePending, duplicateResult }
    let kind: Kind
    let code: String
    let requestID: String
    let action: String
    let payload: [String: Any]
    let cachedResult: [String: Any]?
}

/// Admission control for the local WKWebView contract. Native action handlers
/// must also check the current service state before changing the sender.
final class UICommandGate {
    private var instanceID = ""
    private var capabilities = Set<String>()
    private var pending = Set<String>()
    private var recent = [String: [String: Any]]()
    private var order = [String]()
    private var settingsRevision = "0"
    private var peerID = ""
    private var pairingAttempt = ""

    func startInstance(_ id: String, capabilities: Set<String>) {
        instanceID = id
        self.capabilities = capabilities
        pending.removeAll(); recent.removeAll(); order.removeAll()
    }

    func setContext(settingsRevision: String, peerID: String, pairingAttempt: String) {
        self.settingsRevision = settingsRevision
        self.peerID = peerID
        self.pairingAttempt = pairingAttempt
    }

    func admit(_ raw: String) -> UIAdmission {
        func result(_ kind: UIAdmission.Kind, _ code: String, _ id: String = "",
                    _ action: String = "", _ payload: [String: Any] = [:],
                    _ cached: [String: Any]? = nil) -> UIAdmission {
            UIAdmission(kind: kind, code: code, requestID: id, action: action, payload: payload, cachedResult: cached)
        }
        let bytes = Data(raw.utf8)
        guard bytes.count <= 64 * 1024 else { return result(.rejected, "too_large") }
        guard let object = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any],
              object.count == 6,
              object["type"] as? String == "command",
              object["version"] as? Int == 1,
              let instance = object["instanceId"] as? String, Self.bounded(instance, 128),
              let id = object["requestId"] as? String, Self.bounded(id, 128),
              let action = object["action"] as? String, Self.bounded(action, 80),
              let payload = object["payload"] as? [String: Any], payload.count <= 64 else {
            return result(.rejected, "malformed")
        }
        guard instance == instanceID else { return result(.rejected, "stale_instance", id) }
        guard capabilities.contains(action) else { return result(.rejected, "not_supported", id) }
        if pending.contains(id) { return result(.duplicatePending, "pending", id, action, payload) }
        if let cached = recent[id] { return result(.duplicateResult, "", id, action, payload, cached) }
        guard pending.count < 32 else { return result(.rejected, "busy", id) }
        guard validPayload(action, payload) else { return result(.rejected, "invalid_payload", id) }
        if action == "settings.saveRestart", payload["revision"] as? String != settingsRevision {
            return result(.rejected, "stale_settings", id)
        }
        if action == "cross.approvePairing" || action == "cross.declinePairing" {
            if payload["peerId"] as? String != peerID || payload["attempt"] as? String != pairingAttempt || pairingAttempt.isEmpty {
                return result(.rejected, "expired_pairing", id)
            }
        }
        pending.insert(id)
        return result(.accepted, "", id, action, payload)
    }

    func finish(_ id: String, result: [String: Any]) {
        guard pending.remove(id) != nil else { return }
        recent[id] = result
        order.append(id)
        while order.count > 256 { recent.removeValue(forKey: order.removeFirst()) }
    }

    private static func bounded(_ value: String, _ max: Int) -> Bool {
        !value.isEmpty && value.utf8.count <= max
    }
    private static func decimal(_ value: Any?) -> Bool {
        guard let text = value as? String, bounded(text, 20),
              text == "0" || text.first != "0" else { return false }
        return text.utf8.allSatisfy { $0 >= 48 && $0 <= 57 }
    }
    private func validPayload(_ action: String, _ payload: [String: Any]) -> Bool {
        if action == "settings.saveRestart" {
            guard Self.decimal(payload["revision"]), let values = payload["values"] as? [String: Any], values.count <= 64 else { return false }
            return values.allSatisfy { key, value in
                key.utf8.count <= 64 && (value is String || value is Bool || value is NSNumber)
            }
        }
        if action == "cross.approvePairing" || action == "cross.declinePairing" {
            guard let peer = payload["peerId"] as? String, let attempt = payload["attempt"] as? String else { return false }
            return Self.bounded(peer, 256) && Self.bounded(attempt, 128)
        }
        if action == "cross.forgetPeer" {
            guard let peer = payload["peerId"] as? String else { return false }
            return Self.bounded(peer, 256)
        }
        if action == "power.keepAwake" { return payload["enabled"] is Bool }
        return payload.isEmpty
    }
}
