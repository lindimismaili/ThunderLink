#include "D3D12Renderer.h"
#include "Utils/Logger.h"

#include "protocol.h"

#include <d3dcompiler.h>
#include <vector>
#include <fstream>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace thunderlink {

D3D12Renderer::D3D12Renderer() {
}

D3D12Renderer::~D3D12Renderer() {
    Shutdown();
}

bool D3D12Renderer::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    if (!CreateDevice()) return false;
    if (!CreateCommandInfrastructure()) return false;

    if (!m_swapChain.Initialize(m_factory.Get(), m_commandQueue.Get(),
                                 hwnd, width, height)) {
        LOG_ERROR("D3D12Renderer: swap chain initialization failed");
        return false;
    }

    m_frameIndex = m_swapChain.GetCurrentBackBufferIndex();

    if (!m_textureManager.Initialize(m_device.Get(), TLNK_MAX_WIDTH, TLNK_MAX_HEIGHT)) {
        LOG_ERROR("D3D12Renderer: texture manager initialization failed");
        return false;
    }

    if (!CreateRootSignatureAndPSO()) return false;

    if (!InitImGui(hwnd)) {
        LOG_WARN("D3D12Renderer: ImGui init failed; continuing without overlay");
    }

    m_initialized = true;
    LOG_INFO("D3D12Renderer: initialized successfully");
    return true;
}

bool D3D12Renderer::InitImGui(HWND hwnd) {
    /* Dedicated shader-visible SRV heap for the ImGui font atlas. */
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 1;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_imguiSrvHeap)))) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; /* don't write imgui.ini */

    if (!ImGui_ImplWin32_Init(hwnd)) return false;
    if (!ImGui_ImplDX12_Init(m_device.Get(), SwapChain::BUFFER_COUNT, SwapChain::FORMAT,
                             m_imguiSrvHeap.Get(),
                             m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                             m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart())) {
        return false;
    }

    m_imguiInitialized = true;
    LOG_INFO("D3D12Renderer: ImGui overlay initialized");
    return true;
}

void D3D12Renderer::RenderImGui(ID3D12GraphicsCommandList* cmdList) {
    if (!m_imguiInitialized) return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (m_uiCallback) m_uiCallback();

    ImGui::Render();
    ID3D12DescriptorHeap* heaps[] = { m_imguiSrvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
}

void D3D12Renderer::Shutdown() {
    if (!m_initialized) return;

    WaitForGPU();

    if (m_imguiInitialized) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_imguiInitialized = false;
    }
    m_imguiSrvHeap.Reset();

    m_textureManager.Shutdown();
    m_swapChain.Shutdown();

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_commandList.Reset();
    for (UINT i = 0; i < SwapChain::BUFFER_COUNT; i++) {
        m_commandAllocators[i].Reset();
    }
    m_fence.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
    m_factory.Reset();

    m_initialized = false;
}

bool D3D12Renderer::CreateDevice() {
    UINT dxgiFactoryFlags = 0;

#ifdef _DEBUG
    /* Enable debug layer */
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        LOG_INFO("D3D12Renderer: debug layer enabled");
    }
#endif

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: CreateDXGIFactory2 failed (hr=0x%08X)", hr);
        return false;
    }

    /* Enumerate adapters, prefer high-performance GPU */
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        /* Skip software adapters */
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        /* Try to create device */
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                               IID_PPV_ARGS(&m_device));
        if (SUCCEEDED(hr)) {
            char adapterName[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                adapterName, sizeof(adapterName), nullptr, nullptr);
            LOG_INFO("D3D12Renderer: using adapter '%s'", adapterName);
            return true;
        }

        adapter.Reset();
    }

    /* Fallback: try default adapter */
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: D3D12CreateDevice failed (hr=0x%08X)", hr);
        return false;
    }

    LOG_INFO("D3D12Renderer: using default adapter");
    return true;
}

bool D3D12Renderer::CreateCommandInfrastructure() {
    /* Create direct command queue */
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: CreateCommandQueue failed (hr=0x%08X)", hr);
        return false;
    }

    /* Create command allocators (one per frame in flight) */
    for (UINT i = 0; i < SwapChain::BUFFER_COUNT; i++) {
        hr = m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i]));
        if (FAILED(hr)) {
            LOG_ERROR("D3D12Renderer: CreateCommandAllocator(%u) failed (hr=0x%08X)", i, hr);
            return false;
        }
    }

    /* Create command list */
    hr = m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList));
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: CreateCommandList failed (hr=0x%08X)", hr);
        return false;
    }
    m_commandList->Close(); /* Start in closed state */

    /* Create fence */
    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: CreateFence failed (hr=0x%08X)", hr);
        return false;
    }

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        LOG_ERROR("D3D12Renderer: CreateEvent failed");
        return false;
    }

    return true;
}

