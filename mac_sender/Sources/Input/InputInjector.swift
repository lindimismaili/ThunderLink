import CoreGraphics
import ApplicationServices

/// Injects input events into macOS using the CGEvent API.
/// Requires Accessibility permission (System Preferences > Privacy > Accessibility).
final class InputInjector {
    private var lastMouseLocation = CGPoint.zero
    private var screenWidth: Int = 1920
    private var screenHeight: Int = 1080

    /// Mouse buttons currently held down (TLNK button indices). Used to emit
    /// drag events while moving with a button down — otherwise macOS treats it
    /// as a plain move and click-and-drag doesn't work.
    private var pressedButtons = Set<Int>()

    func setScreenSize(width: Int, height: Int) {
        screenWidth = width
        screenHeight = height
    }

    /// Check if accessibility permission is granted.
    static func checkAccessibilityPermission() -> Bool {
        let trusted = AXIsProcessTrusted()
        if !trusted {
            logger.warn("InputInjector: Accessibility permission NOT granted")
            logger.warn("  Go to System Preferences > Privacy & Security > Accessibility")
        } else {
            logger.info("InputInjector: Accessibility permission granted")
        }
        return trusted
    }

    /// Process a received input event message and inject it.
    func processEvent(_ event: InputEventMessage) {
        switch event.type {
        case "mouse_move":
            guard let x = event.x, let y = event.y else { return }
            let point = EventMapper.denormalize(x: x, y: y,
                                                 screenWidth: screenWidth,
                                                 screenHeight: screenHeight)
            injectMouseMove(to: point)

        case "mouse_button":
            guard let x = event.x, let y = event.y,
                  let button = event.button,
                  let state = event.state else { return }
            let point = EventMapper.denormalize(x: x, y: y,
                                                 screenWidth: screenWidth,
                                                 screenHeight: screenHeight)
            let isDown = (state == "down")
            if isDown { pressedButtons.insert(button) } else { pressedButtons.remove(button) }
            injectMouseButton(at: point, button: button, isDown: isDown)

        case "key":
            guard let keyCode = event.key_code,
                  let state = event.state else { return }
            let isDown = (state == "down")
            let macKeyCode = EventMapper.mapKeyCode(keyCode)
            let modifiers = EventMapper.mapModifiers(event.modifiers)
            injectKey(keyCode: macKeyCode, isDown: isDown, modifiers: modifiers)

        case "scroll":
            let dx = event.delta_x ?? 0
            let dy = event.delta_y ?? 0
            injectScroll(deltaX: dx, deltaY: dy)

        default:
            logger.debug("InputInjector: unknown event type '\(event.type)'")
        }
    }

    // MARK: - CGEvent Injection

    private func injectMouseMove(to point: CGPoint) {
        lastMouseLocation = point

        // If a button is held, this is a DRAG: macOS needs a dragged event of
        // the matching type, otherwise the drag is ignored.
        let type: CGEventType
        let button: CGMouseButton
        if let held = pressedButtons.first {
            switch held {
            case 0: type = .leftMouseDragged;  button = .left
            case 1: type = .rightMouseDragged; button = .right
            default: type = .otherMouseDragged; button = .center
            }
        } else {
            type = .mouseMoved
            button = .left
        }

        guard let event = CGEvent(mouseEventSource: nil,
                                   mouseType: type,
                                   mouseCursorPosition: point,
                                   mouseButton: button) else { return }
        event.post(tap: .cghidEventTap)
    }

    private func injectMouseButton(at point: CGPoint, button: Int, isDown: Bool) {
        lastMouseLocation = point
        let (eventType, mouseButton) = EventMapper.mouseEventTypes(button: button,
                                                                     isDown: isDown)
        guard let event = CGEvent(mouseEventSource: nil,
                                   mouseType: eventType,
                                   mouseCursorPosition: point,
                                   mouseButton: mouseButton) else { return }
        event.post(tap: .cghidEventTap)
    }

    private func injectKey(keyCode: CGKeyCode, isDown: Bool, modifiers: CGEventFlags) {
        guard let event = CGEvent(keyboardEventSource: nil,
                                   virtualKey: keyCode,
                                   keyDown: isDown) else { return }
        event.flags = modifiers
        event.post(tap: .cghidEventTap)
    }

    private func injectScroll(deltaX: Int32, deltaY: Int32) {
        guard let event = CGEvent(scrollWheelEvent2Source: nil,
                                   units: .pixel,
                                   wheelCount: 2,
                                   wheel1: deltaY,
                                   wheel2: deltaX,
                                   wheel3: 0) else { return }
        event.post(tap: .cghidEventTap)
    }
}
