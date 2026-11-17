#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <DirectXMath.h>
using namespace DirectX;

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kMsaaCount = 4;
constexpr UINT kShadowSize = 4096;
constexpr float kSponzaScale = 0.01f;
constexpr UINT kFramesInFlight = 2;
constexpr UINT kCbStride = 256;
constexpr UINT kCbSlotsPerFrame = 512;

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
    XMFLOAT4X4 lightVP;
    float lightX;
    float lightY;
    float lightZ;
    float ambient;
    float lightR;
    float lightG;
    float lightB;
    float shadowTexel;
    float normalBias;
    float metallic;
    float roughness;
    float pad0;
    float camX;
    float camY;
    float camZ;
    float pad1;
};
static_assert(sizeof(SceneCB) <= kCbStride, "SceneCB must fit one ring slot");

struct Mesh
{
    ComPtr<ID3D11Buffer> vb;
    UINT vertexCount = 0;
    ComPtr<ID3D11ShaderResourceView> srvAlbedo;
    ComPtr<ID3D11ShaderResourceView> srvNormal;
    float metallic = 0.0f;
    float roughness = 0.5f;
};

class Gpu
{
public:
    explicit Gpu(HWND hwnd) : hwnd_(hwnd)
    {
        CreateDevice();
        CreatePipeline();
        CreateShadowMap();
        LoadSponza();
    }

    void Render()
    {
        XMMATRIX world = XMMatrixIdentity();
        XMMATRIX view = UpdateCamera();
        XMMATRIX proj = XMMatrixPerspectiveFovLH(
            XMConvertToRadians(60.0f),
            (float)kWidth / (float)kHeight,
            0.08f, far_);

        XMVECTOR lightDir = XMVector3Normalize(XMVectorSet(0.22f, 1.0f, 0.18f, 0.0f));
        XMVECTOR bmin = XMLoadFloat3(&boundsMin_);
        XMVECTOR bmax = XMLoadFloat3(&boundsMax_);
        XMVECTOR center = XMVectorScale(XMVectorAdd(bmin, bmax), 0.5f);
        XMVECTOR ext = XMVectorScale(XMVectorSubtract(bmax, bmin), 0.5f);
        float radius = XMVectorGetX(XMVector3Length(ext));
        XMVECTOR lightEye = XMVectorAdd(center, XMVectorScale(lightDir, radius * 2.2f));
        XMMATRIX lightView = XMMatrixLookAtLH(lightEye, center, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        float ortho = radius * 2.3f;
        XMMATRIX lightProj = XMMatrixOrthographicLH(ortho, ortho, 0.5f, radius * 5.0f);
        XMMATRIX lightVP = lightView * lightProj;

        SceneCB cb{};
        XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
        XMStoreFloat4x4(&cb.lightVP, XMMatrixTranspose(lightVP));
        cb.lightX = XMVectorGetX(lightDir);
        cb.lightY = XMVectorGetY(lightDir);
        cb.lightZ = XMVectorGetZ(lightDir);
        cb.ambient = 0.22f;
        cb.lightR = 4.4f;
        cb.lightG = 4.0f;
        cb.lightB = 3.4f;
        cb.shadowTexel = 1.0f / (float)kShadowSize;
        cb.normalBias = 0.04f;
        cb.camX = camX_;
        cb.camY = camY_;
        cb.camZ = camZ_;

        BeginCbFrame();
        XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(world * lightVP));
        const UINT shadowSlot = PushCb(cb);
        XMStoreFloat4x4(&cb.mvp, XMMatrixTranspose(world * view * proj));
        cbMeshSlots_.resize(meshes_.size());
        for (size_t i = 0; i < meshes_.size(); ++i)
        {
            cb.metallic = meshes_[i].metallic;
            cb.roughness = meshes_[i].roughness;
            cbMeshSlots_[i] = PushCb(cb);
        }
        EndCbFrame();

        DrawShadowPass(shadowSlot);
        DrawScenePass();

        ThrowIfFailed(swapchain_->Present(1, 0), "Present");
        frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
    }

private:
    HWND hwnd_{};
    float camX_ = 0.0f;
    float camY_ = 1.8f;
    float camZ_ = 0.0f;
    float yaw_ = XM_PIDIV2;
    float pitch_ = 0.0f;
    float far_ = 200.0f;
    bool captureMouse_ = false;
    bool cursorHidden_ = false;
    bool skipMouse_ = true;
    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER qpcLast_{};
    XMFLOAT3 boundsMin_{};
    XMFLOAT3 boundsMax_{};
    std::vector<Mesh> meshes_;
    std::vector<UINT> cbMeshSlots_;
    UINT frameIndex_ = 0;
    UINT cbSlot_ = 0;
    BYTE* cbMapped_ = nullptr;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext1> context1_;
    ComPtr<IDXGISwapChain> swapchain_;
    ComPtr<ID3D11Texture2D> msaaColor_;
    ComPtr<ID3D11RenderTargetView> msaaRtv_;
    ComPtr<ID3D11Texture2D> backBuffer_;
    ComPtr<ID3D11Texture2D> depthBuffer_;
    ComPtr<ID3D11DepthStencilView> dsv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11VertexShader> shadowVs_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Buffer> cbRing_[kFramesInFlight];
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11SamplerState> shadowSampler_;
    ComPtr<ID3D11RasterizerState> rsScene_;
    ComPtr<ID3D11RasterizerState> rsShadow_;
    ComPtr<ID3D11DepthStencilState> dss_;
    ComPtr<ID3D11ShaderResourceView> srvWhite_;
    ComPtr<ID3D11ShaderResourceView> srvFlatNormal_;
    ComPtr<ID3D11Texture2D> shadowTex_;
    ComPtr<ID3D11DepthStencilView> shadowDsv_;
    ComPtr<ID3D11ShaderResourceView> shadowSrv_;
    D3D11_VIEWPORT sceneViewport_{};
    D3D11_VIEWPORT shadowViewport_{};

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

