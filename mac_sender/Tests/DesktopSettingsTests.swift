import Foundation
import Testing
@testable import ThunderLinkSender

struct DesktopSettingsTests {
    @Test func settingsRoundTripAndLaunchPrecedence() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("settings.json")
        #expect(try DesktopSettings.load(from: file).isEmpty)
        let saved = ["--peer", "10.0.0.2", "--control-port", "54321", "--fps", "30"]
        try DesktopSettings.save(saved, to: file)
        let loaded = try DesktopSettings.load(from: file)
        let config = try RuntimeConfiguration.parse(arguments: ["app"] + loaded + ["--fps", "60"])
        #expect(config.winIP == "10.0.0.2")
        #expect(config.controlPort == 54321)
        #expect(config.fps == 60)
        #expect(try DesktopSettings.load(from: file) == saved)
    }

    @Test func invalidSettingsNeverOverwriteValidSettings() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("settings.json")
        try DesktopSettings.save(["--fps", "60"], to: file)
        #expect(throws: (any Error).self) { try DesktopSettings.save(["--video-port", "65536"], to: file) }
        #expect(try DesktopSettings.load(from: file) == ["--fps", "60"])
        try Data("{broken".utf8).write(to: file)
        #expect(throws: (any Error).self) { try DesktopSettings.load(from: file) }
        // A regular file cannot act as a settings parent directory.
        #expect(throws: (any Error).self) { try DesktopSettings.save([], to: file.appendingPathComponent("child")) }
    }
}
