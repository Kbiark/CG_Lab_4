Texture2D gDiffuseMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDisplacementMap : register(t2);
Texture2D gAlbedoBuffer : register(t0);
Texture2D gNormalBuffer : register(t1);
Texture2D gDepthBuffer : register(t2);
Texture2D gLightingBuffer : register(t0);
Texture2DArray<float> gShadowMaps : register(t3);
SamplerComparisonState gShadowSampler : register(s1);

SamplerState gSampler : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 mWorldViewProj;
    float2 uvTiling;
    float2 uvOffset;
    float4 eyePosAndDisplacementScale;
    float4 tessellationParams; // maxTF, minTF, nearDistance, farDistance
    float4 normalParams; // normalStrength, mapWidth, mapHeight, debugViewMode
};

struct DirectionalLightGpu
{
    float4 directionIntensity;
    float4 color;
};

struct PointLightGpu
{
    float4 positionRange;
    float4 colorIntensity;
};

struct SpotLightGpu
{
    float4 positionRange;
    float4 directionAngle;
    float4 colorIntensity;
};

cbuffer cbFrame : register(b0)
{
    float4x4 gInvViewProj;
    float4x4 gViewProj;
    float4 gCameraPosition;
    float4 gLightCounts;
    float4 gScreenSize;
    DirectionalLightGpu gDirectionalLights[1];
    SpotLightGpu gSpotLights[2];
    float4x4 gView;
    float4x4 gShadowViewProjection[4];
    float4 gCascadeSplits;
    float4 gShadowParams; // enabled, PCF radius, inverse map resolution, debug cascades
    float4 gShadowConfig; // near plane, blend fraction, receiver bias, distance fade start
};

cbuffer cbPointLight : register(b1)
{
    float4 gPointLightPositionRange;
    float4 gPointLightColorIntensity;
};

struct VSInput
{
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
    float2 Tex : TEXCOORD;
    float3 Tangent : TANGENT;
    float3 Bitangent : BINORMAL;
};

struct GeometryPSInput
{
    float4 PosH : SV_POSITION;
    float3 NormalW : NORMAL;
    float3 TangentW : TANGENT;
    float3 BitangentW : BINORMAL;
    float2 TexC : TEXCOORD;
    float TessFactor : TEXCOORD1;
};

struct GeometryPSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
};

struct FullscreenPSInput
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

struct PointLightVolumeInput
{
    float3 Pos : POSITION;
};

struct PointLightVolumePSInput
{
    float4 PosH : SV_POSITION;
};

GeometryPSInput GeometryVS(VSInput vin)
{
    GeometryPSInput vout;
    vout.PosH = float4(vin.Pos, 1.0f);
    vout.TessFactor = 1.0f;
    vout.NormalW = normalize(vin.Normal);
    vout.TangentW = normalize(vin.Tangent);
    vout.BitangentW = normalize(vin.Bitangent);
    vout.TexC = vin.Tex;
    return vout;
}

GeometryPSInput GeometryNoTessVS(VSInput vin)
{
    GeometryPSInput vout;
    vout.PosH = mul(float4(vin.Pos, 1.0f), mWorldViewProj);
    vout.NormalW = normalize(vin.Normal);
    vout.TangentW = normalize(vin.Tangent);
    vout.BitangentW = normalize(vin.Bitangent);
    vout.TexC = vin.Tex;
    vout.TessFactor = 1.0f;
    return vout;
}

GeometryPSInput GeometryInstanceVS(float3 position : POSITION, float2 uv : TEXCOORD,
    float3 tangent : TANGENT, float3 bitangent : BINORMAL,
    float4 world0 : INSTANCE0, float4 world1 : INSTANCE1,
    float4 world2 : INSTANCE2, float4 world3 : INSTANCE3,
    float4 normal0 : NORMALWORLD0, float4 normal1 : NORMALWORLD1,
    float4 normal2 : NORMALWORLD2, float4 normal3 : NORMALWORLD3)
{
    GeometryPSInput output;
    float4x4 world = float4x4(world0, world1, world2, world3);
    float4x4 normalWorld = float4x4(normal0, normal1, normal2, normal3);
    output.PosH = mul(mul(float4(position, 1.0f), world), mWorldViewProj);
    output.NormalW = normalize(mul(normalize(position), (float3x3)normalWorld));
    output.TangentW = normalize(mul(tangent, (float3x3)world));
    output.BitangentW = normalize(mul(bitangent, (float3x3)world));
    output.TexC = uv;
    output.TessFactor = 1.0f;
    return output;
}

struct HSConstantData
{
    float Edges[3] : SV_TessFactor;
    float Inside : SV_InsideTessFactor;
};

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("GeometryPatchConstants")]
[maxtessfactor(8.0)]
GeometryPSInput GeometryHS(InputPatch<GeometryPSInput, 3> patch, uint controlPointId : SV_OutputControlPointID)
{
    return patch[controlPointId];
}

