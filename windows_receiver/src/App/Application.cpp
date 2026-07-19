#include "Application.h"

#include "App/Window.h"
#include "App/Config.h"
#include "Renderer/D3D12Renderer.h"
#include "Utils/FrameBuffer.h"
#include "Utils/PerformanceMonitor.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include "Network/FrameReceiver.h"
#include "Network/InputSender.h"
#include "Input/InputCapture.h"
#include "Decoder/VideoDecoder.h"

#include "imgui.h"

#include "protocol.h"

#include <string>
#include <vector>

namespace thunderlink {

Application::Application(HINSTANCE hInstance)
    : m_hInstance(hInstance)
{
}

Application::~Application() {
}

bool Application::Initialize(int nCmdShow) {
    const auto& cfg = GetConfig();

    /* Create the window at the (smaller) window size; the 1440p stream is
     * downscaled to fit. */
    m_window = std::make_unique<Window>();
    if (!m_window->Create(m_hInstance, nCmdShow, cfg.windowWidth, cfg.windowHeight)) {
        LOG_ERROR("Application: window creation failed");
        return false;
    }

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

    /* Create performance monitor */
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

    if (!m_networkManager->Start()) {
        LOG_ERROR("Application: network manager start failed");
        return false;
    }

    /* Start frame receiver (UDP listener) */
    m_frameReceiver = std::make_unique<FrameReceiver>(m_frameBuffer.get(), m_perfMonitor.get());
    if (!m_frameReceiver->Start()) {
        LOG_ERROR("Application: frame receiver start failed");
        return false;
    }

    /* Start input sender (TCP to Mac) */
    m_inputSender = std::make_unique<InputSender>();
    if (!m_inputSender->Start()) {
        LOG_ERROR("Application: input sender start failed");
        return false;
    }

    /* Initialize input capture */
    m_inputCapture = std::make_unique<InputCapture>();
    if (!m_inputCapture->Initialize(m_window->GetHandle(),
            [this](const InputEvent& event) {
                m_inputSender->SendEvent(event);
            })) {
        LOG_ERROR("Application: input capture initialization failed");
        return false;
    }

    /* Wire input capture into window message loop */
    m_window->SetInputCapture(m_inputCapture.get());

    LOG_INFO("Application: all subsystems initialized, waiting for Mac connection...");
    return true;
}

int Application::Run() {
    LOG_INFO("Application: entering main loop");

    while (true) {
        /* Process Win32 messages */
        if (!m_window->ProcessMessages()) {
            break; /* WM_QUIT received */
        }

        /* Capture connection/codec changes from the control thread. */
        MaybeInitDecoder();

        /* Try to get a completed (reassembled) encoded frame */
        ReceivedFrame rf{};
        if (m_frameBuffer->WaitForFrame(&rf, 1)) {
            /* Create or rebuild the decoder for the frame's actual resolution.
             * Driving this off the real frame dimensions handles the initial
             * connection, "native", and runtime resolution changes uniformly. */
            if (rf.width != 0 && rf.height != 0 &&
                (!m_decoder || rf.width != m_decoderW || rf.height != m_decoderH)) {
                LOG_INFO("Application: (re)building decoder for %ux%u (was %ux%u)",
                         rf.width, rf.height, m_decoderW, m_decoderH);
                m_decoder = CreateMediaFoundationDecoder();
                if (m_decoder && m_decoder->Initialize(m_curCodec, rf.width, rf.height)) {
                    m_decoderW = rf.width;
                    m_decoderH = rf.height;
                    m_lastFrameId = 0;
                    /* Keep cursor mapping aligned to the new video rectangle. */
                    if (m_inputCapture) m_inputCapture->SetStreamSize(rf.width, rf.height);
                    /* Lock the window to the stream's aspect so there are no bars. */
                    if (m_window) m_window->SetAspectRatio(
                        static_cast<float>(rf.width) / static_cast<float>(rf.height));
                    m_networkManager->RequestKeyframe();
                } else {
                    m_decoder.reset();
                }
            }

            if (m_decoder) {
                /* Frame-id gap (loss) -> drop to keyframe to avoid corruption */
                if (m_lastFrameId != 0 && rf.frameId != m_lastFrameId + 1) {
                    m_decoder->RequireKeyframe();
                    m_networkManager->RequestKeyframe();
                }
                m_lastFrameId = rf.frameId;

                DecodedFrame df{};
                bool decoded = m_decoder->Decode(
                    rf.data, rf.size, (rf.flags & TLNK_FLAG_KEY_FRAME) != 0, &df);
                m_frameBuffer->ReleaseFrame(rf.frameId);

                if (decoded) {
                    m_renderer->RenderFrame(df.yPlane, df.uvPlane, df.width, df.height,
                                            df.yStride, df.uvStride);
                    m_perfMonitor->FrameRendered();
                } else {
                    if (m_decoder->IsWaitingForKeyframe()) m_networkManager->RequestKeyframe();
                    m_renderer->RenderLastFrame();
                }
            } else {
                m_frameBuffer->ReleaseFrame(rf.frameId);
                m_renderer->RenderLastFrame();
            }
        } else {
            /* No new frame - re-render the last frame to keep the window responsive */
            m_renderer->RenderLastFrame();
        }

        /* Periodic stats */
        m_perfMonitor->MaybeLogStats();
    }

    LOG_INFO("Application: main loop exited");
    return 0;
}

void Application::Shutdown() {
    LOG_INFO("Application: shutting down");

    /* Stop subsystems in reverse order */
    if (m_inputCapture) {
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

    m_inputCapture.reset();
    m_inputSender.reset();
    m_frameReceiver.reset();
    m_networkManager.reset();
    if (m_decoder) { m_decoder->Shutdown(); m_decoder.reset(); }
    m_frameBuffer.reset();
    m_perfMonitor.reset();
    m_renderer.reset();
    m_window.reset();

    LOG_INFO("Application: shutdown complete");
}

void Application::OnConnected(const ConnectionInfo& info) {
    LOG_INFO("Application: Mac connected, streaming %ux%u @ %u fps, codec=%s",
             info.width, info.height, info.fps,
             (info.codec == TLNK_CODEC_HEVC ? "HEVC" : "H264"));
    /* Hand the parameters to the render thread, which owns the decoder. */
    std::lock_guard<std::mutex> lock(m_decoderMutex);
    m_pendingInfo = info;
    m_pendingDecoderInit = true;
    m_streaming = true;
}

void Application::OnDisconnected() {
    LOG_INFO("Application: Mac disconnected");
    m_streaming = false;
}

void Application::MaybeInitDecoder() {
    ConnectionInfo info;
    {
        std::lock_guard<std::mutex> lock(m_decoderMutex);
        if (!m_pendingDecoderInit) return;
        m_pendingDecoderInit = false;
        info = m_pendingInfo;
    }

    /* A new connection was negotiated. Record the codec and drop any existing
     * decoder; the render loop lazily (re)creates it from the first frame's real
     * dimensions, which also covers "native" and runtime resolution changes. */
    m_curCodec = info.codec;
    m_decoder.reset();
    m_decoderW = 0;
    m_decoderH = 0;
    m_lastFrameId = 0;
    if (m_networkManager) m_networkManager->RequestKeyframe();
    LOG_INFO("Application: connected (codec=%s); decoder will init on first frame",
             (info.codec == TLNK_CODEC_HEVC ? "HEVC" : "H264"));
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

void Application::DrawUI() {
    /* F1 hides/shows the entire overlay (toolbar + panels) for an unobstructed
     * view. NewFrame always runs, so the key state stays current while hidden. */
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) m_uiVisible = !m_uiVisible;
    if (!m_uiVisible) return;

    const bool connected = m_networkManager && m_networkManager->IsConnected();

    DrawToolbar(connected);
    if (m_showStats) DrawStatsPanel(connected);
    if (m_showLogs)  DrawLogsPanel();
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
        ImGui::TextColored(dot, "%s", connected ? "Connected" : "Waiting for Mac");

        /* Right-aligned button cluster: Show Logs / Show Stats Overlay / Close. */
        const ImGuiStyle& st = ImGui::GetStyle();
        auto btnW = [&](const char* label) {
            return ImGui::CalcTextSize(label).x + st.FramePadding.x * 2.0f;
        };
        const char* kLogs  = "Show Logs";
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

        if (toggleBtn(kLogs, m_showLogs))   m_showLogs  = !m_showLogs;
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
