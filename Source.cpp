#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <DirectXMath.h>
using namespace DirectX;

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kFrameCount = 2;
constexpr UINT kMsaaCount = 4;

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

struct Vertex
{
    float pos[3];
    float nrm[3];
    float uv[2];
};

struct SceneCB
{
    XMFLOAT4X4 mvp;
    XMFLOAT4X4 world;
    float lightX;
    float lightY;
    float lightZ;
    float ambient;
};

class Gpu
{
public:
    explicit Gpu(HWND hwnd) : hwnd_(hwnd)
    {
        CreateDevice();
        CreateQueue();
        CreateSwapchain();
        CreateMsaaTargets();
        CreateFence();
        CreatePipeline();
        CreateGeometry();
        CreateTexture();
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
        toRt.Transition.pResource = msaaColor_.Get();
        toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (!msaaReady_)
        {
            toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        if (toRt.Transition.StateBefore != toRt.Transition.StateAfter)
        {
            list_->ResourceBarrier(1, &toRt);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

        const float clear[4] = { 0.02f, 0.02f, 0.03f, 1 };
        list_->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        list_->RSSetViewports(1, &viewport_);
        list_->RSSetScissorRects(1, &scissor_);
        list_->SetGraphicsRootSignature(rootSignature_.Get());
        ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
        list_->SetDescriptorHeaps(1, heaps);
        D3D12_GPU_DESCRIPTOR_HANDLE pyramidSrv = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE groundSrv = pyramidSrv;
        groundSrv.ptr += srvSize_;

        XMMATRIX world = XMMatrixIdentity();
        XMMATRIX view = UpdateCamera();
        XMMATRIX proj = XMMatrixPerspectiveFovLH(
            XMConvertToRadians(60.0f),
            (float)kWidth / (float)kHeight,
            0.1f, 100.0f);

        SceneCB cb{};
        XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(world * view * proj));
        XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
        XMVECTOR L = XMVector3Normalize(XMVectorSet(0.35f, 0.85f, -0.45f, 0.0f));
        cb.lightX = XMVectorGetX(L);
        cb.lightY = XMVectorGetY(L);
        cb.lightZ = XMVectorGetZ(L);
        cb.ambient = 0.22f;
        list_->SetGraphicsRoot32BitConstants(0, 36, &cb, 0);

        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->IASetVertexBuffers(0, 1, &vbv_);
        list_->SetGraphicsRootDescriptorTable(1, pyramidSrv);
        list_->DrawInstanced(pyramidCount_, 1, 0, 0);
        list_->SetGraphicsRootDescriptorTable(1, groundSrv);
        list_->DrawInstanced(groundCount_, 1, pyramidCount_, 0);

        D3D12_RESOURCE_BARRIER resolveBarriers[2]{};
        resolveBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resolveBarriers[0].Transition.pResource = msaaColor_.Get();
        resolveBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        resolveBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        resolveBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        resolveBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resolveBarriers[1].Transition.pResource = swapTargets_[frameIndex_].Get();
        resolveBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        resolveBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        resolveBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list_->ResourceBarrier(2, resolveBarriers);
        list_->ResolveSubresource(
            swapTargets_[frameIndex_].Get(), 0,
            msaaColor_.Get(), 0,
            DXGI_FORMAT_R8G8B8A8_UNORM);

        D3D12_RESOURCE_BARRIER toPresent{};
        toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toPresent.Transition.pResource = swapTargets_[frameIndex_].Get();
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list_->ResourceBarrier(1, &toPresent);
        msaaReady_ = true;

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
    UINT64 fenceValue_{};
    HANDLE fenceEvent_{};
    bool msaaReady_ = false;
    UINT pyramidCount_ = 0;
    UINT groundCount_ = 0;
    float camX_ = 0.0f;
    float camY_ = 1.2f;
    float camZ_ = -4.0f;
    float yaw_ = 0.0f;
    float pitch_ = -0.28f;
    bool captureMouse_ = false;
    bool cursorHidden_ = false;
    bool skipMouse_ = true;
    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER qpcLast_{};

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12Resource> swapTargets_[kFrameCount];
    ComPtr<ID3D12Resource> msaaColor_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12CommandAllocator> allocator_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
    ComPtr<ID3D12Resource> vertexBuffer_;
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    ComPtr<ID3D12Resource> depthBuffer_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    ComPtr<ID3D12Resource> texturePyramid_;
    ComPtr<ID3D12Resource> textureGround_;
    ComPtr<ID3D12Resource> uploadPyramid_;
    ComPtr<ID3D12Resource> uploadGround_;
    UINT srvSize_{};
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};

    XMMATRIX UpdateCamera()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (qpcLast_.QuadPart == 0)
        {
            QueryPerformanceFrequency(&qpcFreq_);
            qpcLast_ = now;
        }
        float dt = (float)(now.QuadPart - qpcLast_.QuadPart) / (float)qpcFreq_.QuadPart;
        qpcLast_ = now;
        if (dt > 0.1f) dt = 0.1f;

        const bool focused = GetForegroundWindow() == hwnd_;
        if (focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
        {
            captureMouse_ = true;
            skipMouse_ = true;
        }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            captureMouse_ = false;
        }

        if (captureMouse_ && focused)
        {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            POINT center{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
            ClientToScreen(hwnd_, &center);

            POINT p{};
            GetCursorPos(&p);
            if (!skipMouse_)
            {
                yaw_ += (p.x - center.x) * 0.0035f;
                pitch_ -= (p.y - center.y) * 0.0035f;
                if (pitch_ > 1.5f) pitch_ = 1.5f;
                if (pitch_ < -1.5f) pitch_ = -1.5f;
            }
            skipMouse_ = false;
            SetCursorPos(center.x, center.y);
            if (!cursorHidden_)
            {
                ShowCursor(FALSE);
                cursorHidden_ = true;
            }
        }
        else
        {
            skipMouse_ = true;
            if (cursorHidden_)
            {
                ShowCursor(TRUE);
                cursorHidden_ = false;
            }
        }

        XMVECTOR forward = XMVector3Normalize(XMVectorSet(
            sinf(yaw_) * cosf(pitch_),
            sinf(pitch_),
            cosf(yaw_) * cosf(pitch_),
            0.0f));
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));

