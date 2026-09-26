#include "DirectXApp.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include "d3dUtil.h"
#include <string>
#include <DirectXMath.h>
#include "Parser.h"
#include "ThrowIfFailed.h"
#include "TgaLoader.h"
#include "ModelLoader.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace DirectX;

namespace
{
    std::filesystem::path GetExeDirectory()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    std::string WithTrailingSlash(const std::filesystem::path& path)
    {
        std::string result = path.lexically_normal().string();
        if (!result.empty() && result.back() != '\\' && result.back() != '/')
            result += "\\";
        return result;
    }

    constexpr float TessellationMaxFactor = 8.0f;
    constexpr float TessellationMinFactor = 1.0f;
    constexpr float TessellationNearDistance = 0.0f;
    constexpr float TessellationFarDistance = 0.5f;
    constexpr float EarthDisplacementScale = 0.06f;
    constexpr float EarthNormalStrength = 1.0f;
    constexpr float CameraMinSpeed = 0.1f;
    constexpr float CameraMaxSpeed = 20.0f;

    std::string ResolveEarthDirectory()
    {
        std::filesystem::path cwd = std::filesystem::current_path();
        std::filesystem::path exeDir = GetExeDirectory();
        const std::filesystem::path relative = "Assets/Earth";

        std::vector<std::filesystem::path> candidates =
        {
            cwd / relative,
            cwd / "Project1" / relative,
            cwd / ".." / "Project1" / relative,
            exeDir / relative,
            exeDir / ".." / relative,
            exeDir / ".." / ".." / relative,
            exeDir / ".." / "Project1" / relative,
            exeDir / ".." / ".." / "Project1" / relative
        };

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate / "Earth.fbx") &&
                std::filesystem::exists(candidate / "Earth.mtl"))
            {
                return WithTrailingSlash(candidate);
            }
        }

        return {};
    }

    void StartupLog(const std::string& message)
    {
        OutputDebugStringA((message + "\n").c_str());

        std::ofstream log(GetExeDirectory() / "Project1_startup.log", std::ios::app);
        if (log)
            log << message << '\n';
    }
}

// Вспомогательная структура для барьеров
struct CD3DX12_RESOURCE_BARRIER_HELPER {
    static D3D12_RESOURCE_BARRIER Transition(
        _In_ ID3D12Resource* pResource,
        D3D12_RESOURCE_STATES stateBefore,
        D3D12_RESOURCE_STATES stateAfter,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = pResource;
        barrier.Transition.StateBefore = stateBefore;
        barrier.Transition.StateAfter = stateAfter;
        barrier.Transition.Subresource = subresource;
        return barrier;
    }
};

// Вспомогательные структуры CD3DX12 (как на слайдах)
struct CD3DX12_DEFAULT {};
extern const DECLSPEC_SELECTANY CD3DX12_DEFAULT D3D12_DEFAULT;

struct CD3DX12_RASTERIZER_DESC : public D3D12_RASTERIZER_DESC
{
    CD3DX12_RASTERIZER_DESC() = default;
    explicit CD3DX12_RASTERIZER_DESC(const D3D12_RASTERIZER_DESC& o) : D3D12_RASTERIZER_DESC(o) {}
    explicit CD3DX12_RASTERIZER_DESC(CD3DX12_DEFAULT)
    {
        FillMode = D3D12_FILL_MODE_SOLID;
        CullMode = D3D12_CULL_MODE_BACK;
        FrontCounterClockwise = FALSE;
        DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        DepthClipEnable = TRUE;
        MultisampleEnable = FALSE;
        AntialiasedLineEnable = FALSE;
        ForcedSampleCount = 0;
        ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    }
};

struct CD3DX12_BLEND_DESC : public D3D12_BLEND_DESC
{
    CD3DX12_BLEND_DESC() = default;
    explicit CD3DX12_BLEND_DESC(const D3D12_BLEND_DESC& o) : D3D12_BLEND_DESC(o) {}
    explicit CD3DX12_BLEND_DESC(CD3DX12_DEFAULT)
    {
        AlphaToCoverageEnable = FALSE;
        IndependentBlendEnable = FALSE;
        const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc =
        {
            FALSE,FALSE,
            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
            D3D12_LOGIC_OP_NOOP,
            D3D12_COLOR_WRITE_ENABLE_ALL,
        };
        for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            RenderTarget[i] = defaultRenderTargetBlendDesc;
    }
};

struct CD3DX12_DEPTH_STENCIL_DESC : public D3D12_DEPTH_STENCIL_DESC
{
    CD3DX12_DEPTH_STENCIL_DESC() = default;
    explicit CD3DX12_DEPTH_STENCIL_DESC(const D3D12_DEPTH_STENCIL_DESC& o) : D3D12_DEPTH_STENCIL_DESC(o) {}
    explicit CD3DX12_DEPTH_STENCIL_DESC(CD3DX12_DEFAULT)
    {
        DepthEnable = TRUE;
        DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        StencilEnable = FALSE;
        StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        const D3D12_DEPTH_STENCILOP_DESC defaultStencilOp =
        { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
        FrontFace = defaultStencilOp;
        BackFace = defaultStencilOp;
    }
};

DirectXApp::DirectXApp(Window& window) : window(window)
{
    // Инициализируем матрицы
    XMStoreFloat4x4(&mWorld, XMMatrixIdentity());
    XMStoreFloat4x4(&mView, XMMatrixIdentity());
    XMStoreFloat4x4(&mProj, XMMatrixIdentity());
}

DirectXApp::~DirectXApp() {
    Shutdown();
}

// =========== Методы мыши ==========
void DirectXApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;

    SetCapture(window.GetHwnd()); // захват мыши
}

void DirectXApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void DirectXApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if (btnState & MK_LBUTTON)
    {
        float sensitivity = 0.005f;

        float dx = (x - mLastMousePos.x) * sensitivity;
        float dy = (y - mLastMousePos.y) * sensitivity;

        mYaw += dx;
        mPitch += dy;

        // ограничение чтобы не переворачивалась
        if (mPitch > XM_PIDIV2 - 0.1f)
            mPitch = XM_PIDIV2 - 0.1f;

        if (mPitch < -XM_PIDIV2 + 0.1f)
            mPitch = -XM_PIDIV2 + 0.1f;
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void DirectXApp::UpdateUvDirectionFromInput()
{
    InputDevice& input = window.GetInputDevice();

    XMFLOAT2 direction = { 0.0f, 0.0f };

    if (input.IsKeyDown(VK_LEFT))
        direction.x -= 1.0f;

    if (input.IsKeyDown(VK_RIGHT))
        direction.x += 1.0f;

    if (input.IsKeyDown(VK_UP))
        direction.y += 1.0f;

    if (input.IsKeyDown(VK_DOWN))
        direction.y -= 1.0f;

    mUvDirection = direction;
}

// =========== Input Layout ===========
void DirectXApp::BuildInputLayout()
{
    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
}

// =========== Шейдеры ===========
void DirectXApp::BuildShaders()
{
    mvsByteCode = d3dUtil::CompileShader(
        L"../Project1/shaders.hlsl",
        nullptr,
        "VS",
        "vs_5_0"
    );

    mpsByteCode = d3dUtil::CompileShader(
        L"../Project1/shaders.hlsl",
        nullptr,
        "PS",
        "ps_5_0"
    );

    OutputDebugStringW(L"Shaders compiled successfully.\n");
}

// =========== Константный буфер и CBV ===========
void DirectXApp::BuildConstantBuffer()
{
    // 1. Создаем UploadBuffer для констант
    mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(
        device.Get(),
        1,
        true
    );

    // 2. Инициализируем матрицу (орфографическая проекция)
    ObjectConstants objConstants;
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX proj = XMMatrixOrthographicLH(10.0f, 10.0f, 0.1f, 100.0f);
    XMMATRIX viewProj = view * proj;
    XMStoreFloat4x4(&objConstants.mWorldViewProj, XMMatrixTranspose(viewProj));

    mObjectCB->CopyData(0, objConstants);

    // 3. Создаем CBV (Constant Buffer View) в куче дескрипторов
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mObjectCB->Resource()->GetGPUVirtualAddress();

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
    cbvDesc.BufferLocation = cbAddress;
    cbvDesc.SizeInBytes = objCBByteSize;

    // Получаем дескриптор из CBV кучи
    D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle = mCbvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateConstantBufferView(&cbvDesc, cbvHandle);

    OutputDebugStringW(L"Constant buffer and CBV created.\n");
}

// =========== Root Signature ===========
void DirectXApp::BuildRootSignature()
{
    // ===== CBV range (b0)
    D3D12_DESCRIPTOR_RANGE cbvRange = {};
    cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbvRange.NumDescriptors = 1;
    cbvRange.BaseShaderRegister = 0;
    cbvRange.RegisterSpace = 0;
    cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // ===== SRV range (t0)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[2];

    // Slot 0 → CBV
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = &cbvRange;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 1 → SRV
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // ===== Static Sampler (s0)
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParameters;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &sampler;
    rootSigDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;

    ThrowIfFailed(D3D12SerializeRootSignature(
        &rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(),
        errorBlob.GetAddressOf()));

    ThrowIfFailed(device->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));
}

