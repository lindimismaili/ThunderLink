#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <objbase.h>

#include "App/Application.h"
#include "Utils/Logger.h"

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

    /* Initialize logging */
    thunderlink::Logger::Init();
    LOG_INFO("ThunderLink Receiver starting up");

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

    WSACleanup();
    CoUninitialize();

    return exitCode;
}
