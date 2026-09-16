#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace thunderlink {

/* Manages the decoded-video textures as NV12 (two planes):
 *   - Y  plane: R8_UNORM,   width x height        (SRV slot t0)
 *   - UV plane: R8G8_UNORM, width/2 x height/2    (SRV slot t1, interleaved CbCr)
 * The pixel shader samples both and converts BT.709 YUV -> RGB.
 *
 * Decoded NV12 arrives in system memory (tightly packed) and is uploaded to the
 * two GPU textures each frame. */
class TextureManager {
public:
    TextureManager();
    ~TextureManager();

    bool Initialize(ID3D12Device* device, uint32_t maxWidth, uint32_t maxHeight);
    void Shutdown();

    /* Recreate textures transactionally before recording a command list.
     * RollbackTextureResize restores the previous valid frame if later command
     * setup fails; CommitTextureResize releases it after submission. */
    bool EnsureTextureSize(uint32_t width, uint32_t height);
    void CommitTextureResize();
    void RollbackTextureResize();

    /* Upload one NV12 frame (system memory) into the Y/UV textures. The upload
     * slot is fence-coupled to the renderer's frame index. */
    bool UploadNV12(ID3D12GraphicsCommandList* cmdList,
                    const uint8_t* yPlane, const uint8_t* uvPlane,
                    uint32_t width, uint32_t height,
                    uint32_t yStride, uint32_t uvStride,
                    UINT uploadSlot);

    /* GPU handle to the start of the 2-descriptor table (t0=Y, t1=UV). */
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRV() const;
    ID3D12DescriptorHeap* GetSRVHeap() const { return m_srvHeap.Get(); }

    uint32_t GetTextureWidth() const { return m_textureWidth; }
    uint32_t GetTextureHeight() const { return m_textureHeight; }

    static constexpr UINT UPLOAD_BUFFER_COUNT = 3;
    static constexpr DXGI_FORMAT FORMAT_Y  = DXGI_FORMAT_R8_UNORM;
    static constexpr DXGI_FORMAT FORMAT_UV = DXGI_FORMAT_R8G8_UNORM;

private:
    bool CreateTextures(uint32_t width, uint32_t height,
                        D3D12_RESOURCE_STATES initialState);
    bool CreateUploadBuffers(uint32_t maxWidth, uint32_t maxHeight);
    void CreateSRVs();
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList);
    void TransitionToCopyDest(ID3D12GraphicsCommandList* cmdList);

    static uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    ID3D12Device* m_device = nullptr;

    /* GPU-side textures (default heap) */
    ComPtr<ID3D12Resource> m_texY;
    ComPtr<ID3D12Resource> m_texUV;
    uint32_t m_textureWidth = 0;
    uint32_t m_textureHeight = 0;

    /* CPU-side upload buffers (upload heap, ring-buffered). Each holds the Y
     * plane followed by the UV plane (placement-aligned). */
    ComPtr<ID3D12Resource> m_uploadBuffers[UPLOAD_BUFFER_COUNT];
    uint64_t m_uploadBufferSize = 0;
    uint32_t m_maxWidth = 0;
    uint32_t m_maxHeight = 0;

    /* SRV descriptor heap (2 descriptors: Y at 0, UV at 1) */
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvIncrement = 0;

    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    /* Previous textures are retained until a resized frame has been submitted,
     * allowing a failed upload/command-list setup to keep the last valid image. */
    ComPtr<ID3D12Resource> m_previousTexY;
    ComPtr<ID3D12Resource> m_previousTexUV;
    uint32_t m_previousWidth = 0;
    uint32_t m_previousHeight = 0;
    D3D12_RESOURCE_STATES m_previousState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};

} // namespace thunderlink
