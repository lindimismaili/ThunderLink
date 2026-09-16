import AppKit
import PowerSupport

/// App-side lease publisher. Authorization is explicit in Settings, once per
/// app launch, and handled by macOS; ThunderLink never sees the admin password.
final class LidSleepController {
    private var directory: URL?
    private var process: Process?
    private var timer: Timer?
    private var requested = false
    var onStatus: ((String, Bool) -> Void)?

    func setRequested(_ value: Bool) { requested = value; refresh() }

    func authorize() {
        guard process == nil else { return }
        let helper = Bundle.main.executableURL!.deletingLastPathComponent()
            .appendingPathComponent("ThunderLinkPowerHelper")
        guard FileManager.default.isExecutableFile(atPath: helper.path) else {
            onStatus?("Closed-lid helper is missing. Rebuild or reinstall ThunderLink.", false); return
        }
        do {
            let directory = URL(fileURLWithPath: "/private/tmp/thunderlink-power-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false,
                attributes: [.posixPermissions: 0o700])
            self.directory = directory
            FileManager.default.createFile(atPath: directory.appendingPathComponent("status").path,
                contents: Data(), attributes: [.posixPermissions: 0o600])
            try writeLease()
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
            // Quote twice: the command is a shell string within AppleScript.
            let command = [helper.path, directory.path, String(getuid())].map(Self.shellQuote).joined(separator: " ")
            let script = "with timeout of 31536000 seconds\ndo shell script \(Self.appleScriptQuote(command)) with administrator privileges\nend timeout"
            process.arguments = ["-e", script]
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            process.terminationHandler = { [weak self] finished in
                DispatchQueue.main.async {
                    guard let self, self.process === finished else { return }
                    self.timer?.invalidate(); self.timer = nil
                    self.process = nil
                    let status = self.readStatus()
                    self.cleanup()
                    self.onStatus?(status == "conflict" ? "Sleep is already disabled outside ThunderLink. Restore it before authorizing." :
                        PowerHardware.sleepDisabled == true ? "Sleep remains disabled after the helper stopped. Follow the recovery steps in SETUP.md." :
                        "Closed-lid override is not authorized. Authorize it for this app launch.", false)
                }
            }
            self.process = process
            try process.run()
            onStatus?("Waiting for administrator authorization…", true)
            let timer = Timer(timeInterval: 1, repeats: true) { [weak self] _ in self?.refresh() }
            self.timer = timer; RunLoop.main.add(timer, forMode: .common)
        } catch {
            process = nil; cleanup()
            onStatus?("Could not start closed-lid authorization: \(error.localizedDescription)", false)
        }
    }

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
    private static func appleScriptQuote(_ value: String) -> String {
        "\"" + value.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }
    private func writeLease() throws {
        guard let directory else { return }
        let url = directory.appendingPathComponent("lease")
        let next = directory.appendingPathComponent("lease.next")
        let data = try JSONEncoder().encode(PowerLease(uptime: ProcessInfo.processInfo.systemUptime, requested: requested))
        try data.write(to: next, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: next.path)
        guard rename(next.path, url.path) == 0 else { throw CocoaError(.fileWriteUnknown) }
    }
    private func readStatus() -> String? {
        guard let directory else { return nil }
        return try? String(contentsOf: directory.appendingPathComponent("status"), encoding: .utf8)
    }
    private func refresh() {
        guard directory != nil else { return }
        do { try writeLease() } catch {
            onStatus?("Power lease failed; closed-lid override will release within 4 seconds.", true); return
        }
        switch readStatus() {
        case "active": onStatus?("Closed-lid override active on AC power", true)
        case "ready": onStatus?(requested ? "Closed-lid override authorized · waiting for AC power or a safe temperature" :
            "Closed-lid override authorized · waiting for an enabled session", true)
        case "error": onStatus?("Power override failed; the helper is retrying cleanup. Keep the lid open.", true)
        default: break
        }
    }
    func stop() {
        requested = false; try? writeLease()
        timer?.invalidate(); timer = nil
        // Do not kill the root helper: its lease expires and it restores sleep.
        // Removing the directory also revokes any late authorization response.
        cleanup()
    }
    private func cleanup() {
        if let directory { try? FileManager.default.removeItem(at: directory) }
        directory = nil
    }
    deinit { stop() }
}
