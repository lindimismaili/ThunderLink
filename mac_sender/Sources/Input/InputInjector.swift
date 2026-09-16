import CoreGraphics
import ApplicationServices

protocol InputEventPosting: AnyObject {
    @discardableResult func postMouse(type: CGEventType, point: CGPoint, button: CGMouseButton) -> Bool
    @discardableResult func postKey(code: CGKeyCode, isDown: Bool, modifiers: CGEventFlags) -> Bool
    @discardableResult func postScroll(deltaX: Int32, deltaY: Int32) -> Bool
}

final class CGInputEventPoster: InputEventPosting {
    func postMouse(type: CGEventType, point: CGPoint, button: CGMouseButton) -> Bool {
        guard let event = CGEvent(mouseEventSource: nil, mouseType: type,
                                  mouseCursorPosition: point, mouseButton: button) else { return false }
        event.post(tap: .cghidEventTap)
        return true
    }

    func postKey(code: CGKeyCode, isDown: Bool, modifiers: CGEventFlags) -> Bool {
        guard let event = CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: isDown) else {
            return false
        }
        event.flags = modifiers
        event.post(tap: .cghidEventTap)
        return true
    }

    func postScroll(deltaX: Int32, deltaY: Int32) -> Bool {
        guard let event = CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 2,
                                  wheel1: deltaY, wheel2: deltaX, wheel3: 0) else { return false }
        event.post(tap: .cghidEventTap)
        return true
    }
}

/// Serializes injection and held-state changes. `releaseAll` is idempotent and
/// is called for reset, input EOF, control teardown, session changes, and quit.
final class InputInjector {
    private let queue = DispatchQueue(label: "com.thunderlink.input-injector", qos: .userInteractive)
    private let poster: InputEventPosting
    private var lastMouseLocation = CGPoint.zero
    private var screenWidth = 1920
    private var screenHeight = 1080
    private var pressedButtons: [Int: CGPoint] = [:]
    private var pressedKeys: [CGKeyCode: CGEventFlags] = [:]
    private var activeSession: StreamSessionID?

    init(poster: InputEventPosting = CGInputEventPoster()) {
        self.poster = poster
    }

    func setScreenSize(width: Int, height: Int) {
        queue.async { [weak self] in
            self?.screenWidth = max(width, 1)
            self?.screenHeight = max(height, 1)
        }
    }

    static func checkAccessibilityPermission() -> Bool {
        AXIsProcessTrusted()
    }

    func processEvent(_ event: InputEventMessage) {
        queue.async { [weak self] in
            guard let self, self.activeSession == event.sessionID else { return }
            self.process(event)
        }
    }

    func setActiveSession(_ session: StreamSessionID?) {
        queue.sync {
            releaseAllNow()
            activeSession = session
        }
    }

    func releaseAll() {
        queue.async { [weak self] in self?.releaseAllNow() }
    }

    /// Waits for previously queued work. Used by orderly shutdown and tests.
    func drain() { queue.sync {} }

    private func process(_ event: InputEventMessage) {
        switch event.type {
        case "reset":
            releaseAllNow()
        case "mouse_move":
            guard let x = event.x, let y = event.y, x.isFinite, y.isFinite else { return }
            let point = EventMapper.denormalize(x: min(max(x, 0), 1), y: min(max(y, 0), 1),
                                                screenWidth: screenWidth, screenHeight: screenHeight)
            lastMouseLocation = point
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
            guard let x = event.x, let y = event.y, x.isFinite, y.isFinite,
                  let buttonIndex = event.button, (0...2).contains(buttonIndex),
                  let state = event.state, state == "down" || state == "up" else { return }
            let point = EventMapper.denormalize(x: min(max(x, 0), 1), y: min(max(y, 0), 1),
                                                screenWidth: screenWidth, screenHeight: screenHeight)
            lastMouseLocation = point
            let isDown = state == "down"
            let mapped = EventMapper.mouseEventTypes(button: buttonIndex, isDown: isDown)
            if poster.postMouse(type: mapped.eventType, point: point, button: mapped.mouseButton) {
                if isDown { pressedButtons[buttonIndex] = point } else { pressedButtons.removeValue(forKey: buttonIndex) }
            }
        case "key":
            guard let rawCode = event.keyCode,
                  let state = event.state, state == "down" || state == "up" else { return }
            let code = EventMapper.mapKeyCode(rawCode)
            let modifiers = EventMapper.mapModifiers(event.modifiers)
            let isDown = state == "down"
            if poster.postKey(code: code, isDown: isDown, modifiers: modifiers) {
                if isDown { pressedKeys[code] = modifiers } else { pressedKeys.removeValue(forKey: code) }
            }
        case "scroll":
            _ = poster.postScroll(deltaX: event.deltaX ?? 0, deltaY: event.deltaY ?? 0)
        default:
            logger.debug("InputInjector: ignored unknown event type '\(event.type)'")
        }
    }

    private func releaseAllNow() {
        for (buttonIndex, point) in pressedButtons.sorted(by: { $0.key < $1.key }) {
            let mapped = EventMapper.mouseEventTypes(button: buttonIndex, isDown: false)
            _ = poster.postMouse(type: mapped.eventType, point: point, button: mapped.mouseButton)
        }
        for (code, _) in pressedKeys.sorted(by: { $0.key < $1.key }) {
            _ = poster.postKey(code: code, isDown: false, modifiers: [])
        }
        pressedButtons.removeAll()
        pressedKeys.removeAll()
    }
}
