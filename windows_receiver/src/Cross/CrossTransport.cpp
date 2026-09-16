#include "CrossTransport.h"
#include "CrossDiscovery.h"
#include "CrossIdentity.h"
#include "Utils/Logger.h"
#include "cross_framing.h"
#include "cross_heartbeat.h"

#ifdef _WIN32
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <security.h>
#include <schannel.h>
#include <sspi.h>
#include <netioapi.h>
#include <ifdef.h>
#include <iphlpapi.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace thunderlink {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxQueue = 256;
constexpr size_t kMaxJson = 65536;
constexpr auto kWriteTimeout = std::chrono::seconds(5);
constexpr auto kHandshakeTimeout = std::chrono::seconds(5);
constexpr auto kPairingTimeout = std::chrono::seconds(120);
constexpr auto kReconnectDelay = std::chrono::seconds(1);
constexpr uint64_t kHeartbeatIntervalMs = 250;
constexpr uint64_t kHeartbeatTimeoutMs = 1500;
constexpr uint64_t kInternalSequenceBase = uint64_t{1} << 62;

uint64_t MonotonicMilliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

int RouteRank(CrossRouteClass routeClass) {
    switch (routeClass) {
        case CrossRouteClass::Thunderbolt: return 0;
        case CrossRouteClass::Ethernet: return 1;
        case CrossRouteClass::WiFi: return 2;
        case CrossRouteClass::Other: return 3;
    }
    return 3;
}

std::string EndpointKey(const CrossEndpoint& endpoint) {
    const std::string host = !endpoint.host.empty() ? Lower(endpoint.host) : Lower(endpoint.address);
    return host + ":" + std::to_string(endpoint.port);
}

std::string RouteAttemptKey(const CrossEndpoint& endpoint) {
    const std::string address = !endpoint.address.empty() ? Lower(endpoint.address) : Lower(endpoint.host);
    return address + ":" + std::to_string(endpoint.port) + "|" + endpoint.interfaceId;
}

struct Candidate {
    CrossEndpoint endpoint;
    std::string advertisedPeerId;
    std::string peerName;
};

#ifdef _WIN32
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) != count) return {};
    return result;
}

std::string Utf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int sourceCount = static_cast<int>(std::wcslen(value));
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, sourceCount,
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, sourceCount,
                            result.data(), count, nullptr, nullptr) != count) return {};
    return result;
}

std::string WindowsError(const char* action, unsigned long code) {
    return std::string(action) + " (Windows error " + std::to_string(code) + ")";
}

bool ParseInterfaceIndex(const std::string& text, ULONG& index) {
    if (text.empty()) {
        index = 0;
        return true;
    }
    try {
        size_t used = 0;
        const unsigned long value = std::stoul(text, &used, 10);
        if (!value || value > UINT32_MAX || used != text.size()) return false;
        index = static_cast<ULONG>(value);
        return true;
    } catch (...) {
        return false;
    }
}

void InterfaceDetails(ULONG index, std::string& id, std::string& name, CrossRouteClass& routeClass) {
    id = index ? std::to_string(index) : std::string{};
    routeClass = CrossRouteClass::Other;
    if (!index) return;
    MIB_IF_ROW2 row{};
    row.InterfaceIndex = index;
    if (GetIfEntry2(&row) != NO_ERROR) return;
    name = Utf8(row.Alias[0] ? row.Alias : row.Description);
    std::wstring description = row.Description;
    std::transform(description.begin(), description.end(), description.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    if (description.find(L"thunderbolt") != std::wstring::npos) {
        routeClass = CrossRouteClass::Thunderbolt;
    } else if (row.Type == IF_TYPE_IEEE80211) {
        routeClass = CrossRouteClass::WiFi;
    } else if (row.Type == IF_TYPE_ETHERNET_CSMACD || row.Type == IF_TYPE_ISO88025_TOKENRING ||
               row.Type == IF_TYPE_FASTETHER || row.Type == IF_TYPE_GIGABITETHERNET) {
        routeClass = CrossRouteClass::Ethernet;
    }
}

std::vector<CrossEndpoint> AvailableInterfaces() {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return {};
    std::vector<CrossEndpoint> result;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const auto& row = table->Table[index];
        if (row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
            row.Type == IF_TYPE_TUNNEL) continue;
        CrossEndpoint endpoint;
        InterfaceDetails(row.InterfaceIndex, endpoint.interfaceId, endpoint.interfaceName,
                         endpoint.routeClass);
        if (!endpoint.interfaceId.empty()) result.push_back(std::move(endpoint));
    }
    FreeMibTable(table);
    std::stable_sort(result.begin(), result.end(), [](const CrossEndpoint& left,
                                                       const CrossEndpoint& right) {
        const int leftRank = RouteRank(left.routeClass);
        const int rightRank = RouteRank(right.routeClass);
        return leftRank == rightRank ? left.interfaceName < right.interfaceName : leftRank < rightRank;
    });
    return result;
}

bool WouldBlock() {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
}

bool SocketReady(SOCKET socket, bool writing, int waitMilliseconds) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval timeout{waitMilliseconds / 1000, (waitMilliseconds % 1000) * 1000};
    return select(0, writing ? nullptr : &set, writing ? &set : nullptr, nullptr, &timeout) > 0;
}

bool ConnectSocket(SOCKET socket, const sockaddr* address, int addressLength,
                   const std::atomic<bool>& running, std::string& error) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
        error = WindowsError("Could not configure the Cross socket", WSAGetLastError());
        return false;
    }
    if (connect(socket, address, addressLength) == 0) return true;
    if (!WouldBlock()) return false;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (running.load() && Clock::now() < deadline) {
        if (!SocketReady(socket, true, 25)) continue;
        int socketError = 0;
        int length = sizeof(socketError);
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socketError), &length) == 0 && socketError == 0)
            return true;
        WSASetLastError(socketError);
        return false;
    }
    return false;
}

bool ApplyInterface(SOCKET socket, int family, ULONG index) {
    if (!index) return true;
    if (family == AF_INET) {
        ULONG networkIndex = htonl(index);
        return setsockopt(socket, IPPROTO_IP, IP_UNICAST_IF,
                          reinterpret_cast<const char*>(&networkIndex), sizeof(networkIndex)) == 0;
    }
    if (family == AF_INET6) {
        return setsockopt(socket, IPPROTO_IPV6, IPV6_UNICAST_IF,
                          reinterpret_cast<const char*>(&index), sizeof(index)) == 0;
    }
    return false;
}

