import Testing
import CoreGraphics
@testable import ThunderLinkSender

private final class RecordingPoster: InputEventPosting {
    var keys: [Bool] = []
    var buttons: [CGEventType] = []
    var accept = true
    func postMouse(type: CGEventType, point: CGPoint, button: CGMouseButton) -> Bool {
        if accept { buttons.append(type) }; return accept
    }
    func postKey(code: CGKeyCode, isDown: Bool, modifiers: CGEventFlags) -> Bool {
        if accept { keys.append(isDown) }; return accept
    }
    func postScroll(deltaX: Int32, deltaY: Int32) -> Bool { accept }
}

final class InputTests {
    private func event(_ type: String = "key", sid: UInt64 = 1, state: String = "down") -> InputEventMessage {
        InputEventMessage(type: type, sessionID: StreamSessionID(sid)!, timestamp: nil,
                          x: 0.5, y: 0.5, button: 0, state: state, keyCode: 0,
                          modifiers: nil, deltaX: nil, deltaY: nil)
    }
    @Test func testResetAndSessionReplacementReleaseExactlyOnce() {
        let sink = RecordingPoster(); let injector = InputInjector(poster: sink)
        injector.setActiveSession(StreamSessionID(1))
        injector.processEvent(event()); injector.processEvent(event("mouse_button")); injector.drain()
        injector.setActiveSession(StreamSessionID(2))
        injector.processEvent(event()); injector.processEvent(event("reset", sid: 2))
        injector.releaseAll(); injector.drain()
        XCTAssertEqual(sink.keys, [true, false])
        XCTAssertEqual(sink.buttons, [.leftMouseDown, .leftMouseUp])
    }
    @Test func testRejectedDownIsNotReleased() {
        let sink = RecordingPoster(); sink.accept = false
        let injector = InputInjector(poster: sink)
        injector.setActiveSession(StreamSessionID(1))
        injector.processEvent(event()); injector.drain()
        sink.accept = true
        injector.releaseAll(); injector.drain()
        XCTAssertTrue(sink.keys.isEmpty)
    }
}
