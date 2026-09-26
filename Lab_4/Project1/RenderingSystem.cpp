#include "RenderingSystem.h"
#include "d3dUtil.h"
#include "ThrowIfFailed.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <windows.h>

using namespace DirectX;

namespace
{
    std::filesystem::path GetExeDirectory()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    void StartupLog(const std::string& message)
    {
        OutputDebugStringA((message + "\n").c_str());

        std::ofstream log(GetExeDirectory() / "Project1_startup.log", std::ios::app);
        if (log)
            log << message << '\n';
    }

    void StartupLogHr(const std::string& message, HRESULT hr)
    {
        char buffer[128] = {};
        sprintf_s(buffer, " HRESULT=0x%08X", static_cast<unsigned int>(hr));
        StartupLog(message + buffer);
    }

    D3D12_RESOURCE_BARRIER TransitionBarrier(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return barrier;
    }
}

bool GBuffer::Initialize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    UINT rtvDescriptorSize)
{
    mRtvDescriptorSize = rtvDescriptorSize;

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 3;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap)));

    auto createRenderTarget = [&](DXGI_FORMAT format, ComPtr<ID3D12Resource>& resource, bool normalTarget = false)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = format;
        clearValue.Color[3] = 1.0f;
        if (normalTarget)
        {
            clearValue.Color[0] = clearValue.Color[1] = 0.5f;
            clearValue.Color[2] = 1.0f;
        }

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            IID_PPV_ARGS(&resource)));
    };

    createRenderTarget(DXGI_FORMAT_R8G8B8A8_UNORM, mAlbedo);
    createRenderTarget(DXGI_FORMAT_R16G16B16A16_FLOAT, mNormal, true);
    createRenderTarget(DXGI_FORMAT_R16G16B16A16_FLOAT, mLighting);

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &depthClear,
        IID_PPV_ARGS(&mDepth)));

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(mAlbedo.Get(), nullptr, rtvHandle);
    rtvHandle.ptr += mRtvDescriptorSize;
    device->CreateRenderTargetView(mNormal.Get(), nullptr, rtvHandle);
    rtvHandle.ptr += mRtvDescriptorSize;
    device->CreateRenderTargetView(mLighting.Get(), nullptr, rtvHandle);

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(mDepth.Get(), &dsvDesc, mDsvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

void GBuffer::ClearGeometryTargets(ID3D12GraphicsCommandList* commandList) const
{
    const float clearAlbedo[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float clearNormal[] = { 0.5f, 0.5f, 1.0f, 1.0f };

    commandList->ClearRenderTargetView(GetAlbedoRtv(), clearAlbedo, 0, nullptr);
    commandList->ClearRenderTargetView(GetNormalRtv(), clearNormal, 0, nullptr);
    commandList->ClearDepthStencilView(GetDepthDsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void GBuffer::ClearLightingTarget(ID3D12GraphicsCommandList* commandList) const
{
    const float clearLighting[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList->ClearRenderTargetView(GetLightingRtv(), clearLighting, 0, nullptr);
}

void GBuffer::BindForGeometryPass(ID3D12GraphicsCommandList* commandList) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { GetAlbedoRtv(), GetNormalRtv() };
    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv = GetDepthDsv();
    commandList->OMSetRenderTargets(2, rtvs, FALSE, &depthDsv);
}

void GBuffer::TransitionToGeometryPass(ID3D12GraphicsCommandList* commandList) const
{
    D3D12_RESOURCE_BARRIER barriers[] =
    {
        TransitionBarrier(mAlbedo.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        TransitionBarrier(mNormal.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        TransitionBarrier(mDepth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)
    };
    commandList->ResourceBarrier(_countof(barriers), barriers);
}

void GBuffer::TransitionToLightingPass(ID3D12GraphicsCommandList* commandList) const
{
    D3D12_RESOURCE_BARRIER barriers[] =
    {
        TransitionBarrier(mAlbedo.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(mNormal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(mDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    };
    commandList->ResourceBarrier(_countof(barriers), barriers);
}

void GBuffer::TransitionLightingToRenderTarget(ID3D12GraphicsCommandList* commandList) const
{
    D3D12_RESOURCE_BARRIER barrier =
        TransitionBarrier(mLighting.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &barrier);
}

void GBuffer::TransitionLightingToShaderResource(ID3D12GraphicsCommandList* commandList) const
{
    D3D12_RESOURCE_BARRIER barrier =
        TransitionBarrier(mLighting.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetAlbedoRtv() const
{
    return mRtvHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetNormalRtv() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = GetAlbedoRtv();
    handle.ptr += mRtvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetLightingRtv() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = GetAlbedoRtv();
    handle.ptr += mRtvDescriptorSize * 2;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetDepthDsv() const
{
    return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

bool RenderingSystem::Initialize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT backBufferFormat,
    UINT cbvSrvUavDescriptorSize,
    UINT rtvDescriptorSize)
{
    mDevice = device;
    mCbvSrvUavDescriptorSize = cbvSrvUavDescriptorSize;
    mWidth = width;
    mHeight = height;

    StartupLog("RenderingSystem BuildLights begin");
    BuildLights();
    StartupLog("RenderingSystem BuildLights ok");
    StartupLog("RenderingSystem GBuffer Initialize begin");
    mGBuffer.Initialize(device, width, height, rtvDescriptorSize);
    StartupLog("RenderingSystem GBuffer Initialize ok");
    StartupLog("RenderingSystem BuildPointLightVolumeMesh begin");
    BuildPointLightVolumeMesh();
    mCullingScene.Build();
    mInstanceCapacity = static_cast<UINT>(mCullingScene.Instances.size());
    mInstanceBuffer = std::make_unique<UploadBuffer<InstanceData>>(device, mInstanceCapacity, false);
    StartupLog("RenderingSystem BuildPointLightVolumeMesh ok");
    StartupLog("RenderingSystem BuildShaders begin");
    BuildShaders();
    BuildShadowResources();
    StartupLog("RenderingSystem BuildShaders ok");
    StartupLog("RenderingSystem BuildRootSignatures begin");
    BuildRootSignatures();
    StartupLog("RenderingSystem BuildRootSignatures ok");
    StartupLog("RenderingSystem BuildPsos begin");
    BuildPsos(backBufferFormat);
    StartupLog("RenderingSystem BuildPsos ok");
    StartupLog("RenderingSystem BuildDeferredDescriptorHeap begin");
    BuildDeferredDescriptorHeap();
    StartupLog("RenderingSystem BuildDeferredDescriptorHeap ok");
    StartupLog("RenderingSystem BuildFrameConstants begin");
    BuildFrameConstants();
    StartupLog("RenderingSystem BuildFrameConstants ok");
    mParticles.Initialize(device);
    StartupLog("RenderingSystem GPU particles initialized");

    return true;
}

void RenderingSystem::Render(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* backBuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferView,
    const SceneRenderContext& scene)
{
    const UINT requiredSlots = static_cast<UINT>(scene.Submeshes->size()) + 2;
    if (mObjectSlots != requiredSlots)
    {
        mObjectSlots = requiredSlots;
        mDrawConstants = std::make_unique<UploadBuffer<ObjectConstants>>(mDevice.Get(), mObjectSlots * 6, true);
    }
    BoundingBox casters = scene.SceneBounds;
    float displacementMargin = 0.01f;
    for (const auto& material : *scene.Materials)
        displacementMargin = (std::max)(displacementMargin, std::abs(material.DisplacementScale) + 0.01f);
    casters.Extents.x += displacementMargin;
    casters.Extents.y += displacementMargin;
    casters.Extents.z += displacementMargin;
    if (scene.ShowCullingScene && !mCullingScene.Bounds.empty())
        BoundingBox::CreateMerged(casters, casters, mCullingScene.RootBounds());
    mCascades.Update(scene.View, scene.Proj, mDirectionalLights[0].Direction, casters, scene.Shadows);
    UpdateFrameConstants(scene);
    mParticles.Simulate(commandList, scene.DeltaTime, scene.Particles, scene.View, scene.Proj);
    UpdateTessellationCache(commandList, scene);
    if (scene.Shadows.Enabled)
        RenderShadows(commandList, scene);

    const D3D12_VIEWPORT viewport = { 0, 0, float(mWidth), float(mHeight), 0, 1 };
    const D3D12_RECT scissor = { 0, 0, LONG(mWidth), LONG(mHeight) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    mGBuffer.TransitionToGeometryPass(commandList);
    mGBuffer.ClearGeometryTargets(commandList);
    mGBuffer.BindForGeometryPass(commandList);

    XMFLOAT4X4 cameraViewProjection;
    XMStoreFloat4x4(&cameraViewProjection, XMLoadFloat4x4(&scene.View) * XMLoadFloat4x4(&scene.Proj));
    RenderModels(commandList, scene, cameraViewProjection, false, 0);

    if (scene.ShowCullingScene)
        RenderInstances(commandList, scene);
    else
        mCullingScene.Stats = {};

    if (scene.Shadows.ShowGround)
        RenderGround(commandList, scene);

    if (scene.Particles.Visible)
        mParticles.Draw(commandList);

    mGBuffer.TransitionToLightingPass(commandList);
    mGBuffer.TransitionLightingToRenderTarget(commandList);
    mGBuffer.ClearLightingTarget(commandList);

    ID3D12DescriptorHeap* deferredHeaps[] = { mDeferredHeap.Get() };
    commandList->SetDescriptorHeaps(1, deferredHeaps);
    commandList->SetGraphicsRootSignature(mLightingRootSignature.Get());
    commandList->SetPipelineState(mLightingPso.Get());
    D3D12_CPU_DESCRIPTOR_HANDLE lightingRtv = mGBuffer.GetLightingRtv();
    commandList->OMSetRenderTargets(1, &lightingRtv, FALSE, nullptr);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_GPU_DESCRIPTOR_HANDLE gbufferSrvHandle = mDeferredHeap->GetGPUDescriptorHandleForHeapStart();
    commandList->SetGraphicsRootDescriptorTable(0, gbufferSrvHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE frameCbvHandle = gbufferSrvHandle;
    frameCbvHandle.ptr += 4 * mCbvSrvUavDescriptorSize;
    commandList->SetGraphicsRootDescriptorTable(1, frameCbvHandle);
    auto shadowSrv = gbufferSrvHandle;
    shadowSrv.ptr += 5 * mCbvSrvUavDescriptorSize;
    commandList->SetGraphicsRootDescriptorTable(2, shadowSrv);
    commandList->DrawInstanced(3, 1, 0, 0);

    // Dynamic point lights are accumulated with additive blending by drawing one light volume per source.
    commandList->SetGraphicsRootSignature(mPointLightRootSignature.Get());
    commandList->SetPipelineState(mPointLightPso.Get());
    commandList->SetGraphicsRootDescriptorTable(0, gbufferSrvHandle);
    commandList->SetGraphicsRootDescriptorTable(1, frameCbvHandle);
    commandList->IASetVertexBuffers(0, 1, &mPointLightVertexBufferView);
    commandList->IASetIndexBuffer(&mPointLightIndexBufferView);

    UINT lightIndex = 0;
    for (const PointLight& light : mPointLights)
    {
        if (lightIndex >= MaxPointLightVolumes)
            break;

        RenderPointLightVolume(commandList, light, lightIndex++);
    }

    if (scene.DynamicPointLights)
    {
        for (const DynamicPointLight& dynamicLight : *scene.DynamicPointLights)
        {
            if (lightIndex >= MaxPointLightVolumes)
                break;

            RenderPointLightVolume(commandList, dynamicLight.Light, lightIndex++);
        }
    }

    mGBuffer.TransitionLightingToShaderResource(commandList);

    ID3D12Resource* postInputs[] = { mGBuffer.GetAlbedoResource(), mGBuffer.GetNormalResource(),
        mGBuffer.GetDepthResource(), mGBuffer.GetLightingResource() };
    D3D12_RESOURCE_BARRIER postBarriers[5];
    for (UINT i = 0; i < 4; ++i)
        postBarriers[i] = TransitionBarrier(postInputs[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    postBarriers[4] = TransitionBarrier(mPostOutput.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->ResourceBarrier(5, postBarriers);
    commandList->SetComputeRootSignature(mPostRootSignature.Get());
    commandList->SetPipelineState(mPostPso.Get());
    auto postHandle = mDeferredHeap->GetGPUDescriptorHandleForHeapStart();
    commandList->SetComputeRootDescriptorTable(0, postHandle);
    postHandle.ptr += 7 * mCbvSrvUavDescriptorSize;
    commandList->SetComputeRootDescriptorTable(2, postHandle);
    const float postConstants[] = {
        scene.Post.Enabled && scene.Post.Vignette ? (std::clamp)(scene.Post.VignetteStrength, 0.0f, 1.0f) : 0,
        scene.Post.Enabled && scene.Post.ChromaticAberration ? (std::clamp)(scene.Post.ChromaticPixels, 0.0f, 12.0f) : 0,
        1.0f / mWidth, 1.0f / mHeight,
        float((std::min)(scene.Post.BufferView, 3u)), scene.Proj._33, scene.Proj._43, 1.08f
    };
    commandList->SetComputeRoot32BitConstants(1, _countof(postConstants), postConstants, 0);
    commandList->Dispatch((mWidth + 7) / 8, (mHeight + 7) / 8, 1);
    for (auto& barrier : postBarriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commandList->ResourceBarrier(5, postBarriers);

    D3D12_RESOURCE_BARRIER backBufferBarrier =
        TransitionBarrier(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &backBufferBarrier);

    commandList->SetGraphicsRootSignature(mFinalRootSignature.Get());
    commandList->SetPipelineState(mFinalPso.Get());
    commandList->OMSetRenderTargets(1, &backBufferView, FALSE, nullptr);
    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList->ClearRenderTargetView(backBufferView, clearColor, 0, nullptr);

    postHandle.ptr -= mCbvSrvUavDescriptorSize; // Slot 6: computed image SRV.
    commandList->SetGraphicsRootDescriptorTable(0, postHandle);
    const D3D12_VERTEX_BUFFER_VIEW noVertexBuffers[2] = {};
    commandList->IASetVertexBuffers(0, 2, noVertexBuffers);
    commandList->IASetIndexBuffer(nullptr);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    commandList->DrawInstanced(4, 1, 0, 0);
    if (scene.ShowCullingScene && scene.ObserveCulling)
        RenderCullingObserver(commandList, backBufferView, scene);
}

void RenderingSystem::BindObjectConstants(ID3D12GraphicsCommandList* commands, UINT slot, const ObjectConstants& constants)
{
    mDrawConstants->CopyData(slot, constants);
    commands->SetGraphicsRootConstantBufferView(0, mDrawConstants->Resource()->GetGPUVirtualAddress() +
        UINT64(slot) * d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants)));
}

void RenderingSystem::RenderModels(ID3D12GraphicsCommandList* commands, const SceneRenderContext& scene,
    const XMFLOAT4X4& viewProjection, bool shadow, UINT constantBase)
{
    commands->SetGraphicsRootSignature(mGeometryRootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { scene.MaterialHeap };
    commands->SetDescriptorHeaps(1, heaps);
    commands->IASetVertexBuffers(0, 1, &scene.VertexBufferView);
    commands->IASetIndexBuffer(&scene.IndexBufferView);
    for (UINT i = 0; i < scene.Submeshes->size(); ++i)
    {
        const auto& submesh = (*scene.Submeshes)[i];
        const Material* material = FindMaterial(scene, submesh.MaterialName);
        if (!material) continue;
        const bool tessellated = material->DisplacementTexture && material->DisplacementScale > 0;
        const bool cached = tessellated && scene.CacheTessellation && i < mTessMeshes.size() && mTessMeshes[i].Active;
        ID3D12PipelineState* pso = shadow ? (tessellated ? mShadowPso.Get() : mShadowNoTessPso.Get()) :
            (tessellated ? (scene.Wireframe ? mGeometryWireframePso.Get() : mGeometryPso.Get()) :
                (scene.Wireframe ? mGeometryNoTessWireframePso.Get() : mGeometryNoTessPso.Get()));
        if (cached)
            pso = shadow ? mCachedShadowPso.Get() : (scene.Wireframe ? mCachedWirePso.Get() : mCachedPso.Get());
        commands->SetPipelineState(pso);
        commands->IASetPrimitiveTopology(tessellated && !cached ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST :
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ObjectConstants constants;
        XMStoreFloat4x4(&constants.mWorldViewProj, XMMatrixTranspose(XMLoadFloat4x4(&viewProjection)));
        constants.uvTiling = material->Tiling;
        constants.uvOffset = { scene.UvOffset.x + material->StaticUvOffset.x, scene.UvOffset.y + material->StaticUvOffset.y };
        constants.eyePosAndDisplacementScale = { scene.EyePos.x, scene.EyePos.y, scene.EyePos.z, material->DisplacementScale };
        constants.tessellationParams = material->TessellationParams;
        constants.normalParams = { material->NormalStrength, 2048, 2048, float(scene.DebugViewMode) };
        // Each draw/cascade needs its own slot: GPU reads these after CPU finishes recording.
        BindObjectConstants(commands, constantBase + i, constants);
        auto handle = scene.MaterialHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += (1 + material->SrvHeapIndex) * scene.MaterialDescriptorSize;
        commands->SetGraphicsRootDescriptorTable(1, handle);
        if (cached)
        {
            const auto& mesh = mTessMeshes[i];
            const D3D12_VERTEX_BUFFER_VIEW vb = { mesh.Active->GetGPUVirtualAddress(),
                mesh.VertexCount * CachedVertexStride, CachedVertexStride };
            commands->IASetVertexBuffers(0, 1, &vb);
            commands->DrawInstanced(mesh.VertexCount, 1, 0, 0);
        }
        else
        {
            commands->IASetVertexBuffers(0, 1, &scene.VertexBufferView);
            commands->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexStart, 0, 0);
        }
    }
}

void RenderingSystem::OnFrameComplete()
{
    mParticles.ReadCompletedCount();
    ReadCompletedTessellation();
}

void RenderingSystem::UpdateTessellationCache(ID3D12GraphicsCommandList* commands, const SceneRenderContext& scene)
{
    auto makeBuffer = [&](UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
        ComPtr<ID3D12Resource>& resource)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto initialState = type == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_STATE_COMMON : state;
        const HRESULT hr = mDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
            IID_PPV_ARGS(resource.ReleaseAndGetAddressOf()));
        if (SUCCEEDED(hr) && initialState != state)
        {
            auto barrier = TransitionBarrier(resource.Get(), initialState, state);
            commands->ResourceBarrier(1, &barrier);
        }
        return hr;
    };
    const auto& vb = scene.VertexBufferView;
    const auto& ib = scene.IndexBufferView;
    if (mTessMeshes.size() != scene.Submeshes->size() || vb.BufferLocation != mTessSourceVb.BufferLocation ||
        vb.SizeInBytes != mTessSourceVb.SizeInBytes || vb.StrideInBytes != mTessSourceVb.StrideInBytes ||
        ib.BufferLocation != mTessSourceIb.BufferLocation || ib.SizeInBytes != mTessSourceIb.SizeInBytes ||
        ib.Format != mTessSourceIb.Format)
    {
        mTessMeshes.clear();
        mTessMeshes.resize(scene.Submeshes->size());
        mTessSourceVb = vb;
        mTessSourceIb = ib;
        D3D12_QUERY_HEAP_DESC queries = {};
        queries.Type = D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
        queries.Count = (std::max)(1u, static_cast<UINT>(mTessMeshes.size()));
        ThrowIfFailed(mDevice->CreateQueryHeap(&queries, IID_PPV_ARGS(mTessQueries.ReleaseAndGetAddressOf())));
        ThrowIfFailed(makeBuffer(UINT64(queries.Count) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS),
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, mTessReadback));
        mTessZero = std::make_unique<UploadBuffer<UINT64>>(mDevice.Get(), 1, false);
        mTessZero->CopyData(0, 0);
    }
    mTessStats.ReadyMeshes = mTessStats.FallbackMeshes = 0;
    mTessStats.Triangles = 0;
    bool updated = false;
    for (UINT i = 0; i < mTessMeshes.size(); ++i)
    {
        auto& mesh = mTessMeshes[i];
        const auto& submesh = (*scene.Submeshes)[i];
        const auto* material = FindMaterial(scene, submesh.MaterialName);
        if (!material || !material->DisplacementTexture || material->DisplacementScale <= 0 || submesh.IndexCount == 0)
        {
            mesh = {};
            continue;
        }
        if (mesh.IndexStart != submesh.IndexStart || mesh.IndexCount != submesh.IndexCount)
        {
            mesh = {};
            mesh.IndexStart = submesh.IndexStart;
            mesh.IndexCount = submesh.IndexCount;
        }
        ObjectConstants constants = {};
        constants.uvTiling = material->Tiling;
        constants.uvOffset = { scene.UvOffset.x + material->StaticUvOffset.x, scene.UvOffset.y + material->StaticUvOffset.y };
        constants.eyePosAndDisplacementScale = { scene.EyePos.x, scene.EyePos.y, scene.EyePos.z, material->DisplacementScale };
        constants.tessellationParams = material->TessellationParams;
        const auto& last = mesh.LastConstants;
        const float dx = scene.EyePos.x - last.eyePosAndDisplacementScale.x;
        const float dy = scene.EyePos.y - last.eyePosAndDisplacementScale.y;
        const float dz = scene.EyePos.z - last.eyePosAndDisplacementScale.z;
        // Camera rotation changes only the draw matrices, not the cached world-space mesh.
        const bool changed = !mesh.Captured || dx != 0.0f || dy != 0.0f || dz != 0.0f ||
            material->DisplacementTexture.Get() != mesh.HeightMap ||
            constants.eyePosAndDisplacementScale.w != last.eyePosAndDisplacementScale.w ||
            memcmp(&constants.uvTiling, &last.uvTiling, sizeof(XMFLOAT2)) != 0 ||
            memcmp(&constants.uvOffset, &last.uvOffset, sizeof(XMFLOAT2)) != 0 ||
            memcmp(&constants.tessellationParams, &last.tessellationParams, sizeof(XMFLOAT4)) != 0;
        if (scene.CacheTessellation && changed && !mesh.Pending)
        {
            const UINT64 initial = (std::max)(4096ull, UINT64(submesh.IndexCount) * CachedVertexStride * 2);
            const UINT64 needed = (std::max)(initial, mesh.RequiredBytes);
            UINT64 used = 0;
            for (const auto& other : mTessMeshes) used += other.ActiveBytes + other.ScratchBytes;
            bool available = needed <= TessellationCacheBudget - (used - mesh.ScratchBytes);
            if (available && mesh.ScratchBytes < needed)
            {
                const HRESULT hr = makeBuffer(needed, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, mesh.Scratch);
                if (hr == E_OUTOFMEMORY) available = false;
                else ThrowIfFailed(hr);
                mesh.ScratchBytes = available ? needed : 0;
            }
            if (available)
            {
                if (!mesh.Counter)
                    ThrowIfFailed(makeBuffer(sizeof(UINT64), D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_STREAM_OUT, mesh.Counter));
                auto reset = TransitionBarrier(mesh.Counter.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
                commands->ResourceBarrier(1, &reset);
                commands->CopyBufferRegion(mesh.Counter.Get(), 0, mTessZero->Resource(), 0, sizeof(UINT64));
                D3D12_RESOURCE_BARRIER begin[] = {
                    TransitionBarrier(mesh.Counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT),
                    TransitionBarrier(mesh.Scratch.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_STREAM_OUT)
                };
                commands->ResourceBarrier(_countof(begin), begin);
                commands->SetGraphicsRootSignature(mGeometryRootSignature.Get());
                commands->SetPipelineState(mCaptureTessPso.Get());
                commands->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                ID3D12DescriptorHeap* heaps[] = { scene.MaterialHeap };
                commands->SetDescriptorHeaps(1, heaps);
                // Separate from all five render passes: upload memory is not snapshotted by a draw.
                BindObjectConstants(commands, 5 * mObjectSlots + i, constants);
                auto textures = scene.MaterialHeap->GetGPUDescriptorHandleForHeapStart();
                textures.ptr += (1 + material->SrvHeapIndex) * scene.MaterialDescriptorSize;
                commands->SetGraphicsRootDescriptorTable(1, textures);
                commands->IASetVertexBuffers(0, 1, &vb);
                commands->IASetIndexBuffer(&ib);
                commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
                const D3D12_STREAM_OUTPUT_BUFFER_VIEW output = { mesh.Scratch->GetGPUVirtualAddress(),
                    mesh.ScratchBytes, mesh.Counter->GetGPUVirtualAddress() };
                commands->SOSetTargets(0, 1, &output);
                commands->BeginQuery(mTessQueries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, i);
                commands->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexStart, 0, 0);
                commands->EndQuery(mTessQueries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, i);
                commands->ResolveQueryData(mTessQueries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, i, 1,
                    mTessReadback.Get(), UINT64(i) * sizeof(D3D12_QUERY_DATA_SO_STATISTICS));
                const D3D12_STREAM_OUTPUT_BUFFER_VIEW unbound = {};
                commands->SOSetTargets(0, 1, &unbound);
                auto finish = TransitionBarrier(mesh.Scratch.Get(), D3D12_RESOURCE_STATE_STREAM_OUT,
                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                commands->ResourceBarrier(1, &finish);
                mesh.LastConstants = constants;
                mesh.HeightMap = material->DisplacementTexture.Get();
                mesh.Pending = true;
                updated = true;
            }
            else
            {
                // Never display a truncated mesh when the cache budget is exhausted.
                mesh.Active.Reset();
                mesh.ActiveBytes = 0;
                mesh.Captured = false;
            }
        }
        if (scene.CacheTessellation && mesh.Active)
        {
            ++mTessStats.ReadyMeshes;
            mTessStats.Triangles += mesh.VertexCount / 3;
        }
        else ++mTessStats.FallbackMeshes;
    }
    if (updated) ++mTessStats.Updates;
    else if (scene.CacheTessellation && mTessStats.ReadyMeshes > 0 && mTessStats.FallbackMeshes == 0)
        ++mTessStats.ReusedFrames;
}

void RenderingSystem::ReadCompletedTessellation()
{
    if (!mTessReadback || std::none_of(mTessMeshes.begin(), mTessMeshes.end(),
        [](const TessellationMesh& mesh) { return mesh.Pending; })) return;
    D3D12_RANGE range = { 0, mTessMeshes.size() * sizeof(D3D12_QUERY_DATA_SO_STATISTICS) };
    D3D12_QUERY_DATA_SO_STATISTICS* results = nullptr;
    ThrowIfFailed(mTessReadback->Map(0, &range, reinterpret_cast<void**>(&results)));
    for (UINT i = 0; i < mTessMeshes.size(); ++i)
    {
        auto& mesh = mTessMeshes[i];
        if (!mesh.Pending) continue;
        mesh.Pending = false;
        const UINT64 bytes = results[i].PrimitivesStorageNeeded * 3 * CachedVertexStride;
        mesh.Captured = results[i].NumPrimitivesWritten == results[i].PrimitivesStorageNeeded && bytes <= mesh.ScratchBytes;
        if (mesh.Captured)
        {
            std::swap(mesh.Active, mesh.Scratch);
            std::swap(mesh.ActiveBytes, mesh.ScratchBytes);
            mesh.VertexCount = static_cast<UINT>(results[i].NumPrimitivesWritten * 3);
            mesh.RequiredBytes = bytes;
        }
        else mesh.RequiredBytes = bytes + bytes / 4;
    }
    const D3D12_RANGE none = { 0, 0 };
    mTessReadback->Unmap(0, &none);
}

void RenderingSystem::BuildShadowResources()
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = ShadowCascades::Resolution;
    desc.DepthOrArraySize = ShadowCascades::Count;
    desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1;
    ThrowIfFailed(mDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&mShadowMap)));
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeap = {};
    dsvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeap.NumDescriptors = ShadowCascades::Count;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvHeap, IID_PPV_ARGS(&mShadowDsvHeap)));
    auto handle = mShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < ShadowCascades::Count; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.ArraySize = 1;
        dsv.Texture2DArray.FirstArraySlice = i;
        mDevice->CreateDepthStencilView(mShadowMap.Get(), &dsv, handle);
        handle.ptr += mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    mGroundVertices = std::make_unique<UploadBuffer<Vertex>>(mDevice.Get(), 6, false);
}

void RenderingSystem::RenderShadows(ID3D12GraphicsCommandList* commands, const SceneRenderContext& scene)
{
    auto barrier = TransitionBarrier(mShadowMap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commands->ResourceBarrier(1, &barrier);
    const D3D12_VIEWPORT viewport = { 0, 0, float(ShadowCascades::Resolution), float(ShadowCascades::Resolution), 0, 1 };
    const D3D12_RECT scissor = { 0, 0, ShadowCascades::Resolution, ShadowCascades::Resolution };
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    if (mShadowInstanceCapacity < mCullingScene.Instances.size())
    {
        mShadowInstanceCapacity = static_cast<UINT>(mCullingScene.Instances.size());
        mShadowInstances = std::make_unique<UploadBuffer<InstanceData>>(mDevice.Get(), mShadowInstanceCapacity * 4, false);
    }
    auto dsv = mShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT cascade = 0; cascade < ShadowCascades::Count; ++cascade)
    {
        commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        commands->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        const UINT base = (cascade + 1) * mObjectSlots;
        RenderModels(commands, scene, mCascades.ViewProjection[cascade], true, base);
        if (scene.ShowCullingScene)
        {
            UINT count = 0;
            for (UINT i = 0; i < mCullingScene.Bounds.size(); ++i)
                if (mCascades.Intersects(cascade, mCullingScene.Bounds[i]))
                    mShadowInstances->CopyData(cascade * mShadowInstanceCapacity + count++, mCullingScene.Instances[i]);
            if (count > 0)
            {
                ObjectConstants constants;
                XMStoreFloat4x4(&constants.mWorldViewProj, XMMatrixTranspose(XMLoadFloat4x4(&mCascades.ViewProjection[cascade])));
                BindObjectConstants(commands, base + mObjectSlots - 2, constants);
                D3D12_VERTEX_BUFFER_VIEW instances = {};
                instances.BufferLocation = mShadowInstances->Resource()->GetGPUVirtualAddress() +
                    UINT64(cascade) * mShadowInstanceCapacity * sizeof(InstanceData);
                instances.StrideInBytes = sizeof(InstanceData);
                instances.SizeInBytes = count * sizeof(InstanceData);
                const D3D12_VERTEX_BUFFER_VIEW views[] = { mPointLightVertexBufferView, instances };
                commands->IASetVertexBuffers(0, 2, views);
                commands->IASetIndexBuffer(&mPointLightIndexBufferView);
                commands->SetPipelineState(mShadowInstancePso.Get());
                commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commands->DrawIndexedInstanced(mPointLightIndexCount, count, 0, 0, 0);
            }
        }
        dsv.ptr += mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    barrier = TransitionBarrier(mShadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &barrier);
}

void RenderingSystem::RenderGround(ID3D12GraphicsCommandList* commands, const SceneRenderContext& scene)
{
    const float y = scene.SceneBounds.Center.y - scene.SceneBounds.Extents.y - 0.3f;
    const float size = (std::max)(120.0f, scene.Shadows.Distance * 1.5f);
    const XMFLOAT2 corners[] = { {-1,-1}, {-1,1}, {1,1}, {-1,-1}, {1,1}, {1,-1} };
    for (int i = 0; i < 6; ++i)
    {
        Vertex v = {};
        v.position = { scene.SceneBounds.Center.x + corners[i].x * size, y,
            scene.SceneBounds.Center.z + corners[i].y * size };
        v.normal = { 0, 1, 0 };
        v.tangent = { 1, 0, 0 };
        v.bitangent = { 0, 0, -1 };
        v.texcoord = { v.position.x, v.position.z };
        mGroundVertices->CopyData(i, v);
    }
    ObjectConstants constants;
    XMStoreFloat4x4(&constants.mWorldViewProj, XMMatrixTranspose(XMLoadFloat4x4(&scene.View) * XMLoadFloat4x4(&scene.Proj)));
    BindObjectConstants(commands, mObjectSlots - 1, constants);
    D3D12_VERTEX_BUFFER_VIEW vb = { mGroundVertices->Resource()->GetGPUVirtualAddress(), 6 * sizeof(Vertex), sizeof(Vertex) };
    commands->IASetVertexBuffers(0, 1, &vb);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->SetPipelineState(mGroundPso.Get());
    commands->DrawInstanced(6, 1, 0, 0);
}

void RenderingSystem::RenderInstances(ID3D12GraphicsCommandList* commandList, const SceneRenderContext& scene)
{
    BoundingFrustum viewFrustum;
    BoundingFrustum::CreateFromMatrix(viewFrustum, XMLoadFloat4x4(&scene.Proj));
    viewFrustum.Transform(mCullingFrustum, XMMatrixInverse(nullptr, XMLoadFloat4x4(&scene.View)));
    mCullingScene.Select(mCullingFrustum, scene.Culling);
    if (mCullingScene.VisibleIds.empty())
        return;

    // Draw waits for the GPU at frame end, so this upload buffer can be reused here.
    if (mCullingScene.VisibleIds.size() > mInstanceCapacity)
    {
        mInstanceCapacity = static_cast<UINT>(mCullingScene.VisibleIds.size());
        mInstanceBuffer = std::make_unique<UploadBuffer<InstanceData>>(mDevice.Get(), mInstanceCapacity, false);
    }
    for (size_t i = 0; i < mCullingScene.VisibleIds.size(); ++i)
        mInstanceBuffer->CopyData(static_cast<int>(i),
            mCullingScene.Instances[mCullingScene.VisibleIds[i]]);
    D3D12_VERTEX_BUFFER_VIEW instanceView = {};
    instanceView.BufferLocation = mInstanceBuffer->Resource()->GetGPUVirtualAddress();
    instanceView.SizeInBytes = mInstanceCapacity * sizeof(InstanceData);
    instanceView.StrideInBytes = sizeof(InstanceData);
    const D3D12_VERTEX_BUFFER_VIEW views[] = { mPointLightVertexBufferView, instanceView };
    commandList->IASetVertexBuffers(0, 2, views);
    commandList->IASetIndexBuffer(&mPointLightIndexBufferView);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetPipelineState(scene.Wireframe ? mInstanceWireframePso.Get() : mInstancePso.Get());
    commandList->DrawIndexedInstanced(mPointLightIndexCount,
        static_cast<UINT>(mCullingScene.VisibleIds.size()), 0, 0, 0);
}

void RenderingSystem::RenderCullingObserver(ID3D12GraphicsCommandList* commands,
    D3D12_CPU_DESCRIPTOR_HANDLE target, const SceneRenderContext& scene)
{
    // The observer never runs Select(): reuse the PRIMARY camera's exact selection.
    const auto bounds = mCullingScene.RootBounds();
    const XMVECTOR center = XMLoadFloat3(&bounds.Center);
    const float radius = (std::max)({ bounds.Extents.x, bounds.Extents.y, bounds.Extents.z, 1.0f });
    const XMMATRIX view = XMMatrixLookAtLH(center + XMVectorSet(3.0f, 1.3f, -0.8f, 0) * radius,
        center, XMVectorSet(0, 1, 0, 0));
    XMFLOAT3 corners[8];
    bounds.GetCorners(corners);
    float halfWidth = 1, halfHeight = 1;
    for (const auto& corner : corners)
    {
        XMFLOAT3 p;
        XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&corner), view));
        halfWidth = (std::max)(halfWidth, std::abs(p.x));
        halfHeight = (std::max)(halfHeight, std::abs(p.y));
    }
    const float aspect = float(mWidth) / float(mHeight);
    halfHeight = (std::max)(halfHeight, halfWidth / aspect) * 1.12f;
    const XMMATRIX projection = XMMatrixOrthographicLH(halfHeight * aspect * 2, halfHeight * 2,
        0.1f, radius * 10);
    ObjectConstants constants = {};
    XMStoreFloat4x4(&constants.mWorldViewProj, XMMatrixTranspose(view * projection));
    commands->SetGraphicsRootSignature(mGeometryRootSignature.Get());
    // An otherwise unused slot, separate from model, shadow and cache-capture draws.
    BindObjectConstants(commands, 6 * mObjectSlots - 1, constants);

    std::vector<DebugLineVertex> lines;
    auto line = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 color)
    {
        lines.push_back({ a, color });
        lines.push_back({ b, color });
    };
    const unsigned edges[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7} };
    auto wireBox = [&](const XMFLOAT3* points, XMFLOAT3 color)
    {
        for (const auto& edge : edges) line(points[edge[0]], points[edge[1]], color);
    };
    std::vector<bool> visible(mCullingScene.Bounds.size(), false);
    for (unsigned id : mCullingScene.VisibleIds) visible[id] = true;
    if (scene.ShowCulledBounds)
        for (size_t i = 0; i < visible.size(); ++i)
            if (!visible[i])
            {
                mCullingScene.Bounds[i].GetCorners(corners);
                wireBox(corners, { 0.14f, 0.16f, 0.18f });
            }
    const UINT contextVertexCount = static_cast<UINT>(lines.size());
    // Draw actual near/far edges, not a shorter frustum with a misleading far plane.
    // At far=1000 the far rectangle is naturally outside the scene-sized observer view.
    mCullingFrustum.GetCorners(corners);
    wireBox(corners, { 1.0f, 0.8f, 0.08f });
    const XMMATRIX inverseView = XMMatrixInverse(nullptr, XMLoadFloat4x4(&scene.View));
    XMFLOAT3 marker[5];
    const XMFLOAT3 localMarker[5] = { {0,0,0}, {-1,0.7f,2}, {1,0.7f,2}, {1,-0.7f,2}, {-1,-0.7f,2} };
    for (UINT i = 0; i < 5; ++i)
        XMStoreFloat3(&marker[i], XMVector3TransformCoord(XMLoadFloat3(&localMarker[i]), inverseView));
    for (UINT i = 1; i <= 4; ++i)
    {
        line(marker[0], marker[i], { 0.1f, 0.85f, 1.0f });
        line(marker[i], marker[i == 4 ? 1 : i + 1], { 0.1f, 0.85f, 1.0f });
    }
    const UINT vertexCount = static_cast<UINT>(lines.size());
    if (mCullingLineCapacity < vertexCount)
    {
        mCullingLineCapacity = vertexCount;
        mCullingLines = std::make_unique<UploadBuffer<DebugLineVertex>>(mDevice.Get(), vertexCount, false);
    }
    for (UINT i = 0; i < vertexCount; ++i) mCullingLines->CopyData(i, lines[i]);
    const D3D12_VERTEX_BUFFER_VIEW lineView = { mCullingLines->Resource()->GetGPUVirtualAddress(),
        vertexCount * UINT(sizeof(DebugLineVertex)), sizeof(DebugLineVertex) };
    auto depthBarrier = TransitionBarrier(mGBuffer.GetDepthResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commands->ResourceBarrier(1, &depthBarrier);
    const auto depth = mGBuffer.GetDepthDsv();
    commands->OMSetRenderTargets(1, &target, FALSE, &depth);
    const float background[] = { 0, 0, 0, 1 };
    commands->ClearRenderTargetView(target, background, 0, nullptr);
    commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    auto drawLines = [&](UINT start, UINT count)
    {
        commands->SetPipelineState(mCullingLinePso.Get());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        commands->IASetVertexBuffers(0, 1, &lineView);
        commands->DrawInstanced(count, 1, start, 0);
    };
    if (contextVertexCount) drawLines(0, contextVertexCount);
    if (!mCullingScene.VisibleIds.empty())
    {
        const D3D12_VERTEX_BUFFER_VIEW instances = { mInstanceBuffer->Resource()->GetGPUVirtualAddress(),
            UINT(mCullingScene.VisibleIds.size()) * UINT(sizeof(InstanceData)), sizeof(InstanceData) };
        const D3D12_VERTEX_BUFFER_VIEW views[] = { mPointLightVertexBufferView, instances };
        commands->IASetVertexBuffers(0, 2, views);
        commands->IASetIndexBuffer(&mPointLightIndexBufferView);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->SetPipelineState(mCullingObserverPso.Get());
        commands->DrawIndexedInstanced(mPointLightIndexCount, static_cast<UINT>(mCullingScene.VisibleIds.size()), 0, 0, 0);
    }
    drawLines(contextVertexCount, vertexCount - contextVertexCount);
    depthBarrier = TransitionBarrier(mGBuffer.GetDepthResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &depthBarrier);
}

void RenderingSystem::BuildLights()
{
    mDirectionalLights = { {} };
    mDirectionalLights[0].Direction = { -0.35f, -0.85f, 0.35f };
    mDirectionalLights[0].Intensity = 1.05f;
    mDirectionalLights[0].Color = { 1.0f, 0.90f, 0.72f };

    mPointLights =
    {
        { { -3.2f, 2.4f, -3.6f }, 7.5f, { 1.0f, 0.62f, 0.34f }, 0.55f },
        { {  3.0f, 3.2f,  2.8f }, 8.0f, { 0.45f, 0.62f, 1.0f }, 0.35f },
        { {  0.5f, 4.6f, -0.5f }, 7.0f, { 0.85f, 1.0f, 0.72f }, 0.25f }
    };

    mSpotLights =
    {
        { { -2.5f, 5.2f, -5.5f }, 13.0f, { 0.25f, -1.0f, 0.45f }, 0.72f, { 1.0f, 0.86f, 0.62f }, 0.75f },
        { {  3.8f, 4.2f,  3.8f }, 11.0f, { -0.45f, -0.8f, -0.35f }, 0.80f, { 0.55f, 0.72f, 1.0f }, 0.42f }
    };
}

void RenderingSystem::BuildRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE geometryRanges[2] = {};
    geometryRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    geometryRanges[0].NumDescriptors = 1;
    geometryRanges[0].BaseShaderRegister = 0;
    geometryRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    geometryRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    geometryRanges[1].NumDescriptors = 3;
    geometryRanges[1].BaseShaderRegister = 0;
    geometryRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER geometryParams[2] = {};
    geometryParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    geometryParams[0].Descriptor.ShaderRegister = 0;
    geometryParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    geometryParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    geometryParams[1].DescriptorTable.NumDescriptorRanges = 1;
    geometryParams[1].DescriptorTable.pDescriptorRanges = &geometryRanges[1];
    // Displacement is sampled in the domain shader, normal/albedo in the pixel shader.
    geometryParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC geometryDesc = {};
    geometryDesc.NumParameters = 2;
    geometryDesc.pParameters = geometryParams;
    geometryDesc.NumStaticSamplers = 1;
    geometryDesc.pStaticSamplers = &sampler;
    geometryDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;

    ComPtr<ID3DBlob> serialized = nullptr;
    ComPtr<ID3DBlob> error = nullptr;
    ThrowIfFailed(D3D12SerializeRootSignature(&geometryDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mGeometryRootSignature)));

    D3D12_DESCRIPTOR_RANGE lightingRanges[2] = {};
    lightingRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lightingRanges[0].NumDescriptors = 3;
    lightingRanges[0].BaseShaderRegister = 0;
    lightingRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    lightingRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    lightingRanges[1].NumDescriptors = 1;
    lightingRanges[1].BaseShaderRegister = 0;
    lightingRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER lightingParams[3] = {};
    lightingParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    lightingParams[0].DescriptorTable.NumDescriptorRanges = 1;
    lightingParams[0].DescriptorTable.pDescriptorRanges = &lightingRanges[0];
    lightingParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    lightingParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    lightingParams[1].DescriptorTable.NumDescriptorRanges = 1;
    lightingParams[1].DescriptorTable.pDescriptorRanges = &lightingRanges[1];
    lightingParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = 1;
    shadowRange.BaseShaderRegister = 3;
    lightingParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    lightingParams[2].DescriptorTable = { 1, &shadowRange };
    lightingParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC pointSampler = {};
    pointSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    pointSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointSampler.ShaderRegister = 0;
    pointSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC lightingSamplers[] = { pointSampler, pointSampler };
    lightingSamplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    lightingSamplers[1].AddressU = lightingSamplers[1].AddressV = lightingSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    lightingSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    lightingSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    lightingSamplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC lightingDesc = {};
    lightingDesc.NumParameters = 3;
    lightingDesc.pParameters = lightingParams;
    lightingDesc.NumStaticSamplers = 2;
    lightingDesc.pStaticSamplers = lightingSamplers;
    lightingDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    serialized.Reset();
    error.Reset();
    ThrowIfFailed(D3D12SerializeRootSignature(&lightingDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mLightingRootSignature)));

    D3D12_ROOT_PARAMETER pointLightParams[3] = {};
    pointLightParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    pointLightParams[0].DescriptorTable.NumDescriptorRanges = 1;
    pointLightParams[0].DescriptorTable.pDescriptorRanges = &lightingRanges[0];
    pointLightParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    pointLightParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    pointLightParams[1].DescriptorTable.NumDescriptorRanges = 1;
    pointLightParams[1].DescriptorTable.pDescriptorRanges = &lightingRanges[1];
    pointLightParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    pointLightParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    pointLightParams[2].Descriptor.ShaderRegister = 1;
    pointLightParams[2].Descriptor.RegisterSpace = 0;
    pointLightParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC pointLightDesc = {};
    pointLightDesc.NumParameters = 3;
    pointLightDesc.pParameters = pointLightParams;
    pointLightDesc.NumStaticSamplers = 1;
    pointLightDesc.pStaticSamplers = &pointSampler;
    pointLightDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    serialized.Reset();
    error.Reset();
    ThrowIfFailed(D3D12SerializeRootSignature(&pointLightDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mPointLightRootSignature)));

    D3D12_DESCRIPTOR_RANGE finalRange = {};
    finalRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    finalRange.NumDescriptors = 4;
    finalRange.BaseShaderRegister = 0;
    finalRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER finalParams[3] = {};
    finalParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    finalParams[0].DescriptorTable = { 1, &finalRange };
    finalParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    finalParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    finalParams[1].Constants = { 2, 0, 8 };
    finalParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE postUav = {};
    postUav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    postUav.NumDescriptors = 1;
    finalParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    finalParams[2].DescriptorTable = { 1, &postUav };
    D3D12_STATIC_SAMPLER_DESC postSamplers[] = { pointSampler, pointSampler };
    postSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    postSamplers[1].ShaderRegister = 1;
    for (auto& sampler : postSamplers) sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC finalDesc = {};
    finalDesc.NumParameters = 3;
    finalDesc.pParameters = finalParams;
    finalDesc.NumStaticSamplers = 2;
    finalDesc.pStaticSamplers = postSamplers;
    finalDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    serialized.Reset();
    error.Reset();
    ThrowIfFailed(D3D12SerializeRootSignature(&finalDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mPostRootSignature)));

    finalRange.NumDescriptors = 1;
    finalParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    finalDesc.NumParameters = 1;
    finalDesc.NumStaticSamplers = 0;
    finalDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    serialized.Reset();
    error.Reset();
    ThrowIfFailed(D3D12SerializeRootSignature(&finalDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mFinalRootSignature)));
}

void RenderingSystem::BuildPsos(DXGI_FORMAT backBufferFormat)
{
    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryPso = {};
    geometryPso.InputLayout = { inputLayout, _countof(inputLayout) };
    geometryPso.pRootSignature = mGeometryRootSignature.Get();
    geometryPso.VS = { reinterpret_cast<BYTE*>(mGeometryVs->GetBufferPointer()), mGeometryVs->GetBufferSize() };
    geometryPso.HS = { reinterpret_cast<BYTE*>(mGeometryHs->GetBufferPointer()), mGeometryHs->GetBufferSize() };
    geometryPso.DS = { reinterpret_cast<BYTE*>(mGeometryDs->GetBufferPointer()), mGeometryDs->GetBufferSize() };
    geometryPso.PS = { reinterpret_cast<BYTE*>(mGeometryPs->GetBufferPointer()), mGeometryPs->GetBufferSize() };
    geometryPso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    geometryPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    geometryPso.RasterizerState.DepthClipEnable = TRUE;
    geometryPso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    geometryPso.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    geometryPso.SampleMask = UINT_MAX;
    geometryPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    geometryPso.NumRenderTargets = 2;
    geometryPso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    geometryPso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geometryPso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    geometryPso.SampleDesc.Count = 1;
    geometryPso.DepthStencilState.DepthEnable = TRUE;
    geometryPso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    geometryPso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    HRESULT hr = mDevice->CreateGraphicsPipelineState(&geometryPso, IID_PPV_ARGS(&mGeometryPso));
    if (FAILED(hr))
    {
        StartupLogHr("Create geometry PSO failed", hr);
        ThrowIfFailed(hr, "Create geometry PSO failed");
    }
    StartupLog("Create geometry PSO ok");

    geometryPso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    geometryPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    hr = mDevice->CreateGraphicsPipelineState(&geometryPso, IID_PPV_ARGS(&mGeometryWireframePso));
    if (FAILED(hr))
    {
        StartupLogHr("Create geometry wireframe PSO failed", hr);
        ThrowIfFailed(hr, "Create geometry wireframe PSO failed");
    }
    StartupLog("Create geometry wireframe PSO ok");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryNoTessPso = geometryPso;
    geometryNoTessPso.VS = { reinterpret_cast<BYTE*>(mGeometryNoTessVs->GetBufferPointer()), mGeometryNoTessVs->GetBufferSize() };
    geometryNoTessPso.HS = {};
    geometryNoTessPso.DS = {};
    geometryNoTessPso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    geometryNoTessPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    hr = mDevice->CreateGraphicsPipelineState(&geometryNoTessPso, IID_PPV_ARGS(&mGeometryNoTessPso));
    if (FAILED(hr))
    {
        StartupLogHr("Create geometry non-tessellated PSO failed", hr);
        ThrowIfFailed(hr, "Create geometry non-tessellated PSO failed");
    }
    StartupLog("Create geometry non-tessellated PSO ok");

    const D3D12_INPUT_ELEMENT_DESC instanceLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "INSTANCE", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "NORMALWORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "NORMALWORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 80, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "NORMALWORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 96, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "NORMALWORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 112, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 }
    };
    auto instancePso = geometryNoTessPso;
    instancePso.InputLayout = { instanceLayout, _countof(instanceLayout) };
    instancePso.VS = { mInstanceVs->GetBufferPointer(), mInstanceVs->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&instancePso, IID_PPV_ARGS(&mInstancePso)));
    instancePso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&instancePso, IID_PPV_ARGS(&mInstanceWireframePso)));
    auto observerPso = instancePso;
    observerPso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    observerPso.NumRenderTargets = 1;
    for (auto& format : observerPso.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
    observerPso.RTVFormats[0] = backBufferFormat;
    observerPso.PS = { mCullingObserverPs->GetBufferPointer(), mCullingObserverPs->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&observerPso, IID_PPV_ARGS(&mCullingObserverPso)));
    const D3D12_INPUT_ELEMENT_DESC lineLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    auto linePso = observerPso;
    linePso.InputLayout = { lineLayout, _countof(lineLayout) };
    linePso.VS = { mCullingLineVs->GetBufferPointer(), mCullingLineVs->GetBufferSize() };
    linePso.PS = { mCullingLinePs->GetBufferPointer(), mCullingLinePs->GetBufferSize() };
    linePso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    linePso.DepthStencilState.DepthEnable = FALSE;
    linePso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&linePso, IID_PPV_ARGS(&mCullingLinePso)));

    auto shadowPso = geometryPso;
    shadowPso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    shadowPso.RasterizerState.DepthBias = 200;
    shadowPso.RasterizerState.SlopeScaledDepthBias = 1.5f;
    shadowPso.RasterizerState.DepthBiasClamp = 0.003f;
    shadowPso.NumRenderTargets = 0;
    for (auto& format : shadowPso.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
    shadowPso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    shadowPso.HS = { mShadowHs->GetBufferPointer(), mShadowHs->GetBufferSize() };
    shadowPso.PS = { mShadowPs->GetBufferPointer(), mShadowPs->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&shadowPso, IID_PPV_ARGS(&mShadowPso)));
    shadowPso.VS = geometryNoTessPso.VS;
    shadowPso.HS = shadowPso.DS = {};
    shadowPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&shadowPso, IID_PPV_ARGS(&mShadowNoTessPso)));
    const D3D12_INPUT_ELEMENT_DESC cachedLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 52, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    auto cachedPso = geometryNoTessPso;
    cachedPso.InputLayout = { cachedLayout, _countof(cachedLayout) };
    cachedPso.VS = { mCachedVs->GetBufferPointer(), mCachedVs->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&cachedPso, IID_PPV_ARGS(&mCachedPso)));
    cachedPso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&cachedPso, IID_PPV_ARGS(&mCachedWirePso)));
    auto cachedShadow = shadowPso;
    cachedShadow.InputLayout = cachedPso.InputLayout;
    cachedShadow.VS = cachedPso.VS;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&cachedShadow, IID_PPV_ARGS(&mCachedShadowPso)));

    // With no GS, stream output captures the tessellated domain-shader output.
    const D3D12_SO_DECLARATION_ENTRY so[] =
    {
        { 0, "SV_POSITION", 0, 0, 4, 0 }, { 0, "NORMAL", 0, 0, 3, 0 },
        { 0, "TANGENT", 0, 0, 3, 0 }, { 0, "BINORMAL", 0, 0, 3, 0 },
        { 0, "TEXCOORD", 0, 0, 2, 0 }, { 0, "TEXCOORD", 1, 0, 1, 0 }
    };
    const UINT stride = CachedVertexStride;
    auto capturePso = geometryPso;
    capturePso.HS = { mShadowHs->GetBufferPointer(), mShadowHs->GetBufferSize() };
    capturePso.DS = { mCacheDs->GetBufferPointer(), mCacheDs->GetBufferSize() };
    capturePso.PS = {};
    capturePso.StreamOutput = { so, _countof(so), &stride, 1, D3D12_SO_NO_RASTERIZED_STREAM };
    capturePso.NumRenderTargets = 0;
    for (auto& format : capturePso.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
    capturePso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    capturePso.DepthStencilState.DepthEnable = FALSE;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&capturePso, IID_PPV_ARGS(&mCaptureTessPso)));
    shadowPso.VS = instancePso.VS;
    shadowPso.InputLayout = instancePso.InputLayout;
    shadowPso.PS = {}; // The instance spheres are opaque.
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&shadowPso, IID_PPV_ARGS(&mShadowInstancePso)));
    auto groundPso = geometryNoTessPso;
    groundPso.PS = { mGroundPs->GetBufferPointer(), mGroundPs->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&groundPso, IID_PPV_ARGS(&mGroundPso)));

    geometryNoTessPso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    hr = mDevice->CreateGraphicsPipelineState(&geometryNoTessPso, IID_PPV_ARGS(&mGeometryNoTessWireframePso));
    if (FAILED(hr))
    {
        StartupLogHr("Create geometry non-tessellated wireframe PSO failed", hr);
        ThrowIfFailed(hr, "Create geometry non-tessellated wireframe PSO failed");
    }
    StartupLog("Create geometry non-tessellated wireframe PSO ok");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightingPso = {};
    lightingPso.pRootSignature = mLightingRootSignature.Get();
    lightingPso.VS = { reinterpret_cast<BYTE*>(mLightingVs->GetBufferPointer()), mLightingVs->GetBufferSize() };
    lightingPso.PS = { reinterpret_cast<BYTE*>(mLightingPs->GetBufferPointer()), mLightingPs->GetBufferSize() };
    lightingPso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    lightingPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    lightingPso.RasterizerState.DepthClipEnable = TRUE;
    lightingPso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    lightingPso.SampleMask = UINT_MAX;
    lightingPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightingPso.NumRenderTargets = 1;
    lightingPso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lightingPso.SampleDesc.Count = 1;
    lightingPso.DepthStencilState.DepthEnable = FALSE;
    hr = mDevice->CreateGraphicsPipelineState(&lightingPso, IID_PPV_ARGS(&mLightingPso));
    if (FAILED(hr))
    {
        StartupLogHr("Create lighting PSO failed", hr);
        ThrowIfFailed(hr, "Create lighting PSO failed");
    }
    StartupLog("Create lighting PSO ok");

    D3D12_INPUT_ELEMENT_DESC pointLightInputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pointLightPso = lightingPso;
    pointLightPso.InputLayout = { pointLightInputLayout, _countof(pointLightInputLayout) };
    pointLightPso.pRootSignature = mPointLightRootSignature.Get();
    pointLightPso.VS = { reinterpret_cast<BYTE*>(mPointLightVs->GetBufferPointer()), mPointLightVs->GetBufferSize() };
    pointLightPso.PS = { reinterpret_cast<BYTE*>(mPointLightPs->GetBufferPointer()), mPointLightPs->GetBufferSize() };
    pointLightPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pointLightPso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pointLightPso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pointLightPso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    pointLightPso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pointLightPso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pointLightPso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    pointLightPso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pointLightPso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // This is additive light accumulation, not alpha transparency blending.
    hr = mDevice->CreateGraphicsPipelineState(&pointLightPso, IID_PPV_ARGS(&mPointLightPso));
    if (FAILED(hr))
    {
        StartupLogHr("Create point light PSO failed", hr);
        ThrowIfFailed(hr, "Create point light PSO failed");
    }
    StartupLog("Create point light PSO ok");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC finalPso = lightingPso;
    finalPso.pRootSignature = mFinalRootSignature.Get();
    finalPso.VS = { reinterpret_cast<BYTE*>(mFinalVs->GetBufferPointer()), mFinalVs->GetBufferSize() };
    finalPso.PS = { reinterpret_cast<BYTE*>(mFinalPs->GetBufferPointer()), mFinalPs->GetBufferSize() };
    finalPso.RTVFormats[0] = backBufferFormat;
    hr = mDevice->CreateGraphicsPipelineState(&finalPso, IID_PPV_ARGS(&mFinalPso));
    if (FAILED(hr))
    {
        StartupLogHr("Create final PSO failed", hr);
        ThrowIfFailed(hr, "Create final PSO failed");
    }
    StartupLog("Create final PSO ok");
    D3D12_COMPUTE_PIPELINE_STATE_DESC postPso = {};
    postPso.pRootSignature = mPostRootSignature.Get();
    postPso.CS = { mPostCs->GetBufferPointer(), mPostCs->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateComputePipelineState(&postPso, IID_PPV_ARGS(&mPostPso)));
}

