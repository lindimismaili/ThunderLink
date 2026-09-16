import Foundation
import CryptoKit

enum CrossProtocolError: Error, LocalizedError, Equatable {
    case malformed(String)
    case unsupportedVersion(Int)
    case invalidSession
    case invalidTopology
    case invalidGeneration
    case invalidDisplay
    case notPaired
    case notConfigured

    var errorDescription: String? {
        switch self {
        case .malformed(let field): return "Invalid Cross field: \(field)"
        case .unsupportedVersion(let version): return "Cross protocol v\(version) is incompatible; v4 is required"
        case .invalidSession: return "Invalid or stale Cross session"
        case .invalidTopology: return "The display arrangement changed; review it before reconnecting"
        case .invalidGeneration: return "Invalid or stale input ownership generation"
        case .invalidDisplay: return "The selected Mac display is unavailable"
        case .notPaired: return "Approve pairing on both computers before using Cross"
        case .notConfigured: return "Cross is not ready for input"
        }
    }
}

struct CrossPeerHello: Equatable {
    let version: Int
    let deviceID: String
    let name: String
    let role: String
}

struct CrossConfigure: Equatable {
    let sessionID: StreamSessionID
    let topologyRevision: UInt64
    let swapControlCommand: Bool
}

struct CrossInputEnvelope: Equatable {
    let sessionID: StreamSessionID
    let topologyRevision: UInt64
    let generation: UInt64
    let displayID: String?
    let event: InputEventMessage
}

enum CrossProtocol {
    static let pairingTimeoutSeconds: TimeInterval = 120
    static let writeTimeoutSeconds: TimeInterval = 5

    static func hello(deviceID: String, name: String) -> [String: Any] {
        ["type": "hello", "version": Int(NetworkConfig.protocolVersion),
         "device_id": deviceID, "name": name, "role": "mac"]
    }

    static func parseHello(_ object: [String: Any], certificateFingerprint: String) throws -> CrossPeerHello {
        guard try string("type", object) == "hello" else { throw CrossProtocolError.malformed("type") }
        let version = try integer("version", object)
        guard version == Int(NetworkConfig.protocolVersion) else {
            throw CrossProtocolError.unsupportedVersion(version)
        }
        let deviceID = try fingerprint("device_id", object)
        guard deviceID == certificateFingerprint else { throw CrossProtocolError.malformed("device_id") }
        let name = try string("name", object)
        guard !name.isEmpty, name.utf8.count <= 256 else { throw CrossProtocolError.malformed("name") }
        let role = try string("role", object)
        guard role == "windows" else { throw CrossProtocolError.malformed("role") }
        return CrossPeerHello(version: version, deviceID: deviceID, name: name, role: role)
    }

    static func parseConfigure(_ object: [String: Any]) throws -> CrossConfigure {
        guard let text = object["session_id"] as? String,
              let sessionID = StreamSessionID(hex: text) else { throw CrossProtocolError.invalidSession }
        let revision = try positiveUInt64("topology_revision", object)
        guard let swap = object["swap_ctrl_cmd"] as? NSNumber,
              CFGetTypeID(swap) == CFBooleanGetTypeID() else {
            throw CrossProtocolError.malformed("swap_ctrl_cmd")
        }
        return CrossConfigure(sessionID: sessionID, topologyRevision: revision,
                              swapControlCommand: swap.boolValue)
    }

    static func parseGeneration(_ object: [String: Any]) throws -> UInt64 {
        try positiveUInt64("generation", object)
    }

    static func parseSession(_ object: [String: Any]) throws -> (StreamSessionID, UInt64) {
        guard let text = object["session_id"] as? String,
              let sessionID = StreamSessionID(hex: text) else { throw CrossProtocolError.invalidSession }
        return (sessionID, try positiveUInt64("topology_revision", object))
    }

    static func parseSessionID(_ object: [String: Any]) throws -> StreamSessionID {
        guard let text = object["session_id"] as? String,
              let sessionID = StreamSessionID(hex: text) else { throw CrossProtocolError.invalidSession }
        return sessionID
    }

    static func parseEnter(_ object: [String: Any]) throws -> (StreamSessionID, UInt64, UInt64, String, Float, Float) {
        let (session, revision) = try parseSession(object)
        let generation = try parseGeneration(object)
        let displayID = try displayIdentifier("display_id", object)
        let x = try normalized("x", object)
        let y = try normalized("y", object)
        return (session, revision, generation, displayID, x, y)
    }