float TessFactorForPoint(float3 pointW)
{
    float distanceToCamera = distance(pointW, eyePosAndDisplacementScale.xyz);
    float range = max(0.001f, tessellationParams.w - tessellationParams.z);
    float t = saturate((distanceToCamera - tessellationParams.z) / range);
    return lerp(tessellationParams.x, tessellationParams.y, t);
}

HSConstantData GeometryPatchConstants(InputPatch<GeometryPSInput, 3> patch, uint patchId : SV_PrimitiveID)
{
    HSConstantData output;

    float3 center = (patch[0].PosH.xyz + patch[1].PosH.xyz + patch[2].PosH.xyz) / 3.0f;
    float3 patchNormal = normalize(patch[0].NormalW + patch[1].NormalW + patch[2].NormalW);
    float3 toEye = normalize(eyePosAndDisplacementScale.xyz - center);

    // Camera backface rejection is invalid for shadow casters on the far side.
#ifndef SHADOW_PASS
    if (dot(patchNormal, toEye) < -0.25f)
    {
        output.Edges[0] = 0.0f;
        output.Edges[1] = 0.0f;
        output.Edges[2] = 0.0f;
        output.Inside = 0.0f;
        return output;
    }
#endif

    // Shared edge midpoints produce the same factor in both neighboring patches.
    output.Edges[0] = TessFactorForPoint(0.5f * (patch[1].PosH.xyz + patch[2].PosH.xyz));
    output.Edges[1] = TessFactorForPoint(0.5f * (patch[2].PosH.xyz + patch[0].PosH.xyz));
    output.Edges[2] = TessFactorForPoint(0.5f * (patch[0].PosH.xyz + patch[1].PosH.xyz));
    output.Inside = (output.Edges[0] + output.Edges[1] + output.Edges[2]) / 3.0f;
    return output;
}

[domain("tri")]
GeometryPSInput GeometryDS(
    HSConstantData input,
    float3 barycentric : SV_DomainLocation,
    const OutputPatch<GeometryPSInput, 3> patch)
{
    GeometryPSInput output;

    float3 posW =
        patch[0].PosH.xyz * barycentric.x +
        patch[1].PosH.xyz * barycentric.y +
        patch[2].PosH.xyz * barycentric.z;
    float2 uv =
        patch[0].TexC * barycentric.x +
        patch[1].TexC * barycentric.y +
        patch[2].TexC * barycentric.z;
    float3 normalW = normalize(
        patch[0].NormalW * barycentric.x +
        patch[1].NormalW * barycentric.y +
        patch[2].NormalW * barycentric.z);
    float3 tangentW = normalize(
        patch[0].TangentW * barycentric.x +
        patch[1].TangentW * barycentric.y +
        patch[2].TangentW * barycentric.z);
    float3 bitangentW = normalize(
        patch[0].BitangentW * barycentric.x +
        patch[1].BitangentW * barycentric.y +
        patch[2].BitangentW * barycentric.z);

    float2 materialUv = uv * uvTiling + uvOffset;
    float height = gDisplacementMap.SampleLevel(gSampler, materialUv, 0).r;
    float displacement = (height - 0.5f) * 2.0f;
    displacement *= eyePosAndDisplacementScale.w;
    posW += normalW * displacement;

#ifdef CACHE_TESSELLATION
    output.PosH = float4(posW, 1.0f);
#else
    output.PosH = mul(float4(posW, 1.0f), mWorldViewProj);
#endif
    output.NormalW = normalW;
    output.TangentW = tangentW;
    output.BitangentW = bitangentW;
    output.TexC = uv;
    output.TessFactor = input.Inside;
    return output;
}

struct CachedVertexInput
{
    float4 PositionW : POSITION;
    float3 NormalW : NORMAL;
    float3 TangentW : TANGENT;
    float3 BitangentW : BINORMAL;
    float2 TexC : TEXCOORD;
    float TessFactor : TEXCOORD1;
};

GeometryPSInput CachedGeometryVS(CachedVertexInput vin)
{
    GeometryPSInput output;
    output.PosH = mul(vin.PositionW, mWorldViewProj);
    output.NormalW = vin.NormalW;
    output.TangentW = vin.TangentW;
    output.BitangentW = vin.BitangentW;
    output.TexC = vin.TexC;
    output.TessFactor = vin.TessFactor;
    return output;
}

struct CullingLineInput
{
    float3 Position : POSITION;
    float3 Color : COLOR;
};

struct CullingLineOutput
{
    float4 Position : SV_POSITION;
    float3 Color : COLOR;
};