        float speed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 8.0f : 3.0f;
        XMVECTOR move = XMVectorZero();
        if ((GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState(VK_UP) & 0x8000))
            move = XMVectorAdd(move, forward);
        if ((GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState(VK_DOWN) & 0x8000))
            move = XMVectorSubtract(move, forward);
        if ((GetAsyncKeyState('D') & 0x8000) || (GetAsyncKeyState(VK_RIGHT) & 0x8000))
            move = XMVectorAdd(move, right);
        if ((GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState(VK_LEFT) & 0x8000))
            move = XMVectorSubtract(move, right);
        if (GetAsyncKeyState(VK_SPACE) & 0x8000)
            move = XMVectorAdd(move, up);
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
            move = XMVectorSubtract(move, up);
        move = XMVectorScale(move, speed * dt);
        camX_ += XMVectorGetX(move);
        camY_ += XMVectorGetY(move);
        camZ_ += XMVectorGetZ(move);

        XMVECTOR eye = XMVectorSet(camX_, camY_, camZ_, 0.0f);
        return XMMatrixLookAtLH(eye, XMVectorAdd(eye, forward), up);
    }

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
        for (UINT i = 0; i < kFrameCount; ++i)
        {
            ThrowIfFailed(swapchain_->GetBuffer(i, IID_PPV_ARGS(&swapTargets_[i])), "GetBuffer");
        }
    }

    void CreateMsaaTargets()
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.NumDescriptors = 1;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)), "Create RTV heap");

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap_)), "Create DSV heap");

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_CLEAR_VALUE colorClear{};
        colorClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colorClear.Color[3] = 1.0f;

        D3D12_RESOURCE_DESC colorDesc{};
        colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        colorDesc.Width = kWidth;
        colorDesc.Height = kHeight;
        colorDesc.DepthOrArraySize = 1;
        colorDesc.MipLevels = 1;
        colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colorDesc.SampleDesc.Count = kMsaaCount;
        colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ThrowIfFailed(device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &colorDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear, IID_PPV_ARGS(&msaaColor_)),
            "Create MSAA color");

        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        device_->CreateRenderTargetView(msaaColor_.Get(), &rtv, rtvHeap_->GetCPUDescriptorHandleForHeapStart());

        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;

        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = kWidth;
        depthDesc.Height = kHeight;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc.Count = kMsaaCount;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ThrowIfFailed(device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&depthBuffer_)),
            "Create MSAA depth");

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        device_->CreateDepthStencilView(depthBuffer_.Get(), &dsv, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
    }

    void CreatePipeline()
    {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 36;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_ANISOTROPIC;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp.MaxAnisotropy = 8;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samp.MinLOD = 0.0f;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 2;
        rs.pParameters = params;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &samp;
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
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rootSignature_.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.RasterizerState.MultisampleEnable = TRUE;
        pso.InputLayout = { layout, 3 };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.SampleDesc.Count = kMsaaCount;
        ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipelineState_)), "CreatePSO");

        viewport_ = { 0.f, 0.f, (float)kWidth, (float)kHeight, 0.f, 1.f };
        scissor_ = { 0, 0, (LONG)kWidth, (LONG)kHeight };
    }

    static void FaceNormal(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz, float* n)
    {
        const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
        const float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;
        const float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f)
        {
            nx /= len;
            ny /= len;
            nz /= len;
        }
        n[0] = nx;
        n[1] = ny;
        n[2] = nz;
    }

    static Vertex MakeVert(float x, float y, float z, const float* n, float u, float v)
    {
        Vertex vert{};
        vert.pos[0] = x;
        vert.pos[1] = y;
        vert.pos[2] = z;
        vert.nrm[0] = n[0];
        vert.nrm[1] = n[1];
        vert.nrm[2] = n[2];
        vert.uv[0] = u;
        vert.uv[1] = v;
        return vert;
    }

    void CreateGeometry()
    {
        const float ax = 0.0f, ay = 1.85f, az = 0.0f;
        const float bx = -0.85f, by = 0.82f, bz = 0.55f;
        const float cx = 0.85f, cy = 0.82f, cz = 0.55f;
        const float dx = 0.0f, dy = 0.82f, dz = -0.85f;
        const float gy = 0.0f;
        float n0[3], n1[3], n2[3], n3[3], ng[3] = { 0, 1, 0 };
        FaceNormal(ax, ay, az, bx, by, bz, cx, cy, cz, n0);
        FaceNormal(ax, ay, az, cx, cy, cz, dx, dy, dz, n1);
        FaceNormal(ax, ay, az, dx, dy, dz, bx, by, bz, n2);
        FaceNormal(bx, by, bz, dx, dy, dz, cx, cy, cz, n3);

        Vertex verts[18] = {
            MakeVert(ax, ay, az, n0, 0.5f, 1.0f), MakeVert(bx, by, bz, n0, 0.0f, 0.0f), MakeVert(cx, cy, cz, n0, 1.0f, 0.0f),
            MakeVert(ax, ay, az, n1, 0.5f, 1.0f), MakeVert(cx, cy, cz, n1, 0.0f, 0.0f), MakeVert(dx, dy, dz, n1, 1.0f, 0.0f),
            MakeVert(ax, ay, az, n2, 0.5f, 1.0f), MakeVert(dx, dy, dz, n2, 0.0f, 0.0f), MakeVert(bx, by, bz, n2, 1.0f, 0.0f),
            MakeVert(bx, by, bz, n3, 0.0f, 0.0f), MakeVert(dx, dy, dz, n3, 1.0f, 0.0f), MakeVert(cx, cy, cz, n3, 0.5f, 1.0f),
            MakeVert(-8.0f, gy, -8.0f, ng, 0.0f, 0.0f), MakeVert(8.0f, gy, -8.0f, ng, 8.0f, 0.0f), MakeVert(8.0f, gy, 8.0f, ng, 8.0f, 8.0f),
            MakeVert(-8.0f, gy, -8.0f, ng, 0.0f, 0.0f), MakeVert(8.0f, gy, 8.0f, ng, 8.0f, 8.0f), MakeVert(-8.0f, gy, 8.0f, ng, 0.0f, 8.0f),
        };
        pyramidCount_ = 12;
        groundCount_ = 6;

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

    void LoadWicPixels(const wchar_t* path, std::vector<UINT>& pixels, UINT& w, UINT& h)
    {
        ComPtr<IWICImagingFactory> wic;
        ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
            "WIC factory");
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
        {
            char buf[160];
            sprintf_s(buf, "Cannot open texture file");
            throw std::runtime_error(buf);
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        ThrowIfFailed(decoder->GetFrame(0, &frame), "WIC GetFrame");
        ComPtr<IWICFormatConverter> converter;
        ThrowIfFailed(wic->CreateFormatConverter(&converter), "WIC converter");
        ThrowIfFailed(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
            "WIC Initialize");
        ThrowIfFailed(converter->GetSize(&w, &h), "WIC GetSize");
        pixels.resize(static_cast<size_t>(w) * h);
        ThrowIfFailed(converter->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(pixels.data())),
            "WIC CopyPixels");
    }

    void CreateGpuTexture(
        const std::vector<UINT>& pixels, UINT w, UINT h,
        ComPtr<ID3D12Resource>& texture, ComPtr<ID3D12Resource>& upload,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout)
    {
        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = w;
        texDesc.Height = h;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;

        D3D12_HEAP_PROPERTIES def{};
        def.Type = D3D12_HEAP_TYPE_DEFAULT;
        ThrowIfFailed(device_->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
            "Create texture");

        UINT numRows = 0;
        UINT64 rowSize = 0, total = 0;
        device_->GetCopyableFootprints(&texDesc, 0, 1, 0, &layout, &numRows, &rowSize, &total);

        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buf{};
        buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width = total;
        buf.Height = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels = 1;
        buf.SampleDesc.Count = 1;
        buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device_->CreateCommittedResource(
            &up, D3D12_HEAP_FLAG_NONE, &buf,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
            "Create texture upload");

        BYTE* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        ThrowIfFailed(upload->Map(0, &none, reinterpret_cast<void**>(&mapped)), "Map upload");
        for (UINT y = 0; y < h; ++y)
        {
            memcpy(mapped + layout.Offset + y * layout.Footprint.RowPitch,
                reinterpret_cast<const BYTE*>(pixels.data()) + y * w * 4, w * 4);
        }
        upload->Unmap(0, nullptr);
    }

    void RecordTextureCopy(
        ID3D12Resource* texture, ID3D12Resource* upload,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = layout;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list_->ResourceBarrier(1, &barrier);
    }

    void CreateTexture()
    {
        std::vector<UINT> brickPixels, groundPixels;
        UINT bw = 0, bh = 0, gw = 0, gh = 0;
        LoadWicPixels(L"brick.jpg", brickPixels, bw, bh);
        LoadWicPixels(L"ground.jpg", groundPixels, gw, gh);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT brickLayout{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT groundLayout{};
        CreateGpuTexture(brickPixels, bw, bh, texturePyramid_, uploadPyramid_, brickLayout);
        CreateGpuTexture(groundPixels, gw, gh, textureGround_, uploadGround_, groundLayout);

        ThrowIfFailed(allocator_[0]->Reset(), "Reset allocator tex");
        ThrowIfFailed(list_->Reset(allocator_[0].Get(), nullptr), "Reset list tex");
        RecordTextureCopy(texturePyramid_.Get(), uploadPyramid_.Get(), brickLayout);
        RecordTextureCopy(textureGround_.Get(), uploadGround_.Get(), groundLayout);
        ThrowIfFailed(list_->Close(), "Close list tex");
        ID3D12CommandList* lists[] = { list_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        WaitForGpu();

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = 2;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_)), "Create SRV heap");
        srvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateShaderResourceView(texturePyramid_.Get(), &srv, cpu);
        cpu.ptr += srvSize_;
        device_->CreateShaderResourceView(textureGround_.Get(), &srv, cpu);
    }

    void CreateFence()
    {
        ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) throw std::runtime_error("CreateEventW failed");
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
        ThrowIfFailed(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"HelloTriangleWindow";
        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowExW(
            0, wc.lpszClassName, L"D3D12 Pyramid",
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
        CoUninitialize();
        return 0;
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "D3D12", MB_ICONERROR);
        return 1;
    }
}