    static func parseInput(_ object: [String: Any]) throws -> CrossInputEnvelope {
        let (session, revision) = try parseSession(object)
        let generation = try parseGeneration(object)
        guard let eventObject = object["event"] as? [String: Any] else {
            throw CrossProtocolError.malformed("event")
        }
        let eventData = try JSONSerialization.data(withJSONObject: eventObject)
        let decoded: CrossWireInputEvent
        do { decoded = try JSONDecoder().decode(CrossWireInputEvent.self, from: eventData) }
        catch { throw CrossProtocolError.malformed("event") }
        let event = InputEventMessage(type: decoded.type, sessionID: session, timestamp: decoded.timestamp,
                                      x: decoded.x, y: decoded.y, button: decoded.button,
                                      state: decoded.state, keyCode: decoded.keyCode,
                                      modifiers: decoded.modifiers, deltaX: decoded.deltaX,
                                      deltaY: decoded.deltaY)
        let pointerEvent = event.type == "mouse_move" || event.type == "mouse_button"
        let displayID: String?
        if pointerEvent { displayID = try displayIdentifier("display_id", object) }
        else { displayID = nil }
        try validate(event)
        return CrossInputEnvelope(sessionID: session, topologyRevision: revision,
                                  generation: generation, displayID: displayID, event: event)
    }

    static func validate(_ event: InputEventMessage) throws {
        switch event.type {
        case "mouse_move":
            guard let x = event.x, let y = event.y, x.isFinite, y.isFinite,
                  (0...1).contains(x), (0...1).contains(y) else {
                throw CrossProtocolError.malformed("event.x/y")
            }
        case "mouse_button":
            guard let x = event.x, let y = event.y, x.isFinite, y.isFinite,
                  (0...1).contains(x), (0...1).contains(y),
                  let button = event.button, (0...2).contains(button),
                  event.state == "down" || event.state == "up" else {
                throw CrossProtocolError.malformed("event.mouse_button")
            }
        case "key":
            guard event.keyCode != nil,
                  event.state == "down" || event.state == "up",
                  (event.modifiers ?? []).allSatisfy({ ["shift", "ctrl", "alt", "meta"].contains($0) }) else {
                throw CrossProtocolError.malformed("event.key")
            }
        case "scroll":
            guard event.deltaX != nil || event.deltaY != nil else {
                throw CrossProtocolError.malformed("event.scroll")
            }
        case "reset": break
        default: throw CrossProtocolError.malformed("event.type")
        }
    }

    static func verificationCode(clientFingerprint: String, serverFingerprint: String) -> String {
        let material = Data((clientFingerprint.lowercased() + serverFingerprint.lowercased()).utf8)
        return SHA256.hash(data: material).map { String(format: "%02X", $0) }.joined().prefix(12).description
    }

    static func fingerprint(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    static func type(of object: [String: Any]) throws -> String { try string("type", object) }

    static func pairingComplete(localApproved: Bool, remoteApproved: Bool) -> Bool {
        localApproved && remoteApproved
    }

    static func allowsBeforePairing(_ type: String) -> Bool { type == "pair_approve" }

    static func pingRenewsOwnership(_ object: [String: Any]) throws -> Bool {
        guard let raw = object["renew_ownership"] else { return true }
        guard let value = raw as? NSNumber,
              CFGetTypeID(value) == CFBooleanGetTypeID() else {
            throw CrossProtocolError.malformed("renew_ownership")
        }
        return value.boolValue
    }

    private static func string(_ field: String, _ object: [String: Any]) throws -> String {
        guard let value = object[field] as? String else { throw CrossProtocolError.malformed(field) }
        return value
    }

    private static func displayIdentifier(_ field: String, _ object: [String: Any]) throws -> String {
        let value = try string(field, object)
        guard !value.isEmpty, value.utf8.count <= 256 else { throw CrossProtocolError.malformed(field) }
        return value
    }

    private static func fingerprint(_ field: String, _ object: [String: Any]) throws -> String {
        let value = try string(field, object)
        guard value.count == 64, value.allSatisfy({ $0.isNumber || ("a"..."f").contains(String($0)) }) else {
            throw CrossProtocolError.malformed(field)
        }
        return value
    }

    private static func integer(_ field: String, _ object: [String: Any]) throws -> Int {
        guard let number = object[field] as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID(),
              number.int64Value >= Int64(Int.min), number.int64Value <= Int64(Int.max),
              NSNumber(value: number.int64Value) == number else {
            throw CrossProtocolError.malformed(field)
        }
        return Int(number.int64Value)
    }

    private static func positiveUInt64(_ field: String, _ object: [String: Any]) throws -> UInt64 {
        guard let number = object[field] as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID(), number.int64Value > 0,
              NSNumber(value: number.int64Value) == number else {
            throw CrossProtocolError.malformed(field)
        }
        return UInt64(number.int64Value)
    }

    private static func normalized(_ field: String, _ object: [String: Any]) throws -> Float {
        guard let number = object[field] as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID() else { throw CrossProtocolError.malformed(field) }
        let value = number.floatValue
        guard value.isFinite, (0...1).contains(value) else { throw CrossProtocolError.malformed(field) }
        return value
    }
}

private struct CrossWireInputEvent: Decodable {
    let type: String
    let timestamp: Int64?
    let x: Float?
    let y: Float?
    let button: Int?
    let state: String?
    let keyCode: UInt16?
    let modifiers: [String]?
    let deltaX: Int32?
    let deltaY: Int32?

    enum CodingKeys: String, CodingKey {
        case type, timestamp, x, y, button, state, modifiers
        case keyCode = "key_code"
        case deltaX = "delta_x"
        case deltaY = "delta_y"
    }
}
