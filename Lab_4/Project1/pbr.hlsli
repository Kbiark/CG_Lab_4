TextureCube<float4> gIrradianceMap : register(t4);
TextureCube<float4> gPrefilterMap : register(t5);
Texture2D<float2> gBrdfLut : register(t6);
SamplerState gIblSampler : register(s2);

static const float PBR_PI = 3.14159265359f;

float3 FresnelSchlick(float cosine, float3 f0)
{
    return f0 + (1 - f0) * pow(1 - saturate(cosine), 5);
}

float DistributionGGX(float ndh, float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator = ndh * ndh * (alpha2 - 1) + 1;
    return alpha2 / max(PBR_PI * denominator * denominator, 0.0000001f);
}

float GeometrySchlickGGX(float ndx, float roughness)
{
    float k = (roughness + 1) * (roughness + 1) / 8;
    return ndx / max(ndx * (1 - k) + k, 0.00001f);
}

float3 PbrDirect(float3 n, float3 v, float3 l, float3 albedo, float roughness, float metallic, float3 radiance)
{
    float ndl = saturate(dot(n, l));
    float ndv = saturate(dot(n, v));
    if (ndl <= 0 || ndv <= 0) return 0;
    float3 h = normalize(v + l);
    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 f = FresnelSchlick(dot(h, v), f0);
    float d = DistributionGGX(saturate(dot(n, h)), roughness);
    float g = GeometrySchlickGGX(ndv, roughness) * GeometrySchlickGGX(ndl, roughness);
    float3 specular = d * g * f / max(4 * ndv * ndl, 0.0001f);
    float3 diffuse = (1 - f) * (1 - metallic) * albedo / PBR_PI;
    return (diffuse + specular) * radiance * ndl;
}

float3 PbrIbl(float3 n, float3 v, float3 albedo, float roughness, float metallic)
{
    if (gIblParams.x < 0.5f) return 0;
    float ndv = saturate(dot(n, v));
    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 f = f0 + (max(1 - roughness, f0) - f0) * pow(1 - ndv, 5);
    float3 diffuse = gIrradianceMap.SampleLevel(gIblSampler, n, 0).rgb * albedo;
    float3 reflected = reflect(-v, n);
    float3 prefiltered = gPrefilterMap.SampleLevel(gIblSampler, reflected, roughness * gIblParams.z).rgb;
    float2 brdf = gBrdfLut.SampleLevel(gIblSampler, float2(ndv, roughness), 0).rg;
    // The LUT already integrates Fresnel's angular variation; its A term multiplies F0.
    float3 specular = prefiltered * (f0 * brdf.x + brdf.y);
    return ((1 - f) * (1 - metallic) * diffuse + specular) * gIblParams.y;
}
