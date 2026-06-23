#pragma once

#include <memory>
#include <mutex>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Network/NetworkManager.h"   /* ConnectionInfo */

namespace thunderlink {

class Window;
class D3D12Renderer;
class FrameBuffer;
class PerformanceMonitor;
class FrameReceiver;
class InputSender;
class InputCapture;
class VideoDecoder;

class Application {
public:
    explicit Application(HINSTANCE hInstance);
    ~Application();

    bool Initialize(int nCmdShow);
    int Run();
    void Shutdown();

private:
    void OnConnected(const ConnectionInfo& info);
    void OnDisconnected();
    void OnResize(uint32_t width, uint32_t height);

    /* Render-thread: (re)create the decoder when a new connection is negotiated. */
    void MaybeInitDecoder();

    /* Builds the ImGui overlay each frame (status / stats / controls). */
    void DrawUI();

    HINSTANCE m_hInstance;

    std::unique_ptr<Window> m_window;
    std::unique_ptr<D3D12Renderer> m_renderer;
    std::unique_ptr<FrameBuffer> m_frameBuffer;
    std::unique_ptr<PerformanceMonitor> m_perfMonitor;
    std::unique_ptr<NetworkManager> m_networkManager;
    std::unique_ptr<FrameReceiver> m_frameReceiver;
    std::unique_ptr<InputSender> m_inputSender;
    std::unique_ptr<InputCapture> m_inputCapture;
    std::unique_ptr<VideoDecoder> m_decoder;

    /* Decoder (re)init handoff from the control thread to the render thread. */
    std::mutex m_decoderMutex;
    bool m_pendingDecoderInit = false;
    ConnectionInfo m_pendingInfo{};

    uint64_t m_lastFrameId = 0;
    bool m_streaming = false;
};

} // namespace thunderlink
