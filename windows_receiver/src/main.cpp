#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <string>
#include "App/Application.h"
#include "App/DesktopSettings.h"
#include "Utils/Logger.h"

namespace {
std::string WideToUtf8(const wchar_t* text) {
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::invalid_argument("Invalid command line encoding");
    std::string result(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back(); return result;
}
void LoadConfiguration() {
    using namespace thunderlink;
    SavedConfig() = LoadSettings(SettingsPath());
    int count = 0;
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values) throw std::invalid_argument("Could not parse command line");
    std::vector<std::string> args;
    try { for (int i = 1; i < count; ++i) args.push_back(WideToUtf8(values[i])); }
    catch (...) { LocalFree(values); throw; }
    LocalFree(values);
    GetConfig() = ParseArguments(args, SavedConfig());
}
}
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    // Session-local mutex: a second launch only reveals the existing window.
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\ThunderLink.Receiver.Instance");
    if (!singleton) { MessageBoxW(nullptr, L"Could not initialize ThunderLink.", L"ThunderLink", MB_OK | MB_ICONERROR); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = nullptr;
        for (int retry = 0; retry < 100 && !existing; ++retry) {
            existing = FindWindowW(L"ThunderLinkReceiverClass", nullptr);
            if (!existing) Sleep(50);
        }
        if (existing) {
            DWORD process = 0; GetWindowThreadProcessId(existing, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(existing, WM_APP + 40, 0, 0);
        }
        CloseHandle(singleton); return 0;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { CloseHandle(singleton); return 1; }
    try { LoadConfiguration(); }
    catch (const std::exception& e) { thunderlink::StartupSettingsError() = e.what(); }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { CoUninitialize(); CloseHandle(singleton); return 1; }
    try { thunderlink::Logger::InitWide(thunderlink::LogPath().c_str(), thunderlink::GetConfig().showConsole); }
    catch (...) { thunderlink::Logger::Init(nullptr, thunderlink::GetConfig().showConsole); }
    int result = 0;
    bool restart = false;
    {
        thunderlink::Application app(instance);
        try {
            if (!app.Initialize(show)) {
                MessageBoxW(nullptr, L"ThunderLink could not initialize its graphics or input system. Check Direct3D 12 support and your graphics driver. Details are in the ThunderLink log.", L"ThunderLink", MB_OK | MB_ICONERROR);
                result = 1;
            } else result = app.Run();
        }

        catch (const std::exception& e) {
            LOG_ERROR("Application failure: %s", e.what());
            MessageBoxA(nullptr, e.what(), "ThunderLink", MB_OK | MB_ICONERROR); result = 1;
        }
        restart = app.RestartRequested();
        app.Shutdown();
    }
    thunderlink::Logger::Shutdown(); WSACleanup(); CoUninitialize();
    CloseHandle(singleton);
    if (restart) {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", executable, nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            MessageBoxW(nullptr, L"Settings were saved. Please reopen ThunderLink.", L"ThunderLink", MB_OK | MB_ICONERROR);
    }
    return result;
}
