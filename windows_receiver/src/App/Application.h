#pragma once

#include <memory>
#include <mutex>
#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Network/NetworkManager.h"   /* ConnectionInfo */
#include "Cross/CrossSettings.h"
#include "Cross/CrossDisplays.h"

namespace thunderlink {

class Window;
class D3D12Renderer;
class FrameBuffer;
class PerformanceMonitor;
class FrameReceiver;
class InputSender;
class InputCapture;
class VideoDecoder;
class CrossController;

class Application {
public:
    explicit Application(HINSTANCE hInstance);
    ~Application();

    bool Initialize(int nCmdShow);
    int Run();
    void Shutdown();
    bool RestartRequested() const { return m_restartRequested; }

private:
    void OnConnected(const ConnectionInfo& info);
    void OnDisconnected();
    void OnResize(uint32_t width, uint32_t height);

    /* Render-thread: (re)create the decoder when a new connection is negotiated. */
    void MaybeInitDecoder();

    /* Builds the ImGui overlay each frame: a persistent top toolbar plus the
     * toggleable Stats and Logs panels. F1 hides/shows the whole overlay. */
    void DrawUI();
    void DrawDesktop();
    void StartNetwork();
    void StopNetwork();
    void StartCross();
    void StopCross();
    void SwitchMode(CrossMode mode);
    void RefreshCrossDisplays();
    void DrawCrossConnection(float scale);
    void DrawCrossArrangement(float scale);
    bool m_networkEnabled = true;
    bool m_networkStarted = false;
    bool m_mediaStartupAttempted = false;
    bool m_mediaInitialized = false;
    HMODULE m_mediaPlatformModule = nullptr;
    HMODULE m_mediaModule = nullptr;
    bool m_toggleRequested = false;
    bool m_restartRequested = false;
    bool m_dashboard = true;
    int m_section = 0;
    std::string m_desktopError;
    std::string m_settingsError;
    char m_editBind[64]{}, m_editPeer[64]{}, m_editVideo[16]{}, m_editInput[16]{}, m_editControl[16]{};
    char m_editResolution[32]{}, m_editFPS[16]{}, m_editBitrate[16]{};
    int m_editCodec = 1;
    bool m_editVSync = false;
    CrossSettings m_crossSettings{};
    std::string m_crossError;
    char m_editCrossHost[256]{};
    char m_editEmergencyShortcut[48]{};
    bool m_showCrossArrangement = false;
    bool m_crossDisconnected = false;
    float m_crossZoom = 0.16f;
    float m_crossPanX = 0.0f;
    float m_crossPanY = 0.0f;
    std::vector<CrossLocalDisplay> m_localCrossDisplays;
    cross::Layout m_crossDraftLayout;

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
    std::unique_ptr<CrossController> m_crossController;

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
    uint64_t m_sessionId = 0;
    uint32_t m_streamFPS = 60;
    bool m_streaming = false;

    /* Overlay UI state. F1 hides everything; the toolbar buttons toggle panels. */
    bool  m_uiVisible     = true;
    bool  m_showStats     = false;
    bool  m_showLogs      = false;
    bool  m_logAutoScroll = true;
    float m_toolbarHeight = 40.0f;  /* measured each frame; positions panels below */
};

} // namespace thunderlink
