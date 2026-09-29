import Foundation
import Testing
@testable import ThunderLinkSender

struct UICommandGateTests {
    @Test func sharedFixturesAndRequestLifecycle() throws {
        let fixtures = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("shared_ui/contract/fixtures")
        let valid = try String(contentsOf: fixtures.appendingPathComponent("valid-command.json"), encoding: .utf8)
        let invalid = try String(contentsOf: fixtures.appendingPathComponent("invalid-command.json"), encoding: .utf8)
        let gate = UICommandGate()
        gate.startInstance("fixture-instance", capabilities: ["connection.connect", "settings.saveRestart", "cross.approvePairing"])
        gate.setContext(settingsRevision: "1", peerID: "mac-1", pairingAttempt: "attempt-1")
        #expect(gate.admit(valid).kind == .accepted)
        #expect(gate.admit(valid).kind == .duplicatePending)
        gate.finish("req-1", result: ["status": "ok"])
        #expect(gate.admit(valid).kind == .duplicateResult)
        #expect(gate.admit(invalid).code == "malformed")
        #expect(gate.admit(String(repeating: "x", count: 65537)).code == "too_large")
        gate.startInstance("new-instance", capabilities: ["connection.connect"])
        #expect(gate.admit(valid).code == "stale_instance")
    }
}
