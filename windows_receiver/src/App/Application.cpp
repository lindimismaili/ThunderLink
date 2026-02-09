#include "Application.h"

#include "App/Window.h"
#include "Renderer/D3D12Renderer.h"
#include "Utils/FrameBuffer.h"
#include "Utils/PerformanceMonitor.h"
#include "Utils/Logger.h"
#include "Network/NetworkManager.h"
#include "Network/FrameReceiver.h"
#include "Network/InputSender.h"
#include "Input/InputCapture.h"

#include "protocol.h"

namespace thunderlink {

Application::Application(HINSTANCE hInstance)
    : m_hInstance(hInstance)
{
}

Application::~Application() {
}

bool Application::Initialize(int nCmdShow) {
    /* Create window */
    m_window = std::make_unique<Window>();
    if (!m_window->Create(m_hInstance, nCmdShow, 1920, 1080)) {
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

    /* Create performance monitor */
    m_perfMonitor = std::make_unique<PerformanceMonitor>();

    /* Create frame buffer with pre-allocated pool */
    m_frameBuffer = std::make_unique<FrameBuffer>(
        TLNK_FRAME_POOL_SIZE, TLNK_MAX_WIDTH, TLNK_MAX_HEIGHT, m_perfMonitor.get());

    /* Start network manager (control channel) */
    m_networkManager = std::make_unique<NetworkManager>();
    m_networkManager->SetOnConnected([this](const ConnectionInfo& info) {
        OnConnected(info.width, info.height, info.fps);
    });
    m_networkManager->SetOnDisconnected([this]() {
        OnDisconnected();
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

        /* Try to get a completed frame */
        const uint8_t* frameData = nullptr;
        uint32_t width = 0, height = 0;
        uint64_t frameId = 0;

        if (m_frameBuffer->WaitForFrame(&frameData, &width, &height, &frameId, 1)) {
            /* Render the new frame */
            m_renderer->RenderFrame(frameData, width, height);
            m_frameBuffer->ReleaseFrame(frameId);
            m_perfMonitor->FrameRendered();
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
    m_frameBuffer.reset();
    m_perfMonitor.reset();
    m_renderer.reset();
    m_window.reset();

    LOG_INFO("Application: shutdown complete");
}

void Application::OnConnected(uint32_t width, uint32_t height, uint32_t fps) {
    LOG_INFO("Application: Mac connected, streaming %ux%u @ %u fps", width, height, fps);
    m_streaming = true;
}

void Application::OnDisconnected() {
    LOG_INFO("Application: Mac disconnected");
    m_streaming = false;
}

void Application::OnResize(uint32_t width, uint32_t height) {
    if (m_renderer) {
        m_renderer->Resize(width, height);
    }
}

} // namespace thunderlink