void RenderingSystem::BuildDeferredDescriptorHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 8;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mDeferredHeap)));

    D3D12_CPU_DESCRIPTOR_HANDLE handle = mDeferredHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC albedoSrv = {};
    albedoSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    albedoSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    albedoSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    albedoSrv.Texture2D.MipLevels = 1;
    mDevice->CreateShaderResourceView(mGBuffer.GetAlbedoResource(), &albedoSrv, handle);

    handle.ptr += mCbvSrvUavDescriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC normalSrv = albedoSrv;
    normalSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    mDevice->CreateShaderResourceView(mGBuffer.GetNormalResource(), &normalSrv, handle);

    handle.ptr += mCbvSrvUavDescriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = albedoSrv;
    depthSrv.Format = mGBuffer.GetDepthSrvFormat();
    mDevice->CreateShaderResourceView(mGBuffer.GetDepthResource(), &depthSrv, handle);

    handle.ptr += mCbvSrvUavDescriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC lightingSrv = albedoSrv;
    lightingSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    mDevice->CreateShaderResourceView(mGBuffer.GetLightingResource(), &lightingSrv, handle);
    handle.ptr += 2 * mCbvSrvUavDescriptorSize; // Slot 4 is the frame CBV.
    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrv = {};
    shadowSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrv.Format = DXGI_FORMAT_R32_FLOAT;
    shadowSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    shadowSrv.Texture2DArray.MipLevels = 1;
    shadowSrv.Texture2DArray.ArraySize = ShadowCascades::Count;
    mDevice->CreateShaderResourceView(mShadowMap.Get(), &shadowSrv, handle);

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC output = mGBuffer.GetAlbedoResource()->GetDesc();
    output.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ThrowIfFailed(mDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &output,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&mPostOutput)));
    handle.ptr += mCbvSrvUavDescriptorSize;
    mDevice->CreateShaderResourceView(mPostOutput.Get(), &albedoSrv, handle);
    handle.ptr += mCbvSrvUavDescriptorSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav = {};
    outputUav.Format = output.Format;
    outputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    mDevice->CreateUnorderedAccessView(mPostOutput.Get(), nullptr, &outputUav, handle);
}

