// The existing deferred heap stores these four SRVs consecutively.
Texture2D<float4> gPostAlbedo : register(t0);
Texture2D<float4> gPostNormals : register(t1);
Texture2D<float> gPostDepth : register(t2);
Texture2D<float4> gPostHdr : register(t3);
SamplerState gPostLinearClamp : register(s1);
RWTexture2D<float4> gPostOutput : register(u0);

cbuffer cbPost : register(b2)
{
    float4 gPostEffects; // Vignette strength, chromatic pixels, inverse width/height.
    float4 gPostView;    // Buffer view, projection _33/_43, exposure.
};

FullscreenPSInput FinalVS(uint vertexId : SV_VertexID)
{
    // Triangle strip: top-left, top-right, bottom-left, bottom-right. No VB/IB.
    FullscreenPSInput output;
    output.TexC = float2(vertexId & 1, vertexId >> 1);
    output.PosH = float4(output.TexC * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

struct PostInputs
{
    float3 Albedo;
    float3 Normal;
    float Depth;
    float3 Hdr;
};

PostInputs ReadPostInputs(float2 uv)
{
    PostInputs input;
    input.Albedo = gPostAlbedo.SampleLevel(gSampler, uv, 0).rgb;
    input.Normal = gPostNormals.SampleLevel(gSampler, uv, 0).xyz;
    input.Depth = gPostDepth.SampleLevel(gSampler, uv, 0);
    input.Hdr = gPostHdr.SampleLevel(gSampler, uv, 0).rgb;
    return input;
}

float4 ProcessPostPixel(float2 uv)
{
    PostInputs input = ReadPostInputs(uv);
    if (gPostView.x == 1) return float4(input.Albedo, 1);
    if (gPostView.x == 2)
        return float4(input.Depth >= 1 ? float3(0,0,0) : input.Normal * 0.5f + 0.5f, 1);
    if (gPostView.x == 3)
    {
        // D3D perspective depth = projection._33 + projection._43 / viewZ.
        float z = gPostView.z / min(input.Depth - gPostView.y, -0.000001f);
        float value = input.Depth >= 1 ? 1 : saturate(z / (z + 10));
        return float4(value, value, value, 1);
    }

    float2 radial = uv * 2 - 1;
    float radius = saturate(length(radial) / sqrt(2.0f));
    float3 hdr = input.Hdr;
    if (gPostEffects.y > 0)
    {
        // Direction in screen pixels avoids stretching the offset on non-square windows.
        float2 pixelDirection = radial / gPostEffects.zw;
        pixelDirection /= max(length(pixelDirection), 0.0001f);
        float2 offset = pixelDirection * gPostEffects.zw * gPostEffects.y * radius * radius;
        hdr.r = gPostHdr.SampleLevel(gPostLinearClamp, uv + offset, 0).r;
        hdr.b = gPostHdr.SampleLevel(gPostLinearClamp, uv - offset, 0).b;
    }
    hdr *= gPostView.w;
    float3 mapped = hdr / (hdr + 1);
    float vignette = 1 - gPostEffects.x * smoothstep(0.2f, 1.0f, radius);
    mapped *= vignette;
    // Keep the existing UNORM back buffer: encode only once, at the end.
    return float4(pow(saturate(mapped), 1.0f / 2.2f), 1);
}

[numthreads(8, 8, 1)]
void PostCS(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    gPostOutput.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    gPostOutput[id.xy] = ProcessPostPixel((float2(id.xy) + 0.5f) / float2(width, height));
}

float4 FinalPS(FullscreenPSInput pin) : SV_Target
{
    // Presentation binds the computed image at t0 instead of the albedo texture.
    return gPostAlbedo.Load(int3(int2(pin.PosH.xy), 0));
}
