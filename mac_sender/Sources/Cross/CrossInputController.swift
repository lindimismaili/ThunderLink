import CoreGraphics
import Foundation

enum CrossOwnershipLease {
    static func started(now: UInt64) -> UInt64 { now }

    static func expired(now: UInt64, lastPing: UInt64, ownsInput: Bool) -> Bool {
        ownsInput && now >= lastPing && now - lastPing >= UInt64(NetworkConfig.crossLeaseMs)
    }

    static func lastRenewal(previous: UInt64, now: UInt64, renewOwnership: Bool) -> UInt64 {
        renewOwnership ? now : previous
    }
}

/// Owns the Mac side of one Cross input session. Calls are serialized by its
/// private queue so a reset cannot race a button/key down being injected.
final class CrossInputController {
    private let queue = DispatchQueue(label: "com.thunderlink.cross-input", qos: .userInteractive)
    private let poster: InputEventPosting
    private var configuredSession: StreamSessionID?
    private var topologyRevision: UInt64 = 0
    private var displays: [String: CrossDisplayDescriptor] = [:]
    private var swapControlCommand = true
    private var activeGeneration: UInt64?
    private var lastGeneration: UInt64 = 0
    private var activeDisplayID: String?
    private var lastMouseLocation = CGPoint.zero
    private var pressedButtons: [Int: CGPoint] = [:]
    private var pressedKeys: [CGKeyCode: CGEventFlags] = [:]

    init(poster: InputEventPosting = CGInputEventPoster()) { self.poster = poster }

    func configure(_ configuration: CrossConfigure, topology: CrossDisplayTopology) {
        queue.sync {
            releaseAllNow()
            configuredSession = configuration.sessionID
            topologyRevision = topology.revision
            displays = Dictionary(topology.displays.map { ($0.id, $0) },
                                  uniquingKeysWith: { first, _ in first })
            swapControlCommand = configuration.swapControlCommand
            activeGeneration = nil
            activeDisplayID = nil
            lastGeneration = 0
        }
    }

    func enter(sessionID: StreamSessionID, topologyRevision revision: UInt64,
               generation: UInt64, displayID: String, x: Float, y: Float) throws {
        try queue.sync {
            try validate(sessionID: sessionID, revision: revision)
            guard activeGeneration == nil, generation > lastGeneration else {
                throw CrossProtocolError.invalidGeneration
            }
            guard let display = displays[displayID],
                  let point = CrossCoordinateMapper.point(x: x, y: y, in: display) else {
                throw CrossProtocolError.invalidDisplay
            }
            guard poster.postMouse(type: .mouseMoved, point: point, button: .left) else {
                throw CrossProtocolError.malformed("cursor")
            }
            lastMouseLocation = point
            lastGeneration = generation
            activeGeneration = generation
            activeDisplayID = displayID
        }
    }

    func process(_ envelope: CrossInputEnvelope) throws {
        try queue.sync {
            try validate(sessionID: envelope.sessionID, revision: envelope.topologyRevision)
            guard activeGeneration == envelope.generation else { throw CrossProtocolError.invalidGeneration }
            switch envelope.event.type {
            case "reset":
                releaseOwnershipNow()
            case "mouse_move":
                let (display, point) = try pointer(envelope)
                activeDisplayID = display.id
                lastMouseLocation = point
                for index in Array(pressedButtons.keys) { pressedButtons[index] = point }
                let held = pressedButtons.keys.sorted().first
                let type: CGEventType
                let button: CGMouseButton
                switch held {
                case 0?: type = .leftMouseDragged; button = .left
                case 1?: type = .rightMouseDragged; button = .right
                case .some: type = .otherMouseDragged; button = .center
                case nil: type = .mouseMoved; button = .left
                }
                _ = poster.postMouse(type: type, point: point, button: button)
            case "mouse_button":
                let (_, point) = try pointer(envelope)
                guard let index = envelope.event.button, let state = envelope.event.state else {
                    throw CrossProtocolError.malformed("event.mouse_button")
                }
                let isDown = state == "down"
                let mapped = EventMapper.mouseEventTypes(button: index, isDown: isDown)
                if poster.postMouse(type: mapped.eventType, point: point, button: mapped.mouseButton) {
                    if isDown { pressedButtons[index] = point }
                    else { pressedButtons.removeValue(forKey: index) }
                }
            case "key":
                guard let raw = envelope.event.keyCode, let state = envelope.event.state else {
                    throw CrossProtocolError.malformed("event.key")
                }
                guard let code = EventMapper.mapCrossKeyCode(raw,
                                                              swapControlCommand: swapControlCommand) else {
                    return
                }
                let flags = EventMapper.mapModifiers(envelope.event.modifiers,
                                                     swapControlCommand: swapControlCommand)
                let isDown = state == "down"
                if poster.postKey(code: code, isDown: isDown, modifiers: flags) {
                    if isDown { pressedKeys[code] = flags }
                    else { pressedKeys.removeValue(forKey: code) }
                }
            case "scroll":
                _ = poster.postScroll(deltaX: envelope.event.deltaX ?? 0,
                                      deltaY: envelope.event.deltaY ?? 0)
            default: throw CrossProtocolError.malformed("event.type")
            }
        }
    }

    func leave(sessionID: StreamSessionID, topologyRevision revision: UInt64,
               generation: UInt64) throws {
        try queue.sync {
            try validate(sessionID: sessionID, revision: revision)
            guard activeGeneration == generation else { throw CrossProtocolError.invalidGeneration }
            releaseOwnershipNow()
        }
    }

    func reset() { queue.sync { releaseOwnershipNow() } }

    func invalidateSession() {
        queue.sync {
            releaseOwnershipNow()
            configuredSession = nil
            topologyRevision = 0
            displays.removeAll()
            lastGeneration = 0
        }
    }

    func updateTopology(_ topology: CrossDisplayTopology) {
        queue.sync {
            releaseOwnershipNow()
            configuredSession = nil
            topologyRevision = topology.revision
            displays = Dictionary(topology.displays.map { ($0.id, $0) },
                                  uniquingKeysWith: { first, _ in first })
            lastGeneration = 0
        }
    }

    var hasOwnership: Bool { queue.sync { activeGeneration != nil } }

    private func pointer(_ envelope: CrossInputEnvelope) throws -> (CrossDisplayDescriptor, CGPoint) {
        guard let id = envelope.displayID, let display = displays[id],
              let x = envelope.event.x, let y = envelope.event.y,
              let point = CrossCoordinateMapper.point(x: x, y: y, in: display) else {
            throw CrossProtocolError.invalidDisplay
        }
        return (display, point)
    }

    private func validate(sessionID: StreamSessionID, revision: UInt64) throws {
        guard configuredSession == sessionID else { throw CrossProtocolError.invalidSession }
        guard topologyRevision == revision else { throw CrossProtocolError.invalidTopology }
    }

    private func releaseOwnershipNow() {
        releaseAllNow()
        activeGeneration = nil
        activeDisplayID = nil
    }

    private func releaseAllNow() {
        for (index, point) in pressedButtons.sorted(by: { $0.key < $1.key }) {
            let mapped = EventMapper.mouseEventTypes(button: index, isDown: false)
            _ = poster.postMouse(type: mapped.eventType, point: point, button: mapped.mouseButton)
        }
        for code in pressedKeys.keys.sorted() {
            _ = poster.postKey(code: code, isDown: false, modifiers: [])
        }
        pressedButtons.removeAll()
        pressedKeys.removeAll()
    }
}