ULONG BestInterface(const sockaddr* address) {
    DWORD index = 0;
    sockaddr_storage copy{};
    const size_t size = address->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    std::memcpy(&copy, address, size);
    return GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&copy), &index) == NO_ERROR ? index : 0;
}

class SchannelStream {
public:
    SchannelStream(SOCKET socket, const std::atomic<bool>& running)
        : m_socket(socket), m_running(running) {
        SecInvalidateHandle(&m_credentials);
        SecInvalidateHandle(&m_context);
    }

    ~SchannelStream() { Close(); }

    bool Handshake(PCCERT_CONTEXT certificate, const std::string& target,
                   std::string& remoteFingerprint, std::string& error) {
        PCCERT_CONTEXT clientCertificate = certificate;
        SCHANNEL_CRED schannel{};
        schannel.dwVersion = SCHANNEL_CRED_VERSION;
        schannel.cCreds = 1;
        schannel.paCred = &clientCertificate;
        schannel.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
        schannel.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
                           SCH_USE_STRONG_CRYPTO;
        TimeStamp expiry{};
        SECURITY_STATUS status = AcquireCredentialsHandleW(
            nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
            nullptr, &schannel, nullptr, nullptr, &m_credentials, &expiry);
        if (status != SEC_E_OK) {
            error = "Schannel could not load the protected Cross identity (" +
                    std::to_string(static_cast<unsigned long>(status)) + ")";
            return false;
        }

        constexpr DWORD requestFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
            ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY |
            ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_USE_SUPPLIED_CREDS;
        DWORD attributes = 0;
        const auto wideTarget = Wide(target.empty() ? "ThunderLink" : target);
        SecBuffer output{};
        output.BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outputDescription{SECBUFFER_VERSION, 1, &output};
        status = InitializeSecurityContextW(&m_credentials, nullptr,
            const_cast<wchar_t*>(wideTarget.c_str()), requestFlags, 0, SECURITY_NATIVE_DREP,
            nullptr, 0, &m_context, &outputDescription, &attributes, &expiry);
        if (!CompleteStatus(status, outputDescription, error)) {
            FreeToken(output);
            return false;
        }
        if (!SendToken(output, Clock::now() + kHandshakeTimeout, error)) return false;

        const auto deadline = Clock::now() + kHandshakeTimeout;
        while (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE) {
            if (status == SEC_E_INCOMPLETE_MESSAGE || m_encrypted.empty()) {
                if (Clock::now() >= deadline || !ReadNetwork(deadline, error)) {
                    if (error.empty()) error = "Cross TLS handshake timed out";
                    return false;
                }
            }
            SecBuffer inputBuffers[2]{};
            inputBuffers[0].BufferType = SECBUFFER_TOKEN;
            inputBuffers[0].pvBuffer = m_encrypted.data();
            inputBuffers[0].cbBuffer = static_cast<unsigned long>(m_encrypted.size());
            inputBuffers[1].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc inputDescription{SECBUFFER_VERSION, 2, inputBuffers};
            output = {};
            output.BufferType = SECBUFFER_TOKEN;
            outputDescription = {SECBUFFER_VERSION, 1, &output};
            status = InitializeSecurityContextW(&m_credentials, &m_context,
                const_cast<wchar_t*>(wideTarget.c_str()), requestFlags, 0, SECURITY_NATIVE_DREP,
                &inputDescription, 0, &m_context, &outputDescription, &attributes, &expiry);
            if (!CompleteStatus(status, outputDescription, error)) {
                FreeToken(output);
                return false;
            }
            if (!SendToken(output, deadline, error)) return false;
            if (status == SEC_E_INCOMPLETE_MESSAGE) continue;
            PreserveExtra(inputBuffers, 2);
        }
        if (status != SEC_E_OK) {
            error = "Cross TLS handshake failed (" +
                    std::to_string(static_cast<unsigned long>(status)) + ")";
            return false;
        }

        SecPkgContext_ConnectionInfo connection{};
        if (QueryContextAttributesW(&m_context, SECPKG_ATTR_CONNECTION_INFO, &connection) != SEC_E_OK ||
            !AllowedProtocol(connection.dwProtocol)) {
            error = "Cross requires TLS 1.2 or newer";
            return false;
        }
        PCCERT_CONTEXT remoteCertificate = nullptr;
        if (QueryContextAttributesW(&m_context, SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                                    &remoteCertificate) != SEC_E_OK || !remoteCertificate) {
            error = "The Mac did not present its Cross certificate";
            return false;
        }
        if (CertVerifyTimeValidity(nullptr, remoteCertificate->pCertInfo) != 0) {
            CertFreeCertificateContext(remoteCertificate);
            error = "The Mac Cross certificate is expired or not yet valid";
            return false;
        }
        DWORD hashSize = 32;
        std::array<unsigned char, 32> hash{};
        if (!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr,
                                   remoteCertificate->pbCertEncoded, remoteCertificate->cbCertEncoded,
                                   hash.data(), &hashSize) || hashSize != hash.size()) {
            CertFreeCertificateContext(remoteCertificate);
            error = "Windows could not fingerprint the Mac certificate";
            return false;
        }
        static constexpr char digits[] = "0123456789abcdef";
        remoteFingerprint.assign(hash.size() * 2, '0');
        for (size_t index = 0; index < hash.size(); ++index) {
            remoteFingerprint[index * 2] = digits[hash[index] >> 4];
            remoteFingerprint[index * 2 + 1] = digits[hash[index] & 0x0f];
        }
        CertFreeCertificateContext(remoteCertificate);

