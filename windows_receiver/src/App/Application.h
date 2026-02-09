#pragma once

#include <memory>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace thunderlink {

class Window;
class D3D12Renderer;
class FrameBuffer;
class PerformanceMonitor;
class NetworkManager;
class FrameReceiver;
class InputSender;
class InputCapture;

class Application {
public:
    explicit Application(HINSTANCE hInstance);
    ~Application();

    bool Initialize(int nCmdShow);
    int Run();
    void Shutdown();

private:
    void OnConnected(uint32_t width, uint32_t height, uint32_t fps);
    void OnDisconnected();
    void OnResize(uint32_t width, uint32_t height);

    HINSTANCE m_hInstance;

    std::unique_ptr<Window> m_window;
    std::unique_ptr<D3D12Renderer> m_renderer;
    std::unique_ptr<FrameBuffer> m_frameBuffer;
    std::unique_ptr<PerformanceMonitor> m_perfMonitor;
    std::unique_ptr<NetworkManager> m_networkManager;
    std::unique_ptr<FrameReceiver> m_frameReceiver;
    std::unique_ptr<InputSender> m_inputSender;
    std::unique_ptr<InputCapture> m_inputCapture;

    bool m_streaming = false;
};

} // namespace thunderlink