// =========== PSO (Pipeline State Object) ===========
void DirectXApp::BuildPSO()
{
    // Создаем описание PSO (как на слайде 20.26.54)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
    ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    // 1. Шейдеры (как на слайде)
    psoDesc.VS = {
        reinterpret_cast<BYTE*>(mvsByteCode->GetBufferPointer()),
        mvsByteCode->GetBufferSize()
    };
    psoDesc.PS = {
        reinterpret_cast<BYTE*>(mpsByteCode->GetBufferPointer()),
        mpsByteCode->GetBufferSize()
    };

    // 2. Input Layout
    psoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };

    // 3. Корневая сигнатура
    psoDesc.pRootSignature = mRootSignature.Get();

    // 4. Растеризатор 
    D3D12_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    rasterDesc.DepthClipEnable = TRUE;

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    // 5. Blend State 
    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;

    auto& rtBlend = blendDesc.RenderTarget[0];
    rtBlend.BlendEnable = TRUE;
    rtBlend.LogicOpEnable = FALSE;
    rtBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // 6. Depth/Stencil State 
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    // 7. Sample Mask
    psoDesc.SampleMask = UINT_MAX;

    // 8. Примитивы
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // 9. Render Targets
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = mBackBufferFormat;

    // 10. Формат Depth/Stencil
    psoDesc.DSVFormat = mDepthStencilFormat;

    // 11. Multisampling
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    // 12. Создание PSO
    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO));
    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create PSO", L"Error", MB_OK);
        return;
    }

    OutputDebugStringW(L"PSO created successfully (Solid Mode).\n");
}

// =========== Wireframe PSO ===========
void DirectXApp::BuildWireframePSO()
{
    // Создаем описание PSO для проволочного каркаса
    D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframePsoDesc;
    ZeroMemory(&wireframePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

    // 1. Шейдеры (те же самые)
    wireframePsoDesc.VS = {
        reinterpret_cast<BYTE*>(mvsByteCode->GetBufferPointer()),
        mvsByteCode->GetBufferSize()
    };
    wireframePsoDesc.PS = {
        reinterpret_cast<BYTE*>(mpsByteCode->GetBufferPointer()),
        mpsByteCode->GetBufferSize()
    };

    // 2. Input Layout
    wireframePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };

    // 3. Корневая сигнатура
    wireframePsoDesc.pRootSignature = mRootSignature.Get();

    // 4. Растеризатор - НАСТРОЙКА ДЛЯ ПРОВОЛОЧНОГО КАРКАСА
    wireframePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    wireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;  // ПРОВОЛОЧНЫЙ КАРКАС
    wireframePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;       // БЕЗ ОБРЕЗКИ

    // 5. Blend State
    wireframePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // 6. Depth/Stencil State
    wireframePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    // 7. Sample Mask
    wireframePsoDesc.SampleMask = UINT_MAX;

    // 8. Примитивы
    wireframePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // 9. Render Targets
    wireframePsoDesc.NumRenderTargets = 1;
    wireframePsoDesc.RTVFormats[0] = mBackBufferFormat;

    // 10. Формат Depth/Stencil
    wireframePsoDesc.DSVFormat = mDepthStencilFormat;

    // 11. Multisampling
    wireframePsoDesc.SampleDesc.Count = 1;
    wireframePsoDesc.SampleDesc.Quality = 0;

    // 12. Создание PSO
    HRESULT hr = device->CreateGraphicsPipelineState(&wireframePsoDesc, IID_PPV_ARGS(&mWireframePSO));
    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create Wireframe PSO", L"Error", MB_OK);
        return;
    }

    OutputDebugStringW(L"Wireframe PSO created successfully.\n");
}
// =========== Остальные методы ===========
void DirectXApp::BuildModel(const std::string& path)
{
    OutputDebugStringA(("Loading model: " + path + "\n").c_str());

    // Очистить старые данные
    mSubmeshes.clear();
    mIndexCount = 0;
    mSceneVertices.clear();
    mSceneIndices.clear();

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

    bool loaded = false;
    std::string loadError;
    if (extension == ".fbx")
        loaded = LoadModelWithAssimp(path, vertices, indices, mSubmeshes, loadError);
    else
        loaded = LoadOBJ(path, vertices, indices, mSubmeshes);

    if (!loaded)
    {
        const std::string message = loadError.empty()
            ? "Failed to load model: " + path
            : "Failed to load model: " + path + "\n" + loadError;
        MessageBoxA(nullptr, message.c_str(), "Error", MB_OK);
        return;
    }

    mIndexCount = static_cast<UINT>(indices.size());
    mSceneVertices = vertices;
    mSceneIndices = indices;

    UINT vbByteSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    UINT ibByteSize = static_cast<UINT>(indices.size() * sizeof(uint32_t));

    // ====================================================
    //                VERTEX BUFFER
    // ====================================================

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vbDesc = {};
    vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Width = vbByteSize;
    vbDesc.Height = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels = 1;
    vbDesc.SampleDesc.Count = 1;
    vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mVertexBufferGPU)));

    void* mappedData = nullptr;
    mVertexBufferGPU->Map(0, nullptr, &mappedData);
    memcpy(mappedData, vertices.data(), vbByteSize);
    mVertexBufferGPU->Unmap(0, nullptr);

    mVertexBufferView.BufferLocation = mVertexBufferGPU->GetGPUVirtualAddress();
    mVertexBufferView.StrideInBytes = sizeof(Vertex);
    mVertexBufferView.SizeInBytes = vbByteSize;

    // ====================================================
    //                INDEX BUFFER
    // ====================================================

    D3D12_RESOURCE_DESC ibDesc = {};
    ibDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ibDesc.Width = ibByteSize;
    ibDesc.Height = 1;
    ibDesc.DepthOrArraySize = 1;
    ibDesc.MipLevels = 1;
    ibDesc.SampleDesc.Count = 1;
    ibDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mIndexBufferGPU)));

    mIndexBufferGPU->Map(0, nullptr, &mappedData);
    memcpy(mappedData, indices.data(), ibByteSize);
    mIndexBufferGPU->Unmap(0, nullptr);

    mIndexBufferView.BufferLocation = mIndexBufferGPU->GetGPUVirtualAddress();
    mIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    mIndexBufferView.SizeInBytes = ibByteSize;
}