        if (QueryContextAttributesW(&m_context, SECPKG_ATTR_STREAM_SIZES, &m_sizes) != SEC_E_OK ||
            m_sizes.cbMaximumMessage == 0) {
            error = "Schannel did not provide valid Cross stream sizes";
            return false;
        }
        m_established = true;
        return true;
    }

    bool SendJson(const std::string& text, std::string& error) {
        if (!m_established || text.empty() || text.size() > kMaxJson) {
            error = "Cross refused an invalid outbound JSON frame";
            return false;
        }
        std::vector<unsigned char> framed(4 + text.size());
        const uint32_t length = static_cast<uint32_t>(text.size());
        framed[0] = static_cast<unsigned char>(length >> 24);
        framed[1] = static_cast<unsigned char>(length >> 16);
        framed[2] = static_cast<unsigned char>(length >> 8);
        framed[3] = static_cast<unsigned char>(length);
        std::memcpy(framed.data() + 4, text.data(), text.size());
        const auto deadline = Clock::now() + kWriteTimeout;
        size_t offset = 0;
        while (offset < framed.size()) {
            const size_t plainSize = std::min<size_t>(m_sizes.cbMaximumMessage, framed.size() - offset);
            std::vector<unsigned char> record(m_sizes.cbHeader + plainSize + m_sizes.cbTrailer);
            std::memcpy(record.data() + m_sizes.cbHeader, framed.data() + offset, plainSize);
            SecBuffer buffers[4]{};
            buffers[0] = {m_sizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data()};
            buffers[1] = {static_cast<unsigned long>(plainSize), SECBUFFER_DATA,
                          record.data() + m_sizes.cbHeader};
            buffers[2] = {m_sizes.cbTrailer, SECBUFFER_STREAM_TRAILER,
                          record.data() + m_sizes.cbHeader + plainSize};
            buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
            const SECURITY_STATUS status = EncryptMessage(&m_context, 0, &description, 0);
            if (status != SEC_E_OK) {
                error = "Schannel could not encrypt a Cross message (" +
                        std::to_string(static_cast<unsigned long>(status)) + ")";
                return false;
            }
            const size_t encryptedSize = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
            if (!SendRaw(record.data(), encryptedSize, deadline, error)) return false;
            offset += plainSize;
        }
        return true;
    }

    bool Receive(std::vector<std::string>& messages, std::string& error) {
        if (!m_established) return false;
        if (!m_framer.CheckDeadline(MonotonicMilliseconds(), error)) return false;
        if (m_encrypted.empty()) {
            if (!SocketReady(m_socket, false, 10)) return true;
            if (!ReadNetwork(Clock::now() + std::chrono::milliseconds(50), error)) return false;
        }
        for (;;) {
            if (m_encrypted.empty()) break;
            SecBuffer buffers[4]{};
            buffers[0] = {static_cast<unsigned long>(m_encrypted.size()), SECBUFFER_DATA,
                          m_encrypted.data()};
            buffers[1].BufferType = SECBUFFER_EMPTY;
            buffers[2].BufferType = SECBUFFER_EMPTY;
            buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
            const SECURITY_STATUS status = DecryptMessage(&m_context, &description, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!SocketReady(m_socket, false, 0)) return true;
                if (!ReadNetwork(Clock::now() + std::chrono::milliseconds(50), error)) return false;
                continue;
            }
            if (status == SEC_I_CONTEXT_EXPIRED) {
                error = "The Mac closed the Cross TLS connection";
                return false;
            }
            if (status == SEC_I_RENEGOTIATE) {
                error = "Unexpected Cross TLS renegotiation";
                return false;
            }
            if (status != SEC_E_OK) {
                error = "Cross TLS decryption failed (" +
                        std::to_string(static_cast<unsigned long>(status)) + ")";
                return false;
            }
            for (const auto& buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer &&
                    !m_framer.Append(static_cast<unsigned char*>(buffer.pvBuffer), buffer.cbBuffer,
                                     messages, error, MonotonicMilliseconds())) return false;
            }
            const bool hadExtra = PreserveExtra(buffers, 4);
            if (!hadExtra) break;
        }
        return true;
    }

    void Close() {
        if (m_established && SecIsValidHandle(&m_context)) {
            DWORD shutdown = SCHANNEL_SHUTDOWN;
            SecBuffer control{sizeof(shutdown), SECBUFFER_TOKEN, &shutdown};
            SecBufferDesc controlDescription{SECBUFFER_VERSION, 1, &control};
            if (ApplyControlToken(&m_context, &controlDescription) == SEC_E_OK) {
                SecBuffer output{};
                output.BufferType = SECBUFFER_TOKEN;
                SecBufferDesc outputDescription{SECBUFFER_VERSION, 1, &output};
                DWORD attributes = 0;
                TimeStamp expiry{};
                (void)InitializeSecurityContextW(&m_credentials, &m_context, nullptr, 0, 0,
                    SECURITY_NATIVE_DREP, nullptr, 0, &m_context, &outputDescription, &attributes, &expiry);
                std::string ignored;
                (void)SendToken(output, Clock::now() + std::chrono::milliseconds(100), ignored);
            }
        }
        m_established = false;
        if (SecIsValidHandle(&m_context)) {
            DeleteSecurityContext(&m_context);
            SecInvalidateHandle(&m_context);
        }
        if (SecIsValidHandle(&m_credentials)) {
            FreeCredentialsHandle(&m_credentials);
            SecInvalidateHandle(&m_credentials);
        }
    }