CullingLineOutput CullingLineVS(CullingLineInput vin)
{
    CullingLineOutput output;
    output.Position = mul(float4(vin.Position, 1), mWorldViewProj);
    output.Color = vin.Color;
    return output;
}

float4 CullingLinePS(CullingLineOutput pin) : SV_Target
{
    return float4(pin.Color, 1);
}

float4 CullingObserverPS(GeometryPSInput pin) : SV_Target
{
    float lighting = 0.4f + 0.6f * saturate(dot(normalize(pin.NormalW), normalize(float3(-1, 2, -1))));
    return float4(float3(0.15f, 0.9f, 0.45f) * lighting, 1);
}

void ShadowPS(GeometryPSInput pin)
{
    clip(gDiffuseMap.Sample(gSampler, pin.TexC * uvTiling + uvOffset).a - 0.1f);
}

GeometryPSOutput GroundPS(GeometryPSInput pin)
{
    GeometryPSOutput output;
    float checker = frac((floor(pin.TexC.x * 0.5) + floor(pin.TexC.y * 0.5)) * 0.5) * 2;
    output.Albedo = float4(lerp(float3(0.25, 0.29, 0.31), float3(0.35, 0.39, 0.4), checker), 1);
    output.Normal = float4(0, 1, 0, 1);
    return output;
}

GeometryPSOutput GeometryPS(GeometryPSInput pin)
{
    GeometryPSOutput output;
    float2 uv = pin.TexC * uvTiling + uvOffset;
    float4 albedo = gDiffuseMap.Sample(gSampler, uv);
    clip(albedo.a - 0.1f);

    float3 tangentNormal = gNormalMap.Sample(gSampler, uv).rgb * 2.0f - 1.0f;
    tangentNormal.xy *= normalParams.x;
    tangentNormal = normalize(tangentNormal);

    // Gram-Schmidt keeps the tangent basis orthogonal after interpolation.
    float3 N = normalize(pin.NormalW);
    float3 T = normalize(pin.TangentW - dot(pin.TangentW, N) * N);
    float handedness = dot(cross(N, T), pin.BitangentW) < 0.0f ? -1.0f : 1.0f;
    float3 B = normalize(cross(N, T)) * handedness;
    float3x3 tbnMat = float3x3(T, B, N);
    float3 normalW = normalize(mul(tangentNormal, tbnMat));

    if (normalParams.w >= 1.5f && normalParams.w < 2.5f)
    {
        albedo = float4(normalW * 0.5f + 0.5f, 1.0f);
    }
    else if (normalParams.w >= 2.5f)
    {
        float tessLevel = saturate((pin.TessFactor - tessellationParams.y) /
            max(0.001f, tessellationParams.x - tessellationParams.y));
        albedo = float4(
            lerp(float3(0.1f, 0.2f, 1.0f), float3(1.0f, 0.1f, 0.05f), tessLevel),
            1.0f);
    }

    output.Albedo = albedo;
    output.Normal = float4(normalW, 1.0f);
    return output;
}

FullscreenPSInput LightingVS(uint vertexId : SV_VertexID)
{
    FullscreenPSInput vout;

    float2 positions[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  3.0f),
        float2( 3.0f, -1.0f)
    };

    float2 texcoords[3] =
    {
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f),
        float2(2.0f, 1.0f)
    };

    vout.PosH = float4(positions[vertexId], 0.0f, 1.0f);
    vout.TexC = texcoords[vertexId];
    return vout;
}

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 clipPos = float4(uv * 2.0f - 1.0f, depth, 1.0f);
    clipPos.y *= -1.0f;

    float4 worldPos = mul(clipPos, gInvViewProj);
    worldPos /= worldPos.w;
    return worldPos.xyz;
}

float3 ComputeDirectionalLight(float3 normal, float3 viewDir, float3 albedo, float3 worldPos)
{
    float3 color = 0.0f;

    [unroll]
    for (int i = 0; i < (int)gLightCounts.x; ++i)
    {
        float3 lightDir = normalize(-gDirectionalLights[i].directionIntensity.xyz);
        float intensity = gDirectionalLights[i].directionIntensity.w;
        float3 lightColor = gDirectionalLights[i].color.xyz;

        float ndl = saturate(dot(normal, lightDir));
        float3 halfVec = normalize(lightDir + viewDir);
        float spec = pow(saturate(dot(normal, halfVec)), 32.0f);

        color += albedo * lightColor * ndl * intensity;
        color += lightColor * spec * intensity * 0.35f;
    }

    return color;
}

