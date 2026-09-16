import Foundation
import os

/// Thread-safe logging utility for ThunderLink Mac sender.
/// Uses Apple's unified logging system (OSLog) with console fallback.
final class TLLogger {
    static let shared = TLLogger()

    enum Level: Int, Comparable {
        case trace, debug, info, warn, error, fatal

        static func < (lhs: Level, rhs: Level) -> Bool {
            return lhs.rawValue < rhs.rawValue
        }

        var label: String {
            switch self {
            case .trace: return "TRACE"
            case .debug: return "DEBUG"
            case .info:  return "INFO"
            case .warn:  return "WARN"
            case .error: return "ERROR"
            case .fatal: return "FATAL"
            }
        }
    }

    var minLevel: Level = .info

    private let osLog = os.Logger(subsystem: "com.thunderlink.sender", category: "general")
    private let queue = DispatchQueue(label: "com.thunderlink.logger")

    // Appends to the repo's cross-device log (logs/mac.log) so it can be read on
    // Windows after a git sync. nil if the file couldn't be opened.
    private let fileHandle: FileHandle? = TLLogger.openLogFile()

    func log(_ level: Level, _ message: String, file: String = #file, line: Int = #line) {
        guard level >= minLevel else { return }

        let filename = (file as NSString).lastPathComponent
        let formatted = "[\(level.label)] \(filename):\(line) - \(message)"

        queue.async {
            switch level {
            case .trace, .debug:
                self.osLog.debug("\(formatted)")
            case .info:
                self.osLog.info("\(formatted)")
            case .warn:
                self.osLog.warning("\(formatted)")
            case .error, .fatal:
                self.osLog.error("\(formatted)")
            }

            // Also print to stdout for command-line visibility
            print(formatted)

            // Append a timestamped line to the cross-device log file.
            if let handle = self.fileHandle,
               let data = ("[\(TLLogger.timestamp())] " + formatted + "\n").data(using: .utf8) {
                try? handle.write(contentsOf: data)
            }
        }
    }

    // MARK: - Cross-device file logging

    /// `<repo>/logs/mac.log`, derived from this source file's location so it is
    /// independent of the process working directory.
    static func logFileURL() -> URL {
        if Bundle.main.bundleURL.pathExtension == "app" {
            return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/ThunderLink/mac.log")
        }
        // #filePath here = <repo>/mac_sender/Sources/Utils/Logger.swift
        let repo = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // Utils
            .deletingLastPathComponent()   // Sources
            .deletingLastPathComponent()   // mac_sender
            .deletingLastPathComponent()   // <repo>
        return repo.appendingPathComponent("logs").appendingPathComponent("mac.log")
    }

    private static func openLogFile() -> FileHandle? {
        let url = logFileURL()
        let fm = FileManager.default
        try? fm.createDirectory(at: url.deletingLastPathComponent(),
                                withIntermediateDirectories: true)
        if !fm.fileExists(atPath: url.path) {
            fm.createFile(atPath: url.path, contents: nil)
        }
        guard let handle = try? FileHandle(forWritingTo: url) else { return nil }
        handle.seekToEndOfFile()
        let header = "\n===== ThunderLink Mac session start: \(timestamp()) =====\n"
        if let data = header.data(using: .utf8) { try? handle.write(contentsOf: data) }
        return handle
    }

    private static func timestamp() -> String {
        let df = DateFormatter()
        df.dateFormat = "HH:mm:ss.SSS"
        return df.string(from: Date())
    }

    func trace(_ msg: String, file: String = #file, line: Int = #line) {
        log(.trace, msg, file: file, line: line)
    }
    func debug(_ msg: String, file: String = #file, line: Int = #line) {
        log(.debug, msg, file: file, line: line)
    }
    func info(_ msg: String, file: String = #file, line: Int = #line) {
        log(.info, msg, file: file, line: line)
    }
    func warn(_ msg: String, file: String = #file, line: Int = #line) {
        log(.warn, msg, file: file, line: line)
    }
    func error(_ msg: String, file: String = #file, line: Int = #line) {
        log(.error, msg, file: file, line: line)
    }
    func fatal(_ msg: String, file: String = #file, line: Int = #line) {
        log(.fatal, msg, file: file, line: line)
    }
}

// Convenience global reference
let logger = TLLogger.shared
