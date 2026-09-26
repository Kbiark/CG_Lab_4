#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <vector>
#include "ObjectConstants.h"
#include "UploadBuffer.h"
#include "Material.h"
#include "Submesh.h"
#include "CullingScene.h"
#include "ParticleSystem.h"
#include "ShadowCascades.h"
#include "Vertex.h"

using Microsoft::WRL::ComPtr;

struct DirectionalLight
{
    DirectX::XMFLOAT3 Direction = { -0.4f, -1.0f, 0.2f };
    float Intensity = 1.0f;
    DirectX::XMFLOAT3 Color = { 1.0f, 0.95f, 0.85f };
    float Padding = 0.0f;
};

struct PointLight
{
    DirectX::XMFLOAT3 Position = { 0.0f, 2.0f, 0.0f };
    float Range = 8.0f;
    DirectX::XMFLOAT3 Color = { 1.0f, 0.7f, 0.4f };
    float Intensity = 4.0f;
};

struct DynamicPointLight
{
    PointLight Light;
    float Age = 0.0f;
};

struct SpotLight
{
    DirectX::XMFLOAT3 Position = { 0.0f, 4.0f, -4.0f };
    float Range = 14.0f;
    DirectX::XMFLOAT3 Direction = { 0.0f, -1.0f, 0.3f };
    float Angle = 0.75f;
    DirectX::XMFLOAT3 Color = { 0.6f, 0.8f, 1.0f };
    float Intensity = 6.0f;
};

struct PostProcessingSettings
{
    bool Enabled = true;
    bool Vignette = true;
    bool ChromaticAberration = true;
    float VignetteStrength = 0.5f;
    float ChromaticPixels = 3.0f;
    UINT BufferView = 0; // 0: final image, 1: albedo, 2: normals, 3: depth.
};

struct SceneRenderContext
{
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW IndexBufferView = {};
    UploadBuffer<ObjectConstants>* ObjectConstantsBuffer = nullptr;
    ID3D12DescriptorHeap* MaterialHeap = nullptr;
    UINT MaterialDescriptorSize = 0;
    std::vector<Submesh>* Submeshes = nullptr;
    std::vector<Material>* Materials = nullptr;
    DirectX::XMFLOAT4X4 View = {};
    DirectX::XMFLOAT4X4 Proj = {};
    DirectX::XMFLOAT3 EyePos = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT2 UvOffset = { 0.0f, 0.0f };
    const std::vector<DynamicPointLight>* DynamicPointLights = nullptr;
    bool Wireframe = false;
    UINT DebugViewMode = 1;
    CullingMode Culling = CullingMode::Octree;
    bool ShowCullingScene = true;
    bool ObserveCulling = false;
    bool ShowCulledBounds = true;
    ParticleSettings Particles;
    float DeltaTime = 0.0f;
    ShadowSettings Shadows;
    DirectX::BoundingBox SceneBounds;
    bool CacheTessellation = true;
    PostProcessingSettings Post;
};

struct TessellationCacheStats
{
    UINT64 Updates = 0;
    UINT64 ReusedFrames = 0;
    UINT64 Triangles = 0;
    UINT ReadyMeshes = 0;
    UINT FallbackMeshes = 0;
};

class GBuffer
{
public:
    bool Initialize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        UINT rtvDescriptorSize);

    void ClearGeometryTargets(ID3D12GraphicsCommandList* commandList) const;
    void ClearLightingTarget(ID3D12GraphicsCommandList* commandList) const;
    void BindForGeometryPass(ID3D12GraphicsCommandList* commandList) const;
    void TransitionToGeometryPass(ID3D12GraphicsCommandList* commandList) const;
    void TransitionToLightingPass(ID3D12GraphicsCommandList* commandList) const;
    void TransitionLightingToRenderTarget(ID3D12GraphicsCommandList* commandList) const;
    void TransitionLightingToShaderResource(ID3D12GraphicsCommandList* commandList) const;

    ID3D12Resource* GetAlbedoResource() const { return mAlbedo.Get(); }
    ID3D12Resource* GetNormalResource() const { return mNormal.Get(); }
    ID3D12Resource* GetDepthResource() const { return mDepth.Get(); }
    ID3D12Resource* GetLightingResource() const { return mLighting.Get(); }

    D3D12_CPU_DESCRIPTOR_HANDLE GetAlbedoRtv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetNormalRtv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetLightingRtv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthDsv() const;

    DXGI_FORMAT GetDepthSrvFormat() const { return DXGI_FORMAT_R24_UNORM_X8_TYPELESS; }

private:
    ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    ComPtr<ID3D12Resource> mAlbedo;
    ComPtr<ID3D12Resource> mNormal;
    ComPtr<ID3D12Resource> mLighting;
    ComPtr<ID3D12Resource> mDepth;
    UINT mRtvDescriptorSize = 0;
};