private:
    static void FreeToken(SecBuffer& token) {
        if (token.pvBuffer) FreeContextBuffer(token.pvBuffer);
        token = {};
    }

    bool CompleteStatus(SECURITY_STATUS& status, SecBufferDesc& output, std::string& error) {
        if (status != SEC_I_COMPLETE_NEEDED && status != SEC_I_COMPLETE_AND_CONTINUE) return true;
        const bool continueNeeded = status == SEC_I_COMPLETE_AND_CONTINUE;
        const SECURITY_STATUS completed = CompleteAuthToken(&m_context, &output);
        if (completed != SEC_E_OK) {
            error = "Schannel could not complete the Cross handshake (" +
                    std::to_string(static_cast<unsigned long>(completed)) + ")";
            return false;
        }
        status = continueNeeded ? SEC_I_CONTINUE_NEEDED : SEC_E_OK;
        return true;
    }

    static bool AllowedProtocol(DWORD protocol) {
        if (protocol == SP_PROT_TLS1_2_CLIENT) return true;
#ifdef SP_PROT_TLS1_3_CLIENT
        if (protocol == SP_PROT_TLS1_3_CLIENT) return true;
#endif
        return false;
    }

    bool SendRaw(const unsigned char* bytes, size_t size, Clock::time_point deadline,
                 std::string& error) {
        size_t sent = 0;
        while (sent < size && m_running.load() && Clock::now() < deadline) {
            if (!SocketReady(m_socket, true, 20)) continue;
            const int result = send(m_socket, reinterpret_cast<const char*>(bytes + sent),
                                    static_cast<int>(std::min<size_t>(size - sent, INT_MAX)), 0);
            if (result > 0) sent += static_cast<size_t>(result);
            else if (result == 0 || !WouldBlock()) {
                error = WindowsError("Cross TLS write failed", WSAGetLastError());
                return false;
            }
        }
        if (sent != size) {
            error = "Cross TLS write timed out";
            return false;
        }
        return true;
    }

    bool SendToken(SecBuffer& token, Clock::time_point deadline, std::string& error) {
        const bool result = !token.cbBuffer ||
            SendRaw(static_cast<unsigned char*>(token.pvBuffer), token.cbBuffer, deadline, error);
        FreeToken(token);
        return result;
    }

    bool ReadNetwork(Clock::time_point deadline, std::string& error) {
        std::array<unsigned char, 16384> buffer{};
        while (m_running.load() && Clock::now() < deadline) {
            if (!SocketReady(m_socket, false, 20)) continue;
            const int result = recv(m_socket, reinterpret_cast<char*>(buffer.data()),
                                    static_cast<int>(buffer.size()), 0);
            if (result > 0) {
                if (m_encrypted.size() + static_cast<size_t>(result) > 1024 * 1024) {
                    error = "Cross TLS receive buffer exceeded its limit";
                    return false;
                }
                m_encrypted.insert(m_encrypted.end(), buffer.begin(), buffer.begin() + result);
                return true;
            }
            if (result == 0) {
                error = "The Mac closed the Cross connection";
                return false;
            }
            if (!WouldBlock()) {
                error = WindowsError("Cross TLS read failed", WSAGetLastError());
                return false;
            }
        }
        return false;
    }

    bool PreserveExtra(SecBuffer* buffers, size_t count) {
        size_t extra = 0;
        for (size_t index = 0; index < count; ++index) {
            if (buffers[index].BufferType == SECBUFFER_EXTRA) extra = buffers[index].cbBuffer;
        }
        if (extra && extra <= m_encrypted.size()) {
            std::vector<unsigned char> remaining(m_encrypted.end() - extra, m_encrypted.end());
            m_encrypted.swap(remaining);
            return true;
        }
        m_encrypted.clear();
        return false;
    }

    SOCKET m_socket = INVALID_SOCKET;
    const std::atomic<bool>& m_running;
    CredHandle m_credentials{};
    CtxtHandle m_context{};
    SecPkgContext_StreamSizes m_sizes{};
    bool m_established = false;
    std::vector<unsigned char> m_encrypted;
    cross::JsonFramer m_framer;
};

struct ConnectedSocket {
    SOCKET socket = INVALID_SOCKET;
    CrossEndpoint endpoint;

    ConnectedSocket() = default;
    ConnectedSocket(SOCKET value, CrossEndpoint selected)
        : socket(value), endpoint(std::move(selected)) {}
    ConnectedSocket(const ConnectedSocket&) = delete;
    ConnectedSocket& operator=(const ConnectedSocket&) = delete;
    ConnectedSocket(ConnectedSocket&& other) noexcept
        : socket(std::exchange(other.socket, INVALID_SOCKET)), endpoint(std::move(other.endpoint)) {}
    ConnectedSocket& operator=(ConnectedSocket&& other) noexcept {
        if (this != &other) {
            if (socket != INVALID_SOCKET) closesocket(socket);
            socket = std::exchange(other.socket, INVALID_SOCKET);
            endpoint = std::move(other.endpoint);
        }
        return *this;
    }
    ~ConnectedSocket() { if (socket != INVALID_SOCKET) closesocket(socket); }
};

std::optional<ConnectedSocket> ConnectCandidate(const Candidate& candidate,
                                                const CrossTransportOptions& options,
                                                const std::atomic<bool>& running,
                                                std::string& error) {
    const std::string host = !candidate.endpoint.address.empty()
        ? candidate.endpoint.address : candidate.endpoint.host;
    if (host.empty()) return std::nullopt;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(candidate.endpoint.port);
    const int lookup = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        error = "Could not resolve " + host + " for Cross (" + std::to_string(lookup) + ")";
        return std::nullopt;
    }
    std::optional<ConnectedSocket> result;
    ULONG requestedIndex = 0;
    const std::string requestedId = !options.interfaceId.empty()
        ? options.interfaceId : candidate.endpoint.interfaceId;
    if (!ParseInterfaceIndex(requestedId, requestedIndex)) {
        freeaddrinfo(addresses);
        error = "The selected Cross network interface is invalid";
        return std::nullopt;
    }
    for (addrinfo* address = addresses; address && running.load(); address = address->ai_next) {
        SOCKET socket = WSASocketW(address->ai_family, SOCK_STREAM, IPPROTO_TCP,
                                   nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (socket == INVALID_SOCKET) continue;
        const ULONG interfaceIndex = requestedIndex ? requestedIndex : BestInterface(address->ai_addr);
        if (!ApplyInterface(socket, address->ai_family, interfaceIndex)) {
            closesocket(socket);
            continue;
        }
        BOOL keepAlive = TRUE;
        BOOL noDelay = TRUE;
        (void)setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE,
                         reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));
        (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        if (ConnectSocket(socket, address->ai_addr, static_cast<int>(address->ai_addrlen),
                          running, error)) {
            auto endpoint = candidate.endpoint;
            InterfaceDetails(interfaceIndex, endpoint.interfaceId, endpoint.interfaceName,
                             endpoint.routeClass);
            char numeric[INET6_ADDRSTRLEN]{};
            void* addressBytes = address->ai_family == AF_INET
                ? static_cast<void*>(&reinterpret_cast<sockaddr_in*>(address->ai_addr)->sin_addr)
                : static_cast<void*>(&reinterpret_cast<sockaddr_in6*>(address->ai_addr)->sin6_addr);
            if (InetNtopA(address->ai_family, addressBytes, numeric, sizeof(numeric)))
                endpoint.address = numeric;
            result = ConnectedSocket{socket, std::move(endpoint)};
            break;
        }
        closesocket(socket);
    }
    freeaddrinfo(addresses);
    if (!result && error.empty())
        error = "The Mac is unreachable for Cross; check its address, firewall, and Wi-Fi client isolation";
    return result;
}
#endif

