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

    HWND GetHandle() const { return m_hwnd; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

    void SetInputCapture(InputCapture* capture) { m_inputCapture = capture; }
    void SetResizeCallback(ResizeCallback cb) { m_resizeCallback = std::move(cb); }

    /* Process all pending Win32 messages. Returns false if WM_QUIT received. */
    bool ProcessMessages();

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    InputCapture* m_inputCapture = nullptr;
    ResizeCallback m_resizeCallback;

    static constexpr const wchar_t* CLASS_NAME = L"ThunderLinkReceiverClass";
};

} // namespace thunderlink
