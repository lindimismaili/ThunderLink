#include "Window.h"
#include "Input/InputCapture.h"
#include "Utils/Logger.h"

#include <cmath>
#include <shellapi.h>
#include <wtsapi32.h>

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
    const UINT initialDpi = GetDpiForSystem();
    const auto minimumWidth = static_cast<uint32_t>(MulDiv(800, initialDpi ? initialDpi : 96, 96));
    const auto minimumHeight = static_cast<uint32_t>(MulDiv(560, initialDpi ? initialDpi : 96, 96));
    if (width < minimumWidth) width = minimumWidth;
    if (height < minimumHeight) height = minimumHeight;
    m_width = width; m_height = height;


    /* Register window class */
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
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
        L"ThunderLink",
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

    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WTSRegisterSessionNotification(m_hwnd, NOTIFY_FOR_THIS_SESSION);
    AddTrayIcon();
    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    LOG_INFO("Window: created %ux%u", width, height);
    return true;
}

void Window::AddTrayIcon() {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon); icon.hWnd = m_hwnd; icon.uID = 1;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = WM_APP + 41;
    icon.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(1));
    if (!icon.hIcon) icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon.szTip, L"ThunderLink");
    m_trayAdded = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    icon.uVersion = NOTIFYICON_VERSION_4;
    if (m_trayAdded) Shell_NotifyIconW(NIM_SETVERSION, &icon);
}
void Window::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Show");
    AppendMenuW(menu, MF_STRING, 2, L"Quit");
    POINT point{}; GetCursorPos(&point);
    SetForegroundWindow(m_hwnd);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 point.x, point.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    if (command == 1) Show();
    if (command == 2) RequestQuit();
}
void Window::Show() {
    ShowWindow(m_hwnd, IsIconic(m_hwnd) ? SW_RESTORE : SW_SHOW);
    if (m_restoreFullscreen) { m_restoreFullscreen = false; SetFullscreen(true); }
    SetForegroundWindow(m_hwnd);
}
void Window::Hide() {
    // Never leave the user with an invisible app and no way to restore it.
    if (!m_trayAdded) AddTrayIcon();
    if (!m_trayAdded) { ShowWindow(m_hwnd, SW_MINIMIZE); return; }
    if (m_inputCapture) {
        m_inputCapture->SetEnabled(false);
        m_inputCapture->SetCursorConfined(false);
        m_inputCapture->OnFocusLost();
    }
    m_restoreFullscreen = m_fullscreen;
    SetFullscreen(false);
    ShowWindow(m_hwnd, SW_HIDE);
}

void Window::Destroy() {
    if (m_trayAdded) {
        NOTIFYICONDATAW icon{}; icon.cbSize = sizeof(icon); icon.hWnd = m_hwnd; icon.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &icon); m_trayAdded = false;
    }

    if (m_hwnd) {
        WTSUnRegisterSessionNotification(m_hwnd);
        SetFullscreen(false);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    if (m_hInstance) {
        UnregisterClassW(CLASS_NAME, m_hInstance);
        m_hInstance = nullptr;
    }
}

void Window::SetFullscreen(bool fullscreen) {
    if (!m_hwnd || fullscreen == m_fullscreen) return;

    if (fullscreen) {
        GetWindowRect(m_hwnd, &m_savedWindowRect);
        m_savedStyle = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
        m_savedExStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);

        SetWindowLongPtrW(m_hwnd, GWL_STYLE,
                          m_savedStyle & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = { sizeof(info) };
        if (!GetMonitorInfoW(monitor, &info)) {
            SetWindowLongPtrW(m_hwnd, GWL_STYLE, m_savedStyle);
            LOG_ERROR("Window: GetMonitorInfoW failed (err=%lu)", GetLastError());
            return;
        }
        SetWindowPos(m_hwnd, HWND_TOP,
                     info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        m_fullscreen = true;
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, m_savedStyle);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, m_savedExStyle);
        SetWindowPos(m_hwnd, HWND_NOTOPMOST,
                     m_savedWindowRect.left, m_savedWindowRect.top,
                     m_savedWindowRect.right - m_savedWindowRect.left,
                     m_savedWindowRect.bottom - m_savedWindowRect.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        m_fullscreen = false;
    }

    if (m_inputCapture) m_inputCapture->ReapplyCursorClip();
    LOG_INFO("Window: fullscreen %s", m_fullscreen ? "enabled" : "disabled");
}

void Window::SetAspectRatio(float aspect) {
    if (aspect <= 0.0f) { m_aspectRatio = 0.0f; return; }
    if (std::fabs(aspect - m_aspectRatio) < 0.001f) return;  // unchanged
    m_aspectRatio = aspect;
    if (!m_hwnd) return;

    /* Only snap in windowed mode (don't fight F11 borderless fullscreen). */
    if (m_fullscreen) return;

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

    if (m_taskbarCreated && msg == m_taskbarCreated) { m_trayAdded = false; AddTrayIcon(); if (!m_trayAdded) Show(); return 0; }
    switch (msg) {
        case WM_APP + 40: Show(); return 0;
        case WM_APP + 41:
            if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT)
                ShowTrayMenu();
            return 0;
        case WM_CLOSE: Hide(); return 0;
        case WM_DISPLAYCHANGE:
            if (m_crossSafetyCallback) m_crossSafetyCallback();
            return 0;
        case WM_WTSSESSION_CHANGE:
            if ((wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_LOGOFF) && m_crossSafetyCallback) m_crossSafetyCallback();
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND && m_crossSafetyCallback) m_crossSafetyCallback();
            return TRUE;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION: if (wParam) RequestQuit(); return 0;
        case WM_GETMINMAXINFO: {
            if ((GetWindowLongPtrW(m_hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == 0) break;
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindow(m_hwnd);
            limits->ptMinTrackSize.x = MulDiv(800, dpi ? dpi : 96, 96);
            limits->ptMinTrackSize.y = MulDiv(560, dpi ? dpi : 96, 96);
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(m_hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

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
            if (wParam == SIZE_MINIMIZED && m_inputCapture) {
                m_inputCapture->SetEnabled(false); m_inputCapture->SetCursorConfined(false); m_inputCapture->OnFocusLost();
            }
            uint32_t newWidth = LOWORD(lParam);
            uint32_t newHeight = HIWORD(lParam);
            if (newWidth > 0 && newHeight > 0 &&
                (newWidth != m_width || newHeight != m_height)) {
                m_width = newWidth;
                m_height = newHeight;
                LOG_DEBUG("Window: resized to %ux%u", m_width, m_height);

                if (m_inputCapture) {
                    m_inputCapture->SetWindowSize(m_width, m_height);
                    m_inputCapture->ReapplyCursorClip();
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
                    m_inputCapture->SetCursorConfined(false);
                }
                return 0;
            }
            if (wParam == VK_F11 && (lParam & (1LL << 30)) == 0) {
                SetFullscreen(!m_fullscreen);
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
