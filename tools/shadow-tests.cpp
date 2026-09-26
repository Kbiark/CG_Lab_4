#include "../Lab_4/Project1/RenderingSystem.h"
#include "../Lab_4/Project1/ThrowIfFailed.h"
#include <d3d12sdklayers.h>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <random>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct ShadowTestAccess
{
    static ID3D12Resource* Map(RenderingSystem& renderer) { return renderer.mShadowMap.Get(); }
    static ID3D12Resource* PostOutput(RenderingSystem& renderer) { return renderer.mPostOutput.Get(); }
    static std::vector<unsigned> VisibleIds(RenderingSystem& renderer)
    {
        auto ids = renderer.mCullingScene.VisibleIds;
        std::sort(ids.begin(), ids.end());
        return ids;
    }
    static void ExceedTessellationBudget(RenderingSystem& renderer)
    {
        renderer.mTessMeshes[0].RequiredBytes = RenderingSystem::TessellationCacheBudget + 1;
        renderer.mTessMeshes[0].Captured = false;
    }
    static void ClearTessellationBudgetRequest(RenderingSystem& renderer)
    {
        renderer.mTessMeshes[0].RequiredBytes = 0;
    }
};

void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

void TestCascades()
{
    ShadowSettings settings;
    auto nonlinear = ShadowCascades::SplitDistances(0.1f, 80, 0.7f);
    auto linear = ShadowCascades::SplitDistances(0.1f, 80, 0);
    Require(nonlinear[0] < linear[0] && nonlinear.back() == 80, "Nonlinear split distribution");
    std::mt19937 random(123);
    std::uniform_real_distribution<float> value(-60, 60);
    BoundingBox casters({0,0,0}, {80,50,80});
    for (int test = 0; test < 120; ++test)
    {
        XMFLOAT4X4 view, projection;
        XMStoreFloat4x4(&view, XMMatrixLookAtLH(XMVectorSet(value(random), value(random), value(random), 1),
            XMVectorZero(), XMVectorSet(0,1,0,0)));
        XMStoreFloat4x4(&projection, XMMatrixPerspectiveFovLH(0.6f + (test % 5) * 0.15f,
            test % 2 ? 0.6f : 1.8f, 0.1f, 1000));
        settings.SplitLambda = test % 3 == 0 ? 0 : (test % 3 == 1 ? 0.7f : 1.0f);
        ShadowCascades cascades;
        cascades.Update(view, projection, test % 2 ? XMFLOAT3(0,-1,0) : XMFLOAT3(-0.35f,-0.85f,0.35f), casters, settings);
        const auto inverseView = XMMatrixInverse(nullptr, XMLoadFloat4x4(&view));
        for (unsigned c = 0; c < 4; ++c)
        {
            float start = c == 0 ? 0.1f : cascades.Splits[c-1];
            if (c > 0) start -= (start - (c == 1 ? 0.1f : cascades.Splits[c-2])) * ShadowCascades::BlendFraction;
            Require(cascades.Splits[c] > start, "Non-monotonic splits");
            for (float z : {start, cascades.Splits[c]})
                for (float y : {-1.0f,1.0f})
                    for (float x : {-1.0f,1.0f})
                    {
                        const auto world = XMVector3TransformCoord(XMVectorSet(x*z/projection._11,y*z/projection._22,z,1), inverseView);
                        XMFLOAT3 p;
                        XMStoreFloat3(&p, XMVector3TransformCoord(world, XMLoadFloat4x4(&cascades.ViewProjection[c])));
                        Require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
                            && std::abs(p.x) <= 1.0001f && std::abs(p.y) <= 1.0001f && p.z >= -0.0001f && p.z <= 1.0001f,
                            "Receiver/blend corner outside shadow map");
                    }
        }
    }
    std::cout << "PASS: 120 cameras, portrait/landscape, vertical light, linear/log/mixed splits, overlapping cascade coverage\n";
}

