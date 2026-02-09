#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

#include "SwapChain.h"
#include "TextureManager.h"

using Microsoft::WRL::ComPtr;

namespace thunderlink {

class D3D12Renderer {
public:
    D3D12Renderer();
    ~D3D12Renderer();

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    /* Upload a new frame and render it to screen */
    void RenderFrame(const uint8_t* frameData, uint32_t width, uint32_t height);

    /* Render the last uploaded texture (no new upload) */
    void RenderLastFrame();

    /* Handle window resize */
    void Resize(uint32_t width, uint32_t height);

    /* Wait for GPU to finish all work */
    void WaitForGPU();

    ID3D12Device* GetDevice() const { return m_device.Get(); }

private:
    bool CreateDevice();
    bool CreateCommandInfrastructure();
    bool CreateRootSignatureAndPSO();
    bool LoadShaders(ID3DBlob** vsBlob, ID3DBlob** psBlob);

    void MoveToNextFrame();
    void WaitForFence(UINT64 fenceValue);

    /* Device and factory */
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device> m_device;

    /* Command infrastructure */
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[SwapChain::BUFFER_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    /* Synchronization */
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValues[SwapChain::BUFFER_COUNT] = {};
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_currentFenceValue = 0;

    /* Pipeline */
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    /* Sub-components */
    SwapChain m_swapChain;
    TextureManager m_textureManager;

    /* State */
    bool m_initialized = false;
    bool m_hasFrame = false;
    UINT m_frameIndex = 0;
};

} // namespace thunderlink