void RenderingSystem::BuildFrameConstants()
{
    mFrameConstants = std::make_unique<UploadBuffer<FrameConstants>>(mDevice.Get(), 1, true);
    mPointLightConstants = std::make_unique<UploadBuffer<PointLightVolumeConstants>>(mDevice.Get(), MaxPointLightVolumes, true);

    UINT cbSize = d3dUtil::CalcConstantBufferByteSize(sizeof(FrameConstants));
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = mFrameConstants->Resource()->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = cbSize;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = mDeferredHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += 4 * mCbvSrvUavDescriptorSize;
    mDevice->CreateConstantBufferView(&cbvDesc, handle);
}

void RenderingSystem::BuildShaders()
{
    const std::wstring shaderFile = L"../Project1/shaders.hlsl";
    mGeometryVs = d3dUtil::CompileShader(shaderFile, nullptr, "GeometryVS", "vs_5_0");
    mInstanceVs = d3dUtil::CompileShader(shaderFile, nullptr, "GeometryInstanceVS", "vs_5_0");
    mCullingLineVs = d3dUtil::CompileShader(shaderFile, nullptr, "CullingLineVS", "vs_5_0");
    mCullingLinePs = d3dUtil::CompileShader(shaderFile, nullptr, "CullingLinePS", "ps_5_0");
    mCullingObserverPs = d3dUtil::CompileShader(shaderFile, nullptr, "CullingObserverPS", "ps_5_0");
    mGeometryNoTessVs = d3dUtil::CompileShader(shaderFile, nullptr, "GeometryNoTessVS", "vs_5_0");
    mGeometryHs = d3dUtil::CompileShader(shaderFile, nullptr, "GeometryHS", "hs_5_0");
    mGeometryDs = d3dUtil::CompileShader(shaderFile, nullptr, "GeometryDS", "ds_5_0");
    mGeometryPs = d3dUtil::CompileShader(shaderFile, nullptr, "GeometryPS", "ps_5_0");
    const D3D_SHADER_MACRO cacheDefines[] = { { "CACHE_TESSELLATION", "1" }, { nullptr, nullptr } };
    mCacheDs = d3dUtil::CompileShader(shaderFile, cacheDefines, "GeometryDS", "ds_5_0");
    mCachedVs = d3dUtil::CompileShader(shaderFile, nullptr, "CachedGeometryVS", "vs_5_0");
    const D3D_SHADER_MACRO shadowDefines[] = { { "SHADOW_PASS", "1" }, { nullptr, nullptr } };
    mShadowHs = d3dUtil::CompileShader(shaderFile, shadowDefines, "GeometryHS", "hs_5_0");
    mShadowPs = d3dUtil::CompileShader(shaderFile, nullptr, "ShadowPS", "ps_5_0");
    mGroundPs = d3dUtil::CompileShader(shaderFile, nullptr, "GroundPS", "ps_5_0");
    mLightingVs = d3dUtil::CompileShader(shaderFile, nullptr, "LightingVS", "vs_5_0");
    mLightingPs = d3dUtil::CompileShader(shaderFile, nullptr, "LightingPS", "ps_5_0");
    mPointLightVs = d3dUtil::CompileShader(shaderFile, nullptr, "PointLightVolumeVS", "vs_5_0");
    mPointLightPs = d3dUtil::CompileShader(shaderFile, nullptr, "PointLightVolumePS", "ps_5_0");
    mFinalVs = d3dUtil::CompileShader(shaderFile, nullptr, "FinalVS", "vs_5_0");
    mFinalPs = d3dUtil::CompileShader(shaderFile, nullptr, "FinalPS", "ps_5_0");
    mPostCs = d3dUtil::CompileShader(shaderFile, nullptr, "PostCS", "cs_5_0");
}

