// Two physical buffers swap roles after each simulation step.
struct Particle
{
    float3 Position;
    float Age;
    float3 Velocity;
    float Lifetime;
    float4 Color;
    float Size;
    float3 Padding;
};

StructuredBuffer<Particle> DrawParticles : register(t0);
ByteAddressBuffer CountBeforeUpdate : register(t1);
ConsumeStructuredBuffer<Particle> InputParticles : register(u0);
AppendStructuredBuffer<Particle> OutputParticles : register(u1);

cbuffer Constants : register(b0)
{
    float4x4 ViewProj;
    float4 CameraRight;
    float4 CameraUp;
    float4 EmitterSize;
    float DeltaTime;
    float Gravity;
    uint SpawnCount;
    uint FrameSeed;
};

static const uint Capacity = PARTICLE_CAPACITY;

float Random01(inout uint state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return (state & 0x00ffffff) / 16777216.0;
}

[numthreads(256, 1, 1)]
void SimulateCS(uint3 thread : SV_DispatchThreadID)
{
    uint id = thread.x;
    uint alive = min(CountBeforeUpdate.Load(0), Capacity);
    // Use an immutable snapshot, NOT the counter being decremented by Consume.
    if (id < alive)
    {
        Particle p = InputParticles.Consume();
        p.Age += DeltaTime;
        p.Velocity.y -= Gravity * DeltaTime;
        p.Position += p.Velocity * DeltaTime;
        if (p.Age < p.Lifetime)
            OutputParticles.Append(p);
    }

    // Reserve space conservatively before deaths: survivors + births never exceed capacity.
    if (id < min(SpawnCount, Capacity - alive))
    {
        uint seed = (id + 1) * 747796405u + FrameSeed * 2891336453u;
        seed |= 1u;
        float angle = Random01(seed) * 6.2831853;
        float spread = sqrt(Random01(seed));
        Particle p = (Particle)0;
        p.Position = EmitterSize.xyz + float3(cos(angle), 0, sin(angle)) * spread * 0.25;
        p.Velocity = float3(cos(angle) * spread * 2.0,
            4.5 + Random01(seed) * 2.0, sin(angle) * spread * 2.0);
        p.Lifetime = 3.0 + Random01(seed) * 2.0;
        p.Size = EmitterSize.w * (0.75 + Random01(seed) * 0.5);
        p.Color = float4(lerp(float3(0.1, 0.65, 0.95),
            float3(0.95, 0.75, 0.12), Random01(seed)), 1);
        OutputParticles.Append(p);
    }
}

struct Point
{
    float3 Center : POSITION;
    float Size : PSIZE;
    float4 Color : COLOR;
};

Point ParticleVS(uint id : SV_VertexID)
{
    Particle p = DrawParticles[id];
    Point output;
    output.Center = p.Position;
    output.Size = p.Size;
    output.Color = p.Color;
    return output;
}

struct Pixel
{
    float4 Position : SV_POSITION;
    float2 Local : TEXCOORD;
    float4 Color : COLOR;
};

[maxvertexcount(4)]
void ParticleGS(point Point points[1], inout TriangleStream<Pixel> stream)
{
    const float2 corners[4] = { float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1) };
    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        Pixel output;
        float3 position = points[0].Center + points[0].Size *
            (corners[i].x * CameraRight.xyz + corners[i].y * CameraUp.xyz);
        output.Position = mul(float4(position, 1), ViewProj);
        output.Local = corners[i];
        output.Color = points[0].Color;
        stream.Append(output);
    }
    stream.RestartStrip();
}

struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
};

GBufferOutput ParticlePS(Pixel input)
{
    float r2 = dot(input.Local, input.Local);
    clip(1.0 - r2); // Opaque disk, not alpha blending; discarded corners do not write depth.
    float3 facing = normalize(cross(CameraUp.xyz, CameraRight.xyz));
    float3 normal = input.Local.x * CameraRight.xyz + input.Local.y * CameraUp.xyz
        + sqrt(saturate(1.0 - r2)) * facing;
    GBufferOutput output;
    output.Albedo = float4(input.Color.rgb, 0); // Opaque dielectric; alpha now stores metallic.
    output.Normal = float4(normalize(normal), 1);
    return output;
}
