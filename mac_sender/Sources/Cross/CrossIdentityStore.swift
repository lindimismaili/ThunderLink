import CryptoKit
import Foundation
import Security

struct CrossLocalIdentity {
    let identity: SecIdentity
    let certificate: SecCertificate
    let fingerprint: String
}

struct CrossPairedPeer: Identifiable, Equatable {
    let fingerprint: String
    let name: String
    var id: String { fingerprint }
}

enum CrossIdentityError: Error, LocalizedError {
    case keychain(OSStatus)
    case generation(String)
    case identityUnavailable

    var errorDescription: String? {
        switch self {
        case .keychain(let status):
            return SecCopyErrorMessageString(status, nil) as String? ?? "Keychain error \(status)"
        case .generation(let reason): return "Could not create the Cross identity: \(reason)"
        case .identityUnavailable: return "The Cross certificate has no matching private key"
        }
    }
}

/// Cross's private identity and peer pins live in the user's Keychain. JSON
/// settings deliberately contain no private key or trust material.
final class CrossIdentityStore {
    private let keyTag = Data("com.thunderlink.cross.identity.key.v1".utf8)
    private let certificateLabel = "ThunderLink Cross Identity v1"
    private let pinService = "com.thunderlink.cross.paired-peer.v1"

    func loadOrCreateIdentity() throws -> CrossLocalIdentity {
        if let existing = try loadIdentity() { return existing }
        return try createIdentity()
    }

    func isPaired(fingerprint: String) -> Bool {
        pairedPeers().contains { $0.fingerprint == fingerprint }
    }

    func rememberPeer(fingerprint: String, name: String) throws {
        let normalized = fingerprint.lowercased()
        let data = Data(name.utf8)
        let base: [CFString: Any] = [kSecClass: kSecClassGenericPassword,
                                     kSecAttrService: pinService,
                                     kSecAttrAccount: normalized]
        let update: [CFString: Any] = [kSecValueData: data,
                                       kSecAttrLabel: "ThunderLink Cross peer \(name)"]
        let status = SecItemUpdate(base as CFDictionary, update as CFDictionary)
        if status == errSecSuccess { return }
        guard status == errSecItemNotFound else { throw CrossIdentityError.keychain(status) }
        var add = base
        add[kSecValueData] = data
        add[kSecAttrLabel] = "ThunderLink Cross peer \(name)"
        add[kSecAttrAccessible] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let addStatus = SecItemAdd(add as CFDictionary, nil)
        guard addStatus == errSecSuccess else { throw CrossIdentityError.keychain(addStatus) }
    }

