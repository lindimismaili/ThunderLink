import Foundation
import AppKit
import Darwin

/// Settings are validated through the same parser as command-line options.
/// Store only explicitly saved options; launch overrides never leak into this file.
enum DesktopSettings {
    static let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("ThunderLink", isDirectory: true)
    static let file = directory.appendingPathComponent("settings.json")
    static var startupError: String?
    static var savedArguments: [String] = []
    static var lockDescriptor: Int32 = -1
    static let showNotification = Notification.Name("com.thunderlink.sender.show.\(getuid())")

    static func load(from url: URL = file) throws -> [String] {
        guard FileManager.default.fileExists(atPath: url.path) else { return [] }
        let args = try JSONDecoder().decode([String].self, from: Data(contentsOf: url))
        _ = try RuntimeConfiguration.parse(arguments: ["ThunderLink"] + args)
        return args
    }

    static func save(_ arguments: [String], to url: URL = file) throws {
        _ = try RuntimeConfiguration.parse(arguments: ["ThunderLink"] + arguments)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(arguments).write(to: url, options: .atomic)
    }

    static func acquireInstance() throws -> Bool {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        lockDescriptor = open(directory.appendingPathComponent("instance.lock").path, O_CREAT | O_RDWR, 0o600)
        guard lockDescriptor >= 0 else { throw CocoaError(.fileWriteNoPermission) }
        if flock(lockDescriptor, LOCK_EX | LOCK_NB) != 0 {
            close(lockDescriptor); lockDescriptor = -1
            DistributedNotificationCenter.default().postNotificationName(showNotification, object: nil, userInfo: nil, deliverImmediately: true)
            return false
        }
        _ = fcntl(lockDescriptor, F_SETFD, FD_CLOEXEC)
        return true
    }

    static func argumentsSettingKeepAwake(_ enabled: Bool, in arguments: [String]) -> [String] {
        arguments.filter { $0 != "--keep-awake" && $0 != "--no-keep-awake" }
            + [enabled ? "--keep-awake" : "--no-keep-awake"]
    }

    static func releaseInstance() {
        if lockDescriptor >= 0 { flock(lockDescriptor, LOCK_UN); close(lockDescriptor); lockDescriptor = -1 }
    }
}
