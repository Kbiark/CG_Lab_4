#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <string>
#include <vector>
#include "Window.h"
#include "Timer.h"
#include "vertex.h"
#include "UploadBuffer.h"
#include "ObjectConstants.h"
#include <memory>
#include <cstddef>
#include <cstdint>
#include "MathHelper.h"
#include <DirectXMath.h>
#include "Material.h"
#include "Submesh.h"
#include "ThrowIfFailed.h"
#include "RenderingSystem.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

class DirectXApp {
public:
    DirectXApp(Window& window);
    ~DirectXApp();

    bool Initialize();
    void Shutdown();

    // Основные методы фреймворка
    int Run();
    virtual bool InitializeApp();
    virtual void Update(const Timer& gt);
    virtual void Draw(const Timer& gt);
    void BuildModel(const std::string& path);
    virtual void CalculateFrameStats();

    // Управление таймером
    void StopTimer() { mTimer.Stop(); }
    void StartTimer() { mTimer.Start(); }
    bool IsPaused() const { return mAppPaused; }
    Timer& GetTimer() { return mTimer; }

    // Методы для мыши
    virtual void OnMouseDown(WPARAM btnState, int x, int y);
    virtual void OnMouseUp(WPARAM btnState, int x, int y);
    virtual void OnMouseMove(WPARAM btnState, int x, int y);

    // Обработка изменения размера
    virtual void OnResize();

    // Обработка клавиатуры
    virtual void OnKeyDown(WPARAM wParam);
    virtual void OnKeyUp(WPARAM wParam);

    void SetDirectXApp(DirectXApp* app) { dxApp = app; }
    DirectXApp* GetDirectXApp() const { return dxApp; }

private:
    float mYaw = 0.0f;
    float mPitch = 0.0f;
    XMFLOAT2 mUvOffset = { 0.0f, 0.0f };
    XMFLOAT2 mUvDirection = { 0.0f, 0.0f };
    float mUvSpeed = 0.2f;
    float mCameraSpeed = 2.0f;

    struct ShotLightProjectile
    {
        XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 Direction = { 0.0f, 0.0f, 1.0f };
        XMFLOAT3 HitPosition = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 HitNormal = { 0.0f, 1.0f, 0.0f };
        float Age = 0.0f;
        float TravelDistance = 0.0f;
        float HitDistance = 0.0f;
        bool HasHit = false;
    };

    struct RaycastHit
    {
        XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
        XMFLOAT3 Normal = { 0.0f, 1.0f, 0.0f };
        float Distance = 0.0f;
    };

    static constexpr size_t MaxShotLights = 256;
    float mShotLightRadius = 6.0f;
    XMFLOAT3 mShotLightColor = { 1.0f, 0.72f, 0.35f };
    float mShotLightIntensity = 5.5f;
    float mProjectileSpeed = 55.0f;
    float mProjectileLifeTime = 4.0f;
    float mSurfaceLightOffset = 0.08f;
    bool mShootKeyDown = false;
    std::vector<ShotLightProjectile> mShotProjectiles;
    std::vector<DynamicPointLight> mPlacedLights;
    std::vector<Vertex> mSceneVertices;
    std::vector<uint32_t> mSceneIndices;
    XMFLOAT3 mSceneCenter = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 mSceneExtent = { 1.0f, 1.0f, 1.0f };

