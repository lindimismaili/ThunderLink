#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <objbase.h>
#include <shellapi.h>
#include <mfapi.h>
#include <string>

#include "App/Application.h"
#include "App/Config.h"
#include "Utils/Logger.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "mfplat.lib")

namespace {

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

/* Parse command-line overrides into the global Config. Recognized flags mirror
 * the Mac sender:
 *   --bind <ip> --peer <ip> --video-port <n> --control-port <n> --input-port <n>
 *   --codec h264|hevc --bitrate <Mbps> --fps <n> --res <WxH> --fullscreen
 */
void ParseCommandLine(thunderlink::Config& cfg) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;

    auto needsValue = [&](int i) { return i + 1 < argc; };
    for (int i = 1; i < argc; ++i) {
        std::wstring flag = argv[i];
        if (flag == L"--bind" && needsValue(i))               { cfg.bindIP = WideToUtf8(argv[++i]); }
        else if (flag == L"--peer" && needsValue(i))          { cfg.peerIP = WideToUtf8(argv[++i]); }
        else if (flag == L"--video-port" && needsValue(i))    { cfg.videoPort = (uint16_t)_wtoi(argv[++i]); }
        else if (flag == L"--control-port" && needsValue(i))  { cfg.controlPort = (uint16_t)_wtoi(argv[++i]); }
        else if (flag == L"--input-port" && needsValue(i))    { cfg.inputPort = (uint16_t)_wtoi(argv[++i]); }
        else if (flag == L"--codec" && needsValue(i)) {
            std::wstring v = argv[++i];
            cfg.codec = (v == L"h264" || v == L"H264") ? TLNK_CODEC_H264 : TLNK_CODEC_HEVC;
        }
        else if (flag == L"--bitrate" && needsValue(i))       { cfg.bitrateKbps = (uint32_t)_wtoi(argv[++i]) * 1000u; }
        else if (flag == L"--fps" && needsValue(i))           { cfg.fps = (uint32_t)_wtoi(argv[++i]); }
        else if (flag == L"--res" && needsValue(i)) {
            std::wstring v = argv[++i];
            size_t x = v.find_first_of(L"xX");
            if (x != std::wstring::npos) {
                cfg.width  = (uint32_t)_wtoi(v.substr(0, x).c_str());
                cfg.height = (uint32_t)_wtoi(v.substr(x + 1).c_str());
            }
        }
        else if (flag == L"--window" && needsValue(i)) {
            std::wstring v = argv[++i];
            size_t x = v.find_first_of(L"xX");
            if (x != std::wstring::npos) {
                cfg.windowWidth  = (uint32_t)_wtoi(v.substr(0, x).c_str());
                cfg.windowHeight = (uint32_t)_wtoi(v.substr(x + 1).c_str());
            }
        }
        else if (flag == L"--fullscreen")                     { cfg.startFullscreen = true; }
    }

    LocalFree(argv);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int nCmdShow)
{
    /* Initialize COM for D3D12 */
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"ThunderLink Error",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Initialize Winsock */
    WSADATA wsaData;
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0) {
        MessageBoxW(nullptr, L"Failed to initialize Winsock.", L"ThunderLink Error",
                    MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    /* Initialize Media Foundation (hardware video decode) */
    HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(mfHr)) {
        MessageBoxW(nullptr, L"Failed to initialize Media Foundation.", L"ThunderLink Error",
                    MB_OK | MB_ICONERROR);
        WSACleanup();
        CoUninitialize();
        return 1;
    }

    /* Initialize logging. Also append to the repo's cross-device log
     * (logs/windows.log, path baked in by CMake) so it can be read on the Mac
     * after a git sync; falls back to the working directory otherwise. */
#ifdef TLNK_LOG_FILE
    thunderlink::Logger::Init(TLNK_LOG_FILE);
#else
    thunderlink::Logger::Init("thunderlink_receiver.log");
#endif
    LOG_INFO("ThunderLink Receiver starting up");

    /* Parse command-line endpoint/stream overrides into the global config */
    ParseCommandLine(thunderlink::GetConfig());
    {
        const auto& c = thunderlink::GetConfig();
        LOG_INFO("Config: bind=%s peer=%s ports(v/i/c)=%u/%u/%u %ux%u@%u codec=%s %ukbps",
                 c.bindIP.c_str(), c.peerIP.c_str(),
                 c.videoPort, c.inputPort, c.controlPort,
                 c.width, c.height, c.fps,
                 (c.codec == TLNK_CODEC_HEVC ? "HEVC" : "H264"), c.bitrateKbps);
    }

    int exitCode = 0;

    try {
        thunderlink::Application app(hInstance);

        if (!app.Initialize(nCmdShow)) {
            LOG_ERROR("Application initialization failed");
            exitCode = 1;
        } else {
            LOG_INFO("Application initialized successfully");
            exitCode = app.Run();
        }

        app.Shutdown();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal exception: %s", e.what());
        MessageBoxA(nullptr, e.what(), "ThunderLink Fatal Error",
                    MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    LOG_INFO("ThunderLink Receiver shutting down");
    thunderlink::Logger::Shutdown();

    MFShutdown();
    WSACleanup();
    CoUninitialize();

    return exitCode;
}
