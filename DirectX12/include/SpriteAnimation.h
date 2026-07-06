#pragma once
#include <DirectXMath.h>
#include <cstdint>
namespace JGL {
    // Plain data + Update() -- no ties to Renderer2D at all. Feed the result into the EXISTING
    // Renderer2D::Submit(...)'s uvOffset/uvScale params (Submit already supports arbitrary UV
    // sub-rects), so no new SubmitSprite draw path is needed. Deliberately a flat struct rather
    // than a class with hidden behavior, so it slots into a SoA array of entities as-is.
    struct SpriteAnimation
    {
        uint32_t framesX = 1, framesY = 1;   // atlas grid, same convention as ParticleEffectPool
        uint32_t frameCount = 1;             // may be < framesX*framesY if the last row is partial
        float fps = 10.0f;
        float elapsed = 0.0f;
        uint32_t currentFrame = 0;
        bool loop = true;
        bool finished = false;               // only ever set true when loop == false

        void Update(float dt)
        {
            if (finished) return;
            elapsed += dt;
            float frameDuration = 1.0f / fps;
            while (elapsed >= frameDuration)
            {
                elapsed -= frameDuration;
                currentFrame++;
                if (currentFrame >= frameCount)
                {
                    if (loop)
                    {
                        currentFrame = 0;
                    }
                    else
                    {
                        currentFrame = frameCount - 1;
                        finished = true;
                        break;
                    }
                }
            }
        }

        DirectX::XMFLOAT2 GetUVOffset() const
        {
            uint32_t fx = currentFrame % framesX;
            uint32_t fy = currentFrame / framesX;
            return { fx / (float)framesX, fy / (float)framesY };
        }

        DirectX::XMFLOAT2 GetUVScale() const
        {
            return { 1.0f / framesX, 1.0f / framesY };
        }
    };
};