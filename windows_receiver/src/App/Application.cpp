#include "Application.h"

#include "App/Window.h"
#include "App/Config.h"
#include "App/DesktopSettings.h"
#include <chrono>
#include <cstdio>
#include "Renderer/D3D12Renderer.h"
#include "Utils/FrameBuffer.h"
#include "Utils/PerformanceMonitor.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include "Network/FrameReceiver.h"
#include "Network/InputSender.h"
#include "Input/InputCapture.h"
#include "Decoder/VideoDecoder.h"
#include "Cross/CrossController.h"
#include "Cross/CrossDisplays.h"
#include "cross_input_state.h"

#include "imgui.h"

#include "protocol.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mfapi.h>

namespace thunderlink {

namespace {
std::vector<CrossSettings::TopologyDisplay> LocalTopology(const std::vector<CrossLocalDisplay>& displays) {
    std::vector<CrossSettings::TopologyDisplay> result;
    result.reserve(displays.size());
    for (const auto& display : displays) {
        const auto& bounds = display.display.bounds;
        result.push_back({display.display.id, bounds.x, bounds.y, bounds.width, bounds.height, display.scale, display.rotation});
    }
    return result;
}
} // namespace

Application::Application(HINSTANCE hInstance)
    : m_hInstance(hInstance)
{
}

Application::~Application() {
}

bool Application::Initialize(int nCmdShow) {
    const auto& cfg = GetConfig();
    const auto& saved = SavedConfig();
    snprintf(m_editBind, sizeof(m_editBind), "%s", saved.bindIP.c_str());
    snprintf(m_editPeer, sizeof(m_editPeer), "%s", saved.peerIP.c_str());
    snprintf(m_editVideo, sizeof(m_editVideo), "%u", saved.videoPort);
    snprintf(m_editInput, sizeof(m_editInput), "%u", saved.inputPort);
    snprintf(m_editControl, sizeof(m_editControl), "%u", saved.controlPort);
    snprintf(m_editResolution, sizeof(m_editResolution), "%ux%u", saved.width, saved.height);
    snprintf(m_editFPS, sizeof(m_editFPS), "%u", saved.fps);
    snprintf(m_editBitrate, sizeof(m_editBitrate), "%u", saved.bitrateKbps / 1000);
    m_editCodec = saved.codec == TLNK_CODEC_HEVC ? 1 : 0;
    m_editVSync = saved.vsync;
    m_networkEnabled = StartupSettingsError().empty();
    try { m_crossSettings = LoadCrossSettings(CrossSettingsPath()); }
    catch (const std::exception& error) { m_crossError = error.what(); m_crossSettings = {}; }
    snprintf(m_editCrossHost, sizeof(m_editCrossHost), "%s", m_crossSettings.manualHost.c_str());
    snprintf(m_editEmergencyShortcut, sizeof(m_editEmergencyShortcut), "%s", m_crossSettings.emergencyShortcut.c_str());


    /* Create the window at the (smaller) window size; the 1440p stream is
     * downscaled to fit. */
    m_window = std::make_unique<Window>();
    if (!m_window->Create(m_hInstance, nCmdShow, cfg.windowWidth, cfg.windowHeight)) {
        LOG_ERROR("Application: window creation failed");
        return false;
    }
    m_window->SetFullscreen(cfg.startFullscreen);
    m_window->SetCrossSafetyCallback([this]() {
        if (!m_crossController) return;
        m_crossController->InvalidateLocalTopology("Windows displays or power state changed. Review Cross arrangement.");
        m_crossSettings.enabled = false;
        m_crossError = m_crossController->Status();
    });

    /* Initialize D3D12 renderer */
    m_renderer = std::make_unique<D3D12Renderer>();
    if (!m_renderer->Initialize(m_window->GetHandle(),
                                 m_window->GetWidth(), m_window->GetHeight())) {
        LOG_ERROR("Application: renderer initialization failed");
        return false;
    }

    /* Window resize callback */
    m_window->SetResizeCallback([this](uint32_t w, uint32_t h) {
        OnResize(w, h);
    });

    /* Overlay UI (Dear ImGui) drawn by the renderer each frame */
    m_renderer->SetUICallback([this]() { DrawUI(); });

    if (m_crossSettings.mode == CrossMode::Cross) {
        RefreshCrossDisplays();
        StartCross();
        return true;
    }

    /* Screen Share alone allocates the capture/video pipeline. Cross has its
     * own ordered input transport and must not start these services. */
    m_perfMonitor = std::make_unique<PerformanceMonitor>();

    /* Create frame buffer with pre-allocated pool */
    m_frameBuffer = std::make_unique<FrameBuffer>(
        TLNK_FRAME_POOL_SIZE, TLNK_MAX_WIDTH, TLNK_MAX_HEIGHT, m_perfMonitor.get());

    /* Start network manager (control channel) */
    m_networkManager = std::make_unique<NetworkManager>();
    m_networkManager->SetOnConnected([this](const ConnectionInfo& info) {
        OnConnected(info);
    });
    m_networkManager->SetOnDisconnected([this]() {
        OnDisconnected();
    });

    /* Lost/dropped frames -> ask the Mac for an IDR (rate-limited in NetworkManager) */
    m_frameBuffer->SetOnFrameLost([this]() {
        if (m_networkManager) m_networkManager->RequestKeyframe();
    });

    /* Start frame receiver (UDP listener) */
    m_frameReceiver = std::make_unique<FrameReceiver>(m_frameBuffer.get(), m_perfMonitor.get());


    /* Start input sender (TCP to Mac) */
    m_inputSender = std::make_unique<InputSender>();


    /* Initialize input capture */
    m_inputCapture = std::make_unique<InputCapture>();
    if (!m_inputCapture->Initialize(m_window->GetHandle(),
            [this](const InputEvent& event) {
                return m_inputSender->SendEvent(event);
            })) {
        LOG_ERROR("Application: input capture initialization failed");
        return false;
    }

    /* Wire input capture into window message loop */
    m_window->SetInputCapture(m_inputCapture.get());
    m_inputCapture->SetEnabled(false);

    if (m_networkEnabled) StartNetwork();
    return true;
}

int Application::Run() {
    auto nextRetry = std::chrono::steady_clock::now();
    while (m_window->ProcessMessages()) {
        if (m_crossController) m_crossController->Tick();
        // The input worker can fail open while the dashboard is hidden. Keep
        // that safe paused state across a restart without changing an
        // intentional initially-paused arrangement during discovery.
        if (m_crossController && m_crossSettings.enabled && !m_crossController->Enabled()) {
            m_crossSettings.enabled = false;
            try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); }
            catch (const std::exception& error) { m_crossError = error.what(); }
        }
        if (m_toggleRequested) {
            m_toggleRequested = false;
            m_networkEnabled = !m_networkEnabled;
            if (!m_networkEnabled) StopNetwork();
            nextRetry = std::chrono::steady_clock::now();
        }
        if (m_networkEnabled && !m_networkStarted && std::chrono::steady_clock::now() >= nextRetry) {
            StartNetwork();
            nextRetry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        const bool visible = m_window->IsVisible();

        if (m_networkManager) MaybeInitDecoder();
        m_window->SetAspectRatio(!m_dashboard && m_streaming && m_decoderH ? float(m_decoderW) / float(m_decoderH) : 0.0f);
        if (m_inputCapture) m_inputCapture->SetEnabled(visible && m_streaming && !m_dashboard);
        // Consume all available AUs promptly. The decoder owns its own bounded
        // queue; decoding never blocks the message/render loop.
        for (unsigned i = 0; m_frameBuffer && i < TLNK_FRAME_POOL_SIZE; ++i) {
            ReceivedFrame rf{};
            if (!m_frameBuffer->WaitForFrame(&rf, 0)) break;
            if (!m_streaming || rf.sessionId != m_sessionId ||
                rf.format != (m_curCodec == TLNK_CODEC_HEVC ? TLNK_FORMAT_HEVC : TLNK_FORMAT_H264)) {
                m_frameBuffer->ReleaseFrame(rf.sessionId, rf.frameId);
                continue;
            }
            if (!m_decoder || rf.width != m_decoderW || rf.height != m_decoderH) {
                m_decoder = CreateMediaFoundationDecoder();
                m_decoderW = rf.width; m_decoderH = rf.height; m_lastFrameId = 0;
                if (!m_decoder->Initialize(m_curCodec, rf.width, rf.height, m_streamFPS, m_sessionId)) {
                    LOG_ERROR("Decoder rejected stream parameters");
                }
                m_inputCapture->SetStreamSize(rf.width, rf.height);
                if (!m_dashboard) m_window->SetAspectRatio(float(rf.width) / float(rf.height));
            }
            if (m_lastFrameId && rf.frameId != m_lastFrameId + 1) m_decoder->RequireKeyframe();
            m_lastFrameId = rf.frameId;
            m_decoder->Submit(rf.data, rf.size, (rf.flags & TLNK_FLAG_KEY_FRAME) != 0, rf.sessionId);
            m_frameBuffer->ReleaseFrame(rf.sessionId, rf.frameId);
        }
        bool rendered = false;
        if (m_decoder) {
            DecodedFrame frame{}, latest{};
            bool haveFrame = false;
            while (m_decoder->TryGetFrame(&frame)) {
                if (frame.sessionId == m_sessionId) { latest = std::move(frame); haveFrame = true; }
            }
            if (visible && haveFrame && latest.sessionId == m_frameBuffer->GetSession()) {
                rendered = m_renderer->RenderFrame(latest.YPlane(), latest.UVPlane(), latest.width,
                                                   latest.height, latest.yStride, latest.uvStride);
                if (rendered) m_perfMonitor->FrameRendered();
            }
            if (m_decoder->IsWaitingForKeyframe()) m_networkManager->RequestKeyframe();
        }
        if (visible && !rendered) m_renderer->RenderLastFrame();
        if (!visible || !m_streaming) MsgWaitForMultipleObjectsEx(0, nullptr, visible ? 16 : 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        if (m_perfMonitor) m_perfMonitor->MaybeLogStats();
    }
    return 0;
}

void Application::StartNetwork() {
    if (!m_networkEnabled || m_networkStarted || !m_frameReceiver || !m_inputSender || !m_networkManager) return;
    if (m_crossSettings.mode == CrossMode::Cross) return;
    if (!m_mediaStartupAttempted) {
        m_mediaStartupAttempted = true;
        // The executable delay-loads media DLLs. Check their presence before
        // calling any imported function, allowing Cross on Windows N systems.
        m_mediaPlatformModule = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        m_mediaModule = LoadLibraryExW(L"mf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m_mediaPlatformModule && m_mediaModule)
            m_mediaInitialized = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    if (!m_mediaInitialized) {
        m_desktopError = "Screen Share needs Windows Media Foundation. Enable the Media Feature Pack and restart, or choose Cross for input sharing.";
        return;
    }
    m_desktopError.clear();
    if (!m_frameReceiver->Start()) m_desktopError = "Video listener unavailable. Check the local address and video port in Settings.";
    else if (!m_inputSender->Start()) m_desktopError = "Input channel unavailable. Check the peer address and input port.";
    else if (!m_networkManager->Start()) m_desktopError = "Control listener unavailable. Check the local address and control port.";
    else { m_networkStarted = true; return; }
    StopNetwork();
}

void Application::RefreshCrossDisplays() {
    m_localCrossDisplays = EnumerateCrossLocalDisplays();
    cross::Layout refreshed;
    refreshed.reserve(m_localCrossDisplays.size() + m_crossSettings.layout.size());
    for (const auto& display : m_localCrossDisplays) refreshed.push_back(display.display);
    for (const auto& display : m_crossSettings.layout) if (display.remote) refreshed.push_back(display);
    m_crossDraftLayout = std::move(refreshed);
}

void Application::StartCross() {
    if (m_crossSettings.mode != CrossMode::Cross || m_crossController) return;
    m_networkEnabled = false; m_networkStarted = false; m_dashboard = true;
    m_crossController = std::make_unique<CrossController>();
    CrossSettings initial = m_crossSettings;
    initial.layout = m_crossDraftLayout;
    if (!m_crossController->Start(initial, "Windows PC")) m_crossError = m_crossController->Status();
    else { m_crossController->SetCurrentLocalTopology(LocalTopology(m_localCrossDisplays)); m_crossDisconnected = false; }
}

void Application::StopCross() {
    if (m_crossController) m_crossController->Stop();
    m_crossController.reset();
}

void Application::SwitchMode(CrossMode mode) {
    if (m_crossSettings.mode == mode) return;
    try {
        m_crossSettings.mode = mode;
        SaveCrossSettings(m_crossSettings, CrossSettingsPath());
        // A restart is the safe lifecycle boundary: it drains video before
        // Cross or starts Media Foundation only for Screen Share.
        m_restartRequested = true; m_window->RequestQuit();
    } catch (const std::exception& error) { m_crossError = error.what(); }
}

void Application::StopNetwork() {
    if (m_inputCapture) { m_inputCapture->SetEnabled(false); m_inputCapture->SetCursorConfined(false); }
    if (m_networkManager) m_networkManager->Stop();
    if (m_frameReceiver) m_frameReceiver->Stop();
    if (m_inputSender) m_inputSender->Stop();
    OnDisconnected();
    m_networkStarted = false;
    m_dashboard = true;
}

void Application::Shutdown() {

    LOG_INFO("Application: shutting down");

    /* Stop subsystems in reverse order */
    StopCross();
    if (m_inputCapture) {
        m_inputCapture->SetEnabled(false);
        m_inputCapture->SetCursorConfined(false);
    }

    if (m_inputSender) {
        m_inputSender->Stop();
    }

    if (m_frameReceiver) {
        m_frameReceiver->Stop();
    }

    if (m_networkManager) {
        m_networkManager->Stop();
    }

    if (m_renderer) {
        m_renderer->WaitForGPU();
        m_renderer->Shutdown();
    }

    if (m_window) m_window->SetInputCapture(nullptr);
    m_inputCapture.reset();
    m_inputSender.reset();
    m_frameReceiver.reset();
    m_networkManager.reset();
    if (m_decoder) { m_decoder->Shutdown(); m_decoder.reset(); }
    if (m_mediaInitialized) { MFShutdown(); m_mediaInitialized = false; }
    if (m_mediaPlatformModule) { FreeLibrary(m_mediaPlatformModule); m_mediaPlatformModule = nullptr; }
    if (m_mediaModule) { FreeLibrary(m_mediaModule); m_mediaModule = nullptr; }
    m_frameBuffer.reset();
    m_perfMonitor.reset();
    m_crossController.reset();
    m_renderer.reset();
    m_window.reset();

    LOG_INFO("Application: shutdown complete");
}

void Application::OnConnected(const ConnectionInfo& info) {
    if (!m_frameBuffer) return;
    m_frameBuffer->SetSession(info.sessionId, info.width, info.height,
        info.codec == TLNK_CODEC_HEVC ? TLNK_FORMAT_HEVC : TLNK_FORMAT_H264);
    std::lock_guard<std::mutex> lock(m_decoderMutex);
    m_pendingInfo = info;
    m_pendingDecoderInit = true;
}

void Application::OnDisconnected() {
    if (!m_frameBuffer) return;
    m_frameBuffer->SetSession(0);
    std::lock_guard<std::mutex> lock(m_decoderMutex);
    m_pendingInfo = {};
    m_pendingDecoderInit = true;
}

void Application::MaybeInitDecoder() {
    if (!m_inputCapture || !m_inputSender || !m_perfMonitor) return;
    ConnectionInfo info;
    {
        std::lock_guard<std::mutex> lock(m_decoderMutex);
        if (!m_pendingDecoderInit) return;
        m_pendingDecoderInit = false;
        info = m_pendingInfo;
    }
    m_inputCapture->SetEnabled(false);
    m_inputCapture->SetCursorConfined(false);
    m_inputSender->SetSession(info.sessionId);
    m_sessionId = info.sessionId;
    m_streaming = info.connected;
    m_dashboard = !info.connected;
    if (!info.connected) m_window->SetAspectRatio(0);
    m_curCodec = info.codec;
    m_streamFPS = info.fps ? info.fps : GetConfig().fps;
    m_decoder.reset(); m_decoderW = 0; m_decoderH = 0; m_lastFrameId = 0;
    m_perfMonitor->Reset();
    m_inputCapture->SetEnabled(info.connected);
    if (info.connected) m_networkManager->RequestKeyframe();
}

void Application::OnResize(uint32_t width, uint32_t height) {
    if (m_renderer) {
        m_renderer->Resize(width, height);
    }
}

namespace {
/* Text color for a log line by severity (mirrors the console color intent). */
ImVec4 LogLevelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return ImVec4(0.55f, 0.57f, 0.60f, 1.0f);
        case LogLevel::Debug: return ImVec4(0.72f, 0.74f, 0.78f, 1.0f);
        case LogLevel::Info:  return ImVec4(0.45f, 0.85f, 0.55f, 1.0f);
        case LogLevel::Warn:  return ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
        case LogLevel::Error: return ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
        case LogLevel::Fatal: return ImVec4(1.00f, 0.35f, 0.35f, 1.0f);
    }
    return ImVec4(0.92f, 0.93f, 0.95f, 1.0f);
}
} // namespace

void Application::DrawDesktop() {
    const auto* vp = ImGui::GetMainViewport();
    const float scale = ImGui::GetFontSize() / 17.0f;
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 24 * scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.067f, 0.09f, 1));
    ImGui::Begin("ThunderLink Desktop", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::BeginChild("Navigation", ImVec2(190 * scale, 0), false);
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted("ThunderLink");
    ImGui::SetWindowFontScale(1);
    ImGui::Dummy(ImVec2(0, 30 * scale));
    ImGui::TextDisabled("WORKSPACE");
    const char* sections[] = {"Connection", "Settings", "Diagnostics"};
    for (int i = 0; i < 3; ++i) {
        if (ImGui::Selectable(sections[i], m_section == i, 0, ImVec2(0, 38 * scale))) m_section = i;
    }
    ImGui::Dummy(ImVec2(0, 24 * scale));
    ImGui::TextDisabled("WINDOWS / VIEWER");
    ImGui::PushTextWrapPos(170 * scale);
    ImGui::TextDisabled("Close to keep running in the system tray.");
    ImGui::PopTextWrapPos();
    if (m_streaming && ImGui::Button("Back to Video", ImVec2(170 * scale, 36 * scale))) m_dashboard = false;
    ImGui::EndChild();
    ImGui::SameLine(0, 26 * scale);
    ImGui::BeginChild("Content", ImVec2(0, 0), false);
    ImGui::PushTextWrapPos(0);
    const ImVec4 blue(0.34f, 0.57f, 1, 1), muted(0.58f, 0.63f, 0.72f, 1);
    ImGui::TextColored(blue, "%s", sections[m_section]);
    ImGui::Dummy(ImVec2(0, 8 * scale));
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextWrapped("%s", m_section == 0 ? "Your Mac. On the big screen." : m_section == 1 ? "Make the connection yours." : "Everything, in view.");
    ImGui::SetWindowFontScale(1);
    ImGui::TextColored(muted, "%s", m_section == 0 ? "A direct connection. A focused workspace." : m_section == 1 ? "Changes take effect after a safe restart." : "Stream health and local diagnostics.");
    ImGui::Dummy(ImVec2(0, 22 * scale));
    auto restartWith = [&](const Config& cfg) {
        try { SaveSettings(cfg, SettingsPath()); m_restartRequested = true; m_window->RequestQuit(); }
        catch (const std::exception& error) { m_settingsError = error.what(); }
    };
    if (!StartupSettingsError().empty()) {
        ImGui::TextColored(ImVec4(1, 0.68f, 0.36f, 1), "Settings need attention");
        ImGui::TextWrapped("%s", StartupSettingsError().c_str());
        if (ImGui::Button("Reset to Defaults")) restartWith(Config{});
        ImGui::Separator();
    }
    if (m_section == 0) {
        ImGui::TextUnformatted("Mode");
        if (ImGui::RadioButton("Screen Share", m_crossSettings.mode == CrossMode::ScreenShare)) SwitchMode(CrossMode::ScreenShare);
        ImGui::SameLine();
        if (ImGui::RadioButton("Cross", m_crossSettings.mode == CrossMode::Cross)) SwitchMode(CrossMode::Cross);
        ImGui::Separator(); ImGui::Spacing();
        if (m_crossSettings.mode == CrossMode::Cross) {
            DrawCrossConnection(scale);
        } else {
        ImGui::BeginChild("Connection card", ImVec2(0, 228 * scale), true);
        const bool connected = m_networkManager && m_networkManager->IsConnected();
        ImGui::TextColored(blue, "%s", !m_networkEnabled ? "Disconnected" : m_streaming ? "Streaming" : connected ? "Preparing stream" : "Waiting for your Mac");
        ImGui::Text("Mac / %s", GetConfig().peerIP.c_str());
        ImGui::Spacing();
        if (ImGui::Button(m_networkEnabled ? "Disconnect" : "Connect", ImVec2(150 * scale, 36 * scale)) && StartupSettingsError().empty())
            m_toggleRequested = true;
        ImGui::Spacing();
        if (m_streaming) {
            auto info = m_networkManager->GetConnectionInfo();
            ImGui::Text("%u x %u / %u fps / %s / %u Mbps", info.width, info.height, info.fps,
                        info.codec == TLNK_CODEC_HEVC ? "HEVC" : "H.264", info.bitrateKbps / 1000);
            if (ImGui::Button("View Mac Display")) m_dashboard = false;
        }
        std::string error = m_desktopError;
        if (error.empty() && m_networkManager) error = m_networkManager->GetError();
        if (m_decoder && !m_decoder->GetError().empty()) error = m_decoder->GetError();
        if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
        ImGui::TextDisabled("Automatically reconnects when your Mac is available.");
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::TextUnformatted("Ready to connect");
        ImGui::BulletText("Connect the dedicated network adapters and cable.");
        ImGui::BulletText("Open ThunderLink on your Mac and grant Screen Recording and Accessibility access.");
        ImGui::BulletText("Set the PC adapter to 169.254.1.2 with subnet 255.255.255.0 for the default direct link.");
        ImGui::BulletText("Allow ThunderLink through Windows Firewall for the direct-link network profile.");
        if (ImGui::Button("Network Settings")) ShellExecuteW(m_window->GetHandle(), L"open", L"ms-settings:network-ethernet", nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::Button("Firewall Settings")) ShellExecuteW(m_window->GetHandle(), L"open", L"windowsdefender://network/", nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (m_section == 1) {
        ImGui::PushItemWidth(215 * scale);
        ImGui::InputText("Mac address", m_editPeer, sizeof(m_editPeer));
        ImGui::InputText("Local bind address", m_editBind, sizeof(m_editBind));
        ImGui::InputText("Video port", m_editVideo, sizeof(m_editVideo));
        ImGui::InputText("Input port", m_editInput, sizeof(m_editInput));
        ImGui::InputText("Control port", m_editControl, sizeof(m_editControl));
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        const char* codecs[] = {"H.264", "HEVC"};
        ImGui::Combo("Codec", &m_editCodec, codecs, 2);
        ImGui::InputText("Resolution (WxH)", m_editResolution, sizeof(m_editResolution));
        ImGui::TextDisabled("0x0: native / 0x1080: preserve aspect / up to 3840x2160");
        ImGui::InputText("Frame rate", m_editFPS, sizeof(m_editFPS));
        ImGui::InputText("Bitrate (Mbps)", m_editBitrate, sizeof(m_editBitrate));
        ImGui::Checkbox("VSync", &m_editVSync);
        ImGui::PopItemWidth(); ImGui::Spacing();
        if (ImGui::Button("Save and Restart", ImVec2(180 * scale, 38 * scale))) {
            try {
                std::vector<std::string> args = {"--bind", m_editBind, "--peer", m_editPeer,
                    "--video-port", m_editVideo, "--input-port", m_editInput, "--control-port", m_editControl,
                    "--codec", m_editCodec == 1 ? "hevc" : "h264", "--res", m_editResolution,
                    "--fps", m_editFPS, "--bitrate", m_editBitrate};
                if (m_editVSync) args.push_back("--vsync");
                restartWith(ParseArguments(args));
            } catch (const std::exception& error) { m_settingsError = error.what(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to Defaults")) restartWith(Config{});
    } else {
        ImGui::Text("Local / %s", GetConfig().bindIP.c_str());
        ImGui::Text("Peer / %s", GetConfig().peerIP.c_str());
        ImGui::Text("Ports / video %u / input %u / control %u", GetConfig().videoPort, GetConfig().inputPort, GetConfig().controlPort);
        ImGui::TextDisabled("Protocol v3 / bounded decode and frame queues");
        if (m_perfMonitor) {
            const auto stats = m_perfMonitor->GetStats();
            ImGui::Text("%.1f fps / %.1f Mbps", stats.fps, stats.bandwidthMbps);
        }
        if (m_decoder) ImGui::Text("Decoder / %s", m_decoder->GetBackendName().c_str());
        if (ImGui::Button("Open Logs")) { try { OpenLogDirectory(m_window->GetHandle()); } catch (const std::exception& error) { m_settingsError = error.what(); } }
        ImGui::SameLine();
        if (ImGui::Button("Stream Controls")) m_showStats = !m_showStats;
        ImGui::Spacing(); ImGui::Separator();
        ImGui::BeginChild("Recent logs", ImVec2(0, 260 * scale), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& entry : Logger::GetRecentLogs()) ImGui::TextUnformatted(entry.text.c_str());
        ImGui::EndChild();
    }
    if (!m_settingsError.empty()) ImGui::TextWrapped("%s", m_settingsError.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (m_showStats) DrawStatsPanel(m_networkManager && m_networkManager->IsConnected());
}

void Application::DrawCrossConnection(float scale) {
    const ImVec4 blue(0.34f, 0.57f, 1, 1), muted(0.58f, 0.63f, 0.72f, 1);
    if (!m_crossController) {
        ImGui::TextUnformatted("Cross is disconnected.");
        if (ImGui::Button("Connect Cross", ImVec2(150 * scale, 34 * scale))) { RefreshCrossDisplays(); StartCross(); }
        if (!m_crossError.empty()) ImGui::TextColored(ImVec4(1, .55f, .35f, 1), "%s", m_crossError.c_str());
        return;
    }
    const auto snapshot = m_crossController->Snapshot();
    ImGui::BeginChild("Cross connection card", ImVec2(0, 252 * scale), true);
    const bool ready = m_crossController->Ready();
    ImGui::TextColored(blue, "%s", !snapshot.connected ? "Looking for your Mac" : !snapshot.paired ? "Pairing required" : ready ? (m_crossController->Enabled() ? "Cross enabled" : "Cross ready") : "Arrange your displays");
    ImGui::Text("Mac / %s", snapshot.peerName.empty() ? "No paired Mac selected" : snapshot.peerName.c_str());
    if (!snapshot.interfaceName.empty()) ImGui::Text("Route / %s", snapshot.interfaceName.c_str());
    if (snapshot.latencyMs >= 0) ImGui::Text("Latency / %.1f ms", snapshot.latencyMs);
    ImGui::TextWrapped("%s", m_crossController->Status().c_str());
    if (snapshot.pairing) {
        ImGui::Separator(); ImGui::Text("Verify this code on both devices: %s", snapshot.verificationCode.c_str());
        if (ImGui::Button("Approve Pairing", ImVec2(150 * scale, 32 * scale))) m_crossController->ApprovePairing();
    }
    if (snapshot.paired && !snapshot.peerId.empty() && m_crossSettings.peerId != snapshot.peerId) {
        // A layout belongs to the certificate-pinned Mac identity. A legacy
        // unpinned draft is treated as a different peer rather than reused.
        const bool changedPeer = !m_crossSettings.peerId.empty() || !m_crossSettings.layout.empty();
        m_crossSettings.peerId = snapshot.peerId;
        if (changedPeer) { m_crossSettings.enabled = false; m_crossSettings.layout.clear(); m_crossSettings.remoteTopology.clear(); m_crossSettings.localTopology.clear(); RefreshCrossDisplays(); m_crossController->SetEnabled(false); }
        try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); } catch (const std::exception& error) { m_crossError = error.what(); }
    }
    if (ImGui::BeginCombo("Mac", snapshot.peerName.empty() ? "Choose a discovered Mac" : snapshot.peerName.c_str())) {
        for (const auto& peer : snapshot.peers) {
            const std::string label = peer.name.empty() ? peer.id : peer.name;
            if (ImGui::Selectable(label.c_str(), peer.id == m_crossSettings.peerId)) {
                if (peer.id != m_crossSettings.peerId) {
                    m_crossSettings.peerId = peer.id; m_crossSettings.enabled = false; m_crossSettings.layout.clear(); m_crossSettings.remoteTopology.clear(); m_crossSettings.localTopology.clear();
                    try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); m_restartRequested = true; m_window->RequestQuit(); }
                    catch (const std::exception& error) { m_crossError = error.what(); }
                }
            }
        }
        ImGui::EndCombo();
    }
    if (ready) {
        bool enabled = m_crossController->Enabled();
        if (ImGui::Button(enabled ? "Pause Cross" : "Enable Cross", ImVec2(150 * scale, 34 * scale))) {
            m_crossController->SetEnabled(!enabled); m_crossSettings.enabled = !enabled;
            try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); } catch (const std::exception& error) { m_crossError = error.what(); }
        }
        ImGui::SameLine(); ImGui::TextDisabled("Input: %s", m_crossController->RemoteOwnership() ? "Mac" : "Windows");
    }
    ImGui::SameLine();
    if (ImGui::Button("Disconnect")) {
        m_crossSettings.enabled = false;
        try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); } catch (const std::exception& error) { m_crossError = error.what(); }
        StopCross(); m_crossDisconnected = true;
        ImGui::EndChild(); return;
    }
    if (!snapshot.peerId.empty() && ImGui::Button("Forget Mac")) {
        m_crossController->ForgetDevice(snapshot.peerId); m_crossSettings.peerId.clear();
        try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); } catch (const std::exception& error) { m_crossError = error.what(); }
    }
    ImGui::EndChild();
    ImGui::Spacing();
    if (ImGui::Button(m_showCrossArrangement ? "Hide Arrange Displays" : "Arrange Displays", ImVec2(185 * scale, 36 * scale))) {
        m_showCrossArrangement = !m_showCrossArrangement;
        if (m_showCrossArrangement) RefreshCrossDisplays();
    }
    ImGui::SameLine();
    if (ImGui::Button("Identify Windows")) IdentifyCrossLocalDisplays(m_localCrossDisplays);
    ImGui::SameLine();
    if (ImGui::Button("Identify Mac")) m_crossController->IdentifyRemoteDisplays();
    ImGui::Spacing();
    ImGui::InputText("Manual Mac address or hostname", m_editCrossHost, sizeof(m_editCrossHost));
    ImGui::SameLine();
    if (ImGui::Button("Use address")) {
        m_crossSettings.manualHost = m_editCrossHost;
        try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); m_restartRequested = true; m_window->RequestQuit(); }
        catch (const std::exception& error) { m_crossError = error.what(); }
    }
    bool swap = m_crossSettings.swapCtrlCmd;
    if (ImGui::Checkbox("Use Windows-to-Mac modifier mapping", &swap)) {
        m_crossSettings.swapCtrlCmd = swap;
        try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); m_restartRequested = true; m_window->RequestQuit(); }
        catch (const std::exception& error) { m_crossError = error.what(); }
    }
    ImGui::TextDisabled("Enabled: Ctrl-to-Command, Windows-to-Control, Alt-to-Option. Disable for native Mac modifiers.");
    ImGui::InputText("Emergency return shortcut", m_editEmergencyShortcut, sizeof(m_editEmergencyShortcut));
    ImGui::SameLine();
    if (ImGui::Button("Apply shortcut")) {
        crossinput::EmergencyShortcut parsed{};
        if (!crossinput::ParseEmergencyShortcut(m_editEmergencyShortcut, parsed)) {
            m_crossError = "Use modifiers plus one key, for example Ctrl+Alt+Shift+F12.";
        } else {
            m_crossSettings.emergencyShortcut = m_editEmergencyShortcut;
            try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); m_restartRequested = true; m_window->RequestQuit(); }
            catch (const std::exception& error) { m_crossError = error.what(); }
        }
    }
    const char* currentInterface = m_crossSettings.interfaceId.empty() ? "Automatic (Thunderbolt, Ethernet, Wi-Fi)" : m_crossSettings.interfaceId.c_str();
    if (ImGui::BeginCombo("Network interface", currentInterface)) {
        if (ImGui::Selectable("Automatic (Thunderbolt, Ethernet, Wi-Fi)", m_crossSettings.interfaceId.empty())) {
            m_crossSettings.interfaceId.clear();
            try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); m_restartRequested = true; m_window->RequestQuit(); } catch (const std::exception& error) { m_crossError = error.what(); }
        }
        for (const auto& endpoint : snapshot.availableInterfaces) {
            if (endpoint.interfaceId.empty()) continue;
            const std::string label = endpoint.interfaceName.empty() ? endpoint.interfaceId : endpoint.interfaceName + " (" + endpoint.interfaceId + ")";
            if (ImGui::Selectable(label.c_str(), endpoint.interfaceId == m_crossSettings.interfaceId)) {
                m_crossSettings.interfaceId = endpoint.interfaceId;
                try { SaveCrossSettings(m_crossSettings, CrossSettingsPath()); m_restartRequested = true; m_window->RequestQuit(); } catch (const std::exception& error) { m_crossError = error.what(); }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextColored(muted, "Automatic route selection prefers Thunderbolt networking, Ethernet, then Wi-Fi. A manual address works when discovery is blocked.");
    if (!snapshot.error.empty()) ImGui::TextWrapped("%s", snapshot.error.c_str());
    if (!m_crossError.empty()) ImGui::TextColored(ImVec4(1, .55f, .35f, 1), "%s", m_crossError.c_str());
    if (m_showCrossArrangement) DrawCrossArrangement(scale);
}

void Application::DrawCrossArrangement(float scale) {
    if (!m_crossController) return;
    auto remote = m_crossController->RemoteDisplays();
    // Add a new Mac display beside the right-most Windows display. This is a
    // starting point only; it never changes Windows Display Settings.
    double right = 0, top = 0;
    for (const auto& display : m_crossDraftLayout) {
        right = std::max(right, display.bounds.x + display.bounds.width);
        top = std::min(top, display.bounds.y);
    }
    for (const auto& mac : remote) {
        const bool found = std::any_of(m_crossDraftLayout.begin(), m_crossDraftLayout.end(), [&](const auto& display) { return display.id == mac.id; });
        if (!found) { m_crossDraftLayout.push_back({mac.id, mac.name, {right, top, mac.width, mac.height}, true}); right += mac.width; }
    }
    const auto missing = [&](const cross::Display& display) {
        return display.remote && !std::any_of(remote.begin(), remote.end(), [&](const auto& mac) { return mac.id == display.id; });
    };

    ImGui::Separator(); ImGui::TextUnformatted("Arrange Displays");
    ImGui::TextDisabled("Drag Mac tiles until their edges touch Windows or another Mac tile. Windows tiles show the actual desktop arrangement.");
    ImGui::SliderFloat("Canvas scale", &m_crossZoom, 0.04f, 0.30f, "%.2f");
    ImGui::SameLine(); if (ImGui::Button("Reset view")) { m_crossPanX = m_crossPanY = 0; m_crossZoom = .16f; }
    const ImVec2 canvasSize(0, 360 * scale);
    ImGui::BeginChild("Cross canvas", canvasSize, true, ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) { m_crossPanX += ImGui::GetIO().MouseDelta.x; m_crossPanY += ImGui::GetIO().MouseDelta.y; }
    ImDrawList* draw = ImGui::GetWindowDrawList(); const ImVec2 rawOrigin = ImGui::GetCursorScreenPos();
    const ImVec2 origin(rawOrigin.x + m_crossPanX, rawOrigin.y + m_crossPanY);
    constexpr float padding = 26.0f;
    int selected = -1;
    static std::string selectedId;
    double minX = 0, minY = 0;
    for (const auto& display : m_crossDraftLayout) { minX = std::min(minX, display.bounds.x); minY = std::min(minY, display.bounds.y); }
    auto snapTile = [&](cross::Display& tile) {
        const double tolerance = 14.0 / std::max(0.01f, m_crossZoom);
        for (const auto& other : m_crossDraftLayout) {
            if (other.id == tile.id) continue;
            if (std::abs(tile.bounds.x - (other.bounds.x + other.bounds.width)) <= tolerance) tile.bounds.x = other.bounds.x + other.bounds.width;
            if (std::abs(tile.bounds.x + tile.bounds.width - other.bounds.x) <= tolerance) tile.bounds.x = other.bounds.x - tile.bounds.width;
            if (std::abs(tile.bounds.y - (other.bounds.y + other.bounds.height)) <= tolerance) tile.bounds.y = other.bounds.y + other.bounds.height;
            if (std::abs(tile.bounds.y + tile.bounds.height - other.bounds.y) <= tolerance) tile.bounds.y = other.bounds.y - tile.bounds.height;
        }
    };
    for (size_t i = 0; i < m_crossDraftLayout.size(); ++i) {
        auto& display = m_crossDraftLayout[i];
        const ImVec2 minimum(origin.x + padding + float((display.bounds.x - minX) * m_crossZoom), origin.y + padding + float((display.bounds.y - minY) * m_crossZoom));
        const ImVec2 maximum(minimum.x + std::max(24.f, float(display.bounds.width * m_crossZoom)), minimum.y + std::max(24.f, float(display.bounds.height * m_crossZoom)));
        const ImU32 fill = missing(display) ? IM_COL32(230, 128, 74, 70) : display.remote ? IM_COL32(87, 145, 255, 72) : IM_COL32(94, 108, 134, 100);
        const bool active = selectedId == display.id;
        draw->AddRectFilled(minimum, maximum, fill, 5); draw->AddRect(minimum, maximum, active ? IM_COL32(232,237,247,255) : IM_COL32(130,160,210,255), 5, 0, active ? 2.f : 1.f);
        const std::string label = missing(display) ? "Missing Mac display: " + display.name : display.name;
        draw->AddText(ImVec2(minimum.x + 7, minimum.y + 7), IM_COL32(232,237,247,255), label.c_str());
        ImGui::SetCursorScreenPos(minimum); ImGui::PushID(display.id.c_str());
        ImGui::InvisibleButton("display tile", ImVec2(maximum.x - minimum.x, maximum.y - minimum.y));
        if (ImGui::IsItemClicked()) { selectedId = display.id; selected = int(i); }
        if (display.remote && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 delta = ImGui::GetIO().MouseDelta; display.bounds.x += delta.x / m_crossZoom; display.bounds.y += delta.y / m_crossZoom; selected = int(i);
        }
        // InvisibleButton does not mark its manual rectangle edits as an ImGui
        // value edit, so deactivation (mouse release) is the reliable drag end.
        if (display.remote && ImGui::IsItemDeactivated()) snapTile(display);
        ImGui::PopID();
    }
    for (size_t left = 0; left < m_crossDraftLayout.size(); ++left) for (size_t right = left + 1; right < m_crossDraftLayout.size(); ++right) {
        const auto& a = m_crossDraftLayout[left].bounds; const auto& b = m_crossDraftLayout[right].bounds;
        if (!cross::SharesEdge(a, b)) continue;
        const ImVec2 a0(origin.x + padding + float((a.x - minX) * m_crossZoom), origin.y + padding + float((a.y - minY) * m_crossZoom));
        if (cross::Near(a.x + a.width, b.x) || cross::Near(b.x + b.width, a.x)) {
            const float x = cross::Near(a.x + a.width, b.x) ? a0.x + float(a.width * m_crossZoom) : a0.x;
            const double from = std::max(a.y, b.y), to = std::min(a.y + a.height, b.y + b.height);
            draw->AddLine(ImVec2(x, origin.y + padding + float((from - minY) * m_crossZoom)), ImVec2(x, origin.y + padding + float((to - minY) * m_crossZoom)), IM_COL32(92, 220, 156, 255), 3);
        } else {
            const float y = cross::Near(a.y + a.height, b.y) ? a0.y + float(a.height * m_crossZoom) : a0.y;
            const double from = std::max(a.x, b.x), to = std::min(a.x + a.width, b.x + b.width);
            draw->AddLine(ImVec2(origin.x + padding + float((from - minX) * m_crossZoom), y), ImVec2(origin.x + padding + float((to - minX) * m_crossZoom), y), IM_COL32(92, 220, 156, 255), 3);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 330 * scale));
    ImGui::EndChild();
    for (size_t i = 0; i < m_crossDraftLayout.size(); ++i) if (m_crossDraftLayout[i].id == selectedId) selected = int(i);
    if (selected >= 0 && m_crossDraftLayout[size_t(selected)].remote) {
        auto& tile = m_crossDraftLayout[size_t(selected)];
        ImGui::Text("Selected Mac display: %s", tile.name.c_str()); ImGui::SameLine();
        if (ImGui::Button("Left")) tile.bounds.x -= 20; ImGui::SameLine(); if (ImGui::Button("Right")) tile.bounds.x += 20;
        ImGui::SameLine(); if (ImGui::Button("Up")) tile.bounds.y -= 20; ImGui::SameLine(); if (ImGui::Button("Down")) tile.bounds.y += 20;
        ImGui::SameLine(); if (ImGui::Button("Smaller")) { tile.bounds.width *= .9; tile.bounds.height *= .9; }
        ImGui::SameLine(); if (ImGui::Button("Larger")) { tile.bounds.width /= .9; tile.bounds.height /= .9; }
        ImGui::SameLine(); if (ImGui::Button("Snap")) snapTile(tile);
        ImGui::SetNextItemWidth(110 * scale); ImGui::InputDouble("X", &tile.bounds.x, 10, 100, "%.0f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(110 * scale); ImGui::InputDouble("Y", &tile.bounds.y, 10, 100, "%.0f");
    }
    if (ImGui::Button("Reset Mac tiles")) {
        double x = 0; for (const auto& display : m_crossDraftLayout) if (!display.remote) x = std::max(x, display.bounds.x + display.bounds.width);
        for (auto& display : m_crossDraftLayout) if (display.remote) { display.bounds.x = x; display.bounds.y = 0; x += display.bounds.width; }
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove missing Mac displays")) {
        m_crossDraftLayout.erase(std::remove_if(m_crossDraftLayout.begin(), m_crossDraftLayout.end(), missing), m_crossDraftLayout.end());
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply Arrangement", ImVec2(170 * scale, 34 * scale))) {
        const auto validation = cross::ValidateLayout(m_crossDraftLayout);
        if (!validation.empty()) m_crossError = validation;
        else {
            if (!m_crossController->SetLayout(m_crossDraftLayout)) m_crossError = m_crossController->Status();
            else try {
                m_crossSettings.layout = m_crossDraftLayout;
                m_crossSettings.remoteTopology = m_crossController->RemoteTopology();
                m_crossSettings.localTopology = LocalTopology(m_localCrossDisplays);
                SaveCrossSettings(m_crossSettings, CrossSettingsPath());
                m_crossController->RememberAppliedTopology(m_crossSettings.remoteTopology, m_crossSettings.localTopology);
                m_crossError.clear();
            } catch (const std::exception& error) { m_crossError = error.what(); }
        }
    }
}

void Application::DrawUI() {
    if (!m_streaming || m_dashboard) {
        DrawDesktop();
        if (m_inputCapture) m_inputCapture->UpdateInputPolicy();
        return;
    }

    /* F1 hides/shows the entire overlay (toolbar + panels) for an unobstructed
     * view. NewFrame always runs, so the key state stays current while hidden. */
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) m_uiVisible = !m_uiVisible;
    if (!m_uiVisible) { if (m_inputCapture) m_inputCapture->UpdateInputPolicy(); return; }

    const bool connected = m_networkManager && m_networkManager->IsConnected();

    DrawToolbar(connected);
    if (m_showStats) DrawStatsPanel(connected);
    if (m_showLogs)  DrawLogsPanel();
    if (m_inputCapture) m_inputCapture->UpdateInputPolicy();
}

void Application::DrawToolbar(bool connected) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##toolbar", nullptr, flags)) {
        m_toolbarHeight = ImGui::GetWindowSize().y;

        /* Status dot + title on the left. */
        const ImVec4 dot = connected ? ImVec4(0.30f, 0.85f, 0.42f, 1.0f)
                                     : ImVec4(0.95f, 0.75f, 0.25f, 1.0f);
        ImGui::AlignTextToFramePadding();
        const float radius = ImGui::GetFontSize() * 0.32f;
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        const ImVec2 center(cur.x + radius, cur.y + ImGui::GetTextLineHeight() * 0.5f);
        ImGui::GetWindowDrawList()->AddCircleFilled(
            center, radius, ImGui::ColorConvertFloat4ToU32(dot), 20);
        ImGui::Dummy(ImVec2(radius * 2.0f + 8.0f, 0.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted("ThunderLink");
        ImGui::SameLine();
        ImGui::TextColored(dot, "%s", connected ? "Connected" : "Waiting for Mac");        std::string error = m_networkManager ? m_networkManager->GetError() : "";
        if (m_decoder && !m_decoder->GetError().empty()) error = m_decoder->GetError();
        if (!error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", error.c_str());
        }


        /* Right-aligned button cluster: Show Logs / Show Stats Overlay / Close. */
        const ImGuiStyle& st = ImGui::GetStyle();
        auto btnW = [&](const char* label) {
            return ImGui::CalcTextSize(label).x + st.FramePadding.x * 2.0f;
        };
        const char* kLogs  = "Dashboard";
        const char* kStats = "Show Stats Overlay";
        const char* kClose = "Close";
        const float cluster = btnW(kLogs) + btnW(kStats) + btnW(kClose) +
                              st.ItemSpacing.x * 2.0f;

        ImGui::SameLine();
        const float rightX = ImGui::GetWindowContentRegionMax().x - cluster;
        if (rightX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightX);

        auto toggleBtn = [&](const char* label, bool active) {
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            const bool clicked = ImGui::Button(label);
            if (active) ImGui::PopStyleColor();
            return clicked;
        };

        if (toggleBtn(kLogs, m_dashboard)) { m_dashboard = true; m_section = 0; if (m_inputCapture) { m_inputCapture->SetEnabled(false); m_inputCapture->SetCursorConfined(false); } }
        ImGui::SameLine();
        if (toggleBtn(kStats, m_showStats)) m_showStats = !m_showStats;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.26f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.68f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button(kClose) && m_window) {
            PostMessageW(m_window->GetHandle(), WM_CLOSE, 0, 0);
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void Application::DrawStatsPanel(bool connected) {
    const auto& cfg = GetConfig();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.0f,
                                   vp->WorkPos.y + m_toolbarHeight + 10.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("Stats", &m_showStats,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {

        ImGui::TextColored(connected ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                     : ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                           "%s", connected ? "Connected" : "Waiting for Mac...");
        ImGui::Text("Peer: %s   Bind: %s", cfg.peerIP.c_str(), cfg.bindIP.c_str());

        if (connected) {
            ConnectionInfo info = m_networkManager->GetConnectionInfo();
            /* Show the actual decoded resolution (reflects runtime res changes). */
            if (m_decoderW && m_decoderH) {
                ImGui::Text("Stream: %ux%u @ %u fps", m_decoderW, m_decoderH, info.fps);
            } else {
                ImGui::Text("Stream: %ux%u @ %u fps", info.width, info.height, info.fps);
            }
            ImGui::Text("Codec: %s   %u kbps",
                        (info.codec == TLNK_CODEC_HEVC ? "HEVC" : "H.264"), info.bitrateKbps);
            ImGui::Text("Peer latency: %.1f ms", m_networkManager->GetPeerLatency());
            if (m_decoder) ImGui::Text("Decoder: %s", m_decoder->GetBackendName().c_str());

            /* Resolution picker: tells the Mac to recapture at the chosen size.
             * "Source (native)" (0x0) makes the Mac stream its native display res. */
            // Pick a target HEIGHT; the Mac derives the width from its own
            // display aspect so the stream matches the Mac (no distortion, no
            // SCK letterbox). width=0 means "derive from aspect"; (0,0) = native.
            static const char* kResLabels[] = {
                "Source (native)", "1440p", "1080p", "720p", "480p"
            };
            static const uint32_t kResHeights[] = { 0, 1440, 1080, 720, 480 };
            static int resIndex = 0; /* default Source (native) */
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("Resolution", &resIndex, kResLabels, IM_ARRAYSIZE(kResLabels))) {
                const auto& info2 = m_networkManager->GetConnectionInfo();
                uint32_t fps = info2.fps ? info2.fps : GetConfig().fps;
                m_networkManager->RequestConfigure(0, kResHeights[resIndex], fps);
                LOG_INFO("Application: requested resolution %s", kResLabels[resIndex]);
            }
        }

        if (m_perfMonitor) {
            PerformanceStats s = m_perfMonitor->GetStats();
            ImGui::Separator();
            ImGui::Text("FPS: %.1f", s.fps);
            ImGui::Text("Bandwidth: %.1f Mbps", s.bandwidthMbps);
            ImGui::Text("Reassembly: %.0f us", s.reassemblyTimeUs);
            ImGui::Text("Rendered: %llu   Dropped: %llu",
                        (unsigned long long)s.framesRendered,
                        (unsigned long long)s.framesDropped);
        }

        ImGui::Separator();
        if (m_renderer) {
            bool vsync = m_renderer->GetVSync();
            if (ImGui::Checkbox("VSync (fix tearing / flicker)", &vsync)) {
                m_renderer->SetVSync(vsync);
                LOG_INFO("Application: vsync %s", vsync ? "on" : "off");
            }
        }
        if (ImGui::Button("Request keyframe") && m_networkManager) {
            m_networkManager->RequestKeyframe();
        }
        ImGui::TextDisabled("F1 hide overlay  -  F11 fullscreen  -  ESC release cursor");
    }
    ImGui::End();
}

void Application::DrawLogsPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float panelW = 720.0f;
    ImGui::SetNextWindowSize(ImVec2(panelW, 360.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - (panelW + 12.0f),
                                   vp->WorkPos.y + m_toolbarHeight + 10.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("Logs", &m_showLogs)) {
        std::vector<LogEntry> logs = Logger::GetRecentLogs();

        ImGui::Checkbox("Auto-scroll", &m_logAutoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            std::string all;
            for (const auto& e : logs) { all += e.text; all += '\n'; }
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d lines", (int)logs.size());
        ImGui::Separator();

        ImGui::BeginChild("##logscroll", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& e : logs) {
            ImGui::PushStyleColor(ImGuiCol_Text, LogLevelColor(e.level));
            ImGui::TextUnformatted(e.text.c_str());
            ImGui::PopStyleColor();
        }
        if (m_logAutoScroll) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace thunderlink
