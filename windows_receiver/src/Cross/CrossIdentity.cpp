#include "CrossIdentity.h"

#ifdef _WIN32
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <bcrypt.h>
#include <ncrypt.h>
#include <wincred.h>
#include <wincrypt.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cwchar>
#include <sstream>
#include <vector>

namespace thunderlink {
namespace {

constexpr wchar_t kIdentitySubject[] = L"ThunderLink Cross Windows";
constexpr wchar_t kIdentityKeyName[] = L"ThunderLink.Cross.Windows.Identity";
constexpr wchar_t kPinPrefix[] = L"ThunderLink/Cross/Pin/";
constexpr wchar_t kRoutePrefix[] = L"ThunderLink/Cross/Route/";

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string Hex(const unsigned char* bytes, size_t size, bool upper = false) {
    static constexpr char lowerDigits[] = "0123456789abcdef";
    static constexpr char upperDigits[] = "0123456789ABCDEF";
    const char* digits = upper ? upperDigits : lowerDigits;
    std::string result(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

#ifdef _WIN32
std::vector<unsigned char> Sha256(const void* data, size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    DWORD hashSize = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &resultSize, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    object.resize(objectSize);
    result.resize(hashSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0 ||
        (size && BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
                                static_cast<ULONG>(size), 0) < 0) ||
        BCryptFinishHash(hash, result.data(), hashSize, 0) < 0) {
        result.clear();
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                    static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) != count) return {};
    return result;
}

std::wstring PinTarget(const std::string& fingerprint) {
    return std::wstring(kPinPrefix) + Wide(fingerprint);
}

std::wstring RouteTarget(const std::string& routeKey) {
    const auto normalized = Lower(routeKey);
    const auto hash = Sha256(normalized.data(), normalized.size());
    return std::wstring(kRoutePrefix) + Wide(Hex(hash.data(), hash.size()));
}

bool ReadCredential(const std::wstring& target, std::string& value) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return false;
    value.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                 reinterpret_cast<const char*>(credential->CredentialBlob) + credential->CredentialBlobSize);
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return true;
}

