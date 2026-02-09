#include "TextureManager.h"
#include "Utils/Logger.h"

#include <cstring>

namespace thunderlink {

TextureManager::TextureManager() {
}

TextureManager::~TextureManager() {
    Shutdown();
}

bool TextureManager::Initialize(ID3D12Device* device, uint32_t maxWidth, uint32_t maxHeight) {
    m_device = device;

    /* Create SRV descriptor heap (shader-visible) */
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        LOG_ERROR("TextureManager: failed to create SRV heap (hr=0x%08X)", hr);
        return false;
    }

    /* Create upload buffers sized for maximum resolution */
    if (!CreateUploadBuffers(maxWidth, maxHeight)) return false;

    /* Create initial texture at a default size (will be resized on first frame) */
    if (!CreateTexture(maxWidth, maxHeight)) return false;

    LOG_INFO("TextureManager: initialized, max %ux%u", maxWidth, maxHeight);
    return true;
}

void TextureManager::Shutdown() {
    m_texture.Reset();
    for (UINT i = 0; i < UPLOAD_BUFFER_COUNT; i++) {
        m_uploadBuffers[i].Reset();
    }
    m_srvHeap.Reset();
}

bool TextureManager::CreateTexture(uint32_t width, uint32_t height) {
    m_texture.Reset();

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = TEXTURE_FORMAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr, IID_PPV_ARGS(&m_texture));

    if (FAILED(hr)) {
        LOG_ERROR("TextureManager: failed to create texture %ux%u (hr=0x%08X)",
                  width, height, hr);
        return false;
    }

    m_textureWidth = width;
    m_textureHeight = height;
    m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    /* Create SRV for the texture */
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = TEXTURE_FORMAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    m_device->CreateShaderResourceView(m_texture.Get(), &srvDesc,
                                        m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

bool TextureManager::CreateUploadBuffers(uint32_t maxWidth, uint32_t maxHeight) {
    m_uploadBufferRowPitch = AlignRowPitch(maxWidth);
    m_uploadBufferMaxHeight = maxHeight;

    uint64_t bufferSize = static_cast<uint64_t>(m_uploadBufferRowPitch) * maxHeight;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = bufferSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (UINT i = 0; i < UPLOAD_BUFFER_COUNT; i++) {
        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_uploadBuffers[i]));

        if (FAILED(hr)) {
            LOG_ERROR("TextureManager: failed to create upload buffer %u (hr=0x%08X)", i, hr);
            return false;
        }
    }

    LOG_INFO("TextureManager: created %u upload buffers, %llu bytes each",
             UPLOAD_BUFFER_COUNT, bufferSize);
    return true;
}

bool TextureManager::UploadFrame(ID3D12GraphicsCommandList* cmdList,
                                  const uint8_t* data, uint32_t width, uint32_t height) {
    if (!data || width == 0 || height == 0) return false;

    /* Resize texture if resolution changed */
    if (width != m_textureWidth || height != m_textureHeight) {
        LOG_INFO("TextureManager: resizing texture from %ux%u to %ux%u",
                 m_textureWidth, m_textureHeight, width, height);

        /* Ensure texture is in the correct state before destroying */
        if (m_currentState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
            TransitionToShaderResource(cmdList);
        }

        if (!CreateTexture(width, height)) return false;
    }

    /* Select the current upload buffer from the ring */
    ID3D12Resource* uploadBuf = m_uploadBuffers[m_currentUploadBuffer].Get();
    m_currentUploadBuffer = (m_currentUploadBuffer + 1) % UPLOAD_BUFFER_COUNT;

    /* Map the upload buffer and copy pixel data with row pitch alignment */
    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0}; /* We won't read from GPU */
    HRESULT hr = uploadBuf->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        LOG_ERROR("TextureManager: upload buffer Map failed (hr=0x%08X)", hr);
        return false;
    }

    uint32_t srcRowPitch = width * 4; /* Source is tightly packed BGRA32 */
    uint32_t dstRowPitch = AlignRowPitch(width);

    if (srcRowPitch == dstRowPitch) {
        /* No padding needed - single memcpy */
        std::memcpy(mapped, data, static_cast<size_t>(srcRowPitch) * height);
    } else {
        /* Row-by-row copy to handle alignment padding */
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        const uint8_t* src = data;
        for (uint32_t row = 0; row < height; row++) {
            std::memcpy(dst, src, srcRowPitch);
            dst += dstRowPitch;
            src += srcRowPitch;
        }
    }

    uploadBuf->Unmap(0, nullptr);

    /* Copy from upload buffer to texture */
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuf;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = TEXTURE_FORMAT;
    srcLoc.PlacedFootprint.Footprint.Width = width;
    srcLoc.PlacedFootprint.Footprint.Height = height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = dstRowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_texture.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    return true;
}

void TextureManager::TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList) {
    if (m_currentState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_texture.Get();
    barrier.Transition.StateBefore = m_currentState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList->ResourceBarrier(1, &barrier);
    m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void TextureManager::TransitionToCopyDest(ID3D12GraphicsCommandList* cmdList) {
    if (m_currentState == D3D12_RESOURCE_STATE_COPY_DEST) return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_texture.Get();
    barrier.Transition.StateBefore = m_currentState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList->ResourceBarrier(1, &barrier);
    m_currentState = D3D12_RESOURCE_STATE_COPY_DEST;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetSRV() const {
    return m_srvHeap->GetGPUDescriptorHandleForHeapStart();
}

} // namespace thunderlink
