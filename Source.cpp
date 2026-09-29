#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kFrameCount = 2;

void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[160];
        sprintf_s(buf, "%s failed (HRESULT 0x%08X)", what, static_cast<unsigned>(hr));
        throw std::runtime_error(buf);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

class Gpu
{
public:
    explicit Gpu(HWND hwnd) : hwnd_(hwnd)
    {
        CreateDevice();
        CreateQueue();
        CreateSwapchain();
        CreateRtvHeap();
        CreateFence();
    }

    ~Gpu()
    {
        try { WaitForGpu(); }
        catch (...) {}
        if (fenceEvent_) CloseHandle(fenceEvent_);
    }

    void Render()
    {
        ThrowIfFailed(allocator_[frameIndex_]->Reset(), "Reset allocator");
        ThrowIfFailed(list_->Reset(allocator_[frameIndex_].Get(), pipelineState_.Get()), "Reset list");

        D3D12_RESOURCE_BARRIER toRt{};
        toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRt.Transition.pResource = targets_[frameIndex_].Get();
        toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list_->ResourceBarrier(1, &toRt);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;
        list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        const float clear[4] = { 0, 0, 0, 1 };
        list_->ClearRenderTargetView(rtv, clear, 0, nullptr);

        list_->RSSetViewports(1, &viewport_);
        list_->RSSetScissorRects(1, &scissor_);
        list_->SetGraphicsRootSignature(rootSignature_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->IASetVertexBuffers(0, 1, &vbv_);
        list_->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER toPresent = toRt;
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list_->ResourceBarrier(1, &toPresent);

        ThrowIfFailed(list_->Close(), "Close list");
        ID3D12CommandList* lists[] = { list_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        ThrowIfFailed(swapchain_->Present(1, 0), "Present");
        WaitForGpu();
        frameIndex_ = swapchain_->GetCurrentBackBufferIndex();
    }

private:
    HWND hwnd_{};
    UINT frameIndex_{};
    UINT rtvSize_{};
    UINT64 fenceValue_{};
    HANDLE fenceEvent_{};

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12Resource> targets_[kFrameCount];
    ComPtr<ID3D12CommandAllocator> allocator_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
    ComPtr<ID3D12Resource> vertexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};

    void CreateDevice()
    {
        ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");
        ThrowIfFailed(
            D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
            "D3D12CreateDevice");
    }

    void CreateQueue()
    {
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(device_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");

        for (UINT i = 0; i < kFrameCount; ++i)
        {
            ThrowIfFailed(
                device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_[i])),
                "CreateCommandAllocator");
        }
        ThrowIfFailed(
            device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_[0].Get(), nullptr, IID_PPV_ARGS(&list_)),
            "CreateCommandList");
        ThrowIfFailed(list_->Close(), "Close list after create");
    }

    void CreateSwapchain()
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kFrameCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> sc;
        ThrowIfFailed(
            factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &desc, nullptr, nullptr, &sc),
            "CreateSwapChainForHwnd");
        ThrowIfFailed(sc.As(&swapchain_), "Query IDXGISwapChain3");
        frameIndex_ = swapchain_->GetCurrentBackBufferIndex();
    }

    void CreateRtvHeap()
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = kFrameCount;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtvHeap_)), "CreateDescriptorHeap");
        rtvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        for (UINT i = 0; i < kFrameCount; ++i)
        {
            ThrowIfFailed(swapchain_->GetBuffer(i, IID_PPV_ARGS(&targets_[i])), "GetBuffer");
            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(i) * rtvSize_;
            device_->CreateRenderTargetView(targets_[i].Get(), nullptr, handle);
        }
    }

    void CreatePipeline()
    {
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rsBlob, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err),
            "SerializeRootSignature");
        ThrowIfFailed(device_->CreateRootSignature(
            0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_)),
            "CreateRootSignature");

        ComPtr<ID3DBlob> vs, ps;
        HRESULT hr = D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_1", 0, 0, &vs, &err);
        if (FAILED(hr))
            throw std::runtime_error(err ? (const char*)err->GetBufferPointer() : "VS compile failed");
        hr = D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_1", 0, 0, &ps, &err);
        if (FAILED(hr))
            throw std::runtime_error(err ? (const char*)err->GetBufferPointer() : "PS compile failed");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rootSignature_.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.InputLayout = { layout, 2 };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.SampleDesc.Count = 1;
        ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipelineState_)), "CreatePSO");

        viewport_ = { 0.f, 0.f, (float)kWidth, (float)kHeight, 0.f, 1.f };
        scissor_ = { 0, 0, (LONG)kWidth, (LONG)kHeight };
    }

    void CreateTriangle()
    {
        struct Vertex { float pos[3]; float col[4]; };
        const Vertex verts[] = {
            { {  0.0f,  0.5f, 0.0f }, { 1, 1, 0, 1 } },
            { {  0.5f, -0.5f, 0.0f }, { 1, 1, 0, 1 } },
            { { -0.5f, -0.5f, 0.0f }, { 1, 1, 0, 1 } },
        };

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC res{};
        res.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        res.Width = sizeof(verts);
        res.Height = 1;
        res.DepthOrArraySize = 1;
        res.MipLevels = 1;
        res.SampleDesc.Count = 1;
        res.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &res,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer_)),
            "Create vertex buffer");

        void* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        ThrowIfFailed(vertexBuffer_->Map(0, &none, &mapped), "Map VB");
        memcpy(mapped, verts, sizeof(verts));
        vertexBuffer_->Unmap(0, nullptr);

        vbv_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
        vbv_.SizeInBytes = sizeof(verts);
        vbv_.StrideInBytes = sizeof(Vertex);
    }

    void CreateFence()
    {
        ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) throw std::runtime_error("CreateEventW failed");
        CreatePipeline();
        CreateTriangle();
    }


    void WaitForGpu()
    {
        const UINT64 value = ++fenceValue_;
        ThrowIfFailed(queue_->Signal(fence_.Get(), value), "Signal");
        if (fence_->GetCompletedValue() < value)
        {
            ThrowIfFailed(fence_->SetEventOnCompletion(value, fenceEvent_), "SetEventOnCompletion");
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    try
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"HelloTriangleWindow";
        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowExW(
            0, wc.lpszClassName, L"D3D12 Triangle",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight,
            nullptr, nullptr, instance, nullptr);
        ShowWindow(hwnd, SW_SHOW);

        Gpu gpu(hwnd);

        MSG msg{};
        while (msg.message != WM_QUIT)
        {
            if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            else
            {
                gpu.Render();
            }
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "D3D12", MB_ICONERROR);
        return 1;
    }
}