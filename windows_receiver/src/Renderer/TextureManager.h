#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace thunderlink {

class TextureManager {
public:
    TextureManager();
    ~TextureManager();

    bool Initialize(ID3D12Device* device, uint32_t maxWidth, uint32_t maxHeight);
    void Shutdown();

    /* Upload frame pixel data from CPU to GPU.
     * Records commands into the provided command list.
     * Returns true if upload was initiated. */
    bool UploadFrame(ID3D12GraphicsCommandList* cmdList,
                     const uint8_t* data, uint32_t width, uint32_t height);

    /* Transition texture to shader-readable state (call after upload) */
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList);

    /* Transition texture to copy-dest state (call before upload) */
    void TransitionToCopyDest(ID3D12GraphicsCommandList* cmdList);

    ID3D12Resource* GetTexture() const { return m_texture.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRV() const;
    ID3D12DescriptorHeap* GetSRVHeap() const { return m_srvHeap.Get(); }

    uint32_t GetTextureWidth() const { return m_textureWidth; }
    uint32_t GetTextureHeight() const { return m_textureHeight; }

    static constexpr UINT UPLOAD_BUFFER_COUNT = 3;
    static constexpr DXGI_FORMAT TEXTURE_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;

private:
    bool CreateTexture(uint32_t width, uint32_t height);
    bool CreateUploadBuffers(uint32_t maxWidth, uint32_t maxHeight);

    /* D3D12 requires row pitch aligned to 256 bytes */
    static uint32_t AlignRowPitch(uint32_t width) {
        uint32_t rowBytes = width * 4; /* BGRA32 = 4 bytes per pixel */
        return (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
               & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    }

    ID3D12Device* m_device = nullptr;

    /* GPU-side texture (default heap) */
    ComPtr<ID3D12Resource> m_texture;
    uint32_t m_textureWidth = 0;
    uint32_t m_textureHeight = 0;

    /* CPU-side upload buffers (upload heap, ring-buffered) */
    ComPtr<ID3D12Resource> m_uploadBuffers[UPLOAD_BUFFER_COUNT];
    uint32_t m_currentUploadBuffer = 0;
    uint32_t m_uploadBufferRowPitch = 0;   /* Aligned row pitch */
    uint32_t m_uploadBufferMaxHeight = 0;

    /* SRV descriptor heap */
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;

    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};

} // namespace thunderlink
