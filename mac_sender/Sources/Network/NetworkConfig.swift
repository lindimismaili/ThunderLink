import Foundation

/// Network and stream limits mirrored in `common/protocol.h`.
enum NetworkConfig {
    static var macIP = "169.254.1.1"
    static var winIP = "169.254.1.2"
    static var autoAssignIP = true
    static let linkSubnet = "255.255.255.0"
    static var captureDisplayIndex = 0

    static var videoPort: UInt16 = 50001
    static var inputPort: UInt16 = 50002
    static var controlPort: UInt16 = 50003
    static let crossPort: UInt16 = 50004
    static let crossService = "_thunderlink-cross._tcp"
    static let crossLeaseMs = 1_000
    static let crossPingMs = 250
    static let crossQueueLimit = 256
    static let crossMaxDisplays = 32

    static var defaultCodec: TlnkCodecSwift = .hevc
    static var defaultBitrateMbps = 25
    static var defaultWidth = 2560
    static var defaultHeight = 1440
    static var defaultFPS = 60

    static let mtu = 1500
    static let headerSize = 48
    static let maxPayload = 1400
    static let magic: UInt32 = 0x544C4E4B
    static let protocolVersion: UInt32 = 4

    static let recvBufferSize = 4 * 1024 * 1024
    static let sendBufferSize = 4 * 1024 * 1024

    static let maxWidth = 3840
    static let maxHeight = 2160
    static let minFPS = 1
    static let maxFPS = 240
    static let minBitrateKbps = 1_000
    static let maxBitrateKbps = 200_000

    static let frameTimeoutMs = 50
    static let connectTimeoutMs = 2_000
    static let writeTimeoutMs = 2_000
    static let handshakeTimeoutMs = 5_000
    static let peerSilenceTimeoutMs = 5_000
    static let reconnectIntervalMs = 1_000
    static let pingIntervalMs = 1_000

    static let formatBGRA32: UInt16 = 0
    static let formatH264: UInt16 = 1
    static let formatHEVC: UInt16 = 2

    static func wireFormat(for codec: TlnkCodecSwift) -> UInt16 {
        codec == .h264 ? formatH264 : formatHEVC
    }

    /// Applies validated values atomically. Invalid input is reported before
    /// networking or display changes begin.
    static func applyCommandLine(_ args: [String]) throws {
        let parsed = try RuntimeConfiguration.parse(arguments: args)
        macIP = parsed.macIP
        winIP = parsed.winIP
        autoAssignIP = parsed.autoAssignIP
        captureDisplayIndex = parsed.captureDisplayIndex
        videoPort = parsed.videoPort
        inputPort = parsed.inputPort
        controlPort = parsed.controlPort
        defaultCodec = parsed.codec
        defaultBitrateMbps = parsed.bitrateKbps / 1_000
        defaultWidth = parsed.width
        defaultHeight = parsed.height
        defaultFPS = parsed.fps
    }
}

enum TlnkCodecSwift: Int, Codable, Equatable, CustomStringConvertible {
    case h264 = 0
    case hevc = 1

    var description: String { self == .hevc ? "HEVC" : "H.264" }
}

struct RuntimeConfiguration: Equatable {
    var macIP = "169.254.1.1"
    var winIP = "169.254.1.2"
    var autoAssignIP = true
    var captureDisplayIndex = 0
    var videoPort: UInt16 = 50001
    var inputPort: UInt16 = 50002
    var controlPort: UInt16 = 50003
    var codec: TlnkCodecSwift = .hevc
    var bitrateKbps = 25_000
    var width = 2560
    var height = 1440
    var fps = 60

    static func parse(arguments: [String]) throws -> RuntimeConfiguration {
        var result = RuntimeConfiguration()
        var index = 1

        func value(after flag: String) throws -> String {
            guard index + 1 < arguments.count else {
                throw RuntimeConfigurationError.missingValue(flag)
            }
            index += 1
            return arguments[index]
        }

        while index < arguments.count {
            let flag = arguments[index]
            switch flag {
            case "--peer":
                let value = try value(after: flag)
                guard isIPv4(value), value != "0.0.0.0", value != "255.255.255.255" else {
                    throw RuntimeConfigurationError.invalidValue(flag, value)
                }
                result.winIP = value
            case "--bind":
                let value = try value(after: flag)
                guard isIPv4(value) else { throw RuntimeConfigurationError.invalidValue(flag, value) }
                result.macIP = value
            case "--no-auto-ip":
                result.autoAssignIP = false
            case "--display":
                let value = try value(after: flag)
                guard let parsed = Int(value), parsed >= 0 else {
                    throw RuntimeConfigurationError.invalidValue(flag, value)
                }
                result.captureDisplayIndex = parsed
            case "--video-port", "--input-port", "--control-port":
                let value = try value(after: flag)
                guard let parsed = UInt16(value), parsed > 0 else {
                    throw RuntimeConfigurationError.invalidValue(flag, value)
                }
                if flag == "--video-port" { result.videoPort = parsed }
                if flag == "--input-port" { result.inputPort = parsed }
                if flag == "--control-port" { result.controlPort = parsed }
            case "--codec":
                let value = try value(after: flag).lowercased()
                guard let codec = ["h264": TlnkCodecSwift.h264, "hevc": .hevc][value] else {
                    throw RuntimeConfigurationError.invalidValue(flag, value)
                }
                result.codec = codec
            case "--bitrate":
                let value = try value(after: flag)
                guard let mbps = Int(value),
                      mbps >= NetworkConfig.minBitrateKbps / 1_000,
                      mbps <= NetworkConfig.maxBitrateKbps / 1_000 else {
                    throw RuntimeConfigurationError.invalidValue(flag, value)
                }
                result.bitrateKbps = mbps * 1_000
            case "--fps":
                let value = try value(after: flag)
                guard let fps = Int(value), (NetworkConfig.minFPS...NetworkConfig.maxFPS).contains(fps) else {
                    throw RuntimeConfigurationError.invalidValue(flag, value)
                }
                result.fps = fps
            case "--res":
                let value = try value(after: flag)
                let pieces = value.lowercased().split(separator: "x", omittingEmptySubsequences: false)
                guard pieces.count == 2,
                      let width = Int(pieces[0]), let height = Int(pieces[1]),
                      StreamConfiguration.isValidRequestedResolution(width: width, height: height) else {
                    throw RuntimeConfigurationError.invalidValue(flag, value)
                }
                result.width = width
                result.height = height
            default:
                throw RuntimeConfigurationError.unknownFlag(flag)
            }
            index += 1
        }
        return result
    }

    private static func isIPv4(_ value: String) -> Bool {
        let parts = value.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return false }
        return parts.allSatisfy { part in
            guard !part.isEmpty, part.allSatisfy(\.isNumber), let number = Int(part) else { return false }
            return (0...255).contains(number) && (part == "0" || !part.hasPrefix("0"))
        }
    }
}

enum RuntimeConfigurationError: Error, LocalizedError, Equatable {
    case missingValue(String)
    case invalidValue(String, String)
    case unknownFlag(String)

    var errorDescription: String? {
        switch self {
        case .missingValue(let flag): return "Missing value for \(flag)"
        case .invalidValue(let flag, let value): return "Invalid value '\(value)' for \(flag)"
        case .unknownFlag(let flag): return "Unknown option \(flag)"
        }
    }
}
