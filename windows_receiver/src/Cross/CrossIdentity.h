#pragma once

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#include <string>

namespace thunderlink {

class CrossIdentity {
public:
    CrossIdentity();
    ~CrossIdentity();
    CrossIdentity(const CrossIdentity&) = delete;
    CrossIdentity& operator=(const CrossIdentity&) = delete;

    bool LoadOrCreate(std::string& error);
    const std::string& Fingerprint() const { return m_fingerprint; }

#ifdef _WIN32
    PCCERT_CONTEXT Certificate() const { return m_certificate; }
#endif

    static bool IsFingerprint(const std::string& value);
    static std::string NormalizeFingerprint(const std::string& value);
    static std::string VerificationCode(const std::string& clientFingerprint,
                                        const std::string& serverFingerprint);

    static bool HasPin(const std::string& peerFingerprint);
    static std::string KnownFingerprintForName(const std::string& peerName);
    static bool StorePin(const std::string& peerFingerprint, const std::string& peerName,
                         const std::string& routeKey, std::string& error);
    static std::string ExpectedPeerForRoute(const std::string& routeKey);
    static bool Forget(const std::string& peerFingerprint, std::string& error);

private:
#ifdef _WIN32
    PCCERT_CONTEXT m_certificate = nullptr;
#endif
    std::string m_fingerprint;
};

} // namespace thunderlink