void DirectXApp::FitCameraToLoadedScene()
{
    if (mSceneVertices.empty())
        return;

    XMFLOAT3 minPoint =
    {
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)()
    };
    XMFLOAT3 maxPoint =
    {
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)()
    };

    for (const Vertex& vertex : mSceneVertices)
    {
        minPoint.x = (std::min)(minPoint.x, vertex.position.x);
        minPoint.y = (std::min)(minPoint.y, vertex.position.y);
        minPoint.z = (std::min)(minPoint.z, vertex.position.z);

        maxPoint.x = (std::max)(maxPoint.x, vertex.position.x);
        maxPoint.y = (std::max)(maxPoint.y, vertex.position.y);
        maxPoint.z = (std::max)(maxPoint.z, vertex.position.z);
    }

    const XMFLOAT3 center =
    {
        (minPoint.x + maxPoint.x) * 0.5f,
        (minPoint.y + maxPoint.y) * 0.5f,
        (minPoint.z + maxPoint.z) * 0.5f
    };
    const XMFLOAT3 extent =
    {
        maxPoint.x - minPoint.x,
        maxPoint.y - minPoint.y,
        maxPoint.z - minPoint.z
    };
    mSceneCenter = center;
    mSceneExtent = extent;

    XMFLOAT3 targetPoint =
    {
        center.x,
        center.y,
        center.z
    };

    if (extent.y < 0.25f && extent.x > 1.0f && extent.z > 1.0f)
    {
        // Flat material-preview scenes are best viewed from above at an angle.
        mEyePos =
        {
            center.x,
            center.y + (std::max)(2.0f, (std::max)(extent.x, extent.z) * 0.38f),
            center.z - (std::max)(2.0f, extent.z * 0.45f)
        };
    }
    else
    {
        mEyePos =
        {
            center.x,
            center.y + (std::max)(2.0f, extent.y * 0.45f),
            center.z - (std::max)(6.0f, extent.z * 2.0f)
        };

        targetPoint =
        {
            center.x,
            center.y + extent.y * 0.15f,
            center.z
        };
    }

    XMVECTOR eye = XMLoadFloat3(&mEyePos);
    XMVECTOR target = XMLoadFloat3(&targetPoint);
    XMVECTOR forward = XMVector3Normalize(target - eye);

    XMFLOAT3 forwardFloat;
    XMStoreFloat3(&forwardFloat, forward);
    mYaw = std::atan2(forwardFloat.z, forwardFloat.x);
    mPitch = std::asin(std::clamp(forwardFloat.y, -1.0f, 1.0f));

    XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0));
    XMStoreFloat4x4(&mView, view);
}

void DirectXApp::Shutdown() {
    FlushCommandQueue();

    // Освобождаем PSO
    mPSO.Reset();
    mWireframePSO.Reset();
    mRootSignature.Reset();

    for (int i = 0; i < SwapChainBufferCount; i++) {
        mSwapChainBuffer[i].Reset();
    }
    mDepthStencilBuffer.Reset();
    mRtvHeap.Reset();
    mDsvHeap.Reset();
    mCbvHeap.Reset();
    mSwapChain.Reset();

    mVertexBufferGPU.Reset();
    mVertexBufferUploader.Reset();
    mIndexBufferGPU.Reset();
    mIndexBufferUploader.Reset();

    if (mCommandList) {
        mCommandList.Reset();
    }

    mFence.Reset();
    mDirectCmdListAlloc.Reset();
    mCommandQueue.Reset();
    device.Reset();
    adapter.Reset();
    dxgiFactory.Reset();
    if (mComInitialized)
    {
        CoUninitialize();
        mComInitialized = false;
    }
}

bool DirectXApp::CreateDXGIFactory() {
    UINT factoryFlags = 0;
    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
        MessageBox(NULL, L"CreateDXGIFactory2 failed", L"Error", MB_OK);
        return false;
    }
    return true;
}

bool DirectXApp::GetHardwareAdapter() {
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(dxgiFactory.As(&factory6))) {
        for (UINT adapterIndex = 0; ; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> currentAdapter;
            HRESULT hr = factory6->EnumAdapterByGpuPreference(
                adapterIndex,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&currentAdapter));

            if (FAILED(hr)) break;

            DXGI_ADAPTER_DESC1 desc;
            currentAdapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            if (SUCCEEDED(D3D12CreateDevice(currentAdapter.Get(),
                D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr))) {
                adapter = currentAdapter;
                return true;
            }
        }
    }
    return false;
}

bool DirectXApp::CreateD3DDevice() {
    if (!GetHardwareAdapter()) {
        HRESULT hr = dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        if (FAILED(hr)) {
            MessageBox(NULL, L"No hardware adapter found and WARP failed", L"Error", MB_OK);
            return false;
        }
        OutputDebugStringW(L"Using WARP software adapter.\n");
    }

    HRESULT hr = D3D12CreateDevice(
        adapter.Get(),
        D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&device)
    );

    if (FAILED(hr)) {
        MessageBox(NULL, L"D3D12CreateDevice failed", L"Error", MB_OK);
        return false;
    }

    return true;
}

bool DirectXApp::CreateCommandObjects() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue));
    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create command queue", L"Error", MB_OK);
        return false;
    }

    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&mDirectCmdListAlloc)
    );
    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create command allocator", L"Error", MB_OK);
        return false;
    }

    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        mDirectCmdListAlloc.Get(),
        nullptr,
        IID_PPV_ARGS(&mCommandList)
    );
    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create command list", L"Error", MB_OK);
        return false;
    }

    mCommandList->Close();
    return true;
}

bool DirectXApp::CreateFence() {
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence));
    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create fence", L"Error", MB_OK);
        return false;
    }
    mFenceValue = 0;
    return true;
}

void DirectXApp::FlushCommandQueue() {
    mFenceValue++;
    mCommandQueue->Signal(mFence.Get(), mFenceValue);

    if (mFence->GetCompletedValue() < mFenceValue) {
        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        mFence->SetEventOnCompletion(mFenceValue, eventHandle);
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }
}

bool DirectXApp::CreateSwapChain() {
    RECT clientRect;
    GetClientRect(window.GetHandle(), &clientRect);
    mClientWidth = clientRect.right - clientRect.left;
    mClientHeight = clientRect.bottom - clientRect.top;

    mSwapChain.Reset();

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = mClientWidth;
    sd.BufferDesc.Height = mClientHeight;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferDesc.Format = mBackBufferFormat;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = SwapChainBufferCount;
    sd.OutputWindow = window.GetHandle();
    sd.Windowed = true;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    HRESULT hr = dxgiFactory->CreateSwapChain(
        mCommandQueue.Get(),
        &sd,
        &mSwapChain
    );

    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create swap chain", L"Error", MB_OK);
        return false;
    }

    return true;
}

void DirectXApp::QueryDescriptorSizes() {
    mRtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mDsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    mCbvSrvUavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool DirectXApp::CreateDescriptorHeaps() {
    // 1. RTV куча
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
    rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap));
    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create RTV descriptor heap", L"Error", MB_OK);
        return false;
    }

    // 2. DSV куча
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsvHeapDesc.NodeMask = 0;

    hr = device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap));
    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create DSV descriptor heap", L"Error", MB_OK);
        return false;
    }

    // 3. CBV/SRV/UAV куча
    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
    cbvHeapDesc.NumDescriptors = 1 + 512;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbvHeapDesc.NodeMask = 0;

    hr = device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&mCbvHeap));
    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create CBV descriptor heap", L"Error", MB_OK);
        return false;
    }

    return true;
}

bool DirectXApp::CreateRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < SwapChainBufferCount; i++) {
        ComPtr<ID3D12Resource> backBuffer;
        HRESULT hr = mSwapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) {
            MessageBox(NULL, L"Failed to get swap chain buffer", L"Error", MB_OK);
            return false;
        }

        mSwapChainBuffer[i] = backBuffer;
        device->CreateRenderTargetView(mSwapChainBuffer[i].Get(), nullptr, rtvHeapHandle);
        rtvHeapHandle.ptr += mRtvDescriptorSize;
    }

    return true;
}

