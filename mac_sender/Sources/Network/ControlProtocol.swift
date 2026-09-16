import Foundation

enum TlnkControlType: Int, Codable {
    case hello = 1
    case start = 2
    case stop = 3
    case configure = 4
    case ping = 5
    case pong = 6
    case bye = 7
    case requestKeyframe = 8
}

enum ControlField {
    static let type = "type"
    static let version = "version"
    static let width = "width"
    static let height = "height"
    static let fps = "fps"
    static let codec = "codec"
    static let bitrate = "bitrate"
    static let sentTime = "sent_time"
    static let sessionID = "session_id"
    static let reason = "reason"
}

struct StreamSessionID: Hashable, Codable, CustomStringConvertible {
    let rawValue: UInt64

    init?(_ rawValue: UInt64) {
        guard rawValue != 0 else { return nil }
        self.rawValue = rawValue
    }

    init?(hex: String) {
        guard hex.count == 16,
              hex.allSatisfy({ $0.isNumber || ("a"..."f").contains(String($0)) }),
              let value = UInt64(hex, radix: 16), value != 0 else { return nil }
        self.rawValue = value
    }

    var description: String { String(format: "%016llx", rawValue) }

    init(from decoder: Decoder) throws {
        let value = try decoder.singleValueContainer().decode(String.self)
        guard let parsed = StreamSessionID(hex: value) else {
            throw DecodingError.dataCorruptedError(in: try decoder.singleValueContainer(),
                                                   debugDescription: "session_id must be 16 lowercase hex characters and nonzero")
        }
        self = parsed
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(description)
    }
}

struct StreamConfiguration: Equatable {
    let sessionID: StreamSessionID
    let width: Int
    let height: Int
    let fps: Int
    let codec: TlnkCodecSwift
    let bitrateKbps: Int
    let connectionToken: UInt64

    init(sessionID: StreamSessionID, width: Int, height: Int, fps: Int,
         codec: TlnkCodecSwift, bitrateKbps: Int, connectionToken: UInt64) throws {
        guard Self.isValidRequestedResolution(width: width, height: height) else {
            throw StreamConfigurationError.invalidResolution(width, height)
        }
        guard (NetworkConfig.minFPS...NetworkConfig.maxFPS).contains(fps) else {
            throw StreamConfigurationError.invalidFPS(fps)
        }
        guard (NetworkConfig.minBitrateKbps...NetworkConfig.maxBitrateKbps).contains(bitrateKbps) else {
            throw StreamConfigurationError.invalidBitrate(bitrateKbps)
        }
        self.sessionID = sessionID
        self.width = width
        self.height = height
        self.fps = fps
        self.codec = codec
        self.bitrateKbps = bitrateKbps
        self.connectionToken = connectionToken
    }

    static func isValidRequestedResolution(width: Int, height: Int) -> Bool {
        if width == 0 && height == 0 { return true }
        if width == 0 {
            return height >= 2 && height <= NetworkConfig.maxHeight && height.isMultiple(of: 2)
        }
        return width >= 2 && width <= NetworkConfig.maxWidth && width.isMultiple(of: 2)
            && height >= 2 && height <= NetworkConfig.maxHeight && height.isMultiple(of: 2)
    }

    func resolved(width: Int, height: Int) throws -> StreamConfiguration {
        guard width >= 2, width <= NetworkConfig.maxWidth, width.isMultiple(of: 2),
              height >= 2, height <= NetworkConfig.maxHeight, height.isMultiple(of: 2) else {
            throw StreamConfigurationError.invalidResolvedResolution(width, height)
        }
        return try StreamConfiguration(sessionID: sessionID, width: width, height: height,
                                       fps: fps, codec: codec, bitrateKbps: bitrateKbps,
                                       connectionToken: connectionToken)
    }

    var controlFields: [String: Any] {
        [
            ControlField.sessionID: sessionID.description,
            ControlField.width: width,
            ControlField.height: height,
            ControlField.fps: fps,
            ControlField.codec: codec.rawValue,
            ControlField.bitrate: bitrateKbps
        ]
    }
}

enum StreamConfigurationError: Error, LocalizedError, Equatable {
    case missingField(String)
    case invalidSessionID
    case invalidResolution(Int, Int)
    case invalidResolvedResolution(Int, Int)
    case invalidFPS(Int)
    case invalidCodec(Int)
    case invalidBitrate(Int)