        float speed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 12.0f : 4.0f;
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
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = kWidth;
        sd.BufferDesc.Height = kHeight;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd_;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT flags = 0;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        HRESULT hrDevice = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, &sd,
            &swapchain_, &device_, &level, &context_);
        if (FAILED(hrDevice) && (flags & D3D11_CREATE_DEVICE_DEBUG))
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hrDevice = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                nullptr, 0, D3D11_SDK_VERSION, &sd,
                &swapchain_, &device_, &level, &context_);
        }
        ThrowIfFailed(hrDevice, "D3D11CreateDeviceAndSwapChain");
        ThrowIfFailed(context_.As(&context1_), "ID3D11DeviceContext1");

        ThrowIfFailed(swapchain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_)), "GetBuffer");

        D3D11_TEXTURE2D_DESC msaa{};
        msaa.Width = kWidth;
        msaa.Height = kHeight;
        msaa.MipLevels = 1;
        msaa.ArraySize = 1;
        msaa.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        msaa.SampleDesc.Count = kMsaaCount;
        msaa.Usage = D3D11_USAGE_DEFAULT;
        msaa.BindFlags = D3D11_BIND_RENDER_TARGET;
        ThrowIfFailed(device_->CreateTexture2D(&msaa, nullptr, &msaaColor_), "Create MSAA color");
        ThrowIfFailed(device_->CreateRenderTargetView(msaaColor_.Get(), nullptr, &msaaRtv_), "Create MSAA RTV");

        D3D11_TEXTURE2D_DESC depth{};
        depth.Width = kWidth;
        depth.Height = kHeight;
        depth.MipLevels = 1;
        depth.ArraySize = 1;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.SampleDesc.Count = kMsaaCount;
        depth.Usage = D3D11_USAGE_DEFAULT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ThrowIfFailed(device_->CreateTexture2D(&depth, nullptr, &depthBuffer_), "Create depth");
        ThrowIfFailed(device_->CreateDepthStencilView(depthBuffer_.Get(), nullptr, &dsv_), "Create DSV");

        sceneViewport_ = { 0.0f, 0.0f, (float)kWidth, (float)kHeight, 0.0f, 1.0f };
        shadowViewport_ = { 0.0f, 0.0f, (float)kShadowSize, (float)kShadowSize, 0.0f, 1.0f };
    }

    ComPtr<ID3DBlob> CompileShader(const char* entry, const char* target)
    {
        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, entry, target, 0, 0, &blob, &err);
        if (FAILED(hr))
            throw std::runtime_error(err ? (const char*)err->GetBufferPointer() : "shader compile failed");
        return blob;
    }

    void CreatePipeline()
    {
        ComPtr<ID3DBlob> vsBlob = CompileShader("VSMain", "vs_5_0");
        ComPtr<ID3DBlob> psBlob = CompileShader("PSMain", "ps_5_0");
        ComPtr<ID3DBlob> shadowBlob = CompileShader("ShadowVS", "vs_5_0");
        ThrowIfFailed(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_), "VS");
        ThrowIfFailed(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_), "PS");
        ThrowIfFailed(device_->CreateVertexShader(shadowBlob->GetBufferPointer(), shadowBlob->GetBufferSize(), nullptr, &shadowVs_), "ShadowVS");

        D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ThrowIfFailed(device_->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout_), "InputLayout");

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = kCbStride * kCbSlotsPerFrame;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        for (UINT i = 0; i < kFramesInFlight; ++i)
            ThrowIfFailed(device_->CreateBuffer(&cbd, nullptr, &cbRing_[i]), "Create CB ring");

        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_ANISOTROPIC;
        samp.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samp.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samp.MaxAnisotropy = 16;
        samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samp.MinLOD = 0.0f;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        ThrowIfFailed(device_->CreateSamplerState(&samp, &sampler_), "Create sampler");

        D3D11_SAMPLER_DESC ss{};
        ss.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        ss.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        ss.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        ss.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        ss.BorderColor[0] = ss.BorderColor[1] = ss.BorderColor[2] = ss.BorderColor[3] = 1.0f;
        ss.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        ThrowIfFailed(device_->CreateSamplerState(&ss, &shadowSampler_), "Create shadow sampler");

        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.FrontCounterClockwise = TRUE;
        rs.DepthClipEnable = TRUE;
        rs.MultisampleEnable = TRUE;
        ThrowIfFailed(device_->CreateRasterizerState(&rs, &rsScene_), "RS scene");
        rs.CullMode = D3D11_CULL_FRONT;
        rs.MultisampleEnable = FALSE;
        rs.DepthBias = 800;
        rs.SlopeScaledDepthBias = 2.0f;
        rs.DepthBiasClamp = 0.0f;
        ThrowIfFailed(device_->CreateRasterizerState(&rs, &rsShadow_), "RS shadow");

        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_LESS;
        ThrowIfFailed(device_->CreateDepthStencilState(&ds, &dss_), "DSS");
    }

    void CreateShadowMap()
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kShadowSize;
        td.Height = kShadowSize;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ThrowIfFailed(device_->CreateTexture2D(&td, nullptr, &shadowTex_), "Create shadow tex");

        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ThrowIfFailed(device_->CreateDepthStencilView(shadowTex_.Get(), &dsv, &shadowDsv_), "Create shadow DSV");

        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        ThrowIfFailed(device_->CreateShaderResourceView(shadowTex_.Get(), &srv, &shadowSrv_), "Create shadow SRV");
    }

    static void FaceNormal(const Vertex& a, const Vertex& b, const Vertex& c, float* n)
    {
        const float e1x = b.pos[0] - a.pos[0], e1y = b.pos[1] - a.pos[1], e1z = b.pos[2] - a.pos[2];
        const float e2x = c.pos[0] - a.pos[0], e2y = c.pos[1] - a.pos[1], e2z = c.pos[2] - a.pos[2];
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

    static std::string JoinPath(const std::string& dir, std::string rel)
    {
        for (char& c : rel)
        {
            if (c == '\\') c = '/';
        }
        if (dir.empty()) return rel;
        if (!rel.empty() && (rel[0] == '/' || (rel.size() > 1 && rel[1] == ':')))
            return rel;
        if (dir.back() == '/' || dir.back() == '\\')
            return dir + rel;
        return dir + "/" + rel;
    }

    ComPtr<ID3D11ShaderResourceView> CreateTextureSrv(const BYTE* src, UINT w, UINT h, DXGI_FORMAT format)
    {
        UINT levels = 1;
        UINT tw = w, th = h;
        while (levels < 8 && (tw > 1 || th > 1))
        {
            tw = tw > 1 ? tw / 2 : 1;
            th = th > 1 ? th / 2 : 1;
            ++levels;
        }

        std::vector<std::vector<BYTE>> mips(levels);
        std::vector<UINT> mw(levels), mh(levels);
        mw[0] = w;
        mh[0] = h;
        mips[0].assign(src, src + static_cast<size_t>(w) * h * 4);
        for (UINT level = 1; level < levels; ++level)
        {
            const UINT pw = mw[level - 1];
            const UINT ph = mh[level - 1];
            const UINT cw = pw > 1 ? pw / 2 : 1;
            const UINT ch = ph > 1 ? ph / 2 : 1;
            mw[level] = cw;
            mh[level] = ch;
            mips[level].resize(static_cast<size_t>(cw) * ch * 4);
            const BYTE* s = mips[level - 1].data();
            BYTE* d = mips[level].data();
            for (UINT y = 0; y < ch; ++y)
            {
                for (UINT x = 0; x < cw; ++x)
                {
                    const UINT x0 = x * 2;
                    const UINT y0 = y * 2;
                    const UINT x1 = (x0 + 1 < pw) ? x0 + 1 : x0;
                    const UINT y1 = (y0 + 1 < ph) ? y0 + 1 : y0;
                    for (int c = 0; c < 4; ++c)
                    {
                        const UINT sum =
                            s[(y0 * pw + x0) * 4 + c] +
                            s[(y0 * pw + x1) * 4 + c] +
                            s[(y1 * pw + x0) * 4 + c] +
                            s[(y1 * pw + x1) * 4 + c];
                        d[(y * cw + x) * 4 + c] = static_cast<BYTE>(sum / 4);
                    }
                }
            }
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = levels;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        std::vector<D3D11_SUBRESOURCE_DATA> init(levels);
        for (UINT i = 0; i < levels; ++i)
        {
            init[i].pSysMem = mips[i].data();
            init[i].SysMemPitch = mw[i] * 4;
        }

        ComPtr<ID3D11Texture2D> tex;
        ThrowIfFailed(device_->CreateTexture2D(&td, init.data(), &tex), "CreateTexture2D");
        ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = levels;
        ThrowIfFailed(device_->CreateShaderResourceView(tex.Get(), &sd, &srv), "Create SRV");
        return srv;
    }

    ComPtr<ID3D11ShaderResourceView> CreateSolidSrv(float r, float g, float b, DXGI_FORMAT format)
    {
        BYTE px[4] = {
            (BYTE)std::clamp(r * 255.0f, 0.0f, 255.0f),
            (BYTE)std::clamp(g * 255.0f, 0.0f, 255.0f),
            (BYTE)std::clamp(b * 255.0f, 0.0f, 255.0f),
            255
        };
        return CreateTextureSrv(px, 1, 1, format);
    }

    ComPtr<ID3D11ShaderResourceView> LoadTextureFile(const std::string& path, bool srgb)
    {
        int w = 0, h = 0, n = 0;
        stbi_uc* data = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (!data || w <= 0 || h <= 0)
        {
            if (data) stbi_image_free(data);
            return {};
        }
        DXGI_FORMAT format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        auto srv = CreateTextureSrv(data, (UINT)w, (UINT)h, format);
        stbi_image_free(data);
        return srv;
    }

    static void GuessPbr(const std::string& name, float& metallic, float& roughness)
    {
        metallic = 0.0f;
        roughness = 0.48f;
        if (name.find("fabric") != std::string::npos || name.find("curtain") != std::string::npos)
            roughness = 0.78f;
        else if (name.find("leaf") != std::string::npos || name.find("plant") != std::string::npos)
            roughness = 0.72f;
        else if (name.find("chain") != std::string::npos)
        {
            metallic = 0.85f;
            roughness = 0.32f;
        }
        else if (name.find("flagpole") != std::string::npos)
        {
            metallic = 0.55f;
            roughness = 0.38f;
        }
        else if (name.find("floor") != std::string::npos || name.find("column") != std::string::npos ||
            name.find("arch") != std::string::npos || name.find("vase") != std::string::npos)
            roughness = 0.34f;
        else if (name.find("bricks") != std::string::npos || name.find("details") != std::string::npos)
            roughness = 0.58f;
        else if (name.find("roof") != std::string::npos || name.find("ceiling") != std::string::npos)
            roughness = 0.62f;
        else if (name.find("lion") != std::string::npos)
            roughness = 0.40f;
    }

    ComPtr<ID3D11Buffer> CreateVb(const std::vector<Vertex>& verts)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)(verts.size() * sizeof(Vertex));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = verts.data();
        ComPtr<ID3D11Buffer> vb;
        ThrowIfFailed(device_->CreateBuffer(&bd, &init, &vb), "Create VB");
        return vb;
    }

    void LoadSponza()
    {
        srvWhite_ = CreateSolidSrv(1.0f, 1.0f, 1.0f, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        srvFlatNormal_ = CreateSolidSrv(0.5f, 0.5f, 1.0f, DXGI_FORMAT_R8G8B8A8_UNORM);

        const std::string objPath = "Assets/sponza/sponza.obj";
        tinyobj::ObjReaderConfig config;
        config.triangulate = true;
        config.vertex_color = false;
        tinyobj::ObjReader reader;
        if (!reader.ParseFromFile(objPath, config))
            throw std::runtime_error(reader.Error().empty() ? "Failed to load Sponza OBJ" : reader.Error());

        const tinyobj::attrib_t& attrib = reader.GetAttrib();
        const std::vector<tinyobj::shape_t>& shapes = reader.GetShapes();
        const std::vector<tinyobj::material_t>& materials = reader.GetMaterials();

        std::vector<std::vector<Vertex>> byMat(materials.size() + 1);
        boundsMin_ = { FLT_MAX, FLT_MAX, FLT_MAX };
        boundsMax_ = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

        auto consider = [&](float x, float y, float z)
        {
            boundsMin_.x = std::min(boundsMin_.x, x);
            boundsMin_.y = std::min(boundsMin_.y, y);
            boundsMin_.z = std::min(boundsMin_.z, z);
            boundsMax_.x = std::max(boundsMax_.x, x);
            boundsMax_.y = std::max(boundsMax_.y, y);
            boundsMax_.z = std::max(boundsMax_.z, z);
        };

        for (const auto& shape : shapes)
        {
            size_t indexOffset = 0;
            for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f)
            {
                const int fv = shape.mesh.num_face_vertices[f];
                int matId = 0;
                if (f < shape.mesh.material_ids.size())
                    matId = shape.mesh.material_ids[f];
                if (matId < 0 || matId >= (int)materials.size())
                    matId = (int)materials.size();

                Vertex tri[3]{};
                int useN = 0;
                for (int v = 0; v < fv; ++v)
                {
                    const tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                    Vertex vert{};
                    vert.pos[0] = attrib.vertices[3 * idx.vertex_index + 0] * kSponzaScale;
                    vert.pos[1] = attrib.vertices[3 * idx.vertex_index + 1] * kSponzaScale;
                    vert.pos[2] = attrib.vertices[3 * idx.vertex_index + 2] * kSponzaScale;
                    consider(vert.pos[0], vert.pos[1], vert.pos[2]);
                    if (idx.normal_index >= 0)
                    {
                        vert.nrm[0] = attrib.normals[3 * idx.normal_index + 0];
                        vert.nrm[1] = attrib.normals[3 * idx.normal_index + 1];
                        vert.nrm[2] = attrib.normals[3 * idx.normal_index + 2];
                        ++useN;
                    }
                    if (idx.texcoord_index >= 0)
                    {
                        vert.uv[0] = attrib.texcoords[2 * idx.texcoord_index + 0];
                        vert.uv[1] = 1.0f - attrib.texcoords[2 * idx.texcoord_index + 1];
                    }
                    if (v < 3)
                        tri[v] = vert;
                }
                if (fv == 3)
                {
                    if (useN < 3)
                    {
                        float n[3];
                        FaceNormal(tri[0], tri[1], tri[2], n);
                        for (int i = 0; i < 3; ++i)
                        {
                            tri[i].nrm[0] = n[0];
                            tri[i].nrm[1] = n[1];
                            tri[i].nrm[2] = n[2];
                        }
                    }
                    byMat[matId].push_back(tri[0]);
                    byMat[matId].push_back(tri[1]);
                    byMat[matId].push_back(tri[2]);
                }
                indexOffset += fv;
            }
        }

        std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> texCache;
        auto loadCached = [&](const std::string& rel, bool srgb, const tinyobj::material_t* mat) -> ComPtr<ID3D11ShaderResourceView>
        {
            if (rel.empty())
            {
                if (!mat)
                    return srgb ? srvWhite_ : srvFlatNormal_;
                if (srgb)
                    return CreateSolidSrv(mat->diffuse[0], mat->diffuse[1], mat->diffuse[2], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
                return srvFlatNormal_;
            }
            std::string path = JoinPath("Assets/sponza", rel);
            std::string key = path + (srgb ? "|srgb" : "|lin");
            auto it = texCache.find(key);
            if (it != texCache.end())
                return it->second;
            auto srv = LoadTextureFile(path, srgb);
            if (!srv)
            {
                if (srgb && mat)
                    srv = CreateSolidSrv(mat->diffuse[0], mat->diffuse[1], mat->diffuse[2], DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
                else
                    srv = srgb ? srvWhite_ : srvFlatNormal_;
            }
            texCache[key] = srv;
            return srv;
        };

        meshes_.clear();
        for (int i = 0; i < (int)byMat.size(); ++i)
        {
            if (byMat[i].empty())
                continue;
            Mesh mesh;
            mesh.vertexCount = (UINT)byMat[i].size();
            mesh.vb = CreateVb(byMat[i]);
            if (i >= 0 && i < (int)materials.size())
            {
                const auto& mat = materials[i];
                mesh.srvAlbedo = loadCached(mat.diffuse_texname, true, &mat);
                std::string nrm = mat.normal_texname;
                if (nrm.empty()) nrm = mat.bump_texname;
                if (nrm.empty()) nrm = mat.displacement_texname;
                mesh.srvNormal = loadCached(nrm, false, &mat);
                GuessPbr(mat.name, mesh.metallic, mesh.roughness);
            }
            else
            {
                mesh.srvAlbedo = srvWhite_;
                mesh.srvNormal = srvFlatNormal_;
            }
            meshes_.push_back(std::move(mesh));
        }
        if (meshes_.empty())
            throw std::runtime_error("Sponza produced no meshes");

        camX_ = (boundsMin_.x + boundsMax_.x) * 0.5f;
        camY_ = boundsMin_.y + 1.7f;
        camZ_ = (boundsMin_.z + boundsMax_.z) * 0.5f;
        yaw_ = XM_PIDIV2;
        pitch_ = 0.0f;
        XMVECTOR ext = XMVectorScale(XMVectorSubtract(XMLoadFloat3(&boundsMax_), XMLoadFloat3(&boundsMin_)), 0.5f);
        far_ = std::max(80.0f, XMVectorGetX(XMVector3Length(ext)) * 4.0f);
    }

    void BeginCbFrame()
    {
        cbSlot_ = 0;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context_->Map(cbRing_[frameIndex_].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map CB ring");
        cbMapped_ = static_cast<BYTE*>(mapped.pData);
    }

    UINT PushCb(const SceneCB& cb)
    {
        if (cbSlot_ >= kCbSlotsPerFrame)
            throw std::runtime_error("CB ring overflow");
        memcpy(cbMapped_ + cbSlot_ * kCbStride, &cb, sizeof(cb));
        return cbSlot_++;
    }

    void EndCbFrame()
    {
        context_->Unmap(cbRing_[frameIndex_].Get(), 0);
        cbMapped_ = nullptr;
    }

    void BindCb(UINT slot)
    {
        ID3D11Buffer* buf = cbRing_[frameIndex_].Get();
        const UINT firstConstant = (slot * kCbStride) / 16;
        const UINT numConstants = (sizeof(SceneCB) + 15u) / 16u;
        context1_->VSSetConstantBuffers1(0, 1, &buf, &firstConstant, &numConstants);
        context1_->PSSetConstantBuffers1(0, 1, &buf, &firstConstant, &numConstants);
    }

    void DrawMeshes(bool shadowPass)
    {
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(layout_.Get());

        if (shadowPass)
        {
            context_->VSSetShader(shadowVs_.Get(), nullptr, 0);
            context_->PSSetShader(nullptr, nullptr, 0);
            for (const auto& mesh : meshes_)
            {
                context_->IASetVertexBuffers(0, 1, mesh.vb.GetAddressOf(), &stride, &offset);
                context_->Draw(mesh.vertexCount, 0);
            }
            return;
        }

        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->PSSetSamplers(1, 1, shadowSampler_.GetAddressOf());
        context_->PSSetShaderResources(1, 1, shadowSrv_.GetAddressOf());

        for (size_t i = 0; i < meshes_.size(); ++i)
        {
            BindCb(cbMeshSlots_[i]);
            ID3D11ShaderResourceView* albedo = meshes_[i].srvAlbedo ? meshes_[i].srvAlbedo.Get() : srvWhite_.Get();
            ID3D11ShaderResourceView* nrm = meshes_[i].srvNormal ? meshes_[i].srvNormal.Get() : srvFlatNormal_.Get();
            context_->IASetVertexBuffers(0, 1, meshes_[i].vb.GetAddressOf(), &stride, &offset);
            context_->PSSetShaderResources(0, 1, &albedo);
            context_->PSSetShaderResources(2, 1, &nrm);
            context_->Draw(meshes_[i].vertexCount, 0);
        }

        ID3D11ShaderResourceView* none[] = { nullptr, nullptr, nullptr };
        context_->PSSetShaderResources(0, 3, none);
    }

    void DrawShadowPass(UINT cbSlot)
    {
        ID3D11RenderTargetView* none = nullptr;
        context_->OMSetRenderTargets(1, &none, shadowDsv_.Get());
        context_->ClearDepthStencilView(shadowDsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        context_->RSSetViewports(1, &shadowViewport_);
        context_->RSSetState(rsShadow_.Get());
        context_->OMSetDepthStencilState(dss_.Get(), 0);
        BindCb(cbSlot);
        DrawMeshes(true);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void DrawScenePass()
    {
        context_->OMSetRenderTargets(1, msaaRtv_.GetAddressOf(), dsv_.Get());
        const float clear[4] = { 0.02f, 0.02f, 0.03f, 1.0f };
        context_->ClearRenderTargetView(msaaRtv_.Get(), clear);
        context_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        context_->RSSetViewports(1, &sceneViewport_);
        context_->RSSetState(rsScene_.Get());
        context_->OMSetDepthStencilState(dss_.Get(), 0);
        DrawMeshes(false);
        context_->ResolveSubresource(backBuffer_.Get(), 0, msaaColor_.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
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
        wc.lpszClassName = L"D3D11SponzaWindow";
        RegisterClassExW(&wc);

        HWND hwnd = CreateWindowExW(
            0, wc.lpszClassName, L"D3D11 Sponza PBR",
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
        MessageBoxA(nullptr, e.what(), "D3D11", MB_ICONERROR);
        return 1;
    }
}