bool IsMouseMove(const json& message) {
    if (!message.is_object() || message.value("type", std::string{}) != "input") return false;
    const auto event = message.find("event");
    return event != message.end() && event->is_object() &&
           event->value("type", std::string{}) == "mouse_move";
}

bool SameMoveStream(const json& first, const json& second) {
    if (!IsMouseMove(first) || !IsMouseMove(second)) return false;
    return first.value("session_id", std::string{}) == second.value("session_id", std::string{}) &&
           first.value("topology_revision", uint64_t{}) ==
               second.value("topology_revision", uint64_t{}) &&
           first.value("generation", uint64_t{}) == second.value("generation", uint64_t{}) &&
           first.value("display_id", std::string{}) == second.value("display_id", std::string{});
}

bool PairingType(const std::string& type) {
    return type == "hello" || type == "pair_approve" || type == "paired" || type == "error";
}

std::optional<uint64_t> PingSequence(const json& message) {
    const auto sequence = message.find("sequence");
    if (sequence == message.end()) return std::nullopt;
    if (sequence->is_number_unsigned()) {
        const uint64_t value = sequence->get<uint64_t>();
        return value <= static_cast<uint64_t>(INT64_MAX) ? std::optional<uint64_t>(value)
                                                          : std::nullopt;
    }
    if (sequence->is_number_integer()) {
        const int64_t value = sequence->get<int64_t>();
        return value >= 0 ? std::optional<uint64_t>(static_cast<uint64_t>(value))
                          : std::nullopt;
    }
    return std::nullopt;
}

} // namespace

class CrossTransport::Impl {
public:
    ~Impl() { Stop(); }

    bool Start(const CrossTransportOptions& options) {
        Stop();
        if (!options.port) return SetStartError("Cross port must be between 1 and 65535");
        if (!options.peerId.empty() && CrossIdentity::NormalizeFingerprint(options.peerId).empty())
            return SetStartError("The selected Cross peer identity is invalid");
        std::string identityError;
        if (!m_identity.LoadOrCreate(identityError)) return SetStartError(identityError);

        std::string discoveryError;
        const bool discoveryStarted = m_discovery.Start(options.interfaceId, discoveryError);
        if (!discoveryStarted && options.manualHost.empty()) return SetStartError(discoveryError);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_options = options;
            m_options.peerId = CrossIdentity::NormalizeFingerprint(options.peerId);
            m_snapshot = {};
            m_snapshot.running = true;
            if (!discoveryError.empty())
                m_snapshot.error = discoveryError + "; using the manual address";
            m_running = true;
        }
        try {
            m_worker = std::thread(&Impl::Run, this);
        } catch (const std::exception& exception) {
            m_running = false;
            m_discovery.Stop();
            return SetStartError(std::string("Could not start the Cross network worker: ") + exception.what());
        }
        return true;
    }

    void Stop() {
        m_running = false;
        m_condition.notify_all();
        if (m_worker.joinable()) m_worker.join();
        m_discovery.Stop();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outbound.clear();
        m_inbound.clear();
        m_localApproved = false;
        m_remoteApproved = false;
        m_snapshot.running = false;
        m_snapshot.connected = false;
        m_snapshot.paired = false;
        m_snapshot.pairing = false;
        m_snapshot.verificationCode.clear();
        m_snapshot.interfaceName.clear();
        m_snapshot.latencyMs = -1.0;
    }

    bool Send(const json& message) {
        if (!message.is_object()) return false;
        const auto type = message.find("type");
        if (type == message.end() || !type->is_string() || type->get_ref<const std::string&>().empty())
            return false;
        std::string serialized;
        try { serialized = message.dump(); }
        catch (...) { return false; }
        if (serialized.empty() || serialized.size() > kMaxJson) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) return false;
        try {
            if (!m_outbound.empty() && SameMoveStream(m_outbound.back(), message)) {
                m_outbound.back() = message;
                m_condition.notify_one();
                return true;
            }
        } catch (...) { return false; }
        if (m_outbound.size() >= kMaxQueue) {
            m_snapshot.error = "Cross outbound queue overflowed; the connection will be reset";
            m_queueOverflow = true;
            m_condition.notify_one();
            return false;
        }
        m_outbound.push_back(message);
        m_condition.notify_one();
        return true;
    }

    bool ApprovePairing() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load() || !m_snapshot.connected || !m_snapshot.pairing ||
            m_snapshot.verificationCode.empty() || m_localApproved) return false;
        json approval{{"type", "pair_approve"}};
        if (m_outbound.size() >= kMaxQueue) return false;
        m_localApproved = true;
        m_outbound.push_front(std::move(approval));
        m_condition.notify_one();
        return true;
    }

    bool ForgetDevice(const std::string& fingerprint) {
        std::string error;
        const auto normalized = CrossIdentity::NormalizeFingerprint(fingerprint);
        if (normalized.empty() || !CrossIdentity::Forget(normalized, error)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.error = error.empty() ? "The Cross peer identity is invalid" : error;
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_options.peerId == normalized) m_options.peerId.clear();
        if (m_snapshot.peerId == normalized) {
            m_forceDisconnect = true;
            m_forceDisconnectReason = "Cross device trust was removed";
        }
        if (m_stickyPeerId == normalized) {
            m_stickyPeerId.clear();
            m_forceDisconnect = true;
            m_forceDisconnectReason = "Cross device trust was removed";
        }
        return true;
    }

    void AbortConnection() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load() || !m_snapshot.connected) return;
        m_forceDisconnect = true;
        m_forceDisconnectReason = "Cross connection restart requested";
        m_outbound.clear();
        m_inbound.clear();
        m_snapshot.connected = false;
        m_snapshot.paired = false;
        m_snapshot.pairing = false;
        m_snapshot.verificationCode.clear();
        m_snapshot.interfaceName.clear();
        m_snapshot.latencyMs = -1.0;
        m_condition.notify_one();
    }

    CrossTransportSnapshot Snapshot() const {
        CrossTransportSnapshot result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            result = m_snapshot;
        }
        result.peers = m_discovery.Peers();
        return result;
    }

    std::vector<json> DrainMessages() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<json> result;
        result.reserve(m_inbound.size());
        while (!m_inbound.empty()) {
            result.push_back(std::move(m_inbound.front()));
            m_inbound.pop_front();
        }
        return result;
    }

