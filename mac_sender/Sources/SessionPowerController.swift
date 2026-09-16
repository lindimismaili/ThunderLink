import Foundation
import IOKit.pwr_mgt
import IOKit.ps

/// Main-run-loop owner of the assertion; neither discovery nor pairing counts
/// as a session. Injected services let tests exercise transitions without
/// changing the test machine's power policy.
final class SessionPowerController {
    struct Services {
        var onACPower: () -> Bool
        var acquire: () -> IOPMAssertionID?
        var release: (IOPMAssertionID) -> Bool

        static let system = Services(onACPower: {
            guard let info = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
                  let source = IOPSGetProvidingPowerSourceType(info)?.takeUnretainedValue() else { return false }
            return source as String == kIOPSACPowerValue
        }, acquire: {
            var id: IOPMAssertionID = 0
            let result = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString,
                IOPMAssertionLevel(kIOPMAssertionLevelOn),
                "ThunderLink active session on AC power" as CFString, &id)
            if result != kIOReturnSuccess { logger.error("Keep awake: assertion failed (\(result))") }
            return result == kIOReturnSuccess ? id : nil
        }, release: { id in
            let result = IOPMAssertionRelease(id)
            if result != kIOReturnSuccess { logger.error("Keep awake: release failed (\(result))") }
            return result == kIOReturnSuccess
        })
    }

    private let services: Services
    private var assertion: IOPMAssertionID?
    private var source: CFRunLoopSource?
    private var timer: Timer?
    private var enabled = true
    private var sessionActive = false
    private(set) var isKeepingAwake = false
    var onStatus: ((Bool) -> Void)?
    var onRequested: ((Bool) -> Void)?

    init(services: Services = .system) { self.services = services }

    static func hasActiveSession(streaming: Bool, crossConfigured: Bool, paused: Bool) -> Bool {
        (streaming || crossConfigured) && !paused
    }

    func start() {
        guard timer == nil else { return }
        source = IOPSNotificationCreateRunLoopSource({ context in
            guard let context else { return }
            Unmanaged<SessionPowerController>.fromOpaque(context).takeUnretainedValue().refresh()
        }, Unmanaged.passUnretained(self).toOpaque())?.takeRetainedValue()
        if let source { CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes) }
        // Also retry failed assertions and cover an unavailable notification source.
        let timer = Timer(timeInterval: 5, repeats: true) { [weak self] _ in self?.refresh() }
        self.timer = timer
        RunLoop.main.add(timer, forMode: .common)
        refresh()
    }

    func setEnabled(_ enabled: Bool) { self.enabled = enabled; refresh() }
    func setSessionActive(_ active: Bool) { sessionActive = active; refresh() }

    func refresh() {
        let shouldHold = enabled && sessionActive && services.onACPower()
        onRequested?(shouldHold)
        if shouldHold && assertion == nil { assertion = services.acquire() }
        if !shouldHold, let id = assertion, services.release(id) { assertion = nil }
        let held = assertion != nil
        if held != isKeepingAwake { isKeepingAwake = held; onStatus?(held) }
    }

    func stop() {
        timer?.invalidate(); timer = nil
        if let source { CFRunLoopSourceInvalidate(source) }
        source = nil
        sessionActive = false
        refresh()
    }

    deinit {
        timer?.invalidate()
        if let source { CFRunLoopSourceInvalidate(source) }
        if let assertion { _ = services.release(assertion) }
    }
}
