import Testing
import Foundation
@testable import ThunderLinkSender

final class ProtocolTests {
    @Test func testSharedGoldenHeader() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let hex = try String(contentsOf: root.appendingPathComponent("common/fixtures/frame_header_v3.hex"))
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let expected = Data(stride(from: 0, to: hex.count, by: 2).map { offset in
            let start = hex.index(hex.startIndex, offsetBy: offset)
            return UInt8(hex[start..<hex.index(start, offsetBy: 2)], radix: 16)!
        })
        let actual = FrameHeaderEncoder.makeHeader(frameID: 0x0102030405060708,
            sequence: 2, totalPackets: 3, payloadSize: 1400, frameTotalBytes: 4200,
            width: 1920, height: 1080, format: 1, flags: 1,
            sessionID: StreamSessionID(0x1020304050607080)!)
        XCTAssertEqual(actual, expected)
        XCTAssertEqual(actual.count, 48)
        XCTAssertEqual(NetworkConfig.protocolVersion, 4)
    }

    @Test func testFragmentedAndCombinedMessages() throws {
        let bytes = try FramedJSON.encode(["type": 5, "sent_time": 42])
        let parser = FramedJSONDecoder()
        for byte in bytes.dropLast() { XCTAssertTrue(try parser.append(Data([byte])).isEmpty) }
        XCTAssertEqual(try parser.append(Data([bytes.last!])).count, 1)
        XCTAssertEqual(try parser.append(bytes + bytes).count, 2)
        XCTAssertThrowsError(try parser.append(Data([0, 0, 0, 0])))
        parser.reset()
        XCTAssertThrowsError(try parser.append(Data([0, 1, 0, 1])))
    }

    @Test func testConfigurationRejectsUnsafeConversions() throws {
        let valid: [String: Any] = ["session_id": "ffffffffffffffff", "width": 1920,
            "height": 1080, "fps": 60, "codec": 1, "bitrate": 25000]
        XCTAssertEqual(try ControlProtocol.configuration(from: valid, connectionToken: 1).sessionID.rawValue, UInt64.max)
        for (field, bad) in [("width", -1 as Any), ("width", 3842), ("height", 1079),
                             ("fps", true), ("fps", 1.5), ("fps", 241), ("codec", 2),
                             ("bitrate", NSNumber(value: UInt64.max)), ("bitrate", 999)] {
            var object = valid; object[field] = bad
            XCTAssertThrowsError(try ControlProtocol.configuration(from: object, connectionToken: 1), field)
        }
        for sid in ["0", "0000000000000000", "FFFFFFFFFFFFFFFF", "10000000000000000"] {
            XCTAssertNil(StreamSessionID(hex: sid))
        }
        let config = try ControlProtocol.configuration(from: valid, connectionToken: 1)
        XCTAssertThrowsError(try config.resolved(width: 5120, height: 2880))
    }

    @Test func testArgumentsAreValidatedBeforeMutation() throws {
        for args in [["--fps"], ["--fps", "999999999999999999999"], ["--bitrate", "0"],
                     ["--res", "1921x1080"], ["--input-port", "65536"], ["--codec", "av1"],
                     ["--bind", "127.0.0.999"], ["--unknown"]] {
            XCTAssertThrowsError(try RuntimeConfiguration.parse(arguments: ["sender"] + args))
        }
        for resolution in ["0x0", "0x1080", "3840x2160"] {
            XCTAssertNoThrow(try RuntimeConfiguration.parse(arguments: ["sender", "--res", resolution]))
        }
    }

    @Test func testIdlePartialMessageExpiresWithoutSleeping() throws {
        var now: UInt64 = 0
        let parser = FramedJSONDecoder(clock: { now })
        _ = try parser.append(Data([0, 0]))
        now = 1_999
        XCTAssertNoThrow(try parser.checkDeadline())
        now = 2_000
        XCTAssertThrowsError(try parser.checkDeadline())
        XCTAssertThrowsError(try parser.finish())
        parser.reset()
        XCTAssertNoThrow(try parser.finish())
    }
}
