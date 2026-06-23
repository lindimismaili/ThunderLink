#include "TextureManager.h"
#include "Utils/Logger.h"

#include <cstring>
#include <algorithm>

namespace thunderlink {

TextureManager::TextureManager() {
}

TextureManager::~TextureManager() {
    Shutdown();
}

bool TextureManager::Initialize(ID3D12Device* device, uint32_t maxWidth, uint32_t maxHeight) {
    m_device = device;
    m_maxWidth = maxWidth;
    m_maxHeight = maxHeight;

    /* SRV descriptor heap with 2 entries: Y (t0) and UV (t1) */
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) {
        LOG_ERROR("TextureManager: failed to create SRV heap (hr=0x%08X)", hr);
        return false;
    }
    m_srvIncrement = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!CreateUploadBuffers(maxWidth, maxHeight)) return false;
    if (!CreateTextures(maxWidth, maxHeight)) return false;

    LOG_INFO("TextureManager: initialized (NV12), max %ux%u", maxWidth, maxHeight);
    return true;
}

void TextureManager::Shutdown() {
    m_texY.Reset();
    m_texUV.Reset();
    for (UINT i = 0; i < UPLOAD_BUFFER_COUNT; i++) {
        m_uploadBuffers[i].Reset();
    }
    m_srvHeap.Reset();
}

bool TextureManager::CreateTextures(uint32_t width, uint32_t height) {
    m_texY.Reset();
    m_texUV.Reset();

    auto makeTex = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out) -> bool {
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = w;
        texDesc.Height = h;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = fmt;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hr)) {
            LOG_ERROR("TextureManager: CreateCommittedResource %ux%u fmt=%d failed (hr=0x%08X)",
                      w, h, (int)fmt, hr);
            return false;
        }
        return true;
    };

    if (!makeTex(width, height, FORMAT_Y, m_texY)) return false;
    if (!makeTex(width / 2, height / 2, FORMAT_UV, m_texUV)) return false;

    m_textureWidth = width;
    m_textureHeight = height;
    m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    CreateSRVs();
    return true;
}

void TextureManager::CreateSRVs() {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC yDesc = {};
    yDesc.Format = FORMAT_Y;
    yDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    yDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    yDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_texY.Get(), &yDesc, handle);

    handle.ptr += m_srvIncrement;

    D3D12_SHADER_RESOURCE_VIEW_DESC uvDesc = {};
    uvDesc.Format = FORMAT_UV;
    uvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    uvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    uvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_texUV.Get(), &uvDesc, handle);
}