bool D3D12Renderer::LoadShaders(ID3DBlob** vsBlob, ID3DBlob** psBlob) {
    /* Try loading pre-compiled .cso files first */
    auto loadCSO = [](const wchar_t* path, ID3DBlob** blob) -> bool {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;

        size_t size = static_cast<size_t>(file.tellg());
        file.seekg(0);

        HRESULT hr = D3DCreateBlob(size, blob);
        if (FAILED(hr)) return false;

        file.read(static_cast<char*>((*blob)->GetBufferPointer()), size);
        return true;
    };

    /* Try pre-compiled shaders */
    bool vsLoaded = loadCSO(L"shaders/vertex.cso", vsBlob);
    bool psLoaded = loadCSO(L"shaders/pixel.cso", psBlob);

    if (vsLoaded && psLoaded) {
        LOG_INFO("D3D12Renderer: loaded pre-compiled shaders");
        return true;
    }

    /* Fallback: compile from source at runtime */
    LOG_INFO("D3D12Renderer: compiling shaders from source");

    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompileFromFile(L"shaders/vertex.hlsl", nullptr, nullptr,
                                     "main", "vs_5_1", compileFlags, 0, vsBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            LOG_ERROR("Vertex shader compile error: %s",
                      (const char*)errors->GetBufferPointer());
        }
        /* Try relative to executable */
        hr = D3DCompileFromFile(L"../shaders/vertex.hlsl", nullptr, nullptr,
                                "main", "vs_5_1", compileFlags, 0, vsBlob, &errors);
        if (FAILED(hr)) {
            LOG_ERROR("D3D12Renderer: failed to compile vertex shader (hr=0x%08X)", hr);
            return false;
        }
    }

    hr = D3DCompileFromFile(L"shaders/pixel.hlsl", nullptr, nullptr,
                             "main", "ps_5_1", compileFlags, 0, psBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            LOG_ERROR("Pixel shader compile error: %s",
                      (const char*)errors->GetBufferPointer());
        }
        hr = D3DCompileFromFile(L"../shaders/pixel.hlsl", nullptr, nullptr,
                                "main", "ps_5_1", compileFlags, 0, psBlob, &errors);
        if (FAILED(hr)) {
            LOG_ERROR("D3D12Renderer: failed to compile pixel shader (hr=0x%08X)", hr);
            return false;
        }
    }

    return true;
}

bool D3D12Renderer::CreateRootSignatureAndPSO() {
    /* Root signature: 1 descriptor table (SRV) + 1 static sampler */
    D3D12_DESCRIPTOR_RANGE1 srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2; /* t0 = Y plane, t1 = UV plane (NV12) */
    srvRange.BaseShaderRegister = 0; /* t0 */
    srvRange.RegisterSpace = 0;
    srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &srvRange;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; /* Bilinear */
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0; /* s0 */
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = 1;
    rootSigDesc.Desc_1_1.pParameters = &rootParam;
    rootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    rootSigDesc.Desc_1_1.pStaticSamplers = &sampler;
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS
                                 | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                                 | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                                 | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> signatureBlob, errorBlob;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc,
                                                       &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            LOG_ERROR("Root signature serialize error: %s",
                      (const char*)errorBlob->GetBufferPointer());
        }
        return false;
    }

    hr = m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
                                        signatureBlob->GetBufferSize(),
                                        IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: CreateRootSignature failed (hr=0x%08X)", hr);
        return false;
    }

    /* Load shaders */
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!LoadShaders(&vsBlob, &psBlob)) return false;

    /* Create PSO */
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = SwapChain::FORMAT;
    psoDesc.SampleDesc.Count = 1;

    hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: CreateGraphicsPipelineState failed (hr=0x%08X)", hr);
        return false;
    }

    LOG_INFO("D3D12Renderer: root signature and PSO created");
    return true;
}