private:
    bool SetStartError(const std::string& error) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = {};
        m_snapshot.error = error;
        return false;
    }

    std::vector<Candidate> Candidates() {
        CrossTransportOptions options;
        std::string sticky;
        std::vector<CrossEndpoint> interfaces;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            options = m_options;
            sticky = !m_options.peerId.empty() ? m_options.peerId : m_stickyPeerId;
            interfaces = m_snapshot.availableInterfaces;
        }
        std::vector<Candidate> result;
        const auto peers = m_discovery.Peers();
        for (const auto& peer : peers) {
            if (!sticky.empty() && peer.id != sticky) continue;
            for (const auto& endpoint : peer.endpoints) {
                if (!options.interfaceId.empty() && !endpoint.interfaceId.empty() &&
                    options.interfaceId != endpoint.interfaceId) continue;
                result.push_back(Candidate{endpoint, peer.id, peer.name});
            }
        }
        if (!options.manualHost.empty()) {
            CrossEndpoint endpoint;
            endpoint.host = options.manualHost;
            endpoint.port = options.port;
            endpoint.interfaceId = options.interfaceId;
            if (options.interfaceId.empty() && !interfaces.empty()) {
                for (const auto& local : interfaces) {
                    auto routed = endpoint;
                    routed.interfaceId = local.interfaceId;
                    routed.interfaceName = local.interfaceName;
                    routed.routeClass = local.routeClass;
                    result.push_back(Candidate{std::move(routed), sticky, {}});
                }
            } else {
                result.push_back(Candidate{endpoint, sticky, {}});
            }
        }
        std::stable_sort(result.begin(), result.end(), [&](const Candidate& left, const Candidate& right) {
            const int leftRank = RouteRank(left.endpoint.routeClass);
            const int rightRank = RouteRank(right.endpoint.routeClass);
            if (leftRank != rightRank) return leftRank < rightRank;
            const double leftLatency = RouteLatency(left.endpoint);
            const double rightLatency = RouteLatency(right.endpoint);
            if (leftLatency != rightLatency) return leftLatency < rightLatency;
            return RouteAttemptKey(left.endpoint) < RouteAttemptKey(right.endpoint);
        });
        std::set<std::string> seen;
        result.erase(std::remove_if(result.begin(), result.end(), [&](const Candidate& candidate) {
            const std::string key = RouteAttemptKey(candidate.endpoint);
            return !seen.emplace(key).second;
        }), result.end());
        return result;
    }

    double RouteLatency(const CrossEndpoint& endpoint) const {
        const auto found = m_routeLatency.find(RouteAttemptKey(endpoint));
        return found == m_routeLatency.end() ? 1e12 : found->second;
    }

    void Run() {
        try {
#ifdef _WIN32
        size_t nextRoute = 0;
        auto nextInterfaceRefresh = Clock::time_point::min();
        while (m_running.load()) {
            if (Clock::now() >= nextInterfaceRefresh) {
                const auto interfaces = AvailableInterfaces();
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_snapshot.availableInterfaces = interfaces;
                }
                nextInterfaceRefresh = Clock::now() + std::chrono::seconds(2);
            }
            const auto candidates = Candidates();
            if (candidates.empty()) {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_condition.wait_for(lock, std::chrono::milliseconds(200), [&] {
                    return !m_running.load();
                });
                continue;
            }
            if (nextRoute >= candidates.size()) nextRoute = 0;
            const Candidate candidate = candidates[nextRoute++];
            std::string error;
            CrossTransportOptions options;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                options = m_options;
            }
            auto connected = ConnectCandidate(candidate, options, m_running, error);
            if (!connected) {
                SetRuntimeError(error);
                WaitReconnect();
                continue;
            }
            const auto handshakeStarted = Clock::now();
            SchannelStream tls(connected->socket, m_running);
            std::string remoteFingerprint;
            const std::string tlsTarget = !candidate.endpoint.host.empty()
                ? candidate.endpoint.host : connected->endpoint.address;
            if (!tls.Handshake(m_identity.Certificate(), tlsTarget, remoteFingerprint, error)) {
                SetRuntimeError(error);
                WaitReconnect();
                continue;
            }

            const std::string routeKey = EndpointKey(candidate.endpoint);
            std::string expected = options.peerId;
            if (expected.empty()) expected = CrossIdentity::ExpectedPeerForRoute(routeKey);
            if (expected.empty()) expected = CrossIdentity::NormalizeFingerprint(candidate.advertisedPeerId);
            if (!expected.empty() && expected != remoteFingerprint) {
                tls.Close();
                SetRuntimeError("The Mac identity changed. Forget the saved device before pairing it again.");
                WaitReconnect();
                continue;
            }

            const double connectLatency = std::chrono::duration<double, std::milli>(
                Clock::now() - handshakeStarted).count();
            m_routeLatency[RouteAttemptKey(connected->endpoint)] = connectLatency;
            BeginConnection(remoteFingerprint, connected->endpoint, routeKey, connectLatency);
            const json hello{{"type", "hello"}, {"version", 4},
                             {"device_id", m_identity.Fingerprint()},
                             {"name", DeviceName()}, {"role", "windows"}};
            if (!tls.SendJson(hello.dump(), error) ||
                !ConnectionLoop(tls, remoteFingerprint, candidate, routeKey, error)) {
                if (m_running.load() && !error.empty()) SetRuntimeError(error);
            }
            tls.Close();
            EndConnection();
            if (m_running.load()) WaitReconnect();
        }
#else
        SetRuntimeError("Cross transport requires native Windows");
#endif
        } catch (const std::exception& exception) {
            SetRuntimeError(std::string("Cross transport stopped after an internal error: ") + exception.what());
            m_running = false;
            EndConnection();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.running = false;
        } catch (...) {
            SetRuntimeError("Cross transport stopped after an unknown internal error");
            m_running = false;
            EndConnection();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.running = false;
        }
    }