class RenderingSystem
{
    friend struct ShadowTestAccess;
public:
    const CullingStats& GetCullingStats() const { return mCullingScene.Stats; }
    UINT GetParticleCount() const { return mParticles.AliveCount(); }
    void ResetParticles() { mParticles.RequestReset(); }
    // Call only after the frame fence has completed.
    void OnFrameComplete();
    const TessellationCacheStats& GetTessellationCacheStats() const { return mTessStats; }
    size_t GetOctreeNodeCount() const { return mCullingScene.NodeCount(); }
    size_t GetSceneObjectCount() const { return mCullingScene.Objects.size(); }
    const std::array<float, 4>& GetCascadeSplits() const { return mCascades.Splits; }
    bool Initialize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        DXGI_FORMAT backBufferFormat,
        UINT cbvSrvUavDescriptorSize,
        UINT rtvDescriptorSize);

    void Render(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* backBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferView,
        const SceneRenderContext& scene);

private:
    static constexpr UINT CachedVertexStride = 64;
    static constexpr UINT64 TessellationCacheBudget = 512ull * 1024 * 1024;
    struct TessellationMesh
    {
        ComPtr<ID3D12Resource> Active, Scratch, Counter;
        UINT64 ActiveBytes = 0, ScratchBytes = 0, RequiredBytes = 0;
        UINT VertexCount = 0;
        bool Pending = false, Captured = false;
        ObjectConstants LastConstants = {};
        ID3D12Resource* HeightMap = nullptr;
        UINT IndexStart = 0, IndexCount = 0;
    };
    std::vector<TessellationMesh> mTessMeshes;
    TessellationCacheStats mTessStats;
    D3D12_VERTEX_BUFFER_VIEW mTessSourceVb = {};
    D3D12_INDEX_BUFFER_VIEW mTessSourceIb = {};
    ComPtr<ID3D12QueryHeap> mTessQueries;
    ComPtr<ID3D12Resource> mTessReadback;
    std::unique_ptr<UploadBuffer<UINT64>> mTessZero;
    ComPtr<ID3DBlob> mCacheDs, mCachedVs;
    ComPtr<ID3D12PipelineState> mCaptureTessPso, mCachedPso, mCachedWirePso, mCachedShadowPso;
    void UpdateTessellationCache(ID3D12GraphicsCommandList* commands, const SceneRenderContext& scene);
    void ReadCompletedTessellation();
    CullingScene mCullingScene;
    DirectX::BoundingFrustum mCullingFrustum;
    ParticleSystem mParticles;
    ShadowCascades mCascades;
    ComPtr<ID3D12Resource> mShadowMap;
    ComPtr<ID3D12DescriptorHeap> mShadowDsvHeap;
    ComPtr<ID3D12PipelineState> mShadowPso, mShadowNoTessPso, mShadowInstancePso, mGroundPso;
    ComPtr<ID3DBlob> mShadowHs, mShadowPs, mGroundPs;
    std::unique_ptr<UploadBuffer<ObjectConstants>> mDrawConstants;
    std::unique_ptr<UploadBuffer<InstanceData>> mShadowInstances;
    std::unique_ptr<UploadBuffer<Vertex>> mGroundVertices;
    UINT mObjectSlots = 0;
    UINT mShadowInstanceCapacity = 0;
    void BuildShadowResources();
    void RenderShadows(ID3D12GraphicsCommandList* commands, const SceneRenderContext& scene);
    void RenderModels(ID3D12GraphicsCommandList* commands, const SceneRenderContext& scene,
        const DirectX::XMFLOAT4X4& viewProjection, bool shadow, UINT constantBase);
    void BindObjectConstants(ID3D12GraphicsCommandList* commands, UINT slot, const ObjectConstants& constants);
    void RenderGround(ID3D12GraphicsCommandList* commands, const SceneRenderContext& scene);
    std::unique_ptr<UploadBuffer<InstanceData>> mInstanceBuffer;
    UINT mInstanceCapacity = 0;
    ComPtr<ID3DBlob> mInstanceVs;
    ComPtr<ID3D12PipelineState> mInstancePso;
    ComPtr<ID3D12PipelineState> mInstanceWireframePso;
    void RenderInstances(ID3D12GraphicsCommandList* commandList, const SceneRenderContext& scene);
    void RenderCullingObserver(ID3D12GraphicsCommandList* commands,
        D3D12_CPU_DESCRIPTOR_HANDLE target, const SceneRenderContext& scene);
    struct DebugLineVertex
    {
        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT3 Color;
    };
    ComPtr<ID3DBlob> mCullingLineVs, mCullingLinePs, mCullingObserverPs;
    ComPtr<ID3D12PipelineState> mCullingLinePso, mCullingObserverPso;
    std::unique_ptr<UploadBuffer<DebugLineVertex>> mCullingLines;
    UINT mCullingLineCapacity = 0;
    static constexpr UINT MaxDirectionalLights = 1;
    static constexpr UINT MaxSpotLights = 2;
    static constexpr UINT MaxPointLightVolumes = 512;

    struct DirectionalLightGpu
    {
        DirectX::XMFLOAT4 DirectionIntensity;
        DirectX::XMFLOAT4 Color;
    };

    struct PointLightGpu
    {
        DirectX::XMFLOAT4 PositionRange;
        DirectX::XMFLOAT4 ColorIntensity;
    };

    struct SpotLightGpu
    {
        DirectX::XMFLOAT4 PositionRange;
        DirectX::XMFLOAT4 DirectionAngle;
        DirectX::XMFLOAT4 ColorIntensity;
    };

    struct FrameConstants
    {
        DirectX::XMFLOAT4X4 InvViewProj;
        DirectX::XMFLOAT4X4 ViewProj;
        DirectX::XMFLOAT4 CameraPosition;
        DirectX::XMFLOAT4 LightCounts;
        DirectX::XMFLOAT4 ScreenSize;
        DirectionalLightGpu DirectionalLights[MaxDirectionalLights];
        SpotLightGpu SpotLights[MaxSpotLights];
        DirectX::XMFLOAT4X4 View;
        DirectX::XMFLOAT4X4 ShadowViewProjection[4];
        DirectX::XMFLOAT4 CascadeSplits;
        DirectX::XMFLOAT4 ShadowParams;
        DirectX::XMFLOAT4 ShadowConfig;
    };

    struct PointLightVolumeConstants
    {
        DirectX::XMFLOAT4 PositionRange;
        DirectX::XMFLOAT4 ColorIntensity;
    };

    void BuildLights();
    void BuildRootSignatures();
    void BuildPsos(DXGI_FORMAT backBufferFormat);
    void BuildDeferredDescriptorHeap();
    void BuildFrameConstants();
    void BuildShaders();
    void BuildPointLightVolumeMesh();
    struct SphereVertex
    {
        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT2 UV;
        DirectX::XMFLOAT3 Tangent;
        DirectX::XMFLOAT3 Bitangent;
    };
    void UpdateFrameConstants(const SceneRenderContext& scene);
    void RenderPointLightVolume(
        ID3D12GraphicsCommandList* commandList,
        const PointLight& light,
        UINT lightIndex);
    Material* FindMaterial(const SceneRenderContext& scene, const std::string& materialName) const;

    ComPtr<ID3D12Device> mDevice;
    UINT mCbvSrvUavDescriptorSize = 0;
    UINT mWidth = 1;
    UINT mHeight = 1;
    GBuffer mGBuffer;

    ComPtr<ID3D12RootSignature> mGeometryRootSignature;
    ComPtr<ID3D12RootSignature> mLightingRootSignature;
    ComPtr<ID3D12RootSignature> mPointLightRootSignature;
    ComPtr<ID3D12RootSignature> mFinalRootSignature;
    ComPtr<ID3D12RootSignature> mPostRootSignature;
    ComPtr<ID3D12Resource> mPostOutput;
    ComPtr<ID3D12PipelineState> mPostPso;
    ComPtr<ID3DBlob> mPostCs;

    ComPtr<ID3D12PipelineState> mGeometryPso;
    ComPtr<ID3D12PipelineState> mGeometryWireframePso;
    ComPtr<ID3D12PipelineState> mGeometryNoTessPso;
    ComPtr<ID3D12PipelineState> mGeometryNoTessWireframePso;
    ComPtr<ID3D12PipelineState> mLightingPso;
    ComPtr<ID3D12PipelineState> mPointLightPso;
    ComPtr<ID3D12PipelineState> mFinalPso;

    ComPtr<ID3DBlob> mGeometryVs;
    ComPtr<ID3DBlob> mGeometryNoTessVs;
    ComPtr<ID3DBlob> mGeometryHs;
    ComPtr<ID3DBlob> mGeometryDs;
    ComPtr<ID3DBlob> mGeometryPs;
    ComPtr<ID3DBlob> mLightingVs;
    ComPtr<ID3DBlob> mLightingPs;
    ComPtr<ID3DBlob> mPointLightVs;
    ComPtr<ID3DBlob> mPointLightPs;
    ComPtr<ID3DBlob> mFinalVs;
    ComPtr<ID3DBlob> mFinalPs;

    ComPtr<ID3D12DescriptorHeap> mDeferredHeap;
    std::unique_ptr<UploadBuffer<FrameConstants>> mFrameConstants;
    std::unique_ptr<UploadBuffer<PointLightVolumeConstants>> mPointLightConstants;

    ComPtr<ID3D12Resource> mPointLightVertexBuffer;
    ComPtr<ID3D12Resource> mPointLightIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW mPointLightVertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW mPointLightIndexBufferView = {};
    UINT mPointLightIndexCount = 0;

    std::vector<DirectionalLight> mDirectionalLights;
    std::vector<PointLight> mPointLights;
    std::vector<SpotLight> mSpotLights;
};
