#pragma once
#include <DirectXMath.h>

struct ObjectConstants
{
    DirectX::XMFLOAT4X4 mWorldViewProj;

    DirectX::XMFLOAT2 uvTiling;
    DirectX::XMFLOAT2 uvOffset;

    DirectX::XMFLOAT4 eyePosAndDisplacementScale = { 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 tessellationParams = { 8.0f, 1.0f, 0.0f, 0.5f };
    DirectX::XMFLOAT4 normalParams = { 1.0f, 2048.0f, 2048.0f, 0.0f };
    DirectX::XMFLOAT4 materialParams = { 0.6f, 0.0f, 0.0f, 0.0f }; // Roughness, metallic.

    ObjectConstants()
    {
        DirectX::XMStoreFloat4x4(
            &mWorldViewProj,
            DirectX::XMMatrixIdentity());
    }
};
