#include "Renderer/TextureManager.h"
#include "Utils/Logger.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cassert>
#include <vector>
using namespace thunderlink;
int main() {
    Logger::SetLevel(LogLevel::Fatal);
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) return 77;
    debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    assert(SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> warp;
    assert(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    ComPtr<ID3D12Device> device;
    assert(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
    ComPtr<ID3D12InfoQueue> messages;
    assert(SUCCEEDED(device.As(&messages)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    assert(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))));
    ComPtr<ID3D12CommandAllocator> allocator;
    assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    ComPtr<ID3D12GraphicsCommandList> list;
    assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                             nullptr, IID_PPV_ARGS(&list))));
    assert(SUCCEEDED(list->Close()));
    ComPtr<ID3D12Fence> fence;
    assert(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    HANDLE completed = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    assert(completed);
    TextureManager textures;
    assert(textures.Initialize(device.Get(), 1920, 1080));
    uint64_t fenceValue = 0;
    for (uint32_t width : {1280u, 1920u, 640u, 1280u, 1920u}) {
        const uint32_t height = width * 9 / 16;
        assert(textures.EnsureTextureSize(width, height));
        assert(SUCCEEDED(allocator->Reset()));
        assert(SUCCEEDED(list->Reset(allocator.Get(), nullptr)));
        std::vector<uint8_t> y(size_t(width) * height, 16);
        std::vector<uint8_t> uv(size_t(width) * height / 2, 128);
        assert(!textures.UploadNV12(list.Get(), y.data(), uv.data(), width, height,
                                    width - 1, width, 0));
        assert(textures.UploadNV12(list.Get(), y.data(), uv.data(), width, height, width, width, 0));
        assert(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        assert(SUCCEEDED(queue->Signal(fence.Get(), ++fenceValue)));
        assert(SUCCEEDED(fence->SetEventOnCompletion(fenceValue, completed)));
        assert(WaitForSingleObject(completed, 5000) == WAIT_OBJECT_0);
        textures.CommitTextureResize();
    }
    assert(textures.EnsureTextureSize(640, 360));
    textures.RollbackTextureResize();
    assert(textures.GetTextureWidth() == 1920 && textures.GetTextureHeight() == 1080);
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T length = 0;
        assert(SUCCEEDED(messages->GetMessage(i, nullptr, &length)));
        std::vector<uint8_t> storage(length);
        auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        assert(SUCCEEDED(messages->GetMessage(i, message, &length)));
        assert(message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION &&
               message->Severity != D3D12_MESSAGE_SEVERITY_ERROR);
    }
    textures.Shutdown();
    CloseHandle(completed);
}
