// Same GlobalUniforms layout as VertexShader.hlsl -- screenSize is the only field this needs
// (pixel-space particle positions -> NDC), kept identical so both shaders can share the SAME
// constant buffer (m_ConstantBuffers[frame]) without a second upload.
cbuffer GlobalUniforms : register(b0)
{
    float time;
    float3 padding1;
    float2 screenSize;
    float aspectRatio;
    float padding4;
};

struct Particle
{
    float2 position;
    float2 velocity;
    float lifetime;
    float age; // seconds since spawn -- unused here, kept for layout parity with the compute shaders
    uint isActive;
    float size;    // quad side length in pixels -- see the corner-offset table below
    float4 color; // set per-spawn via RequestSpawn -- must stay in sync with the CS structs
};

// Read-only view of the SAME buffer the update shaders write as a UAV -- RendererCore::UpdateParticles()
// transitions it UNORDERED_ACCESS -> NON_PIXEL_SHADER_RESOURCE after the compute dispatch, every frame,
// before this draw runs.
StructuredBuffer<Particle> ParticleBuffer : register(t0);

struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

// No vertex/index buffer bound -- each particle is a procedurally-generated quad (2 triangles,
// 6 vertices), SV_VertexID picks which corner. This is what actually gives particles a
// controllable SIZE: D3D12's POINTLIST topology (the previous approach) draws a fixed ~1px dot
// with no size or UV support at all, so there was no way to make a point-list particle bigger.
// Corner order matches a triangle list: (0,1,2) then (2,1,3) of a quad laid out
// TL(0) TR(1)
// BL(2) BR(3)
static const float2 kCornerOffsets[6] = {
    float2(-0.5f,  0.5f), float2(0.5f,  0.5f), float2(-0.5f, -0.5f), // triangle 1: TL, TR, BL
    float2(-0.5f, -0.5f), float2(0.5f,  0.5f), float2(0.5f, -0.5f), // triangle 2: BL, TR, BR
};

VSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    Particle p = ParticleBuffer[instanceID];

    float2 corner = kCornerOffsets[vertexID] * p.size;
    float2 worldPos = p.position + corner;

    float2 ndc;
    ndc.x = (worldPos.x / screenSize.x) * 2.0f - 1.0f;
    ndc.y = 1.0f - (worldPos.y / screenSize.y) * 2.0f;

    output.pos = float4(ndc, 0.0f, 1.0f);
    // Inactive particles (never spawned, or dead and awaiting reuse) must be invisible, not just
    // faded: they still occupy a real slot in the buffer with a real seeded lifetime, so without
    // this check every unspawned particle would draw at full alpha, burying any actually-active
    // ones in a permanent field of static quads.
    float alpha = (p.isActive == 1) ? saturate(p.lifetime / 5.0f) : 0.0f;
    output.color = float4(p.color.rgb, p.color.a * alpha);
    return output;
}