float3 ComputeSinglePointLight(float3 normal, float3 viewDir, float3 albedo, float3 worldPos)
{
    float3 lightVector = gPointLightPositionRange.xyz - worldPos;
    float distanceToLight = length(lightVector);
    float range = gPointLightPositionRange.w;

    if (distanceToLight > range)
        return 0.0f;

    float attenuation = 1.0f - saturate(distanceToLight / range);
    attenuation *= attenuation;
    float3 lightDir = normalize(lightVector);
    float ndl = saturate(dot(normal, lightDir));
    float3 halfVec = normalize(lightDir + viewDir);
    float spec = pow(saturate(dot(normal, halfVec)), 32.0f);

    float3 lightColor = gPointLightColorIntensity.xyz;
    float intensity = gPointLightColorIntensity.w;

    return albedo * lightColor * ndl * intensity * attenuation +
        lightColor * spec * intensity * attenuation * 0.35f;
}

float3 ComputeSpotLights(float3 normal, float3 viewDir, float3 albedo, float3 worldPos)
{
    float3 color = 0.0f;

    [unroll]
    for (int i = 0; i < (int)gLightCounts.z; ++i)
    {
        float3 lightVector = gSpotLights[i].positionRange.xyz - worldPos;
        float distanceToLight = length(lightVector);
        float range = gSpotLights[i].positionRange.w;

        if (distanceToLight > range)
            continue;

        float3 lightDir = normalize(lightVector);
        float3 spotDir = normalize(-gSpotLights[i].directionAngle.xyz);
        float cone = saturate(dot(lightDir, spotDir));
        float cutoff = gSpotLights[i].directionAngle.w;

        if (cone < cutoff)
            continue;

        float attenuation = (1.0f - saturate(distanceToLight / range)) * smoothstep(cutoff, 1.0f, cone);
        float ndl = saturate(dot(normal, lightDir));
        float3 halfVec = normalize(lightDir + viewDir);
        float spec = pow(saturate(dot(normal, halfVec)), 32.0f);

        float3 lightColor = gSpotLights[i].colorIntensity.xyz;
        float intensity = gSpotLights[i].colorIntensity.w;

        color += albedo * lightColor * ndl * intensity * attenuation;
        color += lightColor * spec * intensity * attenuation * 0.35f;
    }

    return color;
}

#include "shadow_sampling.hlsli"

float4 LightingPS(FullscreenPSInput pin) : SV_Target
{
    float2 uv = pin.TexC;
    float4 albedoSample = gAlbedoBuffer.Sample(gSampler, uv);
    float3 normal = normalize(gNormalBuffer.Sample(gSampler, uv).xyz);
    float depth = gDepthBuffer.Sample(gSampler, uv).r;
    if (depth >= 1.0f) return float4(0, 0, 0, 1);

    float3 worldPos = ReconstructWorldPosition(uv, depth);
    float3 viewDir = normalize(gCameraPosition.xyz - worldPos);
    float3 albedo = albedoSample.rgb;

    float3 color = albedo * float3(0.18f, 0.19f, 0.20f);
    color += ComputeDirectionalLight(normal, viewDir, albedo, worldPos) * DirectionalShadow(worldPos, normal);
    color += ComputeSpotLights(normal, viewDir, albedo, worldPos);

    if (gShadowParams.w > 0.5)
    {
        const float3 colors[4] = { float3(1,0.2,0.2), float3(0.2,1,0.3), float3(0.2,0.5,1), float3(1,0.8,0.15) };
        float viewDepth = mul(float4(worldPos, 1), gView).z;
        if (viewDepth <= gCascadeSplits.w)
            color = lerp(color, colors[SelectCascade(viewDepth)], 0.45);
    }

    return float4(color, 1.0f);
}

PointLightVolumePSInput PointLightVolumeVS(PointLightVolumeInput vin)
{
    PointLightVolumePSInput vout;
    float3 worldPos = vin.Pos * gPointLightPositionRange.w + gPointLightPositionRange.xyz;
    vout.PosH = mul(float4(worldPos, 1.0f), gViewProj);
    return vout;
}

float4 PointLightVolumePS(PointLightVolumePSInput pin) : SV_Target
{
    float2 uv = pin.PosH.xy * gScreenSize.zw;
    if (any(uv < 0.0f) || any(uv > 1.0f))
        discard;

    float4 albedoSample = gAlbedoBuffer.Sample(gSampler, uv);
    float3 normal = normalize(gNormalBuffer.Sample(gSampler, uv).xyz);
    float depth = gDepthBuffer.Sample(gSampler, uv).r;

    if (depth >= 0.99999f)
        discard;

    float3 worldPos = ReconstructWorldPosition(uv, depth);
    float3 viewDir = normalize(gCameraPosition.xyz - worldPos);
    if (length(gPointLightPositionRange.xyz - worldPos) > gPointLightPositionRange.w)
        discard;

    float3 color = ComputeSinglePointLight(normal, viewDir, albedoSample.rgb, worldPos);
    return float4(color, 1.0f);
}

#include "post_processing.hlsli"
