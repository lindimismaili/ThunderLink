#include "Window.h"
#include "Input/InputCapture.h"
#include "Utils/Logger.h"

#include <cmath>

#include "imgui.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace thunderlink {

Window::Window() {
}

Window::~Window() {
    Destroy();
}

bool Window::Create(HINSTANCE hInstance, int nCmdShow, uint32_t width, uint32_t height) {
    m_hInstance = hInstance;
    m_width = width;
    m_height = height;

    /* Enable per-monitor DPI awareness */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* Register window class */
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("Window: RegisterClassExW failed (err=%lu)", GetLastError());
        return false;
    }

    /* Calculate window size to get the desired client area */
    RECT rect = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    int windowWidth = rect.right - rect.left;
    int windowHeight = rect.bottom - rect.top;

    /* Center on screen */
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth - windowWidth) / 2;
    int posY = (screenHeight - windowHeight) / 2;

    m_hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"ThunderLink Receiver",
        WS_OVERLAPPEDWINDOW,
        posX, posY,
        windowWidth, windowHeight,
        nullptr, nullptr,
        hInstance, this  /* Pass 'this' for WM_CREATE */
    );

    if (!m_hwnd) {
        LOG_ERROR("Window: CreateWindowExW failed (err=%lu)", GetLastError());
        return false;
    }

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    LOG_INFO("Window: created %ux%u", width, height);
    return true;
}

void Window::Destroy() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    if (m_hInstance) {
        UnregisterClassW(CLASS_NAME, m_hInstance);
        m_hInstance = nullptr;
    }
}

void Window::SetAspectRatio(float aspect) {
    if (aspect <= 0.0f) { m_aspectRatio = 0.0f; return; }
    if (std::fabs(aspect - m_aspectRatio) < 0.001f) return;  // unchanged
    m_aspectRatio = aspect;
    if (!m_hwnd) return;

    /* Only snap in windowed mode (don't fight F11 borderless fullscreen). */
    if ((GetWindowLongW(m_hwnd, GWL_STYLE) & WS_CAPTION) == 0) return;

    RECT cr;
    if (!GetClientRect(m_hwnd, &cr)) return;
    int clientW = cr.right - cr.left;
    if (clientW < 16) clientW = 16;
    int clientH = (int)(clientW / aspect + 0.5f);

    RECT wr = {0, 0, clientW, clientH};
    AdjustWindowRectEx(&wr, (DWORD)GetWindowLongW(m_hwnd, GWL_STYLE), FALSE, 0);
    SetWindowPos(m_hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    LOG_DEBUG("Window: aspect locked to %.3f", aspect);
}

bool Window::ProcessMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

LRESULT CALLBACK Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Window* self = nullptr;

    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->WndProc(msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Window::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    /* Let the ImGui overlay see input first (once it has been initialized). */
    if (ImGui::GetCurrentContext() != nullptr) {
        if (ImGui_ImplWin32_WndProcHandler(m_hwnd, msg, wParam, lParam)) {
            return true;
        }
    }

    switch (msg) {
        case WM_INPUT:
            if (m_inputCapture) {
                m_inputCapture->ProcessRawInput(lParam);
            }
            return DefWindowProcW(m_hwnd, msg, wParam, lParam);

        case WM_SIZING: {
            /* Constrain interactive resizing to the locked aspect ratio so the
             * video fills the client area with no black bars. */
            if (m_aspectRatio > 0.0f) {
                RECT* rect = reinterpret_cast<RECT*>(lParam);
                RECT cr, wr;
                GetClientRect(m_hwnd, &cr);
                GetWindowRect(m_hwnd, &wr);
                const int frameW = (wr.right - wr.left) - (cr.right - cr.left);
                const int frameH = (wr.bottom - wr.top) - (cr.bottom - cr.top);

                int clientW = (rect->right - rect->left) - frameW;
                int clientH = (rect->bottom - rect->top) - frameH;
                if (clientW < 16) clientW = 16;
                if (clientH < 16) clientH = 16;

                switch (wParam) {
                    case WMSZ_LEFT: case WMSZ_RIGHT:
                        clientH = (int)(clientW / m_aspectRatio + 0.5f);
                        rect->bottom = rect->top + clientH + frameH;
                        break;
                    case WMSZ_TOP: case WMSZ_BOTTOM:
                        clientW = (int)(clientH * m_aspectRatio + 0.5f);
                        rect->right = rect->left + clientW + frameW;
                        break;
                    case WMSZ_TOPLEFT: case WMSZ_TOPRIGHT:
                        clientH = (int)(clientW / m_aspectRatio + 0.5f);
                        rect->top = rect->bottom - (clientH + frameH);
                        break;
                    default: /* bottom corners */
                        clientH = (int)(clientW / m_aspectRatio + 0.5f);
                        rect->bottom = rect->top + clientH + frameH;
                        break;
                }
            }
            return TRUE;
        }

        case WM_SIZE: {
            uint32_t newWidth = LOWORD(lParam);
            uint32_t newHeight = HIWORD(lParam);
            if (newWidth > 0 && newHeight > 0 &&
                (newWidth != m_width || newHeight != m_height)) {
                m_width = newWidth;
                m_height = newHeight;
                LOG_DEBUG("Window: resized to %ux%u", m_width, m_height);

                if (m_inputCapture) {
                    m_inputCapture->SetWindowSize(m_width, m_height);
                }
                if (m_resizeCallback) {
                    m_resizeCallback(m_width, m_height);
                }
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (m_inputCapture) {
                    m_inputCapture->ToggleCursorConfinement();
                }
                return 0;
            }
            if (wParam == VK_F11) {
                /* Toggle borderless fullscreen */
                static bool fullscreen = false;
                static RECT savedRect = {};
                static LONG savedStyle = 0;

                if (!fullscreen) {
                    GetWindowRect(m_hwnd, &savedRect);
                    savedStyle = GetWindowLongW(m_hwnd, GWL_STYLE);

                    SetWindowLongW(m_hwnd, GWL_STYLE, savedStyle & ~WS_OVERLAPPEDWINDOW);

                    HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO info = {sizeof(info)};
                    GetMonitorInfoW(monitor, &info);

                    SetWindowPos(m_hwnd, HWND_TOP,
                                 info.rcMonitor.left, info.rcMonitor.top,
                                 info.rcMonitor.right - info.rcMonitor.left,
                                 info.rcMonitor.bottom - info.rcMonitor.top,
                                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
                    fullscreen = true;
                } else {
                    SetWindowLongW(m_hwnd, GWL_STYLE, savedStyle);
                    SetWindowPos(m_hwnd, HWND_NOTOPMOST,
                                 savedRect.left, savedRect.top,
                                 savedRect.right - savedRect.left,
                                 savedRect.bottom - savedRect.top,
                                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
                    fullscreen = false;
                }
                return 0;
            }
            break;

        case WM_SETFOCUS:
            if (m_inputCapture) {
                m_inputCapture->OnFocusGained();
            }
            return 0;

        case WM_KILLFOCUS:
            if (m_inputCapture) {
                m_inputCapture->OnFocusLost();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

} // namespace thunderlink