    std::vector<Submesh> mSubmeshes;
    std::vector<Material> mMaterials;
    void CreateTextureFromTGA(
        const std::string& path,
        Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    void CreateTextureFromWIC(
        const std::string& path,
        Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    void CreateTextureFromFile(
        const std::string& path,
        Microsoft::WRL::ComPtr<ID3D12Resource>& texture);

    DirectXApp* dxApp = nullptr;

    XMFLOAT3 mEyePos = { 0.0f, 5.0f, -15.0f };

    //

    Window& window;

    // DXGI
    ComPtr<IDXGIFactory4> dxgiFactory;
    ComPtr<IDXGIAdapter1> adapter;

    // D3D12
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> mCommandQueue;
    ComPtr<ID3D12CommandAllocator> mDirectCmdListAlloc;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;
    ComPtr<ID3D12Fence> mFence;
    UINT64 mFenceValue = 0;

    // SwapChain
    ComPtr<IDXGISwapChain> mSwapChain;
    static const int SwapChainBufferCount = 2;
    ComPtr<ID3D12Resource> mSwapChainBuffer[SwapChainBufferCount];
    int mCurrBackBuffer = 0;

    // Дескрипторы
    ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    ComPtr<ID3D12DescriptorHeap> mCbvHeap;  // Для CBV дескрипторов
    ComPtr<ID3D12Resource> mDepthStencilBuffer;

    UINT mRtvDescriptorSize = 0;
    UINT mDsvDescriptorSize = 0;
    UINT mCbvSrvUavDescriptorSize = 0;

    // Форматы
    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    // Размеры
    int mClientWidth = 800;
    int mClientHeight = 600;

    // Viewport и Scissor
    D3D12_VIEWPORT mScreenViewport;
    D3D12_RECT mScissorRect;

    // Таймер и состояние
    Timer mTimer;
    bool mAppPaused = false;
    bool mResizing = false;
    bool mComInitialized = false;
    int mFrameCount = 0;
    float mTimeElapsed = 0.0f;
    std::wstring mMainWndCaption = L"DirectX 12 Framework";

    // =========== Geometry ===========
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;
    Microsoft::WRL::ComPtr<ID3D12Resource> mVertexBufferGPU;
    Microsoft::WRL::ComPtr<ID3D12Resource> mVertexBufferUploader;
    D3D12_VERTEX_BUFFER_VIEW mVertexBufferView;
    Microsoft::WRL::ComPtr<ID3D12Resource> mIndexBufferGPU;
    Microsoft::WRL::ComPtr<ID3D12Resource> mIndexBufferUploader;
    D3D12_INDEX_BUFFER_VIEW mIndexBufferView;

    // =========== Shaders ===========
    Microsoft::WRL::ComPtr<ID3DBlob> mvsByteCode = nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> mpsByteCode = nullptr;

    // =========== Constant Buffer ===========
    std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;
    std::unique_ptr<RenderingSystem> mRenderingSystem = nullptr;

    // =========== Root Signature и PSO ===========
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mWireframePSO;  // Второй PSO для проволочного каркаса
    bool mWireframeMode = false;  // Флаг режима отображения
    enum class DebugViewMode : UINT { Default = 1, NormalMap = 2, Tessellation = 3 };
    DebugViewMode mDebugViewMode = DebugViewMode::Default;
    bool mF1KeyDown = false;
    bool mF2KeyDown = false;
    bool mF3KeyDown = false;
    CullingMode mCullingMode = CullingMode::Octree;
    bool mShowCullingScene = false;
    bool mF7KeyDown = false;
    bool mObserveCulling = false;
    bool mShowCulledBounds = true;
    bool mF10KeyDown = false;
    bool mF11KeyDown = false;
    bool mTessellationCacheEnabled = true;
    bool mTessellationKeyDown = false;
    ParticleSettings mParticleSettings = [] { ParticleSettings p; p.Visible = false; return p; }();
    bool mParticleKeysDown[6] = {};
    ShadowSettings mShadowSettings;
    PostProcessingSettings mPostSettings;
    bool mPostKeysDown[6] = {};
    bool mPostStatusVisible = true;
    bool mShadowKeysDown[5] = {};

    // Математика для камеры
    float mTheta = 1.5f * XM_PI;
    float mPhi = XM_PIDIV4;
    float mRadius = 5.0f;
    POINT mLastMousePos;
    XMFLOAT4X4 mWorld = MathHelper::Identity4x4();
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    UINT mIndexCount = 0;

    // Вспомогательные методы инициализации
    bool CreateDXGIFactory();
    bool GetHardwareAdapter();
    bool CreateD3DDevice();
    bool CreateCommandObjects();
    bool CreateFence();
    void FlushCommandQueue();
    bool CreateSwapChain();
    void QueryDescriptorSizes();
    bool CreateDescriptorHeaps();
    bool CreateRenderTargetViews();
    bool CreateDepthStencilBuffer();
    void CreateViewportAndScissor();
    void SetViewportAndScissor();

    // Методы для геометрии и шейдеров
    void BuildInputLayout();
    //void BuildVertexBuffer();
    //void BuildIndexBuffer();
    void BuildShaders();
    void BuildConstantBuffer();
    void BuildRootSignature();
    void BuildPSO();
    void BuildWireframePSO();  // Новый метод для создания проволочного PSO
    void CreateColorTexture(
        const DirectX::XMFLOAT3& color,
        Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    void UpdateUvDirectionFromInput();
    XMFLOAT3 GetCameraForward() const;
    void ShootLightProjectile();
    void UpdateShotLights(float dt);
    void AddPlacedLight(const XMFLOAT3& position);
    bool RaycastScene(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance, RaycastHit& outHit) const;
    void FitCameraToLoadedScene();

    // Методы для доступа к ресурсам
    ID3D12Resource* CurrentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const {
        return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
    }
};