bool DirectXApp::CreateDepthStencilBuffer() {
    D3D12_RESOURCE_DESC depthStencilDesc = {};
    depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthStencilDesc.Width = mClientWidth;
    depthStencilDesc.Height = mClientHeight;
    depthStencilDesc.DepthOrArraySize = 1;
    depthStencilDesc.MipLevels = 1;
    depthStencilDesc.Format = mDepthStencilFormat;
    depthStencilDesc.SampleDesc.Count = 1;
    depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format = mDepthStencilFormat;
    optClear.DepthStencil.Depth = 1.0f;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthStencilDesc,
        D3D12_RESOURCE_STATE_COMMON,
        &optClear,
        IID_PPV_ARGS(&mDepthStencilBuffer)
    );

    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to create depth stencil buffer", L"Error", MB_OK);
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = mDepthStencilFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    device->CreateDepthStencilView(
        mDepthStencilBuffer.Get(),
        &dsvDesc,
        mDsvHeap->GetCPUDescriptorHandleForHeapStart()
    );

    return true;
}

void DirectXApp::CreateViewportAndScissor() {
    mScreenViewport.TopLeftX = 0.0f;
    mScreenViewport.TopLeftY = 0.0f;
    mScreenViewport.Width = static_cast<float>(mClientWidth);
    mScreenViewport.Height = static_cast<float>(mClientHeight);
    mScreenViewport.MinDepth = 0.0f;
    mScreenViewport.MaxDepth = 1.0f;

    mScissorRect = { 0, 0, mClientWidth, mClientHeight };
}

void DirectXApp::SetViewportAndScissor() {
    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);
}

bool DirectXApp::Initialize() {
    StartupLog("Initialize begin");

    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    mComInitialized = SUCCEEDED(comHr);

    #if defined(_DEBUG)
        {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
            {
                debugController->EnableDebugLayer();
            }
        }
    #endif
    StartupLog("Starting DirectX 12 initialization");

    // Основные этапы инициализации
    if (!CreateDXGIFactory()) { StartupLog("CreateDXGIFactory failed"); return false; }
    StartupLog("CreateDXGIFactory ok");
    if (!CreateD3DDevice()) { StartupLog("CreateD3DDevice failed"); return false; }
    StartupLog("CreateD3DDevice ok");
    if (!CreateCommandObjects()) { StartupLog("CreateCommandObjects failed"); return false; }
    StartupLog("CreateCommandObjects ok");
    if (!CreateFence()) { StartupLog("CreateFence failed"); return false; }
    StartupLog("CreateFence ok");
    if (!CreateSwapChain()) { StartupLog("CreateSwapChain failed"); return false; }
    StartupLog("CreateSwapChain ok");

    QueryDescriptorSizes();

    if (!CreateDescriptorHeaps()) { StartupLog("CreateDescriptorHeaps failed"); return false; }
    StartupLog("CreateDescriptorHeaps ok");
    if (!CreateRenderTargetViews()) { StartupLog("CreateRenderTargetViews failed"); return false; }
    StartupLog("CreateRenderTargetViews ok");
    if (!CreateDepthStencilBuffer()) { StartupLog("CreateDepthStencilBuffer failed"); return false; }
    StartupLog("CreateDepthStencilBuffer ok");

    CreateViewportAndScissor();

   //Геометрия и ресурсы
    BuildInputLayout();
   //BuildVertexBuffer();
   //BuildIndexBuffer();
    const std::string sceneDir = ResolveEarthDirectory();
    if (sceneDir.empty())
    {
        StartupLog("ResolveEarthDirectory failed");
        MessageBoxA(nullptr, "Failed to find Assets/Earth/Earth.fbx and Earth.mtl", "Asset path error", MB_OK);
        return false;
    }
    StartupLog("Resolved scene dir: " + sceneDir);

    BuildModel(sceneDir + "Earth.fbx");
    if (mIndexCount == 0)
    {
        StartupLog("BuildModel failed");
        MessageBoxA(nullptr, ("Failed to load FBX: " + sceneDir + "Earth.fbx").c_str(), "Asset load error", MB_OK);
        return false;
    }
    StartupLog("BuildModel ok");
    FitCameraToLoadedScene();
    StartupLog("FitCameraToLoadedScene ok");

    std::vector<ParsedMaterial> parsed;
    if (!LoadMTL(sceneDir + "Earth.mtl", parsed))
    {
        StartupLog("LoadMTL failed");
        MessageBoxA(nullptr, ("Failed to load MTL: " + sceneDir + "Earth.mtl").c_str(), "Asset load error", MB_OK);
        return false;
    }
    StartupLog("LoadMTL ok");

    UINT srvIndex = 0;
    constexpr UINT MaterialTextureCount = 3;

    for (auto& p : parsed)
    {
        Material mat;
        mat.Name = p.Name;
        mat.DiffuseMap = p.DiffuseMap;
        mat.NormalMap = p.NormalMap;
        mat.DisplacementMap = p.DisplacementMap;
        mat.DisplacementScale = p.DisplacementMap.empty() ? 0.0f : mat.DisplacementScale;
        mat.SrvHeapIndex = srvIndex;
        srvIndex += MaterialTextureCount;

        if (!p.DiffuseMap.empty())
        {
            CreateTextureFromFile(sceneDir + p.DiffuseMap, mat.DiffuseTexture);
        }
        else
        {
            CreateColorTexture(p.Kd, mat.DiffuseTexture);
        }

        if (!p.NormalMap.empty())
            CreateTextureFromFile(sceneDir + p.NormalMap, mat.NormalTexture);
        else
            CreateColorTexture({ 0.5f, 0.5f, 1.0f }, mat.NormalTexture);

        if (!p.DisplacementMap.empty())
            CreateTextureFromFile(sceneDir + p.DisplacementMap, mat.DisplacementTexture);
        else
            CreateColorTexture({ 0.5f, 0.5f, 0.5f }, mat.DisplacementTexture);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE hDescriptor =
            mCbvHeap->GetCPUDescriptorHandleForHeapStart();

        hDescriptor.ptr += (1 + mat.SrvHeapIndex) * mCbvSrvUavDescriptorSize;

        device->CreateShaderResourceView(
            mat.DiffuseTexture.Get(),
            &srvDesc,
            hDescriptor);

        hDescriptor.ptr += mCbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(
            mat.NormalTexture.Get(),
            &srvDesc,
            hDescriptor);

        hDescriptor.ptr += mCbvSrvUavDescriptorSize;
        device->CreateShaderResourceView(
            mat.DisplacementTexture.Get(),
            &srvDesc,
            hDescriptor);

        mMaterials.push_back(mat);
    }
    //MATERIAL SETTINGS (TILING/ANIM)
    for (auto& mat : mMaterials)
    {
        if (mat.Name.find("fabric") != std::string::npos)
        {
            mat.UVSpeed = { 0.1f, 0.0f }; // флаги движутся
            mat.Tiling = { 1.0f, 1.0f };
        }

        if (mat.Name.find("floor") != std::string::npos)
        {
            mat.Tiling = { 6.0f, 6.0f }; // тайлинг пола
        }

        if (mat.Name == "Earth")
        {
            mat.Tiling = { 1.0f, 1.0f };
            mat.DisplacementScale = EarthDisplacementScale;
            mat.NormalStrength = EarthNormalStrength;
            mat.TessellationParams =
            {
                TessellationMaxFactor,
                TessellationMinFactor,
                TessellationNearDistance,
                TessellationFarDistance
            };
        }
    }

    BuildConstantBuffer();
    StartupLog("BuildConstantBuffer ok");
    mRenderingSystem = std::make_unique<RenderingSystem>();
    StartupLog("RenderingSystem Initialize begin");
    mRenderingSystem->Initialize(
        device.Get(),
        static_cast<UINT>(mClientWidth),
        static_cast<UINT>(mClientHeight),
        mBackBufferFormat,
        mCbvSrvUavDescriptorSize,
        mRtvDescriptorSize);
    StartupLog("RenderingSystem Initialize ok");

    // Инициализация проекционной матрицы
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * XM_PI,
        (float)mClientWidth / (float)mClientHeight, 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);

    mTimer.Reset();
    StartupLog("Initialize ok");
    return true;
}

