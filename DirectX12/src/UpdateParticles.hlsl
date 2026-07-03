// "Linear" behavior: straight-line motion by velocity, fading out over lifetime. The FIRST
// registered per-effect update shader -- see ParticleCommon.hlsli for the shared
// struct/buffers/root-constants every update shader (and SpawnParticles.hlsl) agree on.
#include "ParticleCommon.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint i = DTid.x;
    if (i >= maxParticles) return;

    if (ParticleBuffer[i].isActive == 1)
    {
        ParticleBuffer[i].position += ParticleBuffer[i].velocity * DeltaTime;
        ParticleBuffer[i].lifetime -= DeltaTime;
        ParticleBuffer[i].age += DeltaTime;

        if (ParticleBuffer[i].lifetime <= 0)
            KillParticle(i);
    }
}