    func forgetPeer(fingerprint: String) throws {
        let query: [CFString: Any] = [kSecClass: kSecClassGenericPassword,
                                      kSecAttrService: pinService,
                                      kSecAttrAccount: fingerprint.lowercased()]
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw CrossIdentityError.keychain(status)
        }
    }

    func pairedPeers() -> [CrossPairedPeer] {
        let query: [CFString: Any] = [kSecClass: kSecClassGenericPassword,
                                      kSecAttrService: pinService,
                                      kSecMatchLimit: kSecMatchLimitAll,
                                      kSecReturnAttributes: true,
                                      kSecReturnData: true]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let items = result as? [[String: Any]] else { return [] }
        return items.compactMap { item in
            guard let account = item[kSecAttrAccount as String] as? String,
                  let data = item[kSecValueData as String] as? Data,
                  let name = String(data: data, encoding: .utf8) else { return nil }
            return CrossPairedPeer(fingerprint: account, name: name)
        }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    private func loadIdentity() throws -> CrossLocalIdentity? {
        let certificateQuery: [CFString: Any] = [kSecClass: kSecClassCertificate,
                                                 kSecAttrLabel: certificateLabel,
                                                 kSecMatchLimit: kSecMatchLimitOne,
                                                 kSecReturnRef: true]
        var result: CFTypeRef?
        let status = SecItemCopyMatching(certificateQuery as CFDictionary, &result)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let certificate = result as! SecCertificate? else {
            throw CrossIdentityError.keychain(status)
        }
        var identity: SecIdentity?
        let identityStatus = SecIdentityCreateWithCertificate(nil, certificate, &identity)
        guard identityStatus == errSecSuccess, let identity else {
            // A certificate without its private key is not usable. Remove only
            // this app-owned certificate and create a fresh matched pair.
            SecItemDelete(certificateQuery as CFDictionary)
            return nil
        }
        return local(identity: identity, certificate: certificate)
    }

    private func createIdentity() throws -> CrossLocalIdentity {
        // Remove an orphaned app-owned key before retrying generation.
        let oldKey: [CFString: Any] = [kSecClass: kSecClassKey,
                                       kSecAttrApplicationTag: keyTag,
                                       kSecAttrKeyType: kSecAttrKeyTypeECSECPrimeRandom]
        SecItemDelete(oldKey as CFDictionary)
        let attributes: [CFString: Any] = [
            kSecAttrKeyType: kSecAttrKeyTypeECSECPrimeRandom,
            kSecAttrKeySizeInBits: 256,
            kSecPrivateKeyAttrs: [kSecAttrIsPermanent: true,
                                  kSecAttrApplicationTag: keyTag,
                                  kSecAttrLabel: certificateLabel,
                                  kSecAttrAccessible: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]
        ]
        var error: Unmanaged<CFError>?
        guard let privateKey = SecKeyCreateRandomKey(attributes as CFDictionary, &error) else {
            throw CrossIdentityError.generation(error?.takeRetainedValue().localizedDescription ?? "key generation failed")
        }
        guard let publicKey = SecKeyCopyPublicKey(privateKey),
              let publicData = SecKeyCopyExternalRepresentation(publicKey, &error) as Data? else {
            throw CrossIdentityError.generation(error?.takeRetainedValue().localizedDescription ?? "public key export failed")
        }
        let certificateData = try Self.makeCertificate(publicKey: publicData, privateKey: privateKey)
        guard let certificate = SecCertificateCreateWithData(nil, certificateData as CFData) else {
            throw CrossIdentityError.generation("generated certificate was invalid")
        }
        let add: [CFString: Any] = [kSecClass: kSecClassCertificate,
                                    kSecAttrLabel: certificateLabel,
                                    kSecValueRef: certificate]
        let status = SecItemAdd(add as CFDictionary, nil)
        guard status == errSecSuccess else { throw CrossIdentityError.keychain(status) }
        var identity: SecIdentity?
        guard SecIdentityCreateWithCertificate(nil, certificate, &identity) == errSecSuccess,
              let identity else { throw CrossIdentityError.identityUnavailable }
        return local(identity: identity, certificate: certificate)
    }

    private func local(identity: SecIdentity, certificate: SecCertificate) -> CrossLocalIdentity {
        let data = SecCertificateCopyData(certificate) as Data
        return CrossLocalIdentity(identity: identity, certificate: certificate,
                                  fingerprint: CrossProtocol.fingerprint(data))
    }

    static func makeCertificate(publicKey: Data, privateKey: SecKey) throws -> Data {
        let algorithm = DER.sequence(DER.oid([0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02]))
        let publicAlgorithm = DER.sequence(
            DER.oid([0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01]) +
            DER.oid([0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07]))
        let subjectPublicKeyInfo = DER.sequence(publicAlgorithm + DER.bitString(publicKey))
        let commonName = DER.sequence(DER.set(DER.sequence(
            DER.oid([0x55, 0x04, 0x03]) + DER.utf8("ThunderLink Cross"))))
        var serial = Data((0..<16).map { _ in UInt8.random(in: 0...255) })
        serial[serial.startIndex] &= 0x7f
        if serial.allSatisfy({ $0 == 0 }) { serial[serial.startIndex] = 1 }
        let now = Date()
        let validity = DER.sequence(DER.utcTime(now.addingTimeInterval(-86_400)) +
                                    DER.utcTime(now.addingTimeInterval(86_400 * 365 * 20)))
        let tbs = DER.sequence(DER.context(0, DER.integer(Data([2]))) + DER.integer(serial) +
            algorithm + commonName + validity + commonName + subjectPublicKeyInfo)
        var signingError: Unmanaged<CFError>?
        guard let signature = SecKeyCreateSignature(privateKey, .ecdsaSignatureMessageX962SHA256,
                                                    tbs as CFData, &signingError) as Data? else {
            throw CrossIdentityError.generation(signingError?.takeRetainedValue().localizedDescription ?? "certificate signing failed")
        }
        return DER.sequence(tbs + algorithm + DER.bitString(signature))
    }
}

private enum DER {
    static func sequence(_ content: Data) -> Data { tagged(0x30, content) }
    static func set(_ content: Data) -> Data { tagged(0x31, content) }
    static func oid(_ bytes: [UInt8]) -> Data { tagged(0x06, Data(bytes)) }
    static func utf8(_ text: String) -> Data { tagged(0x0c, Data(text.utf8)) }
    static func integer(_ bytes: Data) -> Data {
        var value = Data(bytes.drop { $0 == 0 })
        if value.isEmpty { value = Data([0]) }
        if value.first! & 0x80 != 0 { value.insert(0, at: 0) }
        return tagged(0x02, value)
    }
    static func bitString(_ bytes: Data) -> Data { tagged(0x03, Data([0]) + bytes) }
    static func context(_ number: UInt8, _ content: Data) -> Data { tagged(0xa0 | number, content) }
    static func utcTime(_ date: Date) -> Data {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(secondsFromGMT: 0)
        formatter.dateFormat = "yyMMddHHmmss'Z'"
        return tagged(0x17, Data(formatter.string(from: date).utf8))
    }
    static func tagged(_ tag: UInt8, _ content: Data) -> Data {
        Data([tag]) + length(content.count) + content
    }
    static func length(_ count: Int) -> Data {
        if count < 128 { return Data([UInt8(count)]) }
        var value = count
        var bytes: [UInt8] = []
        while value > 0 { bytes.insert(UInt8(value & 0xff), at: 0); value >>= 8 }
        return Data([0x80 | UInt8(bytes.count)] + bytes)
    }
}
