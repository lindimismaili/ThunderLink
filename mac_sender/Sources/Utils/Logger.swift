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
        }
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