#ifdef _WIN32
    bool ConnectionLoop(SchannelStream& tls, const std::string& remoteFingerprint,
                        const Candidate& candidate, const std::string& routeKey,
                        std::string& error) {
        const auto helloDeadline = Clock::now() + kHandshakeTimeout;
        auto pairDeadline = Clock::time_point::max();
        bool helloReceived = false;
        cross::HeartbeatTracker heartbeat;
        uint64_t internalSequence = kInternalSequenceBase;
        uint64_t lastPingSent = 0;
        std::string configuredSession;
        while (m_running.load()) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_forceDisconnect) {
                    m_forceDisconnect = false;
                    error = m_forceDisconnectReason.empty()
                        ? "Cross connection restart requested" : std::move(m_forceDisconnectReason);
                    m_forceDisconnectReason.clear();
                    return false;
                }
                if (m_queueOverflow) {
                    m_queueOverflow = false;
                    error = "Cross outbound queue overflowed";
                    return false;
                }
            }
            if (!helloReceived && Clock::now() >= helloDeadline) {
                error = "The Mac did not send a Cross v4 HELLO";
                return false;
            }
            if (pairDeadline != Clock::time_point::max() && !IsPaired() &&
                Clock::now() >= pairDeadline) {
                error = "Cross pairing timed out";
                return false;
            }

            json outbound;
            bool haveOutbound = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_outbound.empty()) {
                    const std::string type = m_outbound.front().value("type", std::string{});
                    if (m_snapshot.paired || PairingType(type)) {
                        outbound = std::move(m_outbound.front());
                        m_outbound.pop_front();
                        haveOutbound = true;
                    }
                }
            }
            if (haveOutbound) {
                const std::string type = outbound.value("type", std::string{});
                const auto pingSequence = type == "ping" ? PingSequence(outbound) : std::nullopt;
                if (type == "ping" && !pingSequence) {
                    error = "Cross refused an invalid outbound ping sequence";
                    return false;
                }
                if (!tls.SendJson(outbound.dump(), error)) return false;
                if (type == "ping") {
                    const uint64_t sentAt = MonotonicMilliseconds();
                    if (!heartbeat.Sent(*pingSequence, sentAt)) {
                        error = "Cross heartbeat tracking overflowed or reused a ping sequence";
                        return false;
                    }
                    lastPingSent = sentAt;
                } else if (type == "configure") {
                    const auto session = outbound.find("session_id");
                    configuredSession = session != outbound.end() && session->is_string()
                        ? session->get<std::string>() : std::string{};
                } else if (type == "stop") {
                    configuredSession.clear();
                }
            }

            const uint64_t heartbeatNow = MonotonicMilliseconds();
            if (IsPaired() && (lastPingSent == 0 || heartbeatNow - lastPingSent >= kHeartbeatIntervalMs)) {
                if (internalSequence == static_cast<uint64_t>(INT64_MAX))
                    internalSequence = kInternalSequenceBase;
                const uint64_t sequence = ++internalSequence;
                json ping{{"type", "ping"}, {"sequence", sequence},
                          {"renew_ownership", false}};
                if (!configuredSession.empty()) ping["session_id"] = configuredSession;
                if (!tls.SendJson(ping.dump(), error)) return false;
                if (!heartbeat.Sent(sequence, heartbeatNow)) {
                    error = "Cross heartbeat tracking overflowed";
                    return false;
                }
                lastPingSent = heartbeatNow;
            }

            std::vector<std::string> messages;
            if (!tls.Receive(messages, error)) return false;
            for (const auto& text : messages) {
                json message;
                try { message = json::parse(text); }
                catch (...) {
                    error = "The Mac sent malformed Cross JSON";
                    return false;
                }
                if (!message.is_object()) {
                    error = "The Mac sent a non-object Cross message";
                    return false;
                }
                const auto typeValue = message.find("type");
                if (typeValue == message.end() || !typeValue->is_string()) {
                    error = "The Mac sent a Cross message without a type";
                    return false;
                }
                try {
                    const std::string type = typeValue->get<std::string>();
                    if (!helloReceived) {
                        if (type != "hello" || !ValidateHello(message, remoteFingerprint,
                                                               candidate, routeKey, pairDeadline, error))
                            return false;
                        helloReceived = true;
                        continue;
                    }
                    if (!IsPaired()) {
                        if (type == "pair_approve") {
                            if (!ValidateVerificationCode(message)) {
                                error = "The Mac approved a different Cross verification code";
                                return false;
                            }
                            MarkRemoteApproved();
                        } else if (type == "paired") {
                            if (!ValidateVerificationCode(message)) {
                                error = "The Mac confirmed a different Cross verification code";
                                return false;
                            }
                            if (!PairApprovalsComplete()) {
                                error = "The Mac completed Cross pairing before both approvals";
                                return false;
                            }
                            if (!FinishPairing(routeKey, error)) return false;
                        } else if (type == "error") {
                            error = message.value("reason", std::string("The Mac rejected Cross pairing"));
                            return false;
                        } else {
                            error = "The Mac sent application data before Cross pairing completed";
                            return false;
                        }
                        continue;
                    }
                    if (type == "hello" || type == "pair_approve" || type == "paired") {
                        error = "The Mac sent an unexpected Cross pairing message";
                        return false;
                    }
                    if (type == "pong") UpdateHeartbeat(message, heartbeat);
                    if (type == "topology") configuredSession.clear();
                    if (!QueueInbound(std::move(message), error)) return false;
                } catch (const std::exception&) {
                    error = "The Mac sent invalid fields in a Cross message";
                    return false;
                }
            }
            if (IsPaired() && heartbeat.Expired(MonotonicMilliseconds(), kHeartbeatTimeoutMs)) {
                error = "The Mac stopped responding to Cross heartbeats; trying another network route";
                return false;
            }
        }
        return true;
    }

    bool ValidateHello(const json& message, const std::string& remoteFingerprint,
                       const Candidate& candidate, const std::string& routeKey,
                       Clock::time_point& pairDeadline, std::string& error) {
        if (message.value("version", 0) != 4 || message.value("role", std::string{}) != "mac") {
            error = "Cross protocol mismatch: both computers must use protocol v4";
            return false;
        }
        const std::string claimed = CrossIdentity::NormalizeFingerprint(
            message.value("device_id", std::string{}));
        if (claimed.empty() || claimed != remoteFingerprint) {
            error = "The Mac HELLO identity does not match its TLS certificate";
            return false;
        }
        std::string name = message.value("name", std::string{});
        if (name.empty() || name.size() > 256) {
            error = "The Mac sent an invalid Cross device name";
            return false;
        }
        const std::string knownForName = CrossIdentity::KnownFingerprintForName(name);
        if (!knownForName.empty() && knownForName != remoteFingerprint) {
            error = "This known Mac presented a changed identity. Forget it before pairing again.";
            return false;
        }
        if (!candidate.advertisedPeerId.empty() &&
            CrossIdentity::NormalizeFingerprint(candidate.advertisedPeerId) != remoteFingerprint) {
            error = "The discovered Mac identity does not match its TLS certificate";
            return false;
        }
        const bool pinned = CrossIdentity::HasPin(remoteFingerprint);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.peerId = remoteFingerprint;
            m_snapshot.peerName = std::move(name);
            m_snapshot.error.clear();
            m_snapshot.verificationCode = CrossIdentity::VerificationCode(
                m_identity.Fingerprint(), remoteFingerprint);
            m_snapshot.pairing = !pinned;
            m_localApproved = pinned;
            m_remoteApproved = false;
            if (pinned) {
                m_outbound.push_front(json{{"type", "pair_approve"}});
            }
            pairDeadline = Clock::now() + kPairingTimeout;
        }
        (void)routeKey;
        return true;
    }

    bool ValidateVerificationCode(const json& message) const {
        const auto code = message.find("verification_code");
        if (code == message.end()) return true;
        if (!code->is_string()) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        return code->get_ref<const std::string&>() == m_snapshot.verificationCode;
    }

    void MarkRemoteApproved() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remoteApproved = true;
    }

    bool PairApprovalsComplete() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_localApproved && m_remoteApproved;
    }

    bool FinishPairing(const std::string& routeKey, std::string& error) {
        std::string peerId;
        std::string peerName;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_localApproved) {
                error = "The Mac completed Cross pairing before local approval";
                return false;
            }
            peerId = m_snapshot.peerId;
            peerName = m_snapshot.peerName;
        }
        if (!CrossIdentity::StorePin(peerId, peerName, routeKey, error)) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remoteApproved = true;
        m_snapshot.paired = true;
        m_snapshot.pairing = false;
        m_snapshot.verificationCode.clear();
        m_snapshot.error.clear();
        m_stickyPeerId = peerId;
        return true;
    }

    bool QueueInbound(json message, std::string& error) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inbound.size() >= kMaxQueue) {
            error = "Cross inbound queue overflowed";
            return false;
        }
        m_inbound.push_back(std::move(message));
        return true;
    }

    void UpdateHeartbeat(const json& message, cross::HeartbeatTracker& heartbeat) {
        const auto sequence = PingSequence(message);
        if (!sequence) return;
        const auto latency = heartbeat.Acknowledge(*sequence, MonotonicMilliseconds());
        if (!latency) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.latencyMs = static_cast<double>(*latency);
    }