void RenderingSystem::BuildPointLightVolumeMesh()
{
    std::vector<SphereVertex> vertices;
    std::vector<uint32_t> indices;
    constexpr UINT subdivisions = 12;
    struct Face { XMFLOAT3 Normal, U, V; XMFLOAT2 Tile; };
    // Earth_ALB/NORM use a vertical cube-cross atlas, not an equirectangular map.
    const Face faces[] = {
        { {0,0,1},  {1,0,0},  {0,-1,0}, {0.375f,0.25f} },
        { {-1,0,0}, {0,0,1},  {0,-1,0}, {0.125f,0.25f} },
        { {1,0,0},  {0,0,-1}, {0,-1,0}, {0.625f,0.25f} },
        { {0,1,0},  {1,0,0},  {0,0,1},  {0.375f,0.0f} },
        { {0,-1,0}, {1,0,0},  {0,0,-1}, {0.375f,0.5f} },
        { {0,0,-1}, {1,0,0},  {0,1,0},  {0.375f,0.75f} }
    };
    for (const Face& face : faces)
    {
        const UINT base = static_cast<UINT>(vertices.size());
        for (UINT y = 0; y <= subdivisions; ++y)
            for (UINT x = 0; x <= subdivisions; ++x)
            {
                const float u = static_cast<float>(x) / subdivisions;
                const float v = static_cast<float>(y) / subdivisions;
                const XMVECTOR normal = XMVector3Normalize(XMLoadFloat3(&face.Normal) +
                    (2 * u - 1) * XMLoadFloat3(&face.U) + (2 * v - 1) * XMLoadFloat3(&face.V));
                const XMVECTOR tangent = XMVector3Normalize(XMLoadFloat3(&face.U) -
                    XMVector3Dot(XMLoadFloat3(&face.U), normal) * normal);
                SphereVertex vertex;
                XMStoreFloat3(&vertex.Position, normal);
                XMStoreFloat3(&vertex.Tangent, tangent);
                XMStoreFloat3(&vertex.Bitangent, XMVector3Cross(normal, tangent));
                vertex.UV = { face.Tile.x + u * 0.25f, face.Tile.y + v * 0.25f };
                vertices.push_back(vertex);
            }
        for (UINT y = 0; y < subdivisions; ++y)
            for (UINT x = 0; x < subdivisions; ++x)
            {
                const UINT a = base + y * (subdivisions + 1) + x;
                const UINT b = a + subdivisions + 1;
                indices.insert(indices.end(), { a, a + 1, b, b, a + 1, b + 1 });
            }
    }

    const UINT vertexBufferSize = static_cast<UINT>(vertices.size() * sizeof(SphereVertex));
    const UINT indexBufferSize = static_cast<UINT>(indices.size() * sizeof(uint32_t));

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vertexDesc = {};
    vertexDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertexDesc.Width = vertexBufferSize;
    vertexDesc.Height = 1;
    vertexDesc.DepthOrArraySize = 1;
    vertexDesc.MipLevels = 1;
    vertexDesc.SampleDesc.Count = 1;
    vertexDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &vertexDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mPointLightVertexBuffer)));

    void* mappedData = nullptr;
    mPointLightVertexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, vertices.data(), vertexBufferSize);
    mPointLightVertexBuffer->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC indexDesc = vertexDesc;
    indexDesc.Width = indexBufferSize;
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &indexDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mPointLightIndexBuffer)));

    mPointLightIndexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, indices.data(), indexBufferSize);
    mPointLightIndexBuffer->Unmap(0, nullptr);

    mPointLightVertexBufferView.BufferLocation = mPointLightVertexBuffer->GetGPUVirtualAddress();
    mPointLightVertexBufferView.StrideInBytes = sizeof(SphereVertex);
    mPointLightVertexBufferView.SizeInBytes = vertexBufferSize;

    mPointLightIndexBufferView.BufferLocation = mPointLightIndexBuffer->GetGPUVirtualAddress();
    mPointLightIndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    mPointLightIndexBufferView.SizeInBytes = indexBufferSize;
    mPointLightIndexCount = static_cast<UINT>(indices.size());
}