bool DirectXApp::InitializeApp() {
    return Initialize();
}

ID3D12Resource* DirectXApp::CurrentBackBuffer() const {
    return mSwapChainBuffer[mCurrBackBuffer].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE DirectXApp::CurrentBackBufferView() const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += mCurrBackBuffer * mRtvDescriptorSize;
    return handle;
}

void DirectXApp::OnResize() {
    // TODO: реализовать позже
}

// Обработка клавиатуры
void DirectXApp::OnKeyDown(WPARAM wParam)
{
    if (wParam >= '0' && wParam <= '5' && !mPostKeysDown[wParam - '0'])
    {
        mPostKeysDown[wParam - '0'] = true;
        mPostStatusVisible = true;
        if (wParam == '0') mPostSettings.Enabled = !mPostSettings.Enabled;
        if (wParam == '1') mPostSettings.Vignette = !mPostSettings.Vignette;
        if (wParam == '2') mPostSettings.ChromaticAberration = !mPostSettings.ChromaticAberration;
        if (wParam == '3') mPostSettings.BufferView = (mPostSettings.BufferView + 1) % 4;
        if (wParam == '4')
        {
            mPostSettings.VignetteStrength += 0.25f;
            if (mPostSettings.VignetteStrength > 1) mPostSettings.VignetteStrength = 0;
        }
        if (wParam == '5')
        {
            mPostSettings.ChromaticPixels += 3;
            if (mPostSettings.ChromaticPixels > 12) mPostSettings.ChromaticPixels = 0;
        }
    }
    if (wParam == 'T' || wParam == 'J' || wParam == 'K' || wParam == 'L' || wParam == 'U' || wParam == 'G')
        mPostStatusVisible = false;
    if (wParam == VK_F10 && !mF10KeyDown)
    {
        mF10KeyDown = true;
        mObserveCulling = !mObserveCulling;
        if (mObserveCulling) mShowCullingScene = true;
    }
    if (wParam == VK_F11 && !mF11KeyDown)
    {
        mF11KeyDown = true;
        mShowCulledBounds = !mShowCulledBounds;
    }
    if (wParam == 'T' && !mTessellationKeyDown)
    {
        mTessellationKeyDown = true;
        mTessellationCacheEnabled = !mTessellationCacheEnabled;
    }
    const WPARAM shadowKeys[] = { 'J', 'K', 'L', 'G', 'U' };
    for (UINT i = 0; i < _countof(shadowKeys); ++i)
    {
        if (wParam != shadowKeys[i] || mShadowKeysDown[i]) continue;
        mShadowKeysDown[i] = true;
        if (i == 0) mShadowSettings.Enabled = !mShadowSettings.Enabled;
        if (i == 1) mShadowSettings.PcfRadius = (mShadowSettings.PcfRadius + 1) % 3;
        if (i == 2) mShadowSettings.ShowCascades = !mShadowSettings.ShowCascades;
        if (i == 3) mShadowSettings.ShowGround = !mShadowSettings.ShowGround;
        if (i == 4) mShadowSettings.SplitLambda = mShadowSettings.SplitLambda > 0 ? 0.0f : 0.7f;
    }
    const WPARAM particleKeys[] = { VK_F8, VK_F9, 'E', 'R', VK_OEM_4, VK_OEM_6 };
    for (UINT i = 0; i < _countof(particleKeys); ++i)
    {
        if (wParam != particleKeys[i] || mParticleKeysDown[i]) continue;
        mParticleKeysDown[i] = true;
        if (i == 0) mParticleSettings.Visible = !mParticleSettings.Visible;
        if (i == 1) mParticleSettings.Paused = !mParticleSettings.Paused;
        if (i == 2) mParticleSettings.Emit = !mParticleSettings.Emit;
        if (i == 3 && mRenderingSystem) mRenderingSystem->ResetParticles();
        if (i == 4) mParticleSettings.EmissionRate = (std::max)(100.0f, mParticleSettings.EmissionRate / 1.5f);
        if (i == 5) mParticleSettings.EmissionRate = (std::min)(30000.0f, mParticleSettings.EmissionRate * 1.5f);
    }
    if (wParam == VK_F4) mCullingMode = CullingMode::None;
    if (wParam == VK_F5) mCullingMode = CullingMode::Linear;
    if (wParam == VK_F6) mCullingMode = CullingMode::Octree;
    if (wParam == VK_F7 && !mF7KeyDown)
    {
        mF7KeyDown = true;
        mShowCullingScene = !mShowCullingScene;
        if (!mShowCullingScene) mObserveCulling = false;
    }
    // M toggles render mode; SPACE remains reserved for camera movement.
    if (wParam == 'M') {
        mWireframeMode = !mWireframeMode;

        if (mWireframeMode) {
            SetWindowText(window.GetHandle(), L"DirectX 12 Framework - Wireframe Mode (Press M to switch)");
        }
        else {
            SetWindowText(window.GetHandle(), L"DirectX 12 Framework - Solid Mode (Press M to switch)");
        }
    }

    if (wParam == VK_F1 && !mF1KeyDown) {
        mF1KeyDown = true;
        mDebugViewMode = DebugViewMode::Default;
    }

    if (wParam == VK_F2 && !mF2KeyDown) {
        mF2KeyDown = true;
        mDebugViewMode = DebugViewMode::NormalMap;
    }

    if (wParam == VK_F3 && !mF3KeyDown) {
        mF3KeyDown = true;
        mDebugViewMode = DebugViewMode::Tessellation;
    }

    if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) {
        UpdateUvDirectionFromInput();
    }

    if (wParam == 'F' && !mShootKeyDown) {
        mShootKeyDown = true;
        ShootLightProjectile();
    }
}

void DirectXApp::OnKeyUp(WPARAM wParam)
{
    if (wParam >= '0' && wParam <= '5') mPostKeysDown[wParam - '0'] = false;
    if (wParam == VK_F10) mF10KeyDown = false;
    if (wParam == VK_F11) mF11KeyDown = false;
    if (wParam == 'T') mTessellationKeyDown = false;
    const WPARAM shadowKeys[] = { 'J', 'K', 'L', 'G', 'U' };
    for (UINT i = 0; i < _countof(shadowKeys); ++i)
        if (wParam == shadowKeys[i]) mShadowKeysDown[i] = false;
    const WPARAM particleKeys[] = { VK_F8, VK_F9, 'E', 'R', VK_OEM_4, VK_OEM_6 };
    for (UINT i = 0; i < _countof(particleKeys); ++i)
        if (wParam == particleKeys[i]) mParticleKeysDown[i] = false;
    if (wParam == VK_F7) mF7KeyDown = false;
    if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) {
        UpdateUvDirectionFromInput();
    }

    if (wParam == 'F') {
        mShootKeyDown = false;
    }

    if (wParam == VK_F1)
        mF1KeyDown = false;
    if (wParam == VK_F2)
        mF2KeyDown = false;
    if (wParam == VK_F3)
        mF3KeyDown = false;
}

int DirectXApp::Run() {
    MSG msg = { 0 };
    mTimer.Reset();

    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            mTimer.Tick();
            if (!mAppPaused) {
                CalculateFrameStats();
                Update(mTimer);
                Draw(mTimer);
            }
            else {
                Sleep(100);
            }
        }
    }
    return (int)msg.wParam;
}

