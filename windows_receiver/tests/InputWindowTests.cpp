#include "App/Window.h"
#include "Input/InputCapture.h"
#include "Network/InputSender.h"

#include "input_event.h"
#include "imgui.h"

#include <cassert>
#include <cstddef>
#include <vector>

namespace {

void TestReleaseIsIdempotentAndSessionGated() {
    thunderlink::Window window;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    assert(window.Create(instance, SW_HIDE, 320, 240));

    std::vector<thunderlink::InputEvent> events;
    thunderlink::InputCapture capture([](HWND) { return true; });
    assert(capture.Initialize(window.GetHandle(),
        [&events](const thunderlink::InputEvent& event) {
            events.push_back(event);
            return true;
        }));

    capture.SetEnabled(true);
    RAWKEYBOARD key{};
    key.VKey = 'A';
    capture.ProcessKeyboard(key);
    assert(events.back().type == TLNK_INPUT_KEY && events.back().state == TLNK_STATE_DOWN);
    capture.OnFocusLost();
    assert(events[events.size()-2].state == TLNK_STATE_UP);
    capture.OnFocusGained();
    capture.ProcessKeyboard(key);
    ImGui::CreateContext();
    ImGui::GetIO().WantCaptureMouse = true;
    capture.UpdateInputPolicy();
    assert(events[events.size()-2].type == TLNK_INPUT_KEY && events[events.size()-2].state == TLNK_STATE_UP);
    assert(events.back().type == TLNK_INPUT_RESET);
    ImGui::DestroyContext();
    capture.UpdateInputPolicy();
    capture.ProcessKeyboard(key);
    capture.SetEnabled(false);
    assert(!capture.IsEnabled());
    assert(!events.empty());
    assert(events.back().type == TLNK_INPUT_RESET);

    const std::size_t afterDisable = events.size();
    capture.ReleaseAll();
    assert(events.size() == afterDisable + 1);
    assert(events.back().type == TLNK_INPUT_RESET);

    window.Destroy();
}

void TestFullscreenRestoresTheSavedWindow() {
    thunderlink::Window window;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    assert(window.Create(instance, SW_HIDE, 320, 240));

    const HWND hwnd = window.GetHandle();
    RECT before{};
    assert(GetWindowRect(hwnd, &before));
    const LONG_PTR styleBefore = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyleBefore = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    window.SetFullscreen(true);
    assert(window.IsFullscreen());
    assert((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == 0);

    window.SetFullscreen(false);
    assert(!window.IsFullscreen());
    assert(GetWindowLongPtrW(hwnd, GWL_STYLE) == styleBefore);
    assert(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) == exStyleBefore);
    RECT after{};
    assert(GetWindowRect(hwnd, &after));
    assert(after.left == before.left && after.top == before.top &&
           after.right == before.right && after.bottom == before.bottom);

    SendMessageW(hwnd, WM_KEYDOWN, VK_F11, 0);
    assert(window.IsFullscreen());
    SendMessageW(hwnd, WM_KEYDOWN, VK_F11, LPARAM(1) << 30);
    assert(window.IsFullscreen());
    SendMessageW(hwnd, WM_KEYDOWN, VK_F11, 0);
    assert(!window.IsFullscreen());

    window.Destroy();
}

void TestCloseHidesAndShowRestoresWithoutDestroyingWindow() {
    thunderlink::Window window;
    assert(window.Create(GetModuleHandleW(nullptr), SW_SHOWNORMAL, 900, 640));
    auto hwnd = window.GetHandle();
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    assert(IsWindow(hwnd));
    assert(!window.IsVisible());
    SendMessageW(hwnd, WM_APP + 40, 0, 0);
    assert(window.IsVisible());
    window.Hide(); window.Show();
    assert(IsWindow(hwnd) && window.IsVisible());
    window.Destroy();
}

} // namespace

int main() {
    TestCloseHidesAndShowRestoresWithoutDestroyingWindow();
    TestReleaseIsIdempotentAndSessionGated();
    TestFullscreenRestoresTheSavedWindow();
    return 0;
}