bool WriteCredential(const std::wstring& target, const std::string& value,
                     const std::string& userName, std::string& error) {
    if (value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        error = "Credential value is too large";
        return false;
    }
    std::wstring user = Wide(userName.empty() ? "ThunderLink" : userName);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = user.data();
    if (!CredWriteW(&credential, 0)) {
        error = "Windows Credential Manager could not save the Cross trust (" +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

std::string CertificateFingerprint(PCCERT_CONTEXT certificate) {
    if (!certificate) return {};
    const auto hash = Sha256(certificate->pbCertEncoded, certificate->cbCertEncoded);
    return hash.size() == 32 ? Hex(hash.data(), hash.size()) : std::string{};
}

bool HasUsablePrivateKey(PCCERT_CONTEXT certificate) {
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
    DWORD keySpec = 0;
    BOOL release = FALSE;
    if (!CryptAcquireCertificatePrivateKey(certificate,
            CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
            nullptr, &key, &keySpec, &release)) return false;
    if (release) NCryptFreeObject(key);
    return CertVerifyTimeValidity(nullptr, certificate->pCertInfo) == 0;
}

PCCERT_CONTEXT FindIdentity(HCERTSTORE store) {
    PCCERT_CONTEXT current = nullptr;
    while ((current = CertEnumCertificatesInStore(store, current)) != nullptr) {
        wchar_t subject[256]{};
        const DWORD count = CertGetNameStringW(current, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                               nullptr, subject,
                                               static_cast<DWORD>(sizeof(subject) / sizeof(subject[0])));
        if (count > 1 && _wcsicmp(subject, kIdentitySubject) == 0 && HasUsablePrivateKey(current))
            return CertDuplicateCertificateContext(current);
    }
    return nullptr;
}

PCCERT_CONTEXT CreateIdentity(HCERTSTORE store, std::string& error) {
    NCRYPT_PROV_HANDLE provider = 0;
    NCRYPT_KEY_HANDLE key = 0;
    if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) {
        error = "Windows protected key storage is unavailable";
        return nullptr;
    }
    SECURITY_STATUS status = NCryptOpenKey(provider, &key, kIdentityKeyName, 0, NCRYPT_SILENT_FLAG);
    if (status != ERROR_SUCCESS) {
        status = NCryptCreatePersistedKey(provider, &key, NCRYPT_RSA_ALGORITHM,
                                          kIdentityKeyName, 0, 0);
        if (status == ERROR_SUCCESS) {
            DWORD length = 2048;
            DWORD usage = NCRYPT_ALLOW_SIGNING_FLAG;
            status = NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY,
                                       reinterpret_cast<PBYTE>(&length), sizeof(length), 0);
            if (status == ERROR_SUCCESS)
                status = NCryptSetProperty(key, NCRYPT_KEY_USAGE_PROPERTY,
                                           reinterpret_cast<PBYTE>(&usage), sizeof(usage), 0);
            if (status == ERROR_SUCCESS) status = NCryptFinalizeKey(key, 0);
        }
    }
    if (status != ERROR_SUCCESS) {
        if (key) NCryptFreeObject(key);
        NCryptFreeObject(provider);
        error = "Windows could not create the protected Cross identity key (" +
                std::to_string(status) + ")";
        return nullptr;
    }

    DWORD encodedSize = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, (std::wstring(L"CN=") + kIdentitySubject).c_str(),
                        CERT_X500_NAME_STR, nullptr, nullptr, &encodedSize, nullptr)) {
        error = "Windows could not encode the Cross certificate name";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }
    std::vector<BYTE> encodedName(encodedSize);
    if (!CertStrToNameW(X509_ASN_ENCODING, (std::wstring(L"CN=") + kIdentitySubject).c_str(),
                        CERT_X500_NAME_STR, nullptr, encodedName.data(), &encodedSize, nullptr)) {
        error = "Windows could not encode the Cross certificate name";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }
    CERT_NAME_BLOB subject{encodedSize, encodedName.data()};
    CRYPT_ALGORITHM_IDENTIFIER signature{};
    signature.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    SYSTEMTIME now{};
    GetSystemTime(&now);
    FILETIME startFile{};
    SystemTimeToFileTime(&now, &startFile);
    ULARGE_INTEGER startValue{};
    startValue.LowPart = startFile.dwLowDateTime;
    startValue.HighPart = startFile.dwHighDateTime;
    startValue.QuadPart -= 24ULL * 60ULL * 60ULL * 10000000ULL;
    startFile.dwLowDateTime = startValue.LowPart;
    startFile.dwHighDateTime = startValue.HighPart;
    SYSTEMTIME start{};
    FileTimeToSystemTime(&startFile, &start);
    SYSTEMTIME end = now;
    end.wYear = static_cast<WORD>(end.wYear + 20);

    PCCERT_CONTEXT created = CertCreateSelfSignCertificate(
        static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(key), &subject, 0, nullptr,
        &signature, &start, &end, nullptr);
    if (!created) {
        error = "Windows could not create the Cross certificate (" +
                std::to_string(GetLastError()) + ")";
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }

    CRYPT_KEY_PROV_INFO keyInfo{};
    keyInfo.pwszContainerName = const_cast<wchar_t*>(kIdentityKeyName);
    keyInfo.pwszProvName = const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER);
    keyInfo.dwProvType = 0;
    keyInfo.dwKeySpec = CERT_NCRYPT_KEY_SPEC;
    if (!CertSetCertificateContextProperty(created, CERT_KEY_PROV_INFO_PROP_ID, 0, &keyInfo)) {
        error = "Windows could not bind the Cross certificate to its protected key";
        CertFreeCertificateContext(created);
        NCryptFreeObject(key);
        NCryptFreeObject(provider);
        return nullptr;
    }
    const wchar_t friendly[] = L"ThunderLink Cross identity";
    CRYPT_DATA_BLOB friendlyBlob{static_cast<DWORD>(sizeof(friendly)),
                                 reinterpret_cast<BYTE*>(const_cast<wchar_t*>(friendly))};
    (void)CertSetCertificateContextProperty(created, CERT_FRIENDLY_NAME_PROP_ID, 0, &friendlyBlob);

    PCCERT_CONTEXT stored = nullptr;
    if (!CertAddCertificateContextToStore(store, created, CERT_STORE_ADD_REPLACE_EXISTING, &stored)) {
        error = "Windows could not persist the Cross certificate (" +
                std::to_string(GetLastError()) + ")";
    }
    CertFreeCertificateContext(created);
    NCryptFreeObject(key);
    NCryptFreeObject(provider);
    return stored;
}
#else
std::vector<unsigned char> Sha256(const void*, size_t) { return {}; }
#endif

} // namespace

CrossIdentity::CrossIdentity() = default;

CrossIdentity::~CrossIdentity() {
#ifdef _WIN32
    if (m_certificate) CertFreeCertificateContext(m_certificate);
#endif
}

bool CrossIdentity::LoadOrCreate(std::string& error) {
#ifdef _WIN32
    if (m_certificate) return true;
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                     CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
    if (!store) {
        error = "Windows personal certificate store is unavailable";
        return false;
    }
    m_certificate = FindIdentity(store);
    if (!m_certificate) m_certificate = CreateIdentity(store, error);
    CertCloseStore(store, 0);
    if (!m_certificate) return false;
    m_fingerprint = CertificateFingerprint(m_certificate);
    if (!IsFingerprint(m_fingerprint)) {
        error = "Windows could not fingerprint the Cross certificate";
        CertFreeCertificateContext(m_certificate);
        m_certificate = nullptr;
        return false;
    }
    return true;
#else
    error = "Cross identity requires Windows";
    return false;
#endif
}

