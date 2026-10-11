#pragma once

#include <string>
#include <wrl/client.h>
#include <d3d12.h>
#include <DirectXMath.h>

struct Material
{
    std::string Name;

    std::string DiffuseMap;
    std::string NormalMap;
    std::string DisplacementMap;
    UINT SrvHeapIndex = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> DiffuseTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> NormalTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> DisplacementTexture;

    DirectX::XMFLOAT2 Tiling = { 1.0f, 1.0f };
    DirectX::XMFLOAT2 StaticUvOffset = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 UVSpeed = { 0.0f, 0.0f };
    float DisplacementScale = 0.12f;
    float NormalStrength = 1.0f;
    float Roughness = 0.6f;
    float Metallic = 0.0f;
    DirectX::XMFLOAT4 TessellationParams = { 8.0f, 1.0f, 0.0f, 0.5f };
};
