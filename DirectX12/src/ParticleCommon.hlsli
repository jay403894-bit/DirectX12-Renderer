// Shared by every per-effect update shader (UpdateParticles.hlsl, WaveParticles.hlsl, future
// ones) AND SpawnParticles.hlsl -- all of them bind through the SAME root signature (see
// Renderer::CreateComputeRootSignature), so they must all agree on this exact layout.
struct Particle
{
    float2 position;
    float2 velocity;
    float lifetime;
    // Seconds since this particle last (re)spawned -- lets per-particle behavior branch on how
    // long it's been alive (e.g. "age < 1: burst, else: drift") without needing a separate field
    // per phase.
    float age;
    uint isActive; // 1 if alive, 0 if dead
    float size;    // quad side length in pixels -- see ParticleVS.hlsl
    float4 color; // must stay in sync with ParticleVS.hlsl/RendererCore.h's Particle
};

// u0: this pool's own particle buffer. u1: this pool's own dead-list free list + hidden UAV
// counter (CounterOffsetInBytes, set up in Renderer::RegisterParticleEffect). An UPDATE shader
// only ever PUSHES onto DeadList (IncrementCounter -- Append semantics); SpawnParticles.hlsl is
// the only one that POPS (DecrementCounter -- Consume semantics) -- HLSL forbids doing both in
// one shader, which is why spawning is a separate dispatch/PSO even though it's the same buffer.
RWStructuredBuffer<Particle> ParticleBuffer : register(u0);
RWStructuredBuffer<uint> DeadList : register(u1);

// Root constants (see CreateComputeRootSignature): DeltaTime is the SAME dt main.cpp's
// GetFrameTime() computed this frame (real elapsed time, not a 60fps assumption). spawnCount is
// only meaningful to SpawnParticles.hlsl. maxParticles is this DISPATCH's pool size -- a runtime
// value now (not a shader constant) so one compiled update shader works correctly no matter
// which pool's buffers it's currently bound to.
cbuffer TimeCB : register(b0)
{
    float DeltaTime;
    uint spawnCount;
    uint maxParticles;
    float padding1;
};

// Marks particle i dead and pushes its slot onto the free list for a future spawn to claim.
void KillParticle(uint i)
{
    ParticleBuffer[i].isActive = 0;
    // IncrementCounter() returns the PRE-increment count -- the slot to push this index into --
    // then bumps the counter so SpawnParticles.hlsl's DecrementCounter() can pop it.
    DeadList[DeadList.IncrementCounter()] = i;
}