void D3D12Renderer::RenderFrame(const uint8_t* yPlane, const uint8_t* uvPlane,
                                uint32_t width, uint32_t height,
                                uint32_t yStride, uint32_t uvStride) {
    if (!m_initialized) return;

    /* Wait for frame latency and fence */
    m_swapChain.WaitForFrameLatency();

    m_frameIndex = m_swapChain.GetCurrentBackBufferIndex();
    WaitForFence(m_fenceValues[m_frameIndex]);

    /* Reset command allocator and command list */
    HRESULT hr = m_commandAllocators[m_frameIndex]->Reset();
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: command allocator reset failed (hr=0x%08X)", hr);
        return;
    }

    hr = m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), m_pipelineState.Get());
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: command list reset failed (hr=0x%08X)", hr);
        return;
    }

    /* Upload texture: transition to copy dest, upload, transition to SRV */
    m_textureManager.TransitionToCopyDest(m_commandList.Get());
    m_textureManager.UploadNV12(m_commandList.Get(), yPlane, uvPlane,
                                width, height, yStride, uvStride);
    m_textureManager.TransitionToShaderResource(m_commandList.Get());

    m_hasFrame = true;

    /* Transition back buffer to render target */
    D3D12_RESOURCE_BARRIER rtBarrier = {};
    rtBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rtBarrier.Transition.pResource = m_swapChain.GetCurrentBackBuffer();
    rtBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    rtBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rtBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &rtBarrier);

    /* Set render target */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain.GetCurrentRTV();
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    /* Clear to black */
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    /* Set viewport and scissor */
    D3D12_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_swapChain.GetWidth());
    viewport.Height = static_cast<float>(m_swapChain.GetHeight());
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor = {};
    scissor.right = static_cast<LONG>(m_swapChain.GetWidth());
    scissor.bottom = static_cast<LONG>(m_swapChain.GetHeight());
    m_commandList->RSSetScissorRects(1, &scissor);

    /* Set root signature and descriptor heaps */
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = {m_textureManager.GetSRVHeap()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetGraphicsRootDescriptorTable(0, m_textureManager.GetSRV());

    /* Draw fullscreen triangle */
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    /* Overlay UI */
    RenderImGui(m_commandList.Get());

    /* Transition back buffer to present */
    D3D12_RESOURCE_BARRIER presentBarrier = {};
    presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    presentBarrier.Transition.pResource = m_swapChain.GetCurrentBackBuffer();
    presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &presentBarrier);

    /* Execute */
    hr = m_commandList->Close();
    if (FAILED(hr)) {
        LOG_ERROR("D3D12Renderer: command list close failed (hr=0x%08X)", hr);
        return;
    }

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);

    /* Present */
    m_swapChain.Present();

    /* Signal fence for this frame */
    MoveToNextFrame();
}

void D3D12Renderer::RenderLastFrame() {
    if (!m_initialized) return;

    m_swapChain.WaitForFrameLatency();
    m_frameIndex = m_swapChain.GetCurrentBackBufferIndex();
    WaitForFence(m_fenceValues[m_frameIndex]);

    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), m_pipelineState.Get());

    /* Back buffer -> render target */
    D3D12_RESOURCE_BARRIER rtBarrier = {};
    rtBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rtBarrier.Transition.pResource = m_swapChain.GetCurrentBackBuffer();
    rtBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    rtBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rtBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &rtBarrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain.GetCurrentRTV();
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_swapChain.GetWidth());
    viewport.Height = static_cast<float>(m_swapChain.GetHeight());
    viewport.MaxDepth = 1.0f;
    m_commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor = {};
    scissor.right = static_cast<LONG>(m_swapChain.GetWidth());
    scissor.bottom = static_cast<LONG>(m_swapChain.GetHeight());
    m_commandList->RSSetScissorRects(1, &scissor);

    /* Re-draw the last decoded frame only if we actually have one. */
    if (m_hasFrame) {
        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        ID3D12DescriptorHeap* heaps[] = {m_textureManager.GetSRVHeap()};
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_commandList->SetGraphicsRootDescriptorTable(0, m_textureManager.GetSRV());

        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->DrawInstanced(3, 1, 0, 0);
    }

    /* Overlay UI (also shown on the black screen before streaming starts) */
    RenderImGui(m_commandList.Get());

    /* Back buffer -> present */
    D3D12_RESOURCE_BARRIER presentBarrier = {};
    presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    presentBarrier.Transition.pResource = m_swapChain.GetCurrentBackBuffer();
    presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &presentBarrier);

    m_commandList->Close();

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);

    m_swapChain.Present();
    MoveToNextFrame();
}

void D3D12Renderer::Resize(uint32_t width, uint32_t height) {
    if (!m_initialized) return;
    if (width == 0 || height == 0) return;

    WaitForGPU();
    m_swapChain.Resize(width, height);
    m_frameIndex = m_swapChain.GetCurrentBackBufferIndex();
}

void D3D12Renderer::WaitForGPU() {
    if (!m_commandQueue || !m_fence) return;

    m_currentFenceValue++;
    m_commandQueue->Signal(m_fence.Get(), m_currentFenceValue);
    WaitForFence(m_currentFenceValue);
}

void D3D12Renderer::MoveToNextFrame() {
    m_currentFenceValue++;
    m_commandQueue->Signal(m_fence.Get(), m_currentFenceValue);
    m_fenceValues[m_frameIndex] = m_currentFenceValue;
}

void D3D12Renderer::WaitForFence(UINT64 fenceValue) {
    if (m_fence->GetCompletedValue() < fenceValue) {
        m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

} // namespace thunderlink
