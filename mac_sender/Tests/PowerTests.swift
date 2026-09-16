import Foundation
import Testing
import PowerSupport
@testable import ThunderLinkSender

struct PowerTests {
    @Test func bothModesHoldPowerOnlyDuringSessions() {
        #expect(!SessionPowerController.hasActiveSession(streaming: false, crossConfigured: false, paused: false))
        #expect(SessionPowerController.hasActiveSession(streaming: true, crossConfigured: false, paused: false))
        #expect(SessionPowerController.hasActiveSession(streaming: false, crossConfigured: true, paused: false))
        #expect(!SessionPowerController.hasActiveSession(streaming: true, crossConfigured: true, paused: true))
    }
    @Test func idleAssertionRequiresEnabledSessionAndAC() {
        var ac = false
        var acquired = 0
        var released: [UInt32] = []
        var requested: [Bool] = []
        let controller = SessionPowerController(services: .init(onACPower: { ac }, acquire: {
            acquired += 1; return 42
        }, release: { released.append($0); return true }))
        controller.onRequested = { requested.append($0) }
        controller.refresh()
        controller.setSessionActive(true)
        #expect(acquired == 0)
        ac = true; controller.refresh(); controller.refresh()
        #expect(acquired == 1 && controller.isKeepingAwake)
        controller.setEnabled(false)
        #expect(released == [42] && !controller.isKeepingAwake)
        controller.setEnabled(true)
        #expect(acquired == 2)
        ac = false; controller.refresh()
        #expect(released.count == 2 && !controller.isKeepingAwake)
        ac = true; controller.refresh()
        controller.setSessionActive(false)
        #expect(released.count == 3)
        controller.setSessionActive(true); controller.stop()
        #expect(released.count == 4 && requested.last == false)
    }

    @Test func retriesAcquisitionAndRetainsFailedReleaseBookkeeping() {
        var acquisitionSucceeds = false
        var releaseSucceeds = false
        var releases = 0
        let controller = SessionPowerController(services: .init(onACPower: { true }, acquire: {
            acquisitionSucceeds ? 7 : nil
        }, release: { _ in releases += 1; return releaseSucceeds }))
        controller.setSessionActive(true)
        #expect(!controller.isKeepingAwake)
        acquisitionSucceeds = true; controller.refresh()
        #expect(controller.isKeepingAwake)
        controller.setSessionActive(false)
        #expect(controller.isKeepingAwake && releases == 1)
        releaseSucceeds = true; controller.refresh()
        #expect(!controller.isKeepingAwake && releases == 2)
    }

    @Test func leaseRejectsStaleFutureMalformedBatteryAndThermalStates() throws {
        let lease = PowerLease(uptime: 100, requested: true)
        #expect(lease.shouldOverride(now: 100, onAC: true, thermalSafe: true))
        #expect(lease.shouldOverride(now: 103.999, onAC: true, thermalSafe: true))
        #expect(!lease.shouldOverride(now: 104, onAC: true, thermalSafe: true))
        #expect(!lease.shouldOverride(now: 99, onAC: true, thermalSafe: true))
        #expect(!lease.shouldOverride(now: 101, onAC: false, thermalSafe: true))
        #expect(!lease.shouldOverride(now: 101, onAC: true, thermalSafe: false))
        #expect(!PowerLease(uptime: .nan, requested: true).isFresh(now: 101))
        #expect(!PowerLease(uptime: 100, requested: false).shouldOverride(now: 101, onAC: true, thermalSafe: true))
        #expect(throws: (any Error).self) {
            try JSONDecoder().decode(PowerLease.self, from: Data("{\"requested\":true}".utf8))
        }
        let decoded = try JSONDecoder().decode(PowerLease.self, from: JSONEncoder().encode(lease))
        #expect(decoded.uptime == 100 && decoded.requested)
    }

    @Test func failedOverrideStillRequiresVerifiedRestoration() {
        var policy = SleepOverride()
        #expect(policy.update(requested: false) { _ in Issue.record("No mutation expected"); return false })
        #expect(!policy.update(requested: true) { $0 == false })
        #expect(policy.needsRestore)
        #expect(!policy.update(requested: false) { _ in false })
        #expect(policy.needsRestore)
        #expect(policy.update(requested: false) { $0 == false })
        #expect(!policy.needsRestore)
        #expect(policy.update(requested: true) { $0 })
        #expect(policy.needsRestore)
    }

    @Test func powerPreferenceMigrationAndPersistence() throws {
        #expect(try RuntimeConfiguration.parse(arguments: ["app"]).keepAwakeOnPower)
        let old = ["--peer", "10.0.0.9", "--fps", "30"]
        #expect(try RuntimeConfiguration.parse(arguments: ["app"] + old).keepAwakeOnPower)
        let off = DesktopSettings.argumentsSettingKeepAwake(false, in: old + ["--keep-awake"])
        #expect(off == old + ["--no-keep-awake"])
        #expect(try !RuntimeConfiguration.parse(arguments: ["app"] + off).keepAwakeOnPower)
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("settings.json")
        try DesktopSettings.save(off, to: file)
        #expect(try DesktopSettings.load(from: file) == off)
        let on = DesktopSettings.argumentsSettingKeepAwake(true, in: off)
        #expect(on == old + ["--keep-awake"])
    }
}
