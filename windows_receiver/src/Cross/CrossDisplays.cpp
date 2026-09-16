#include "CrossDisplays.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cwchar>
#include <mutex>
#include <unordered_set>

namespace thunderlink {
namespace {

std::string Utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string value(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, value.data(), length, nullptr, nullptr);
    value.pop_back();
    return value;
}

std::wstring StableIdForGdiDevice(const wchar_t* gdiName) {
    UINT32 pathsCount = 0, modesCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathsCount, &modesCount) != ERROR_SUCCESS || !pathsCount) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathsCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modesCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathsCount, paths.data(), &modesCount, modes.data(), nullptr) != ERROR_SUCCESS) return {};
    for (UINT32 index = 0; index < pathsCount; ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source); source.header.adapterId = paths[index].sourceInfo.adapterId; source.header.id = paths[index].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || _wcsicmp(source.viewGdiDeviceName, gdiName) != 0) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target); target.header.adapterId = paths[index].targetInfo.adapterId; target.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorDevicePath[0]) return target.monitorDevicePath;
    }
    return {};
}

float MonitorScale(HMONITOR monitor) {
    using GetDpiForMonitorFn = HRESULT (WINAPI *)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    HMODULE module = LoadLibraryW(L"Shcore.dll");
    if (!module) return 1.0f;
    const auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(module, "GetDpiForMonitor"));
    UINT x = 96, y = 96;
    const bool valid = getDpi && SUCCEEDED(getDpi(monitor, MDT_EFFECTIVE_DPI, &x, &y));
    FreeLibrary(module);
    return valid && x ? static_cast<float>(x) / 96.0f : 1.0f;
}

struct EnumerateContext { std::vector<CrossLocalDisplay> displays; };

BOOL CALLBACK EnumMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM opaque) {
    auto& context = *reinterpret_cast<EnumerateContext*>(opaque);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;

    const RECT& rect = info.rcMonitor;
    // Duplicate desktop bounds are Windows clone/mirror surfaces. Cross treats
    // them as one logical display, matching the protocol's mirrored-display rule.
    for (const auto& prior : context.displays) {
        const auto& bounds = prior.display.bounds;
        if (bounds.x == rect.left && bounds.y == rect.top &&
            bounds.width == rect.right - rect.left && bounds.height == rect.bottom - rect.top) return TRUE;
    }

    DISPLAY_DEVICEW adapter{}; adapter.cb = sizeof(adapter);
    std::wstring stableId = StableIdForGdiDevice(info.szDevice);
    if (stableId.empty()) stableId = info.szDevice; // legacy fallback for uncommon virtual adapters.
    std::wstring friendly = info.szDevice;
    if (EnumDisplayDevicesW(info.szDevice, 0, &adapter, 0)) {
        if (adapter.DeviceID[0]) stableId += L"|" + std::wstring(adapter.DeviceID);
        if (adapter.DeviceString[0]) friendly = adapter.DeviceString;
    }
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    int rotation = 0;
    if (EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0)) {
        switch (mode.dmDisplayOrientation) {
            case DMDO_90: rotation = 90; break;
            case DMDO_180: rotation = 180; break;
            case DMDO_270: rotation = 270; break;
            default: break;
        }
    }

    CrossLocalDisplay display{};
    display.display.id = "windows:" + Utf8(stableId.c_str());
    display.display.name = Utf8(friendly.c_str());
    if (display.display.name.empty()) display.display.name = Utf8(info.szDevice);
    display.display.bounds = {double(rect.left), double(rect.top),
                              double(rect.right - rect.left), double(rect.bottom - rect.top)};
    display.display.remote = false;
    display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    display.scale = MonitorScale(monitor);
    display.rotation = rotation;
    context.displays.push_back(std::move(display));
    return TRUE;
}

constexpr UINT kIdentifyTimer = 102;
constexpr wchar_t kIdentifyClass[] = L"ThunderLinkCrossIdentify";
std::once_flag g_identifyClass;

LRESULT CALLBACK IdentifyProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{}; HDC hdc = BeginPaint(hwnd, &ps);
            RECT rect{}; GetClientRect(hwnd, &rect);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(232, 237, 247));
            HFONT font = CreateFontW(-MulDiv(56, GetDpiForWindow(hwnd), 96), 0, 0, 0, FW_BOLD,
                                     FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HGDIOBJ previous = SelectObject(hdc, font);
            wchar_t text[16]{}; GetWindowTextW(hwnd, text, 16);
            DrawTextW(hdc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, previous); DeleteObject(font); EndPaint(hwnd, &ps); return 0;
        }
        case WM_TIMER: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: KillTimer(hwnd, kIdentifyTimer); return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void RegisterIdentifyClass() {
    WNDCLASSW wc{}; wc.lpfnWndProc = IdentifyProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kIdentifyClass; wc.hbrBackground = CreateSolidBrush(RGB(23, 27, 36));
    RegisterClassW(&wc);
}
} // namespace

std::vector<CrossLocalDisplay> EnumerateCrossLocalDisplays() {
    EnumerateContext context;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitor, reinterpret_cast<LPARAM>(&context));
    std::sort(context.displays.begin(), context.displays.end(), [](const auto& left, const auto& right) {
        if (left.primary != right.primary) return left.primary;
        return left.display.id < right.display.id;
    });
    return context.displays;
}

void IdentifyCrossLocalDisplays(const std::vector<CrossLocalDisplay>& displays) {
    std::call_once(g_identifyClass, RegisterIdentifyClass);
    unsigned index = 1;
    for (const auto& display : displays) {
        const auto& r = display.display.bounds;
        wchar_t number[16]{}; swprintf_s(number, L"%u", index++);
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                                    kIdentifyClass, number, WS_POPUP | WS_VISIBLE,
                                    static_cast<int>(r.x), static_cast<int>(r.y),
                                    static_cast<int>(r.width), static_cast<int>(r.height),
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) continue;
        SetLayeredWindowAttributes(hwnd, 0, 180, LWA_ALPHA);
        SetTimer(hwnd, kIdentifyTimer, 1500, nullptr);
    }
}

} // namespace thunderlink
