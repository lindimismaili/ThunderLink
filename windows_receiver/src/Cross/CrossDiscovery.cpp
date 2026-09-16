#include "CrossDiscovery.h"
#include "CrossIdentity.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
#include <netioapi.h>
#include <ifdef.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <set>
#include <stdexcept>

namespace thunderlink {

struct CrossDiscovery::State : std::enable_shared_from_this<CrossDiscovery::State> {
    mutable std::mutex mutex;
    std::condition_variable stopped;
    std::atomic<bool> running{false};
    bool browseActive = false;
    uint32_t interfaceIndex = 0;
    std::vector<CrossPeer> peers;
    std::set<std::wstring> resolving;
#ifdef _WIN32
    DNS_SERVICE_CANCEL browseCancel{};
#endif
};

namespace {

#ifdef _WIN32
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

std::string LowerUtf8(const wchar_t* value) {
    auto result = Utf8(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool ParseInterfaceId(const std::string& value, ULONG& result) {
    if (value.empty()) {
        result = 0;
        return true;
    }
    try {
        size_t used = 0;
        const unsigned long parsed = std::stoul(value, &used, 10);
        if (used != value.size() || parsed == 0 || parsed > UINT32_MAX) return false;
        result = static_cast<ULONG>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

void InterfaceDetails(ULONG index, std::string& name, CrossRouteClass& routeClass) {
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

std::string NumericAddress(const DNS_SERVICE_INSTANCE& instance, bool ipv6) {
    if (!ipv6 && instance.ip4Address) {
        IN_ADDR address{};
        address.S_un.S_addr = *instance.ip4Address;
        char text[INET_ADDRSTRLEN]{};
        return InetNtopA(AF_INET, &address, text, static_cast<DWORD>(sizeof(text)))
            ? std::string(text) : std::string{};
    }
    if (ipv6 && instance.ip6Address) {
        IN6_ADDR address{};
        static_assert(sizeof(address) == sizeof(*instance.ip6Address), "IPv6 DNS-SD address size mismatch");
        std::memcpy(&address, instance.ip6Address, sizeof(address));
        char narrow[INET6_ADDRSTRLEN]{};
        if (!InetNtopA(AF_INET6, &address, narrow, static_cast<DWORD>(sizeof(narrow)))) return {};
        std::string result = narrow;
        if (IN6_IS_ADDR_LINKLOCAL(&address) && instance.dwInterfaceIndex)
            result += "%" + std::to_string(instance.dwInterfaceIndex);
        return result;
    }
    return {};
}

std::string ServiceName(const wchar_t* fullName) {
    std::string value = Utf8(fullName);
    const auto marker = value.find("._thunderlink-cross._tcp.");
    return marker == std::string::npos ? value : value.substr(0, marker);
}

void MergeResolved(const std::shared_ptr<CrossDiscovery::State>& state,
                   const DNS_SERVICE_INSTANCE& instance) {
    std::string version;
    std::string role;
    std::string id;
    if (instance.dwPropertyCount > 32) return;
    for (DWORD index = 0; index < instance.dwPropertyCount; ++index) {
        const std::string key = LowerUtf8(instance.keys ? instance.keys[index] : nullptr);
        const std::string value = Utf8(instance.values ? instance.values[index] : nullptr);
        if (key == "version") version = value;
        else if (key == "role") role = value;
        else if (key == "id") id = CrossIdentity::NormalizeFingerprint(value);
    }
    if (version != "4" || role != "mac" || id.empty() || instance.wPort == 0) return;

    CrossEndpoint base;
    base.host = Utf8(instance.pszHostName);
    base.port = instance.wPort;
    base.interfaceId = instance.dwInterfaceIndex ? std::to_string(instance.dwInterfaceIndex) : std::string{};
    InterfaceDetails(instance.dwInterfaceIndex, base.interfaceName, base.routeClass);
    base.discovered = true;

    std::vector<CrossEndpoint> endpoints;
    if (instance.ip4Address) {
        auto endpoint = base;
        endpoint.address = NumericAddress(instance, false);
        if (!endpoint.address.empty()) endpoints.push_back(std::move(endpoint));
    }
    if (instance.ip6Address) {
        auto endpoint = base;
        endpoint.address = NumericAddress(instance, true);
        if (!endpoint.address.empty()) endpoints.push_back(std::move(endpoint));
    }
    if (endpoints.empty() && !base.host.empty()) endpoints.push_back(base);
    if (endpoints.empty()) return;

    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->running) return;
    auto peer = std::find_if(state->peers.begin(), state->peers.end(), [&](const CrossPeer& value) {
        return value.id == id;
    });
    if (peer == state->peers.end()) {
        if (state->peers.size() >= 64) return;
        state->peers.push_back(CrossPeer{id, ServiceName(instance.pszInstanceName),
                                         CrossIdentity::HasPin(id), {}});
        peer = std::prev(state->peers.end());
    }
    peer->name = ServiceName(instance.pszInstanceName);
    peer->paired = CrossIdentity::HasPin(id);
    for (auto& endpoint : endpoints) {
        const auto duplicate = std::find_if(peer->endpoints.begin(), peer->endpoints.end(),
            [&](const CrossEndpoint& current) {
                return current.address == endpoint.address && current.port == endpoint.port &&
                       current.interfaceId == endpoint.interfaceId;
            });
        if (duplicate == peer->endpoints.end() && peer->endpoints.size() < 32)
            peer->endpoints.push_back(std::move(endpoint));
    }
}

struct ResolveContext {
    std::shared_ptr<CrossDiscovery::State> state;
    std::wstring name;
    DNS_SERVICE_CANCEL cancel{};
};

void WINAPI ResolveComplete(DWORD status, PVOID queryContext, PDNS_SERVICE_INSTANCE instance) {
    std::unique_ptr<std::shared_ptr<ResolveContext>> holder(
        static_cast<std::shared_ptr<ResolveContext>*>(queryContext));
    const auto context = *holder;
    if (status == ERROR_SUCCESS && instance && context->state->running)
        MergeResolved(context->state, *instance);
    if (instance) DnsServiceFreeInstance(instance);
    std::lock_guard<std::mutex> lock(context->state->mutex);
    context->state->resolving.erase(context->name);
}

void Resolve(const std::shared_ptr<CrossDiscovery::State>& state, const wchar_t* name) {
    if (!name || !*name) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->running || state->resolving.size() >= 64 ||
            !state->resolving.emplace(name).second) return;
    }
    auto context = std::make_shared<ResolveContext>();
    context->state = state;
    context->name = name;
    DNS_SERVICE_RESOLVE_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.InterfaceIndex = state->interfaceIndex;
    request.QueryName = context->name.data();
    request.pResolveCompletionCallback = ResolveComplete;
    auto* callbackContext = new std::shared_ptr<ResolveContext>(context);
    request.pQueryContext = callbackContext;
    const DNS_STATUS status = DnsServiceResolve(&request, &context->cancel);
    if (status != DNS_REQUEST_PENDING) {
        delete callbackContext;
        std::lock_guard<std::mutex> lock(state->mutex);
        state->resolving.erase(context->name);
    }
}

struct BrowseContext {
    std::shared_ptr<CrossDiscovery::State> state;
};

void WINAPI BrowseComplete(DWORD status, PVOID queryContext, PDNS_RECORD records) {
    auto* context = static_cast<BrowseContext*>(queryContext);
    const auto state = context->state;
    if (status == ERROR_CANCELLED) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->browseActive = false;
        state->stopped.notify_all();
    } else if (status == ERROR_SUCCESS && records && state->running) {
        const auto owner = state->shared_from_this();
        for (PDNS_RECORD record = records; record; record = record->pNext) {
            if (record->wType == DNS_TYPE_PTR) Resolve(owner, record->Data.PTR.pNameHost);
        }
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordListDeep);
    if (status == ERROR_CANCELLED) delete context;
}
#endif

} // namespace

CrossDiscovery::CrossDiscovery() = default;

CrossDiscovery::~CrossDiscovery() { Stop(); }

bool CrossDiscovery::Start(const std::string& interfaceId, std::string& error) {
    Stop();
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
#ifdef _WIN32
    ULONG interfaceIndex = 0;
    if (!ParseInterfaceId(interfaceId, interfaceIndex)) {
        error = "The selected network interface ID is invalid";
        return false;
    }
    m_state = std::make_shared<State>();
    m_state->running = true;
    m_state->browseActive = true;
    m_state->interfaceIndex = interfaceIndex;
    static constexpr wchar_t service[] = L"_thunderlink-cross._tcp.local";
    DNS_SERVICE_BROWSE_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.InterfaceIndex = interfaceIndex;
    request.QueryName = service;
    request.pBrowseCallback = BrowseComplete;
    auto context = std::make_unique<BrowseContext>();
    context->state = m_state;
    request.pQueryContext = context.get();
    const DNS_STATUS status = DnsServiceBrowse(&request, &m_state->browseCancel);
    if (status != DNS_REQUEST_PENDING) {
        error = "Windows DNS-SD discovery could not start (" + std::to_string(status) + ")";
        m_state.reset();
        return false;
    }
    context.release();
    return true;
#else
    (void)interfaceId;
    error = "Cross discovery requires Windows";
    return false;
#endif
}

void CrossDiscovery::Stop() {
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    auto state = m_state;
    if (!state) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->running = false;
    }
#ifdef _WIN32
    bool browseActive = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browseActive = state->browseActive;
    }
    if (browseActive) {
        (void)DnsServiceBrowseCancel(&state->browseCancel);
        std::unique_lock<std::mutex> lock(state->mutex);
        (void)state->stopped.wait_for(lock, std::chrono::seconds(2), [&] {
            return !state->browseActive;
        });
    }
#endif
    m_state.reset();
}

std::vector<CrossPeer> CrossDiscovery::Peers() const {
    std::shared_ptr<State> state;
    {
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
        state = m_state;
    }
    if (!state) return {};
    std::lock_guard<std::mutex> lock(state->mutex);
    auto result = state->peers;
    for (auto& peer : result) peer.paired = CrossIdentity::HasPin(peer.id);
    return result;
}

} // namespace thunderlink