bool CrossIdentity::IsFingerprint(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) || (std::tolower(ch) >= 'a' && std::tolower(ch) <= 'f');
    });
}

std::string CrossIdentity::NormalizeFingerprint(const std::string& value) {
    return IsFingerprint(value) ? Lower(value) : std::string{};
}

std::string CrossIdentity::VerificationCode(const std::string& clientFingerprint,
                                            const std::string& serverFingerprint) {
    const auto client = NormalizeFingerprint(clientFingerprint);
    const auto server = NormalizeFingerprint(serverFingerprint);
    if (client.empty() || server.empty()) return {};
    const auto material = client + server;
    const auto hash = Sha256(material.data(), material.size());
    return hash.size() == 32 ? Hex(hash.data(), 6, true) : std::string{};
}

bool CrossIdentity::HasPin(const std::string& peerFingerprint) {
#ifdef _WIN32
    const auto normalized = NormalizeFingerprint(peerFingerprint);
    if (normalized.empty()) return false;
    std::string stored;
    return ReadCredential(PinTarget(normalized), stored) &&
           NormalizeFingerprint(stored) == normalized;
#else
    (void)peerFingerprint;
    return false;
#endif
}

std::string CrossIdentity::KnownFingerprintForName(const std::string& peerName) {
#ifdef _WIN32
    const std::wstring expectedName = Wide(peerName);
    if (expectedName.empty()) return {};
    DWORD count = 0;
    PCREDENTIALW* credentials = nullptr;
    const std::wstring filter = std::wstring(kPinPrefix) + L"*";
    if (!CredEnumerateW(filter.c_str(), 0, &count, &credentials)) return {};
    std::string result;
    for (DWORD index = 0; index < count; ++index) {
        const auto* item = credentials[index];
        if (item->UserName && _wcsicmp(item->UserName, expectedName.c_str()) == 0) {
            std::string value(reinterpret_cast<const char*>(item->CredentialBlob),
                              reinterpret_cast<const char*>(item->CredentialBlob) +
                                  item->CredentialBlobSize);
            result = NormalizeFingerprint(value);
            if (!result.empty()) break;
        }
    }
    CredFree(credentials);
    return result;
#else
    (void)peerName;
    return {};
#endif
}

bool CrossIdentity::StorePin(const std::string& peerFingerprint, const std::string& peerName,
                             const std::string& routeKey, std::string& error) {
#ifdef _WIN32
    const auto normalized = NormalizeFingerprint(peerFingerprint);
    if (normalized.empty()) {
        error = "The peer certificate fingerprint is invalid";
        return false;
    }
    const bool alreadyPinned = HasPin(normalized);
    if (!WriteCredential(PinTarget(normalized), normalized, peerName, error)) return false;
    if (!routeKey.empty() && !WriteCredential(RouteTarget(routeKey), normalized, peerName, error)) {
        if (!alreadyPinned) (void)CredDeleteW(PinTarget(normalized).c_str(), CRED_TYPE_GENERIC, 0);
        return false;
    }
    return true;
#else
    (void)peerFingerprint; (void)peerName; (void)routeKey;
    error = "Cross trust storage requires Windows";
    return false;
#endif
}

std::string CrossIdentity::ExpectedPeerForRoute(const std::string& routeKey) {
#ifdef _WIN32
    if (routeKey.empty()) return {};
    std::string result;
    if (!ReadCredential(RouteTarget(routeKey), result)) return {};
    return NormalizeFingerprint(result);
#else
    (void)routeKey;
    return {};
#endif
}

bool CrossIdentity::Forget(const std::string& peerFingerprint, std::string& error) {
#ifdef _WIN32
    const auto normalized = NormalizeFingerprint(peerFingerprint);
    if (normalized.empty()) {
        error = "The peer certificate fingerprint is invalid";
        return false;
    }
    const std::wstring pin = PinTarget(normalized);
    if (!CredDeleteW(pin.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
        error = "Windows Credential Manager could not forget the Cross peer (" +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD count = 0;
    PCREDENTIALW* credentials = nullptr;
    const std::wstring filter = std::wstring(kRoutePrefix) + L"*";
    if (CredEnumerateW(filter.c_str(), 0, &count, &credentials)) {
        for (DWORD index = 0; index < count; ++index) {
            const auto* item = credentials[index];
            std::string value(reinterpret_cast<const char*>(item->CredentialBlob),
                              reinterpret_cast<const char*>(item->CredentialBlob) + item->CredentialBlobSize);
            if (NormalizeFingerprint(value) == normalized)
                (void)CredDeleteW(item->TargetName, CRED_TYPE_GENERIC, 0);
        }
        CredFree(credentials);
    }
    return true;
#else
    (void)peerFingerprint;
    error = "Cross trust storage requires Windows";
    return false;
#endif
}

} // namespace thunderlink