    var errorDescription: String? {
        switch self {
        case .missingField(let field): return "Missing configuration field '\(field)'"
        case .invalidSessionID: return "session_id must be 16 lowercase hex characters and nonzero"
        case .invalidResolution(let width, let height): return "Unsupported requested resolution \(width)x\(height)"
        case .invalidResolvedResolution(let width, let height):
            return "Resolved resolution \(width)x\(height) is unsupported; choose a smaller even-sized preset"
        case .invalidFPS(let fps): return "FPS \(fps) is outside 1...240"
        case .invalidCodec(let codec): return "Unknown codec \(codec)"
        case .invalidBitrate(let bitrate): return "Bitrate \(bitrate) kbps is outside 1000...200000"
        }
    }
}

enum ControlProtocol {
    static func configuration(from object: [String: Any], connectionToken: UInt64) throws -> StreamConfiguration {
        guard let sessionText = object[ControlField.sessionID] as? String,
              let sessionID = StreamSessionID(hex: sessionText) else {
            throw StreamConfigurationError.invalidSessionID
        }
        let width = try integer(ControlField.width, in: object)
        let height = try integer(ControlField.height, in: object)
        let fps = try integer(ControlField.fps, in: object)
        let codecRaw = try integer(ControlField.codec, in: object)
        let bitrate = try integer(ControlField.bitrate, in: object)
        guard let codec = TlnkCodecSwift(rawValue: codecRaw) else {
            throw StreamConfigurationError.invalidCodec(codecRaw)
        }
        return try StreamConfiguration(sessionID: sessionID, width: width, height: height,
                                       fps: fps, codec: codec, bitrateKbps: bitrate,
                                       connectionToken: connectionToken)
    }

    static func integer(_ field: String, in object: [String: Any]) throws -> Int {
        guard let number = object[field] as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID() else {
            throw StreamConfigurationError.missingField(field)
        }
        let value = number.int64Value
        guard value >= Int64(Int.min), value <= Int64(Int.max),
              NSNumber(value: value) == number else {
            throw StreamConfigurationError.missingField(field)
        }
        return Int(value)
    }
}

enum FramedJSON {
    static let maximumBodyBytes = 65_536

    static func encode(_ object: [String: Any]) throws -> Data {
        let body = try JSONSerialization.data(withJSONObject: object, options: [])
        guard !body.isEmpty, body.count <= maximumBodyBytes else {
            throw FramedJSONError.invalidLength(body.count)
        }
        var length = UInt32(body.count).bigEndian
        var data = Data(bytes: &length, count: MemoryLayout<UInt32>.size)
        data.append(body)
        return data
    }
}

final class FramedJSONDecoder {
    private var buffer = Data()
    private let clock: () -> UInt64
    private var partialSince: UInt64?

    init(clock: @escaping () -> UInt64 = { DispatchTime.now().uptimeNanoseconds / 1_000_000 }) {
        self.clock = clock
    }

    func checkDeadline() throws {
        if let since = partialSince, clock() - since >= UInt64(NetworkConfig.writeTimeoutMs) {
            throw FramedJSONError.timedOut
        }
    }

    func finish() throws {
        guard buffer.isEmpty else { throw FramedJSONError.truncated }
    }

    func append(_ bytes: Data) throws -> [[String: Any]] {
        try checkDeadline()
        if buffer.isEmpty && !bytes.isEmpty { partialSince = clock() }
        buffer.append(bytes)
        var messages: [[String: Any]] = []
        while buffer.count >= 4 {
            let length = buffer.prefix(4).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
            guard length > 0, length <= FramedJSON.maximumBodyBytes else {
                throw FramedJSONError.invalidLength(Int(length))
            }
            let fullLength = 4 + Int(length)
            guard buffer.count >= fullLength else { break }
            let body = buffer.subdata(in: 4..<fullLength)
            guard let object = try JSONSerialization.jsonObject(with: body) as? [String: Any] else {
                throw FramedJSONError.invalidJSON
            }
            messages.append(object)
            buffer.removeSubrange(0..<fullLength)
            partialSince = buffer.isEmpty ? nil : clock()
        }
        return messages
    }

    func reset() { buffer.removeAll(keepingCapacity: true); partialSince = nil }
}

enum FramedJSONError: Error, Equatable {
    case invalidLength(Int)
    case invalidJSON
    case timedOut
    case truncated
}