void DirectXApp::CalculateFrameStats() {
    mFrameCount++;
    if ((mTimer.TotalTime() - mTimeElapsed) >= 1.0f) {
        float fps = (float)mFrameCount;
        float mspf = 1000.0f / fps;

        std::wstring windowText = mMainWndCaption;
        if (mPostStatusVisible && !mShowCullingScene && !mParticleSettings.Visible)
        {
            const wchar_t* views[] = { L"Final", L"Albedo", L"Normals", L"Depth" };
            std::wostringstream title;
            title << L"HW7 FX " << (mPostSettings.Enabled ? L"ON" : L"OFF")
                << L" | Vignette " << (mPostSettings.Vignette ? L"ON " : L"OFF ") << mPostSettings.VignetteStrength
                << L" | Chromatic " << (mPostSettings.ChromaticAberration ? L"ON " : L"OFF ") << mPostSettings.ChromaticPixels
                << L"px | " << views[mPostSettings.BufferView] << L" | " << int(fps) << L" FPS";
            SetWindowText(window.GetHandle(), title.str().c_str());
            mFrameCount = 0;
            mTimeElapsed += 1.0f;
            return;
        }
        std::wstring tessStatus;
        if (mRenderingSystem)
        {
            const auto& stats = mRenderingSystem->GetTessellationCacheStats();
            tessStatus = mTessellationCacheEnabled ? L" | Tess cache" : L" | Tess EVERY FRAME";
            if (mTessellationCacheEnabled)
                tessStatus += L" | Builds " + std::to_wstring(stats.Updates) + L" Reuse " +
                    std::to_wstring(stats.ReusedFrames) + L" Tris " + std::to_wstring(stats.Triangles) +
                    (stats.FallbackMeshes ? L" | Fallback " + std::to_wstring(stats.FallbackMeshes) : L"");
        }
        if (!mShowCullingScene && !mParticleSettings.Visible && mRenderingSystem)
        {
            std::wostringstream title;
            title << tessStatus.substr(3) << L" | HW6 CSM " << (mShadowSettings.Enabled ? L"ON" : L"OFF")
                << L" | PCF " << (mShadowSettings.PcfRadius * 2 + 1)
                << L" | Lambda " << std::fixed << std::setprecision(1) << mShadowSettings.SplitLambda
                << L" | Splits";
            for (float split : mRenderingSystem->GetCascadeSplits()) title << L" " << split;
            title << L" | " << int(fps) << L" FPS";
            SetWindowText(window.GetHandle(), title.str().c_str());
            mFrameCount = 0;
            mTimeElapsed += 1.0f;
            return;
        }
        if (!mShowCullingScene && mParticleSettings.Visible && mRenderingSystem)
        {
            std::wostringstream title;
            title << L"HW5 GPU particles " << mRenderingSystem->GetParticleCount() << L"/" << ParticleSystem::Capacity
                << L" | Emit " << static_cast<int>(mParticleSettings.EmissionRate) << L"/s"
                << (mParticleSettings.Emit ? L" ON" : L" OFF")
                << (mParticleSettings.Paused ? L" | PAUSED" : L" | RUNNING")
                << L" | " << static_cast<int>(fps) << L" FPS | Speed " << std::fixed << std::setprecision(2) << mCameraSpeed << tessStatus;
            SetWindowText(window.GetHandle(), title.str().c_str());
            mFrameCount = 0;
            mTimeElapsed += 1.0f;
            return;
        }
        if (mShowCullingScene && mRenderingSystem)
        {
            const auto& stats = mRenderingSystem->GetCullingStats();
            const wchar_t* mode = mCullingMode == CullingMode::None ? L"Off" :
                (mCullingMode == CullingMode::Linear ? L"Linear" : L"Octree");
            std::wostringstream title;
            title << L"HW4 " << (mObserveCulling ? L"SIDE | " : L"CAMERA | ") << mode << L" | Drawn " << stats.Visible
                << L"/" << mRenderingSystem->GetSceneObjectCount() << L" | Tests " << stats.ObjectTests << L"+" << stats.NodeTests
                << L" | " << std::fixed << std::setprecision(2) << stats.Milliseconds
                << L" ms | " << static_cast<int>(fps) << L" FPS | Speed " << mCameraSpeed;
            if (mObserveCulling) title << (mShowCulledBounds ? L" | Culled bounds ON" : L" | Culled bounds OFF");
            else title << tessStatus;
            SetWindowText(window.GetHandle(), title.str().c_str());
            mFrameCount = 0;
            mTimeElapsed += 1.0f;
            return;
        }
        if (mDebugViewMode == DebugViewMode::Tessellation) {
            windowText += L" - Tessellation Debug";
        }
        else if (mDebugViewMode == DebugViewMode::NormalMap) {
            windowText += L" - Normal Map Debug";
        }
        else if (mWireframeMode) {
            windowText += L" - Wireframe Mode";
        }
        else {
        windowText += L" - Solid Mode";
        }
        windowText += L" Speed: " + std::to_wstring(mCameraSpeed) + L" (+/-)";
        windowText += L" FPS: " + std::to_wstring(fps);
        windowText += L" MSPF: " + std::to_wstring(mspf);
        windowText += L" Projectiles: " + std::to_wstring(mShotProjectiles.size());
        windowText += L" Placed lights: " + std::to_wstring(mPlacedLights.size());
        windowText += L"/" + std::to_wstring(MaxShotLights);
        XMVECTOR eye = XMLoadFloat3(&mEyePos);
        XMVECTOR center = XMLoadFloat3(&mSceneCenter);
        float centerDistance = XMVectorGetX(XMVector3Length(eye - center));
        float sceneRadius = 0.5f * (std::max)({ mSceneExtent.x, mSceneExtent.y, mSceneExtent.z });
        float tessDistance = (std::max)(0.0f, centerDistance - sceneRadius);
        float tessT = (std::clamp)((tessDistance - TessellationNearDistance) / (TessellationFarDistance - TessellationNearDistance), 0.0f, 1.0f);
        float tessFactor = TessellationMaxFactor + (TessellationMinFactor - TessellationMaxFactor) * tessT;
        windowText += L" Dist: " + std::to_wstring(tessDistance);
        windowText += L" Tess: " + std::to_wstring(tessFactor);
        windowText += L" (F1 default, F2 normals, F3 tessellation, M wireframe)";

        windowText += tessStatus;
        SetWindowText(window.GetHandle(), windowText.c_str());

        mFrameCount = 0;
        mTimeElapsed += 1.0f;
    }
}

XMFLOAT3 DirectXApp::GetCameraForward() const
{
    XMFLOAT3 forward =
    {
        cosf(mPitch) * cosf(mYaw),
        sinf(mPitch),
        cosf(mPitch) * sinf(mYaw)
    };

    XMVECTOR forwardVec = XMVector3Normalize(XMLoadFloat3(&forward));
    XMStoreFloat3(&forward, forwardVec);
    return forward;
}

void DirectXApp::ShootLightProjectile()
{
    XMFLOAT3 forward = GetCameraForward();
    const float maxDistance = mProjectileSpeed * mProjectileLifeTime;

    ShotLightProjectile projectile;
    projectile.Position = mEyePos;
    projectile.Direction = forward;

    RaycastHit hit;
    if (RaycastScene(mEyePos, forward, maxDistance, hit))
    {
        projectile.HasHit = true;
        projectile.HitPosition = hit.Position;
        projectile.HitNormal = hit.Normal;
        projectile.HitDistance = hit.Distance;
    }

    // Projectile is logical while flying; it becomes a real deferred point light on geometry impact.
    mShotProjectiles.push_back(projectile);

    std::ostringstream stream;
    stream << "Shot light projectile. Projectiles=" << mShotProjectiles.size()
        << " Hit=" << (projectile.HasHit ? "yes" : "no") << "\n";
    OutputDebugStringA(stream.str().c_str());
}

