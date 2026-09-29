#include "App/UIAssetStore.h"
#include <cassert>
#include <bcrypt.h>
#include <climits>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

static std::string Sha256(const void* data, size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, propertySize = 0;
    assert(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0);
    assert(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                             reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &propertySize, 0) >= 0);
    std::vector<unsigned char> object(objectSize);
    unsigned char digest[32]{};
    assert(BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0);
    assert(size <= ULONG_MAX);
    assert(BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const unsigned char*>(data)),
                          static_cast<ULONG>(size), 0) >= 0);
    assert(BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

int wmain(int argc, wchar_t** argv) {
    using thunderlink::ui::LookupAsset;
    const auto module = argc > 1 ? LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE)
                                  : GetModuleHandleW(nullptr);
    assert(module);
    const auto index = LookupAsset(module, L"https://thunderlink.invalid/");
    assert(index && index.size > 100 && std::strcmp(index.mime, "text/html") == 0);
    assert(LookupAsset(module, L"https://thunderlink.invalid/index.html"));
    assert(LookupAsset(module, L"https://thunderlink.invalid/ui.js"));
    assert(!LookupAsset(module, L"http://thunderlink.invalid/index.html"));
    assert(!LookupAsset(module, L"https://example.com/index.html"));
    assert(!LookupAsset(module, L"https://thunderlink.invalid/../index.html"));
    assert(!LookupAsset(module, L"https://thunderlink.invalid/%2e%2e/index.html"));
    assert(!LookupAsset(module, L"https://thunderlink.invalid/index.html?x=1"));
    assert(!LookupAsset(module, L"https://thunderlink.invalid/missing.css"));
    for (const auto& asset : thunderlink::ui::kAssets) {
        std::wstring url = L"https://thunderlink.invalid/";
        for (const char* p = asset.path; *p; ++p) url.push_back(static_cast<wchar_t>(*p));
        const auto resource = LookupAsset(module, url);
        assert(resource && std::strcmp(resource.mime, asset.mime) == 0);
        if (*asset.sha256) assert(Sha256(resource.bytes, resource.size) == asset.sha256);
    }
    const auto manifest = LookupAsset(module, L"https://thunderlink.invalid/manifest.json");
    const auto parsed = nlohmann::json::parse(static_cast<const char*>(manifest.bytes),
                                              static_cast<const char*>(manifest.bytes) + manifest.size);
    assert(parsed.at("contractVersion") == thunderlink::ui::kContractVersion);
    assert(parsed.at("buildId") == thunderlink::ui::kBuildId);
    if (argc > 1) FreeLibrary(module);
    std::cout << "UI embedded resource allowlist tests passed\n";
}
