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

    /* Builds the ImGui overlay each frame: a persistent top toolbar plus the
     * toggleable Stats and Logs panels. F1 hides/shows the whole overlay. */
    void DrawUI();
    void DrawToolbar(bool connected);
    void DrawStatsPanel(bool connected);
    void DrawLogsPanel();

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

    /* Resolution the decoder is currently built for; the render loop rebuilds it
     * when the incoming stream resolution changes (e.g. user picks a new res). */
    uint32_t m_decoderW = 0;
    uint32_t m_decoderH = 0;
    TlnkCodec m_curCodec = TLNK_CODEC_HEVC;

    uint64_t m_lastFrameId = 0;
    bool m_streaming = false;

    /* Overlay UI state. F1 hides everything; the toolbar buttons toggle panels. */
    bool  m_uiVisible     = true;
    bool  m_showStats     = false;
    bool  m_showLogs      = false;
    bool  m_logAutoScroll = true;
    float m_toolbarHeight = 40.0f;  /* measured each frame; positions panels below */
};

} // namespace thunderlink