void DirectXApp::UpdateShotLights(float dt)
{
    std::vector<ShotLightProjectile> activeProjectiles;
    activeProjectiles.reserve(mShotProjectiles.size());

    for (ShotLightProjectile projectile : mShotProjectiles)
    {
        projectile.Age += dt;
        const float step = mProjectileSpeed * dt;
        projectile.TravelDistance += step;

        XMVECTOR pos = XMLoadFloat3(&projectile.Position);
        XMVECTOR dir = XMLoadFloat3(&projectile.Direction);
        pos += dir * step;
        XMStoreFloat3(&projectile.Position, pos);

        if (projectile.HasHit && projectile.TravelDistance >= projectile.HitDistance)
        {
            XMVECTOR hitPos = XMLoadFloat3(&projectile.HitPosition);
            XMVECTOR hitNormal = XMLoadFloat3(&projectile.HitNormal);
            XMFLOAT3 placedPosition;
            XMStoreFloat3(&placedPosition, hitPos + hitNormal * mSurfaceLightOffset);
            AddPlacedLight(placedPosition);
            continue;
        }

        if (projectile.Age < mProjectileLifeTime)
            activeProjectiles.push_back(projectile);
    }

    mShotProjectiles.swap(activeProjectiles);

    for (DynamicPointLight& light : mPlacedLights)
        light.Age += dt;
}

void DirectXApp::AddPlacedLight(const XMFLOAT3& position)
{
    if (mPlacedLights.size() >= MaxShotLights)
        mPlacedLights.erase(mPlacedLights.begin());

    DynamicPointLight placedLight;
    placedLight.Light.Position = position;
    placedLight.Light.Range = mShotLightRadius;
    placedLight.Light.Color = mShotLightColor;
    placedLight.Light.Intensity = mShotLightIntensity;
    mPlacedLights.push_back(placedLight);

    std::ostringstream stream;
    stream << "Placed point light. Count=" << mPlacedLights.size()
        << "/" << MaxShotLights << "\n";
    OutputDebugStringA(stream.str().c_str());
}

bool DirectXApp::RaycastScene(
    const XMFLOAT3& origin,
    const XMFLOAT3& direction,
    float maxDistance,
    RaycastHit& outHit) const
{
    if (mSceneVertices.empty() || mSceneIndices.size() < 3)
        return false;

    const float epsilon = 1e-5f;
    float closestDistance = maxDistance;
    bool foundHit = false;

    XMVECTOR rayOrigin = XMLoadFloat3(&origin);
    XMVECTOR rayDirection = XMVector3Normalize(XMLoadFloat3(&direction));

    for (size_t i = 0; i + 2 < mSceneIndices.size(); i += 3)
    {
        const Vertex& vertex0 = mSceneVertices[mSceneIndices[i + 0]];
        const Vertex& vertex1 = mSceneVertices[mSceneIndices[i + 1]];
        const Vertex& vertex2 = mSceneVertices[mSceneIndices[i + 2]];

        XMVECTOR v0 = XMLoadFloat3(&vertex0.position);
        XMVECTOR v1 = XMLoadFloat3(&vertex1.position);
        XMVECTOR v2 = XMLoadFloat3(&vertex2.position);
        XMVECTOR edge1 = v1 - v0;
        XMVECTOR edge2 = v2 - v0;
        XMVECTOR pvec = XMVector3Cross(rayDirection, edge2);

        float det = XMVectorGetX(XMVector3Dot(edge1, pvec));
        if (fabsf(det) < epsilon)
            continue;

        float invDet = 1.0f / det;
        XMVECTOR tvec = rayOrigin - v0;
        float u = XMVectorGetX(XMVector3Dot(tvec, pvec)) * invDet;
        if (u < 0.0f || u > 1.0f)
            continue;

        XMVECTOR qvec = XMVector3Cross(tvec, edge1);
        float v = XMVectorGetX(XMVector3Dot(rayDirection, qvec)) * invDet;
        if (v < 0.0f || u + v > 1.0f)
            continue;

        float distance = XMVectorGetX(XMVector3Dot(edge2, qvec)) * invDet;
        if (distance <= epsilon || distance >= closestDistance)
            continue;

        closestDistance = distance;
        foundHit = true;

        XMVECTOR hitPosition = rayOrigin + rayDirection * distance;
        XMVECTOR normal = XMVector3Normalize(XMVector3Cross(edge1, edge2));
        if (XMVectorGetX(XMVector3Dot(normal, rayDirection)) > 0.0f)
            normal = -normal;

        XMStoreFloat3(&outHit.Position, hitPosition);
        XMStoreFloat3(&outHit.Normal, normal);
        outHit.Distance = distance;
    }

    return foundHit;
}

void DirectXApp::Update(const Timer& gt)
{
    float dt = (std::min)(gt.DeltaTime(), 0.033f);
    if (GetForegroundWindow() == window.GetHandle())
    {
        const bool increaseSpeed = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) ||
            (GetAsyncKeyState(VK_ADD) & 0x8000);
        const bool decreaseSpeed = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) ||
            (GetAsyncKeyState(VK_SUBTRACT) & 0x8000);
        const int speedDirection = static_cast<int>(increaseSpeed) - static_cast<int>(decreaseSpeed);
        // Multiplicative adjustment gives fine control at low movement speeds.
        mCameraSpeed = (std::clamp)(mCameraSpeed * std::exp2(speedDirection * dt),
            CameraMinSpeed, CameraMaxSpeed);
    }
    float speed = mCameraSpeed;

    // ===== Forward Vector =====
    XMFLOAT3 forward =
    {
        cosf(mPitch) * cosf(mYaw),
        sinf(mPitch),
        cosf(mPitch) * sinf(mYaw)
    };

    XMVECTOR forwardVec = XMLoadFloat3(&forward);
    forwardVec = XMVector3Normalize(forwardVec);

    XMVECTOR rightVec = XMVector3Normalize(
        XMVector3Cross(
            XMVectorSet(0, 1, 0, 0),
            forwardVec));

    // ===== Movement =====
    XMVECTOR pos = XMLoadFloat3(&mEyePos);

    if (GetAsyncKeyState('W') & 0x8000)
        pos += forwardVec * speed * dt;

    if (GetAsyncKeyState('S') & 0x8000)
        pos -= forwardVec * speed * dt;

    if (GetAsyncKeyState('A') & 0x8000)
        pos -= rightVec * speed * dt;

    if (GetAsyncKeyState('D') & 0x8000)
        pos += rightVec * speed * dt;

    if (GetAsyncKeyState(VK_SPACE) & 0x8000)
        pos += XMVectorSet(0, 1, 0, 0) * speed * dt;

    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        pos -= XMVectorSet(0, 1, 0, 0) * speed * dt;

    XMStoreFloat3(&mEyePos, pos);

    // ===== View Matrix =====
    XMVECTOR target = pos + forwardVec;
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
    XMStoreFloat4x4(&mView, view);

    // ===== Projection =====
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4,
        (float)mClientWidth / (float)mClientHeight,
        0.1f,
        1000.0f);

    XMStoreFloat4x4(&mProj, proj);

    mUvOffset.x += mUvDirection.x * mUvSpeed * dt;
    mUvOffset.y += mUvDirection.y * mUvSpeed * dt;
    UpdateShotLights(dt);

    // ===== WVP =====
    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX worldViewProj = world * view * proj;

    ObjectConstants objConstants;

    // WVP
    XMStoreFloat4x4(
        &objConstants.mWorldViewProj,
        XMMatrixTranspose(worldViewProj));

    static float time = 0.0f;
    time += gt.DeltaTime();

    // TEST — чтобы увидеть эффект сразу
    objConstants.uvTiling = { 6.0f, 6.0f };
    objConstants.uvOffset = { time * -0.2f, 0.0f };

    //      TEXTURE TILING + ANIMATION
    //if (!mMaterials.empty())
    //{
    //    Material& mat = mMaterials[0];

    //    objConstants.uvTiling = mat.Tiling;

    //    float t = (float)mTimer.TotalTime();

    //    objConstants.uvOffset.x = mat.UVSpeed.x * t;
    //    objConstants.uvOffset.y = mat.UVSpeed.y * t;
    //}

    //mObjectCB->CopyData(0, objConstants);
}