bool TextureManager::CreateUploadBuffers(uint32_t maxWidth, uint32_t maxHeight) {
    /* Each upload buffer holds Y (aligned rows) then UV (aligned rows, placed). */
    const uint32_t yRowPitch  = Align(maxWidth, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t ySize      = static_cast<uint64_t>(yRowPitch) * maxHeight;
    const uint32_t uvRowPitch = Align(maxWidth, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t uvOffset   = Align((uint32_t)std::min<uint64_t>(ySize, 0xFFFFFFFF),
                                      D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const uint64_t uvSize     = static_cast<uint64_t>(uvRowPitch) * (maxHeight / 2);
    m_uploadBufferSize = uvOffset + uvSize;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = m_uploadBufferSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (UINT i = 0; i < UPLOAD_BUFFER_COUNT; i++) {
        HRESULT hr = m_device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_uploadBuffers[i]));
        if (FAILED(hr)) {
            LOG_ERROR("TextureManager: upload buffer %u create failed (hr=0x%08X)", i, hr);
            return false;
        }
    }

    LOG_INFO("TextureManager: created %u upload buffers, %llu bytes each",
             UPLOAD_BUFFER_COUNT, m_uploadBufferSize);
    return true;
}

bool TextureManager::UploadNV12(ID3D12GraphicsCommandList* cmdList,
                                const uint8_t* yPlane, const uint8_t* uvPlane,
                                uint32_t width, uint32_t height,
                                uint32_t yStride, uint32_t uvStride) {
    if (!yPlane || !uvPlane || width == 0 || height == 0) return false;
    if (width > m_maxWidth || height > m_maxHeight) return false;

    if (width != m_textureWidth || height != m_textureHeight) {
        LOG_INFO("TextureManager: resizing %ux%u -> %ux%u",
                 m_textureWidth, m_textureHeight, width, height);
        if (m_currentState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
            TransitionToShaderResource(cmdList);
        }
        if (!CreateTextures(width, height)) return false;
    }

    const uint32_t chromaH = height / 2;
    const uint32_t yRowPitch  = Align(width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t uvRowPitch = Align(width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t ySize      = static_cast<uint64_t>(yRowPitch) * height;
    const uint64_t uvOffset   = Align((uint32_t)std::min<uint64_t>(ySize, 0xFFFFFFFF),
                                      D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    ID3D12Resource* uploadBuf = m_uploadBuffers[m_currentUploadBuffer].Get();
    m_currentUploadBuffer = (m_currentUploadBuffer + 1) % UPLOAD_BUFFER_COUNT;

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    HRESULT hr = uploadBuf->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        LOG_ERROR("TextureManager: upload Map failed (hr=0x%08X)", hr);
        return false;
    }

    uint8_t* base = static_cast<uint8_t*>(mapped);

    /* Y plane */
    for (uint32_t r = 0; r < height; r++) {
        std::memcpy(base + static_cast<size_t>(r) * yRowPitch,
                    yPlane + static_cast<size_t>(r) * yStride, width);
    }
    /* UV plane (placed after Y) */
    uint8_t* uvDst = base + uvOffset;
    for (uint32_t r = 0; r < chromaH; r++) {
        std::memcpy(uvDst + static_cast<size_t>(r) * uvRowPitch,
                    uvPlane + static_cast<size_t>(r) * uvStride, width);
    }

    uploadBuf->Unmap(0, nullptr);

    /* Copy Y region -> Y texture */
    D3D12_TEXTURE_COPY_LOCATION ySrc = {};
    ySrc.pResource = uploadBuf;
    ySrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    ySrc.PlacedFootprint.Offset = 0;
    ySrc.PlacedFootprint.Footprint.Format = FORMAT_Y;
    ySrc.PlacedFootprint.Footprint.Width = width;
    ySrc.PlacedFootprint.Footprint.Height = height;
    ySrc.PlacedFootprint.Footprint.Depth = 1;
    ySrc.PlacedFootprint.Footprint.RowPitch = yRowPitch;

    D3D12_TEXTURE_COPY_LOCATION yDst = {};
    yDst.pResource = m_texY.Get();
    yDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    yDst.SubresourceIndex = 0;
    cmdList->CopyTextureRegion(&yDst, 0, 0, 0, &ySrc, nullptr);

    /* Copy UV region -> UV texture (R8G8 => width/2 texels, height/2 rows) */
    D3D12_TEXTURE_COPY_LOCATION uvSrc = {};
    uvSrc.pResource = uploadBuf;
    uvSrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    uvSrc.PlacedFootprint.Offset = uvOffset;
    uvSrc.PlacedFootprint.Footprint.Format = FORMAT_UV;
    uvSrc.PlacedFootprint.Footprint.Width = width / 2;
    uvSrc.PlacedFootprint.Footprint.Height = chromaH;
    uvSrc.PlacedFootprint.Footprint.Depth = 1;
    uvSrc.PlacedFootprint.Footprint.RowPitch = uvRowPitch;

    D3D12_TEXTURE_COPY_LOCATION uvDstLoc = {};
    uvDstLoc.pResource = m_texUV.Get();
    uvDstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    uvDstLoc.SubresourceIndex = 0;
    cmdList->CopyTextureRegion(&uvDstLoc, 0, 0, 0, &uvSrc, nullptr);

    return true;
}

void TextureManager::TransitionToShaderResource(ID3D12GraphicsCommandList* cmdList) {
    if (m_currentState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return;

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    for (int i = 0; i < 2; i++) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = (i == 0) ? m_texY.Get() : m_texUV.Get();
        barriers[i].Transition.StateBefore = m_currentState;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmdList->ResourceBarrier(2, barriers);
    m_currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void TextureManager::TransitionToCopyDest(ID3D12GraphicsCommandList* cmdList) {
    if (m_currentState == D3D12_RESOURCE_STATE_COPY_DEST) return;

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    for (int i = 0; i < 2; i++) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = (i == 0) ? m_texY.Get() : m_texUV.Get();
        barriers[i].Transition.StateBefore = m_currentState;
        barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmdList->ResourceBarrier(2, barriers);
    m_currentState = D3D12_RESOURCE_STATE_COPY_DEST;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureManager::GetSRV() const {
    return m_srvHeap->GetGPUDescriptorHandleForHeapStart();
}

} // namespace thunderlink
