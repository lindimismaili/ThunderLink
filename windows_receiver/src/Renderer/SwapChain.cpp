#include "SwapChain.h"
#include "Utils/Logger.h"
#include "App/Config.h"

namespace thunderlink {

SwapChain::SwapChain() {
}

SwapChain::~SwapChain() {
    Shutdown();
}

bool SwapChain::Initialize(IDXGIFactory6* factory, ID3D12CommandQueue* commandQueue,
                           HWND hwnd, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    m_vsync = GetConfig().vsync;

    /* Check tearing support */
    BOOL tearingSupported = FALSE;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                       &tearingSupported, sizeof(tearingSupported));
    }
    m_tearingSupported = (tearingSupported == TRUE);
    LOG_INFO("SwapChain: tearing support = %s", m_tearingSupported ? "yes" : "no");

    /* Create swap chain */
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = FORMAT;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = BUFFER_COUNT;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (m_tearingSupported) {
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = factory->CreateSwapChainForHwnd(
        commandQueue, hwnd, &desc, nullptr, nullptr, &swapChain1);
    if (FAILED(hr)) {
        LOG_ERROR("SwapChain: CreateSwapChainForHwnd failed (hr=0x%08X)", hr);
        return false;
    }

    /* Disable Alt+Enter fullscreen toggle */
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        LOG_ERROR("SwapChain: failed to get IDXGISwapChain4 (hr=0x%08X)", hr);
        return false;
    }

    /* Set maximum frame latency to 1 for lowest latency */
    m_swapChain->SetMaximumFrameLatency(1);
    m_frameLatencyWaitableObject = m_swapChain->GetFrameLatencyWaitableObject();

    /* Get device from command queue for RTV creation */
    ComPtr<ID3D12Device> device;
    commandQueue->GetDevice(IID_PPV_ARGS(&device));
    m_device = device.Get();

    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = BUFFER_COUNT;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        LOG_ERROR("SwapChain: failed to create RTV heap (hr=0x%08X)", hr);
        return false;
    }

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!CreateRTVs()) return false;

    LOG_INFO("SwapChain: initialized %ux%u, %u buffers", width, height, BUFFER_COUNT);
    return true;
}

void SwapChain::Shutdown() {
    ReleaseBuffers();
    if (m_frameLatencyWaitableObject) {
        CloseHandle(m_frameLatencyWaitableObject);
        m_frameLatencyWaitableObject = nullptr;
    }
    m_swapChain.Reset();
    m_rtvHeap.Reset();
}

void SwapChain::Present() {
    UINT syncInterval;
    UINT flags;
    if (m_vsync) {
        /* Block until vblank; no tearing flag (ALLOW_TEARING is invalid with a
         * non-zero sync interval). Stable refresh — no tearing/flicker. */
        syncInterval = 1;
        flags = 0;
    } else {
        /* Lowest latency: present immediately, tear where supported. */
        syncInterval = 0;
        flags = m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    }

    HRESULT hr = m_swapChain->Present(syncInterval, flags);
    if (FAILED(hr)) {
        LOG_ERROR("SwapChain: Present failed (hr=0x%08X)", hr);
    }
}

bool SwapChain::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (width == m_width && height == m_height) return true;

    ReleaseBuffers();

    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (m_tearingSupported) {
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    HRESULT hr = m_swapChain->ResizeBuffers(BUFFER_COUNT, width, height, FORMAT, flags);
    if (FAILED(hr)) {
        LOG_ERROR("SwapChain: ResizeBuffers failed (hr=0x%08X)", hr);
        return false;
    }

    m_width = width;
    m_height = height;

    return CreateRTVs();
}

void SwapChain::WaitForFrameLatency() {
    if (m_frameLatencyWaitableObject) {
        WaitForSingleObject(m_frameLatencyWaitableObject, 1000);
    }
}

UINT SwapChain::GetCurrentBackBufferIndex() const {
    return m_swapChain->GetCurrentBackBufferIndex();
}

ID3D12Resource* SwapChain::GetCurrentBackBuffer() const {
    UINT index = m_swapChain->GetCurrentBackBufferIndex();
    return m_backBuffers[index].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::GetCurrentRTV() const {
    UINT index = m_swapChain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_rtvDescriptorSize;
    return handle;
}

bool SwapChain::CreateRTVs() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < BUFFER_COUNT; i++) {
        HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]));
        if (FAILED(hr)) {
            LOG_ERROR("SwapChain: GetBuffer(%u) failed (hr=0x%08X)", i, hr);
            return false;
        }

        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    return true;
}

void SwapChain::ReleaseBuffers() {
    for (UINT i = 0; i < BUFFER_COUNT; i++) {
        m_backBuffers[i].Reset();
    }
}

} // namespace thunderlink
