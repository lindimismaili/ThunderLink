#pragma once

#include <cstdint>
#include <functional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace thunderlink {

class InputCapture;

class Window {
public:
    using ResizeCallback = std::function<void(uint32_t, uint32_t)>;

    Window();
    ~Window();

    bool Create(HINSTANCE hInstance, int nCmdShow, uint32_t width, uint32_t height);
    void Destroy();
    void Show();
    void Hide();
    bool IsVisible() const { return m_hwnd && IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd); }
    void RequestQuit() { PostQuitMessage(0); }


    HWND GetHandle() const { return m_hwnd; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

    void SetInputCapture(InputCapture* capture) { m_inputCapture = capture; }
    void SetResizeCallback(ResizeCallback cb) { m_resizeCallback = std::move(cb); }
    /* Cross stays active while this window is hidden. These callbacks are only
     * for system boundaries that must release remote ownership immediately. */
    void SetCrossSafetyCallback(std::function<void()> cb) { m_crossSafetyCallback = std::move(cb); }

    /* Borderless monitor fullscreen. The original window style and rectangle
     * are retained by this window instance and restored on exit. */
    void SetFullscreen(bool fullscreen);
    bool IsFullscreen() const { return m_fullscreen; }

    /* Lock the window's client area to this aspect ratio (width/height). The
     * window snaps to it now and stays constrained while the user resizes.
     * Pass <= 0 to disable. */
    void SetAspectRatio(float aspect);

    /* Process all pending Win32 messages. Returns false if WM_QUIT received. */
    bool ProcessMessages();

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    void AddTrayIcon();
    void ShowTrayMenu();
    bool m_trayAdded = false;
    bool m_restoreFullscreen = false;
    UINT m_taskbarCreated = 0;
    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    InputCapture* m_inputCapture = nullptr;
    ResizeCallback m_resizeCallback;
    std::function<void()> m_crossSafetyCallback;
    float m_aspectRatio = 0.0f;  /* client-area aspect lock (width/height); 0 = off */
    bool m_fullscreen = false;
    RECT m_savedWindowRect{};
    LONG_PTR m_savedStyle = 0;
    LONG_PTR m_savedExStyle = 0;

    static constexpr const wchar_t* CLASS_NAME = L"ThunderLinkReceiverClass";
};

} // namespace thunderlink
