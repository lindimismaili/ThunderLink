#pragma once

#include <cstddef>
#include <string_view>
#include <windows.h>
#include "UIAssets.h"

namespace thunderlink::ui {

struct AssetView {
    const void* bytes = nullptr;
    size_t size = 0;
    const char* mime = nullptr;
    explicit operator bool() const { return bytes != nullptr; }
};

// Only exact, generated manifest paths are served. Encoded separators,
// traversal, queries, fragments and all other origins fail the lookup.
inline AssetView LookupAsset(HINSTANCE module, std::wstring_view uri) {
    constexpr std::wstring_view origin = L"https://thunderlink.invalid/";
    if (!module || uri.substr(0, origin.size()) != origin) return {};
    auto path = uri.substr(origin.size());
    if (path.empty()) path = L"index.html";
    for (const auto& asset : kAssets) {
        const std::string_view allowed(asset.path);
        if (path.size() != allowed.size()) continue;
        bool match = true;
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] != static_cast<wchar_t>(allowed[i])) { match = false; break; }
        }
        if (!match) continue;
        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(asset.resourceId), RT_RCDATA);
        if (!resource) return {};
        HGLOBAL loaded = LoadResource(module, resource);
        if (!loaded) return {};
        const void* bytes = LockResource(loaded);
        const DWORD size = SizeofResource(module, resource);
        return bytes && size ? AssetView{bytes, size, asset.mime} : AssetView{};
    }
    return {};
}

} // namespace thunderlink::ui