void DirectXApp::Draw(const Timer& gt)
{
    if (mIndexCount == 0 || !mRenderingSystem)
        return;

    mDirectCmdListAlloc->Reset();
    mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr);
    SetViewportAndScissor();

    SceneRenderContext scene = {};
    scene.VertexBufferView = mVertexBufferView;
    scene.IndexBufferView = mIndexBufferView;
    scene.ObjectConstantsBuffer = mObjectCB.get();
    scene.MaterialHeap = mCbvHeap.Get();
    scene.MaterialDescriptorSize = mCbvSrvUavDescriptorSize;
    scene.Submeshes = &mSubmeshes;
    scene.Materials = &mMaterials;
    scene.View = mView;
    scene.Proj = mProj;
    scene.EyePos = mEyePos;
    scene.UvOffset = mUvOffset;
    scene.DynamicPointLights = &mPlacedLights;
    scene.Wireframe = mWireframeMode || mDebugViewMode == DebugViewMode::Tessellation;
    scene.DebugViewMode = static_cast<UINT>(mDebugViewMode);
    scene.Culling = mCullingMode;
    scene.ShowCullingScene = mShowCullingScene;
    scene.ObserveCulling = mObserveCulling;
    scene.ShowCulledBounds = mShowCulledBounds;
    scene.Particles = mParticleSettings;
    scene.DeltaTime = gt.DeltaTime();
    scene.Shadows = mShadowSettings;
    scene.CacheTessellation = mTessellationCacheEnabled;
    scene.Post = mPostSettings;
    scene.SceneBounds = BoundingBox(mSceneCenter, XMFLOAT3(mSceneExtent.x * 0.5f, mSceneExtent.y * 0.5f, mSceneExtent.z * 0.5f));

    mRenderingSystem->Render(
        mCommandList.Get(),
        CurrentBackBuffer(),
        CurrentBackBufferView(),
        scene);

    D3D12_RESOURCE_BARRIER barrier =
        CD3DX12_RESOURCE_BARRIER_HELPER::Transition(
            CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &barrier);

    mCommandList->Close();

    ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdLists);

    mSwapChain->Present(0, 0);
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    FlushCommandQueue();
    mRenderingSystem->OnFrameComplete();
}

void DirectXApp::CreateTextureFromFile(
    const std::string& path,
    Microsoft::WRL::ComPtr<ID3D12Resource>& texture)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".tga")
    {
        CreateTextureFromTGA(path, texture);
        return;
    }

    CreateTextureFromWIC(path, texture);
}

void DirectXApp::CreateTextureFromWIC(
    const std::string& path,
    Microsoft::WRL::ComPtr<ID3D12Resource>& texture)
{
    std::wstring widePath(path.begin(), path.end());

    ComPtr<IWICImagingFactory> wicFactory;
    ThrowIfFailed(CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)));

    ComPtr<IWICBitmapDecoder> decoder;
    ThrowIfFailed(wicFactory->CreateDecoderFromFilename(
        widePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder));

    ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(decoder->GetFrame(0, &frame));

    UINT width = 0;
    UINT height = 0;
    ThrowIfFailed(frame->GetSize(&width, &height));

    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(wicFactory->CreateFormatConverter(&converter));
    ThrowIfFailed(converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom));

    std::vector<BYTE> pixels(width * height * 4);
    ThrowIfFailed(converter->CopyPixels(
        nullptr,
        width * 4,
        static_cast<UINT>(pixels.size()),
        pixels.data()));

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&texture)));

    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = uploadSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> uploadBuffer;
    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, nullptr);

    void* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, &mapped);
    BYTE* dest = reinterpret_cast<BYTE*>(mapped);
    for (UINT y = 0; y < height; ++y)
    {
        memcpy(
            dest + y * footprint.Footprint.RowPitch,
            pixels.data() + y * width * 4,
            width * 4);
    }
    uploadBuffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr);
    mCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mCommandList->ResourceBarrier(1, &barrier);

    mCommandList->Close();

    ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdLists);
    FlushCommandQueue();
}

void DirectXApp::CreateTextureFromTGA(
    const std::string& path,
    Microsoft::WRL::ComPtr<ID3D12Resource>& texture)
{
    TgaImage image;
    if (!LoadTGA(path, image))
    {
        throw std::runtime_error("Failed to load TGA: " + path);
    }

    // ===== FIX RGB → RGBA =====
    UINT pixelSize = image.data.size() / (image.width * image.height);

    if (pixelSize == 3)
    {
        std::vector<uint8_t> converted;
        converted.resize(image.width * image.height * 4);

        for (UINT i = 0; i < image.width * image.height; i++)
        {
            converted[i * 4 + 0] = image.data[i * 3 + 0];
            converted[i * 4 + 1] = image.data[i * 3 + 1];
            converted[i * 4 + 2] = image.data[i * 3 + 2];
            converted[i * 4 + 3] = 255;
        }

        image.data = std::move(converted);
        pixelSize = 4;
    }

    // ===== TEXTURE RESOURCE =====
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = image.width;
    texDesc.Height = image.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&texture)));

    // ===== UPLOAD BUFFER =====
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &texDesc, 0, 1, 0,
        nullptr, nullptr, nullptr,
        &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = uploadSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;

    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer)));

    // ===== COPY DATA =====
    void* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, &mapped);

    BYTE* dest = reinterpret_cast<BYTE*>(mapped);
    BYTE* srcData = image.data.data();

    UINT rowPitch = (image.width * 4 + 255) & ~255;

    for (UINT y = 0; y < image.height; y++)
    {
        memcpy(
            dest + y * rowPitch,
            srcData + y * image.width * 4,
            image.width * 4);
    }

    uploadBuffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    device->GetCopyableFootprints(
        &texDesc, 0, 1, 0,
        &src.PlacedFootprint,
        nullptr, nullptr, nullptr);

    mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr);
    mCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    mCommandList->ResourceBarrier(1, &barrier);
    mCommandList->Close();

    ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdLists);

    FlushCommandQueue();
}

void DirectXApp::CreateColorTexture(
    const DirectX::XMFLOAT3& color,
    Microsoft::WRL::ComPtr<ID3D12Resource>& texture)
{
    UINT r = (UINT)(color.x * 255.0f);
    UINT g = (UINT)(color.y * 255.0f);
    UINT b = (UINT)(color.z * 255.0f);

    UINT pixel = (255 << 24) | (b << 16) | (g << 8) | r;

    // ---- TEXTURE (DEFAULT heap) ----
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&texture)));

    // ---- UPLOAD BUFFER ----
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &texDesc, 0, 1, 0,
        nullptr, nullptr, nullptr,
        &uploadSize);

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = uploadSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.SampleDesc.Count = 1;

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;

    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer)));

    // ---- MAP ----
    void* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, &pixel, sizeof(UINT));
    uploadBuffer->Unmap(0, nullptr);

    // ---- COPY ----
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    device->GetCopyableFootprints(
        &texDesc, 0, 1, 0,
        &src.PlacedFootprint,
        nullptr, nullptr, nullptr);

    mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr);

    mCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    mCommandList->ResourceBarrier(1, &barrier);

    mCommandList->Close();

    ID3D12CommandList* cmdLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdLists);

    FlushCommandQueue();
}
