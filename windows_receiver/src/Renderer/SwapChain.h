#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace thunderlink {

class SwapChain {
public:
    SwapChain();
    ~SwapChain();

    bool Initialize(IDXGIFactory6* factory, ID3D12CommandQueue* commandQueue,
                    HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    /* Present the current back buffer. Default: syncInterval=0 + tearing for
     * lowest latency. With vsync enabled: syncInterval=1, no tearing (stable
     * refresh, no tearing/flicker, ~1 frame more latency). */
    void Present();

    /* Toggle vsync at runtime (from the overlay). Cheap: no swapchain rebuild,
     * only the next Present()'s sync interval / tearing flag changes. */
    void SetVSync(bool enabled) { m_vsync = enabled; }
    bool GetVSync() const { return m_vsync; }

    /* Resize swap chain buffers (call when window resizes) */
    bool Resize(uint32_t width, uint32_t height);

    /* Wait on the waitable object for frame pacing */
    void WaitForFrameLatency();

    UINT GetCurrentBackBufferIndex() const;
    ID3D12Resource* GetCurrentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() const;

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

    static constexpr UINT BUFFER_COUNT = 3;
    static constexpr DXGI_FORMAT FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;

private:
    bool CreateRTVs();
    void ReleaseBuffers();

    ComPtr<IDXGISwapChain4> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12Resource> m_backBuffers[BUFFER_COUNT];

    HANDLE m_frameLatencyWaitableObject = nullptr;
    uint32_t m_rtvDescriptorSize = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_tearingSupported = false;
    bool m_vsync = false;

    ID3D12Device* m_device = nullptr; /* Non-owning */
};

} // namespace thunderlink