#endif

    bool IsPaired() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_snapshot.paired;
    }

    std::string DeviceName() const {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_options.deviceName.empty()) return m_options.deviceName.substr(0, 256);
        }
#ifdef _WIN32
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD size = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
        if (GetComputerNameW(name, &size) && size) return Utf8(name);
#endif
        return "Windows PC";
    }

    void BeginConnection(const std::string& remoteFingerprint, const CrossEndpoint& endpoint,
                         const std::string&, double latency) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outbound.clear();
        m_inbound.clear();
        m_snapshot.connected = true;
        m_snapshot.paired = false;
        m_snapshot.pairing = false;
        m_snapshot.verificationCode.clear();
        m_snapshot.peerId = remoteFingerprint;
        m_snapshot.interfaceName = endpoint.interfaceName;
        m_snapshot.latencyMs = latency;
        m_snapshot.error.clear();
        m_localApproved = false;
        m_remoteApproved = false;
        m_queueOverflow = false;
        m_forceDisconnect = false;
        m_forceDisconnectReason.clear();
    }

    void EndConnection() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outbound.clear();
        m_inbound.clear();
        m_snapshot.connected = false;
        m_snapshot.paired = false;
        m_snapshot.pairing = false;
        m_snapshot.verificationCode.clear();
        m_snapshot.interfaceName.clear();
        m_snapshot.latencyMs = -1.0;
        m_localApproved = false;
        m_remoteApproved = false;
    }

    void SetRuntimeError(const std::string& error) {
        if (error.empty()) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.error = error;
        }
        LOG_WARN("Cross transport: %s", error.c_str());
    }

    void WaitReconnect() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait_for(lock, kReconnectDelay, [&] { return !m_running.load(); });
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::atomic<bool> m_running{false};
    std::thread m_worker;
    CrossTransportOptions m_options;
    CrossTransportSnapshot m_snapshot;
    CrossIdentity m_identity;
    CrossDiscovery m_discovery;
    std::deque<json> m_outbound;
    std::deque<json> m_inbound;
    std::map<std::string, double> m_routeLatency;
    std::string m_stickyPeerId;
    bool m_localApproved = false;
    bool m_remoteApproved = false;
    bool m_queueOverflow = false;
    bool m_forceDisconnect = false;
    std::string m_forceDisconnectReason;
};

CrossTransport::CrossTransport() : m_impl(std::make_unique<Impl>()) {}
CrossTransport::~CrossTransport() = default;

bool CrossTransport::Start(const CrossTransportOptions& options) { return m_impl->Start(options); }
void CrossTransport::Stop() { m_impl->Stop(); }
bool CrossTransport::Send(const nlohmann::json& message) { return m_impl->Send(message); }
bool CrossTransport::ApprovePairing() { return m_impl->ApprovePairing(); }
bool CrossTransport::ForgetDevice(const std::string& peerFingerprint) {
    return m_impl->ForgetDevice(peerFingerprint);
}
void CrossTransport::AbortConnection() { m_impl->AbortConnection(); }
CrossTransportSnapshot CrossTransport::Snapshot() const { return m_impl->Snapshot(); }
std::vector<nlohmann::json> CrossTransport::DrainMessages() { return m_impl->DrainMessages(); }

} // namespace thunderlink