int main()
try
{
    TestCascades();
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> validation;
        if (SUCCEEDED(debug.As(&validation))) validation->SetEnableGPUBasedValidation(TRUE);
    }
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages;
    device.As(&messages);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    UINT64 frame = 0;
    auto submit = [&]
    {
        ThrowIfFailed(commands->Close());
        ID3D12CommandList* lists[] = { commands.Get() };
        queue->ExecuteCommandLists(1, lists);
        ThrowIfFailed(queue->Signal(fence.Get(), ++frame));
        ThrowIfFailed(fence->SetEventOnCompletion(frame, event));
        Require(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "GPU timeout");
        ThrowIfFailed(allocator->Reset());
        ThrowIfFailed(commands->Reset(allocator.Get(), nullptr));
    };
    auto buffer = [&](UINT64 bytes, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES heap = {}; heap.Type = type;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&result)));
        return result;
    };
    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        commands->ResourceBarrier(1, &b);
    };
    const UINT descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12DescriptorHeap> materialHeap, rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heapDesc.NumDescriptors = 4;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&materialHeap)));
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = 1; heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)));
    std::array<ComPtr<ID3D12Resource>,3> textures;
    auto textureUpload = buffer(1536, D3D12_HEAP_TYPE_UPLOAD);
    unsigned char* upload = nullptr;
    D3D12_RANGE none = {0,0};
    ThrowIfFailed(textureUpload->Map(0, &none, reinterpret_cast<void**>(&upload)));
    const unsigned char pixels[3][4] = {{230,230,230,255}, {128,128,255,255}, {192,192,192,255}};
    for (int i=0;i<3;++i) memcpy(upload+i*512, pixels[i], 4);
    textureUpload->Unmap(0,nullptr);
    D3D12_HEAP_PROPERTIES defaultHeap = {}; defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture = {};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = texture.Height = texture.DepthOrArraySize = texture.MipLevels = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    auto srvHandle = materialHeap->GetCPUDescriptorHandleForHeapStart(); srvHandle.ptr += descriptorSize;
    for (int i=0;i<3;++i)
    {
        ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&textures[i])));
        D3D12_TEXTURE_COPY_LOCATION source = {}, target = {};
        source.pResource = textureUpload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = { UINT64(i*512), { texture.Format, 1,1,1,256 } };
        target.pResource = textures[i].Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        transition(textures[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        device->CreateShaderResourceView(textures[i].Get(),nullptr,srvHandle); srvHandle.ptr += descriptorSize;
    }
    texture.Width = texture.Height = 256; texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> output;
    D3D12_CLEAR_VALUE clear = {}; clear.Format = texture.Format; clear.Color[3] = 1;
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&texture,
        D3D12_RESOURCE_STATE_COMMON,&clear,IID_PPV_ARGS(&output)));
    auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(output.Get(),nullptr,rtv);
    auto outputReadback = buffer(256*1024,D3D12_HEAP_TYPE_READBACK);
    std::vector<Vertex> vertices;
    std::vector<UINT> indices;
    const XMFLOAT3 normals[] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (const auto& normal : normals)
    {
        const XMVECTOR n = XMLoadFloat3(&normal);
        const XMVECTOR tangent = XMVector3Normalize(XMVector3Cross(n, std::abs(normal.y) > 0.5f ? XMVectorSet(0,0,1,0) : XMVectorSet(0,1,0,0)));
        const XMVECTOR bitangent = XMVector3Cross(n,tangent);
        const UINT base = static_cast<UINT>(vertices.size());
        for (const XMFLOAT2 uv : { XMFLOAT2(-1,-1), XMFLOAT2(-1,1), XMFLOAT2(1,-1), XMFLOAT2(1,1) })
        {
            Vertex v = {}; v.normal = normal;
            XMStoreFloat3(&v.position,n + tangent*uv.x + bitangent*uv.y + XMVectorSet(0,1,0,0));
            XMStoreFloat3(&v.tangent,tangent); XMStoreFloat3(&v.bitangent,bitangent);
            v.texcoord = uv; vertices.push_back(v);
        }
        for (UINT i : {0u,1u,2u,2u,1u,3u}) indices.push_back(base+i);
    }
    UploadBuffer<Vertex> vb(device.Get(),static_cast<UINT>(vertices.size()),false);
    UploadBuffer<UINT> ib(device.Get(),static_cast<UINT>(indices.size()),false);
    for (UINT i=0;i<vertices.size();++i) vb.CopyData(i,vertices[i]);
    for (UINT i=0;i<indices.size();++i) ib.CopyData(i,indices[i]);
    submit();
    RenderingSystem renderer;
    renderer.Initialize(device.Get(),256,256,texture.Format,descriptorSize,rtvSize);
    Material material; material.Name="test"; material.SrvHeapIndex=0;
    material.DisplacementTexture=textures[2]; material.DisplacementMap="test"; material.DisplacementScale=0.06f;
    material.TessellationParams={8,1,0,0.5f};
    std::vector<Material> materials={material};
    Submesh submesh; submesh.MaterialName="test"; submesh.IndexStart=0; submesh.IndexCount=static_cast<UINT>(indices.size());
    std::vector<Submesh> submeshes={submesh};
    SceneRenderContext scene;
    scene.Post.Enabled = false; // Existing lighting/tessellation tests compare the unprocessed image.
    scene.VertexBufferView={vb.Resource()->GetGPUVirtualAddress(),UINT(vertices.size()*sizeof(Vertex)),sizeof(Vertex)};
    scene.IndexBufferView={ib.Resource()->GetGPUVirtualAddress(),UINT(indices.size()*sizeof(UINT)),DXGI_FORMAT_R32_UINT};
    scene.Materials=&materials; scene.Submeshes=&submeshes; scene.MaterialHeap=materialHeap.Get();
    scene.MaterialDescriptorSize=descriptorSize; scene.SceneBounds={{0,1,0},{1,1,1}};
    scene.ShowCullingScene=false; scene.Particles.Visible=false; scene.Particles.Emit=false;
    scene.EyePos={6,6,-10};
    XMStoreFloat4x4(&scene.View,XMMatrixLookAtLH(XMLoadFloat3(&scene.EyePos),XMVectorSet(0,0,0,1),XMVectorSet(0,1,0,0)));
    XMStoreFloat4x4(&scene.Proj,XMMatrixPerspectiveFovLH(XM_PIDIV4,1,0.1f,1000));
    ComPtr<ID3D12QueryHeap> pipelineQueries;
    D3D12_QUERY_HEAP_DESC pipelineDesc = {};
    pipelineDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    pipelineDesc.Count = 1;
    ThrowIfFailed(device->CreateQueryHeap(&pipelineDesc, IID_PPV_ARGS(&pipelineQueries)));
    auto pipelineReadback = buffer(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS), D3D12_HEAP_TYPE_READBACK);
    D3D12_QUERY_DATA_PIPELINE_STATISTICS pipeline = {};
    auto postReadback = buffer(256 * 1024, D3D12_HEAP_TYPE_READBACK);
    auto render = [&]
    {
        commands->BeginQuery(pipelineQueries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        renderer.Render(commands.Get(),output.Get(),rtv,scene);
        commands->EndQuery(pipelineQueries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        commands->ResolveQueryData(pipelineQueries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, pipelineReadback.Get(), 0);
        transition(output.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source={},target={};
        source.pResource=output.Get(); source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.pResource=outputReadback.Get(); target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint.Footprint={texture.Format,256,256,1,1024};
        commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        auto* computed = ShadowTestAccess::PostOutput(renderer);
        transition(computed, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        source.pResource = computed;
        target.pResource = postReadback.Get();
        commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        transition(computed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        submit(); renderer.OnFrameComplete();
        void* pipelineData = nullptr;
        D3D12_RANGE pipelineRange = { 0, sizeof(pipeline) };
        ThrowIfFailed(pipelineReadback->Map(0, &pipelineRange, &pipelineData));
        memcpy(&pipeline, pipelineData, sizeof(pipeline));
        pipelineReadback->Unmap(0, &none);
        void* data=nullptr; D3D12_RANGE range={0,256*1024};
        ThrowIfFailed(outputReadback->Map(0,&range,&data));
        auto* bytes=static_cast<unsigned char*>(data);
        std::vector<unsigned char> image(bytes,bytes+range.End);
        outputReadback->Unmap(0,&none);
        if (!scene.ObserveCulling)
        {
            ThrowIfFailed(postReadback->Map(0, &range, &data));
            const bool same = memcmp(image.data(), data, image.size()) == 0;
            postReadback->Unmap(0, &none);
            Require(same, "Presentation must match compute output pixel-for-pixel");
            Require(pipeline.CSInvocations >= 256 * 256, "Missing fullscreen compute work");
        }
        return image;
    };
    scene.Shadows.Enabled=false;
    const auto unshadowed=render();
    Require(pipeline.HSInvocations > 0 && pipeline.DSInvocations > 0, "First frame must generate tessellation");
    scene.Shadows.Enabled=true; scene.Shadows.PcfRadius=0;
    const auto hard=render();
    Require(pipeline.HSInvocations == 0 && pipeline.DSInvocations == 0,
        "Cached model and all four shadow passes must bypass HS/DS");
    scene.Shadows.PcfRadius=2;
    const auto soft=render();
    UINT darkened=0, filtered=0;
    for (size_t i=0;i<hard.size();i+=4)
    {
        if (int(unshadowed[i])-int(hard[i])>10) ++darkened;
        if (std::abs(int(hard[i])-int(soft[i]))>1) ++filtered;
    }
    Require(darkened>50,"Shadow map does not darken receivers");
    Require(filtered>5,"PCF does not change shadow edges");
    scene.Shadows.ShowCascades=true;
    Require(render()!=soft,"Cascade visualization missing");
    scene.Shadows.SplitLambda=0; render();
    materials[0].DisplacementScale=0; render();
    scene.ShowCullingScene=true; render();
    scene.Particles.Visible=true; scene.Particles.Emit=true; scene.DeltaTime=1.0f/60; render();

    auto* map=ShadowTestAccess::Map(renderer);
    auto mapDesc=map->GetDesc();
    Require(mapDesc.DepthOrArraySize==4 && mapDesc.Width==2048,"Shadow texture dimensions");
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,4> layouts;
    UINT64 bytes=0; device->GetCopyableFootprints(&mapDesc,0,4,0,layouts.data(),nullptr,nullptr,&bytes);
    auto depthReadback=buffer(bytes,D3D12_HEAP_TYPE_READBACK);
    transition(map,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i=0;i<4;++i)
    {
        D3D12_TEXTURE_COPY_LOCATION source={},target={};
        source.pResource=map; source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; source.SubresourceIndex=i;
        target.pResource=depthReadback.Get(); target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; target.PlacedFootprint=layouts[i];
        commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);
    }
    transition(map,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    submit();
    void* data=nullptr; D3D12_RANGE range={0,SIZE_T(bytes)};
    ThrowIfFailed(depthReadback->Map(0,&range,&data));
    UINT nonempty=0;
    for (const auto& layout : layouts)
    {
        UINT written=0;
        for (UINT y=0;y<2048;++y)
        {
            const float* row=reinterpret_cast<const float*>(static_cast<const char*>(data)+layout.Offset+UINT64(y)*layout.Footprint.RowPitch);
            for (UINT x=0;x<2048;++x) { Require(std::isfinite(row[x]) && row[x]>=0 && row[x]<=1,"Invalid shadow depth"); if(row[x]<1) ++written; }
        }
        if(written) ++nonempty;
    }
    depthReadback->Unmap(0,&none);
    Require(nonempty==4,"Empty cascade in large caster scene");

    scene.ShowCullingScene = false;
    scene.Particles.Visible = false;
    scene.Particles.Emit = false;
    scene.Shadows.ShowCascades = false;
    scene.DeltaTime = 0.1f;
    materials[0].DisplacementScale = 0.06f;
    render(); render();
    Require(pipeline.HSInvocations == 0, "New cache should be reusable after fence");
    const auto before = renderer.GetTessellationCacheStats().Updates;
    for (int i = 0; i < 8; ++i)
    {
        render();
        Require(pipeline.HSInvocations == 0 && pipeline.DSInvocations == 0, "Stationary camera retessellated");
    }
    Require(renderer.GetTessellationCacheStats().Updates == before, "Stationary cache update count");
    const auto savedView = scene.View;
    XMStoreFloat4x4(&scene.View, XMMatrixLookAtLH(XMLoadFloat3(&scene.EyePos),
        XMVectorSet(1, 0, 0, 1), XMVectorSet(0, 1, 0, 0)));
    render();
    Require(pipeline.HSInvocations == 0 && pipeline.DSInvocations == 0,
        "Rotation in place must reuse world-space geometry");
    scene.View = savedView;
    // Force SO overflow, then verify exact query-driven growth instead of accepting a partial mesh.
    materials[0].TessellationParams = { 8,8,0,0.5f };
    render();
    Require(pipeline.DSInvocations > 0, "Changed factors must rebuild cache");
    render(); render(); render();
    Require(renderer.GetTessellationCacheStats().Triangles > 12 && pipeline.HSInvocations == 0,
        "Dense cache missing after stream-output overflow recovery");
    scene.Wireframe = true;
    scene.DebugViewMode = 3;
    const auto cachedImage = render();
    Require(pipeline.HSInvocations == 0, "Wireframe/debug changes should not rebuild geometry");
    scene.CacheTessellation = false;
    const auto directImage = render();
    Require(pipeline.HSInvocations > 0 && pipeline.DSInvocations > 0, "Every-frame comparison mode missing");
    UINT different = 0;
    for (size_t i = 0; i < directImage.size(); i += 4)
        if (std::abs(int(directImage[i]) - int(cachedImage[i])) > 4) ++different;
    Require(different < 256 * 256 / 50, "Cached wireframe differs from original tessellated geometry");
    scene.CacheTessellation = true;
    scene.Wireframe = false;
    scene.DebugViewMode = 1;
    // Even sub-threshold movement must rebuild immediately, independent of elapsed time.
    scene.DeltaTime = 0;
    UINT builds = 0, reuse = 0;
    for (int i = 0; i < 12; ++i)
    {
        if (i % 3 == 0) scene.EyePos.x += 0.0001f;
        else if (i % 3 == 1) scene.EyePos.y += 0.0001f;
        else scene.EyePos.z += 0.0001f;
        render();
        if (pipeline.HSInvocations > 0) ++builds;
        else ++reuse;
    }
    Require(builds == 12 && reuse == 0, "Every position change must rebuild without a timer");
    render();
    Require(pipeline.HSInvocations == 0 && pipeline.DSInvocations == 0,
        "Stopping camera must immediately reuse the completed mesh");
    scene.DeltaTime = 1;
    scene.UvOffset.x += 0.25f;
    render(); render(); render();
    Require(pipeline.HSInvocations == 0, "UV change should settle to a reusable cache");
    materials[0].TessellationParams = {8,1,0,0.5f};
    render(); render();
    const auto farTriangles = renderer.GetTessellationCacheStats().Triangles;
    scene.EyePos = {0,1,-1.05f};
    render(); render();
    Require(renderer.GetTessellationCacheStats().Triangles > farTriangles && pipeline.HSInvocations == 0,
        "Approaching model must increase cached triangle count within 0..0.5 distance");
    ShadowTestAccess::ExceedTessellationBudget(renderer);
    render();
    Require(renderer.GetTessellationCacheStats().FallbackMeshes == 1 && pipeline.HSInvocations > 0,
        "Cache budget exhaustion must use complete direct geometry");
    ShadowTestAccess::ClearTessellationBudgetRequest(renderer);
    render(); render(); render();
    Require(renderer.GetTessellationCacheStats().FallbackMeshes == 0 && pipeline.HSInvocations == 0,
        "Cache should recover after a budget fallback");
    submeshes[0].IndexCount -= 3;
    render(); render(); render(); render();
    Require(renderer.GetTessellationCacheStats().FallbackMeshes == 0 && pipeline.HSInvocations == 0,
        "Submesh range change did not regenerate cache");
    std::cout << "PASS: tessellation cache, HS/DS=0 in reused frames (including 4 shadows), static camera, "
        << "factor invalidation, SO overflow recovery, wireframe parity, T mode, moving camera "
        << builds << " rebuilds / " << reuse << " reused frames, UV invalidation, near/far detail, budget fallback/recovery, submesh invalidation\n";
    scene.ShowCullingScene = true;
    scene.Shadows.Enabled = false;
    scene.DeltaTime = 0;
    scene.EyePos = {6,6,-10};
    XMStoreFloat4x4(&scene.View, XMMatrixLookAtLH(XMLoadFloat3(&scene.EyePos), XMVectorZero(), XMVectorSet(0,1,0,0)));
    const auto primaryImage = render();
    const auto primaryIds = ShadowTestAccess::VisibleIds(renderer);
    Require(!primaryIds.empty() && primaryIds.size() < CullingScene::ObjectCount, "Test camera needs partial visibility");
    scene.ObserveCulling = true;
    const auto sideImage = render();
    Require(ShadowTestAccess::VisibleIds(renderer) == primaryIds, "Observer changed the primary camera selection");
    Require(sideImage != primaryImage, "Observer view did not render");
    UINT greenPixels = 0, yellowPixels = 0;
    for (size_t i = 0; i < sideImage.size(); i += 4)
    {
        if (sideImage[i+1] > 70 && sideImage[i+1] > sideImage[i] * 2 && sideImage[i+1] > sideImage[i+2]) ++greenPixels;
        if (sideImage[i] > 220 && sideImage[i+1] > 150 && sideImage[i+2] < 40) ++yellowPixels;
    }
    Require(greenPixels > 20 && yellowPixels > 20, "Observer objects/frustum missing from output pixels");
    scene.ShowCulledBounds = false;
    Require(render() != sideImage, "Culled bounds toggle has no effect");
    Require(ShadowTestAccess::VisibleIds(renderer) == primaryIds, "Debug bounds changed selection");
    scene.Culling = CullingMode::None; render();
    Require(renderer.GetCullingStats().Visible == CullingScene::ObjectCount, "Observer culling-off mode");
    scene.Culling = CullingMode::Linear; render();
    Require(ShadowTestAccess::VisibleIds(renderer) == primaryIds, "Linear selection differs in observer mode");
    scene.Culling = CullingMode::Octree;
    XMStoreFloat4x4(&scene.View, XMMatrixLookAtLH(XMLoadFloat3(&scene.EyePos), XMVectorSet(30,12,20,1), XMVectorSet(0,1,0,0)));
    render();
    Require(ShadowTestAccess::VisibleIds(renderer) != primaryIds, "Moving primary camera must update observer selection");
    scene.EyePos = {1000,1000,-1000};
    XMStoreFloat4x4(&scene.View, XMMatrixLookAtLH(XMLoadFloat3(&scene.EyePos), XMVectorSet(1000,1000,-1100,1), XMVectorSet(0,1,0,0)));
    render();
    Require(renderer.GetCullingStats().Visible == 0, "Empty observer selection");
    scene.ObserveCulling = false; render();
    std::cout << "PASS: side observer pixels (green objects, yellow frustum), unchanged primary selection, "
        << "culled bounds toggle, None/Linear/Octree, moving camera, empty selection, return to main view\n";
    scene.ShowCullingScene = false;
    scene.EyePos = {6,6,-10};
    XMStoreFloat4x4(&scene.View, XMMatrixLookAtLH(XMLoadFloat3(&scene.EyePos), XMVectorZero(), XMVectorSet(0,1,0,0)));
    const auto originalPost = render();
    scene.Post.Enabled = true;
    scene.Post.VignetteStrength = 0;
    scene.Post.ChromaticPixels = 0;
    Require(render() == originalPost, "Zero-strength effects must preserve the image exactly");
    scene.Post.VignetteStrength = 1;
    const auto vignetteImage = render();
    UINT darkerPixels = 0;
    for (size_t i = 0; i < originalPost.size(); i += 4)
    {
        Require(vignetteImage[i] <= originalPost[i], "Vignette unexpectedly brightens pixels");
        if (int(originalPost[i]) - int(vignetteImage[i]) > 5) ++darkerPixels;
    }
    Require(darkerPixels > 200, "Vignette has no measurable effect");
    for (UINT y = 120; y < 136; ++y)
        for (UINT x = 120; x < 136; ++x)
            for (UINT channel = 0; channel < 3; ++channel)
                Require(vignetteImage[(y*256+x)*4+channel] == originalPost[(y*256+x)*4+channel],
                    "Vignette must preserve the center");
    scene.Post.Vignette = false;
    Require(render() == originalPost, "Independent vignette disable failed");
    scene.Post.ChromaticPixels = 12;
    const auto chromaticImage = render();
    UINT chromaticPixels = 0;
    for (size_t i = 0; i < originalPost.size(); i += 4)
    {
        Require(chromaticImage[i+1] == originalPost[i+1], "Chromatic aberration must preserve green channel");
        if (std::abs(int(originalPost[i]) - int(chromaticImage[i])) > 3) ++chromaticPixels;
    }
    Require(chromaticPixels > 50, "Chromatic aberration has no measurable effect");
    scene.Post.ChromaticAberration = false;
    Require(render() == originalPost, "Independent chromatic disable failed");
    scene.Post.ChromaticAberration = true;
    scene.Post.Vignette = true;
    const auto combinedPost = render();
    Require(combinedPost != vignetteImage && combinedPost != chromaticImage, "Effects are not composed");
    scene.Post.Enabled = false;
    Require(render() == originalPost, "Master post-processing bypass failed");
    scene.Post.BufferView = 1;
    const auto albedoPost = render();
    scene.Post.BufferView = 2;
    const auto normalsPost = render();
    scene.Post.BufferView = 3;
    const auto depthPost = render();
    Require(albedoPost != originalPost && normalsPost != albedoPost && depthPost != normalsPost,
        "Post compute shader G-buffer views are missing");
    // An empty scene has depth=1 everywhere: every quad pixel, not only its center, must be white.
    scene.Shadows.ShowGround = false;
    submeshes.clear();
    const auto emptyDepth = render();
    Require(std::all_of(emptyDepth.begin(), emptyDepth.end(), [](unsigned char c) { return c == 255; }),
        "Fullscreen quad misses pixels in an empty depth view");
    std::cout << "PASS: compute output equals presentation, post-processing quad, G-buffer views, zero-strength identity, master/individual bypass, "
        << "vignette (" << darkerPixels << " pixels), chromatic (" << chromaticPixels << " pixels), combined effects\n";
    UINT errors=0;
    if(messages) for(UINT64 i=0;i<messages->GetNumStoredMessages();++i)
    {
        SIZE_T size=0; messages->GetMessage(i,nullptr,&size); std::vector<char> storage(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); messages->GetMessage(i,message,&size);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING) { std::cerr<<message->pDescription<<'\n'; ++errors; }
    }
    CloseHandle(event);
    Require(errors==0,"D3D12 validation warnings/errors");
    std::cout<<"PASS: integrated depth passes, 4 nonempty maps, tessellation, shadow on/off, PCF ("<<filtered
        <<" pixels), darkening ("<<darkened<<" pixels), debug colors, linear splits, instancing, particles. Debug layer: "<<(debug?"enabled":"unavailable")<<'\n';
    return 0;
}
catch(const std::exception& error)
{
    std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1;
}
