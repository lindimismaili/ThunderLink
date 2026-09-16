import CoreGraphics
import Carbon.HIToolbox
import Foundation
import Security
import Testing
@testable import ThunderLinkSender

private final class CrossRecordingPoster: InputEventPosting {
    struct Key { let code: CGKeyCode; let down: Bool; let flags: CGEventFlags }
    var points: [CGPoint] = []
    var mouseTypes: [CGEventType] = []
    var keys: [Key] = []
    var scroll: [(Int32, Int32)] = []
    func postMouse(type: CGEventType, point: CGPoint, button: CGMouseButton) -> Bool {
        mouseTypes.append(type); points.append(point); return true
    }
    func postKey(code: CGKeyCode, isDown: Bool, modifiers: CGEventFlags) -> Bool {
        keys.append(Key(code: code, down: isDown, flags: modifiers)); return true
    }
    func postScroll(deltaX: Int32, deltaY: Int32) -> Bool {
        scroll.append((deltaX, deltaY)); return true
    }
}

struct CrossTests {
    @Test func goldenContractAndNegativeOriginMapping() throws {
        let fixtureURL = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("common/fixtures/cross_v4.json")
        let fixture = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: fixtureURL)) as? [String: Any])
        #expect((fixture["version"] as? NSNumber)?.uint32Value == NetworkConfig.protocolVersion)
        #expect((fixture["port"] as? NSNumber)?.uint16Value == NetworkConfig.crossPort)
        #expect(fixture["service"] as? String == NetworkConfig.crossService)
        #expect((fixture["lease_ms"] as? NSNumber)?.intValue == NetworkConfig.crossLeaseMs)
        #expect((fixture["ping_ms"] as? NSNumber)?.intValue == NetworkConfig.crossPingMs)
        #expect((fixture["queue_limit"] as? NSNumber)?.intValue == NetworkConfig.crossQueueLimit)

        let pairing = try #require(fixture["pairing"] as? [String: Any])
        #expect(CrossProtocol.verificationCode(clientFingerprint: try #require(pairing["client_fingerprint"] as? String),
                                               serverFingerprint: try #require(pairing["server_fingerprint"] as? String)) ==
                pairing["verification_code"] as? String)

        let topologyObject = try #require(fixture["topology"] as? [String: Any])
        let displays = try #require(topologyObject["displays"] as? [[String: Any]])
        let external = try #require(displays.first)
        let descriptor = CrossDisplayDescriptor(id: try #require(external["id"] as? String),
            name: try #require(external["name"] as? String),
            bounds: CGRect(x: try #require(external["x"] as? NSNumber).doubleValue,
                           y: try #require(external["y"] as? NSNumber).doubleValue,
                           width: try #require(external["width"] as? NSNumber).doubleValue,
                           height: try #require(external["height"] as? NSNumber).doubleValue),
            scale: try #require(external["scale"] as? NSNumber).doubleValue,
            rotation: try #require(external["rotation"] as? NSNumber).doubleValue,
            primary: try #require(external["primary"] as? Bool), systemID: 1)
        let enter = try CrossProtocol.parseEnter(try #require(fixture["enter"] as? [String: Any]))
        let point = try #require(CrossCoordinateMapper.point(x: enter.4, y: enter.5, in: descriptor))
        let expected = try #require(fixture["expected_native_center"] as? [String: Any])
        let expectedX = try #require(expected["x"] as? NSNumber).doubleValue
        let expectedY = try #require(expected["y"] as? NSNumber).doubleValue
        #expect(abs(Double(point.x) - expectedX) < 0.0001)
        #expect(abs(Double(point.y) - expectedY) < 0.0001)
    }

    @Test func ownershipGenerationMappingAndRelease() throws {
        let poster = CrossRecordingPoster()
        let input = CrossInputController(poster: poster)
        let session = try #require(StreamSessionID(hex: "1020304050607080"))
        let display = CrossDisplayDescriptor(id: "mac", name: "Mac", bounds: CGRect(x: -100, y: 50, width: 200, height: 100),
                                             scale: 2, rotation: 0, primary: true, systemID: 1)
        let topology = CrossDisplayTopology(revision: 7, displays: [display])
        input.configure(CrossConfigure(sessionID: session, topologyRevision: 7, swapControlCommand: true), topology: topology)
        try input.enter(sessionID: session, topologyRevision: 7, generation: 1,
                        displayID: "mac", x: 0.5, y: 0.5)
        let down = InputEventMessage(type: "key", sessionID: session, timestamp: nil,
            x: nil, y: nil, button: nil, state: "down", keyCode: 0x11,
            modifiers: ["ctrl"], deltaX: nil, deltaY: nil)
        try input.process(CrossInputEnvelope(sessionID: session, topologyRevision: 7,
            generation: 1, displayID: nil, event: down))
        #expect(poster.keys.last?.flags.contains(.maskCommand) == true)
        try input.leave(sessionID: session, topologyRevision: 7, generation: 1)
        #expect(poster.keys.map(\.down) == [true, false])
        #expect(throws: CrossProtocolError.self) {
            try input.process(CrossInputEnvelope(sessionID: session, topologyRevision: 7,
                generation: 1, displayID: nil, event: down))
        }
        #expect(throws: CrossProtocolError.self) {
            try input.enter(sessionID: session, topologyRevision: 7, generation: 1,
                            displayID: "mac", x: 0.5, y: 0.5)
        }
        try input.enter(sessionID: session, topologyRevision: 7, generation: 2,
                        displayID: "mac", x: 1, y: 1)
        #expect(poster.points.last == CGPoint(x: 99.5, y: 149.5))
        if CrossOwnershipLease.expired(now: 2_000, lastPing: 1_000, ownsInput: input.hasOwnership) {
            input.reset()
        }
        #expect(throws: CrossProtocolError.self) {
            try input.process(CrossInputEnvelope(sessionID: session, topologyRevision: 7,
                generation: 2, displayID: nil, event: down))
        }
        input.invalidateSession()
        #expect(!input.hasOwnership)
    }

    @Test func leaseExpiresOnlyActiveOwnershipAtDeadline() {
        #expect(!CrossOwnershipLease.expired(now: 1_999, lastPing: 1_000, ownsInput: true))
        #expect(CrossOwnershipLease.expired(now: 2_000, lastPing: 1_000, ownsInput: true))
        #expect(!CrossOwnershipLease.expired(now: 3_000, lastPing: 1_000, ownsInput: false))
        #expect(!CrossOwnershipLease.expired(now: 999, lastPing: 1_000, ownsInput: true))
    }

    @Test func transportLivenessPingCannotRenewInputOwnership() throws {
        let previous: UInt64 = 1_000
        #expect(try CrossProtocol.pingRenewsOwnership(["type": "ping", "sequence": 1]))
        #expect(try CrossProtocol.pingRenewsOwnership(
            ["type": "ping", "sequence": 1, "renew_ownership": true]))
        #expect(try !CrossProtocol.pingRenewsOwnership(
            ["type": "ping", "sequence": 1, "renew_ownership": false]))
        #expect(CrossOwnershipLease.lastRenewal(previous: previous, now: 1_250,
                                                renewOwnership: false) == previous)
        #expect(CrossOwnershipLease.expired(now: 2_000, lastPing: previous, ownsInput: true))
        #expect(CrossOwnershipLease.lastRenewal(previous: previous, now: 1_250,
                                                renewOwnership: true) == 1_250)
        let freshOwnership = CrossOwnershipLease.started(now: 5_000)
        #expect(!CrossOwnershipLease.expired(now: 5_999, lastPing: freshOwnership, ownsInput: true))
        #expect(CrossOwnershipLease.expired(now: 6_000, lastPing: freshOwnership, ownsInput: true))
        #expect(throws: CrossProtocolError.self) {
            _ = try CrossProtocol.pingRenewsOwnership(
                ["type": "ping", "sequence": 1, "renew_ownership": 0])
        }
    }

    @Test func independentSystemInhibitionsRequireEveryResumeSignal() {
        var state = CrossSystemInhibitionState()
        state.update(.screenLocked, active: true)
        state.update(.sleeping, active: true)
        state.update(.sleeping, active: false)
        #expect(state.isSuspended)
        #expect(state.reasons == [.screenLocked])
        state.update(.screensSleeping, active: true)
        state.update(.screenLocked, active: false)
        #expect(state.isSuspended)
        #expect(state.reasons == [.screensSleeping])
        state.update(.screensSleeping, active: false)
        #expect(!state.isSuspended)
    }

    @Test func crossKeyMappingSupportsNumpadAndDropsUnknownKeys() throws {
        #expect(EventMapper.mapCrossKeyCode(0x60, swapControlCommand: true) == CGKeyCode(kVK_ANSI_Keypad0))
        #expect(EventMapper.mapCrossKeyCode(0x69, swapControlCommand: true) == CGKeyCode(kVK_ANSI_Keypad9))
        #expect(EventMapper.mapCrossKeyCode(0x14, swapControlCommand: true) == CGKeyCode(kVK_CapsLock))
        #expect(EventMapper.mapCrossKeyCode(0xFF, swapControlCommand: true) == nil)

        let poster = CrossRecordingPoster()
        let input = CrossInputController(poster: poster)
        let session = try #require(StreamSessionID(hex: "1020304050607080"))
        let display = CrossDisplayDescriptor(id: "mac", name: "Mac",
            bounds: CGRect(x: 0, y: 0, width: 100, height: 100),
            scale: 1, rotation: 0, primary: true, systemID: 1)
        input.configure(CrossConfigure(sessionID: session, topologyRevision: 1, swapControlCommand: true),
                        topology: CrossDisplayTopology(revision: 1, displays: [display]))
        try input.enter(sessionID: session, topologyRevision: 1, generation: 1,
                        displayID: "mac", x: 0.5, y: 0.5)
        let unknown = InputEventMessage(type: "key", sessionID: session, timestamp: nil,
            x: nil, y: nil, button: nil, state: "down", keyCode: 0xFF,
            modifiers: [], deltaX: nil, deltaY: nil)
        try input.process(CrossInputEnvelope(sessionID: session, topologyRevision: 1,
            generation: 1, displayID: nil, event: unknown))
        #expect(poster.keys.isEmpty)
    }

    @Test func configureRequiresAJSONBoolean() throws {
        let base: [String: Any] = ["session_id": "1020304050607080", "topology_revision": 1]
        #expect(throws: Never.self) {
            var object = base
            object["swap_ctrl_cmd"] = true
            _ = try CrossProtocol.parseConfigure(object)
        }
        #expect(throws: CrossProtocolError.self) {
            var object = base
            object["swap_ctrl_cmd"] = 1
            _ = try CrossProtocol.parseConfigure(object)
        }
    }

    @Test func pairingRequiresBothApprovalsAndNestedInputUsesEnvelopeSession() throws {
        #expect(!CrossProtocol.pairingComplete(localApproved: false, remoteApproved: false))
        #expect(!CrossProtocol.pairingComplete(localApproved: true, remoteApproved: false))
        #expect(!CrossProtocol.pairingComplete(localApproved: false, remoteApproved: true))
        #expect(CrossProtocol.pairingComplete(localApproved: true, remoteApproved: true))

        let object: [String: Any] = [
            "type": "input", "session_id": "1020304050607080", "topology_revision": 7,
            "generation": 2, "event": ["type": "key", "state": "down", "key_code": 65,
                                                     "modifiers": ["ctrl"]]
        ]
        let envelope = try CrossProtocol.parseInput(object)
        #expect(envelope.sessionID.description == "1020304050607080")
        #expect(envelope.event.sessionID == envelope.sessionID)
        #expect(envelope.event.keyCode == 65)
    }

    @Test func helloBindsClaimedIdentityAndPrePairingTrafficIsRestricted() throws {
        let fingerprint = String(repeating: "a", count: 64)
        let hello: [String: Any] = ["type": "hello", "version": 4,
                                    "device_id": fingerprint, "name": "Windows PC",
                                    "role": "windows"]
        #expect(throws: Never.self) {
            _ = try CrossProtocol.parseHello(hello, certificateFingerprint: fingerprint)
        }
        #expect(throws: CrossProtocolError.self) {
            _ = try CrossProtocol.parseHello(hello,
                certificateFingerprint: String(repeating: "b", count: 64))
        }
        #expect(throws: CrossProtocolError.self) {
            var incompatible = hello
            incompatible["version"] = 3
            _ = try CrossProtocol.parseHello(incompatible, certificateFingerprint: fingerprint)
        }
        #expect(CrossProtocol.allowsBeforePairing("pair_approve"))
        #expect(!CrossProtocol.allowsBeforePairing("topology"))
        #expect(!CrossProtocol.allowsBeforePairing("configure"))
        #expect(!CrossProtocol.allowsBeforePairing("input"))
    }

    @Test func malformedEnterScopeGenerationAndCoordinatesAreRejected() throws {
        let valid: [String: Any] = ["type": "enter", "session_id": "1020304050607080",
                                    "topology_revision": 7, "generation": 1,
                                    "display_id": "mac-display", "x": 0.5, "y": 0.5]
        #expect(throws: Never.self) { _ = try CrossProtocol.parseEnter(valid) }

        var invalidObjects: [[String: Any]] = []
        var invalid = valid; invalid["session_id"] = "0000000000000000"; invalidObjects.append(invalid)
        invalid = valid; invalid["topology_revision"] = 0; invalidObjects.append(invalid)
        invalid = valid; invalid["generation"] = 0; invalidObjects.append(invalid)
        invalid = valid; invalid["x"] = -0.01; invalidObjects.append(invalid)
        invalid = valid; invalid["y"] = 1.01; invalidObjects.append(invalid)
        invalid = valid; invalid["x"] = "0.5"; invalidObjects.append(invalid)
        for object in invalidObjects {
            #expect(throws: CrossProtocolError.self) { _ = try CrossProtocol.parseEnter(object) }
        }
    }

    @Test func duplicateDisplayIdentitiesAreReportedWithoutDictionaryFailure() {
        let first = CrossDisplayDescriptor(id: "duplicate", name: "One",
            bounds: CGRect(x: 0, y: 0, width: 100, height: 100), scale: 1,
            rotation: 0, primary: true, systemID: 1)
        let second = CrossDisplayDescriptor(id: "duplicate", name: "Two",
            bounds: CGRect(x: 100, y: 0, width: 100, height: 100), scale: 1,
            rotation: 0, primary: false, systemID: 2)
        let topology = CrossDisplayTopology(revision: 1, displays: [first, second])
        #expect(topology.identityError != nil)
        let input = CrossInputController(poster: CrossRecordingPoster())
        input.updateTopology(topology)
        #expect(!input.hasOwnership)
    }

    @Test func crossFramingRejectsOversizeAndExpiresPartialFrames() throws {
        var now: UInt64 = 100
        let decoder = CrossFrameDecoder(clock: { now })
        _ = try decoder.append(Data([0, 0]))
        now += 4_999
        #expect(throws: Never.self) { try decoder.checkDeadline(now: now) }
        now += 1
        #expect(throws: CrossProtocolError.self) { try decoder.checkDeadline(now: now) }

        let invalid = CrossFrameDecoder()
        #expect(throws: CrossProtocolError.self) {
            try invalid.append(Data([0, 1, 0, 1]))
        }
    }

    @Test func ephemeralSelfSignedIdentityIsValidAndMatchesPublicKey() throws {
        var error: Unmanaged<CFError>?
        let attributes: [CFString: Any] = [kSecAttrKeyType: kSecAttrKeyTypeECSECPrimeRandom,
                                           kSecAttrKeySizeInBits: 256]
        let privateKey = try #require(SecKeyCreateRandomKey(attributes as CFDictionary, &error))
        let publicKey = try #require(SecKeyCopyPublicKey(privateKey))
        let publicData = try #require(SecKeyCopyExternalRepresentation(publicKey, &error) as Data?)
        let der = try CrossIdentityStore.makeCertificate(publicKey: publicData, privateKey: privateKey)
        let certificate = try #require(SecCertificateCreateWithData(nil, der as CFData))
        let certificateKey = try #require(SecCertificateCopyKey(certificate))
        let certificatePublicData = try #require(SecKeyCopyExternalRepresentation(certificateKey, &error) as Data?)
        #expect(certificatePublicData == publicData)

        var trust: SecTrust?
        #expect(SecTrustCreateWithCertificates(certificate, SecPolicyCreateBasicX509(), &trust) == errSecSuccess)
        let resolvedTrust = try #require(trust)
        #expect(SecTrustSetAnchorCertificates(resolvedTrust, [certificate] as CFArray) == errSecSuccess)
        #expect(SecTrustSetAnchorCertificatesOnly(resolvedTrust, true) == errSecSuccess)
        var trustError: CFError?
        #expect(SecTrustEvaluateWithError(resolvedTrust, &trustError))
    }
}
