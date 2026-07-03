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
    float age; // seconds since spawn -- unused here, kept for layout parity with UpdateParticles.hlsl
    uint isActive;
    uint padding;
    float4 color; // set per-spawn via Renderer::RequestSpawn -- must stay in sync with the CS structs
};

// Read-only view of the SAME buffer UpdateParticles.hlsl writes as a UAV -- Renderer::UpdateParticles()
// transitions it UNORDERED_ACCESS -> NON_PIXEL_SHADER_RESOURCE after the compute dispatch, every frame,
// before this draw runs (see the barrier in Renderer.cpp).
StructuredBuffer<Particle> ParticleBuffer : register(t0);

struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

// No vertex buffer bound -- one point per instance, SV_InstanceID selects the particle.
VSOutput VSMain(uint instanceID : SV_InstanceID)
{
    VSOutput output;
    Particle p = ParticleBuffer[instanceID];

    float2 ndc;
    ndc.x = (p.position.x / screenSize.x) * 2.0f - 1.0f;
    ndc.y = 1.0f - (p.position.y / screenSize.y) * 2.0f;

    output.pos = float4(ndc, 0.0f, 1.0f);
    // Inactive particles (never spawned, or dead and awaiting reuse -- see SpawnParticles.hlsl/
    // UpdateParticles.hlsl) must be invisible, not just faded: they still occupy a real slot in
    // the buffer with a real seeded lifetime (5.0 from Renderer::SeedParticles), so without this
    // check every one of the 256000 unspawned particles would draw at full alpha, burying any
    // actually-active ones in a permanent field of static points.
    float alpha = (p.isActive == 1) ? saturate(p.lifetime / 5.0f) : 0.0f;
    output.color = float4(p.color.rgb, p.color.a * alpha);
    return output;
}