void RenderingSystem::UpdateFrameConstants(const SceneRenderContext& scene)
{
    FrameConstants frame = {};

    XMMATRIX view = XMLoadFloat4x4(&scene.View);
    XMMATRIX proj = XMLoadFloat4x4(&scene.Proj);
    XMMATRIX viewProj = view * proj;
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);
    XMStoreFloat4x4(&frame.InvViewProj, XMMatrixTranspose(invViewProj));
    XMStoreFloat4x4(&frame.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&frame.View, XMMatrixTranspose(view));
    for (UINT i = 0; i < ShadowCascades::Count; ++i)
        XMStoreFloat4x4(&frame.ShadowViewProjection[i], XMMatrixTranspose(XMLoadFloat4x4(&mCascades.ViewProjection[i])));
    frame.CascadeSplits = { mCascades.Splits[0], mCascades.Splits[1], mCascades.Splits[2], mCascades.Splits[3] };
    frame.ShadowParams = { scene.Shadows.Enabled ? 1.0f : 0.0f, float((std::min)(scene.Shadows.PcfRadius, 2u)),
        1.0f / ShadowCascades::Resolution, scene.Shadows.ShowCascades ? 1.0f : 0.0f };
    frame.ShadowConfig = { mCascades.Near, ShadowCascades::BlendFraction, 0.00008f, 0.9f };
    frame.CameraPosition = XMFLOAT4(scene.EyePos.x, scene.EyePos.y, scene.EyePos.z, 1.0f);
    frame.LightCounts = XMFLOAT4(
        static_cast<float>(mDirectionalLights.size()),
        scene.DynamicPointLights ? static_cast<float>(scene.DynamicPointLights->size()) : 0.0f,
        static_cast<float>(mSpotLights.size()),
        0.0f);
    frame.ScreenSize = XMFLOAT4(
        static_cast<float>(mWidth),
        static_cast<float>(mHeight),
        1.0f / static_cast<float>(mWidth),
        1.0f / static_cast<float>(mHeight));

    for (size_t i = 0; i < mDirectionalLights.size() && i < MaxDirectionalLights; ++i)
    {
        frame.DirectionalLights[i].DirectionIntensity =
            XMFLOAT4(mDirectionalLights[i].Direction.x, mDirectionalLights[i].Direction.y, mDirectionalLights[i].Direction.z, mDirectionalLights[i].Intensity);
        frame.DirectionalLights[i].Color =
            XMFLOAT4(mDirectionalLights[i].Color.x, mDirectionalLights[i].Color.y, mDirectionalLights[i].Color.z, 1.0f);
    }

    for (size_t i = 0; i < mSpotLights.size() && i < MaxSpotLights; ++i)
    {
        frame.SpotLights[i].PositionRange =
            XMFLOAT4(mSpotLights[i].Position.x, mSpotLights[i].Position.y, mSpotLights[i].Position.z, mSpotLights[i].Range);
        frame.SpotLights[i].DirectionAngle =
            XMFLOAT4(mSpotLights[i].Direction.x, mSpotLights[i].Direction.y, mSpotLights[i].Direction.z, mSpotLights[i].Angle);
        frame.SpotLights[i].ColorIntensity =
            XMFLOAT4(mSpotLights[i].Color.x, mSpotLights[i].Color.y, mSpotLights[i].Color.z, mSpotLights[i].Intensity);
    }

    mFrameConstants->CopyData(0, frame);
}

void RenderingSystem::RenderPointLightVolume(
    ID3D12GraphicsCommandList* commandList,
    const PointLight& light,
    UINT lightIndex)
{
    PointLightVolumeConstants constants = {};
    constants.PositionRange = XMFLOAT4(light.Position.x, light.Position.y, light.Position.z, light.Range);
    constants.ColorIntensity = XMFLOAT4(light.Color.x, light.Color.y, light.Color.z, light.Intensity);
    mPointLightConstants->CopyData(lightIndex, constants);

    const UINT cbSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PointLightVolumeConstants));
    D3D12_GPU_VIRTUAL_ADDRESS cbAddress =
        mPointLightConstants->Resource()->GetGPUVirtualAddress() + static_cast<UINT64>(lightIndex) * cbSize;
    commandList->SetGraphicsRootConstantBufferView(2, cbAddress);
    commandList->DrawIndexedInstanced(mPointLightIndexCount, 1, 0, 0, 0);
}

Material* RenderingSystem::FindMaterial(const SceneRenderContext& scene, const std::string& materialName) const
{
    for (auto& material : *scene.Materials)
    {
        if (material.Name == materialName)
            return &material;
    }

    return nullptr;
}
