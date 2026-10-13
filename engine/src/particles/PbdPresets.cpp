#include "engine/particles/PbdDriver.hpp"

#include <algorithm>
#include <cmath>

namespace N2Engine::Particles
{
    PbdPreset GetPbdPreset(const Physics::ParticleMaterial material)
    {
        PbdPreset preset;
        switch (material)
        {
        case Physics::ParticleMaterial::Water:
            // A fluid, after SnippetPBF: low friction and viscosity, some surface tension and cohesion so it pools
            preset.material = {.friction = 0.05f, .damping = 0.05f, .adhesion = 0.0f, .viscosity = 0.001f,
                               .vorticityConfinement = 0.5f, .surfaceTension = 0.005f, .cohesion = 0.01f,
                               .lift = 0.0f, .drag = 0.0f, .cflCoefficient = 1.0f, .gravityScale = 1.0f};
            preset.particleSpacing = 0.1f;
            preset.density = 1000.0f;
            preset.fluid = true;
            break;
        case Physics::ParticleMaterial::Sand:
            // Granular: no fluid constraints, high friction, no cohesion, so it piles into a heap
            preset.material = {.friction = 0.6f, .damping = 0.1f, .adhesion = 0.0f, .viscosity = 0.0f,
                               .vorticityConfinement = 0.0f, .surfaceTension = 0.0f, .cohesion = 0.0f,
                               .lift = 0.0f, .drag = 0.0f, .cflCoefficient = 1.0f, .gravityScale = 1.0f};
            preset.particleSpacing = 0.08f;
            preset.density = 1600.0f;
            preset.fluid = false;
            break;
        case Physics::ParticleMaterial::Smoke:
            // Light: heavy damping so it drifts, a little viscosity, no cohesion
            preset.material = {.friction = 0.0f, .damping = 0.8f, .adhesion = 0.0f, .viscosity = 0.01f,
                               .vorticityConfinement = 0.2f, .surfaceTension = 0.0f, .cohesion = 0.0f,
                               .lift = 0.0f, .drag = 0.0f, .cflCoefficient = 1.0f, .gravityScale = 1.0f};
            preset.particleSpacing = 0.2f;
            preset.density = 100.0f;
            preset.fluid = true;
            break;
        }
        return preset;
    }

    PbdMaterialParams ComposePbdMaterial(const PbdPreset &preset, const Math::Vector3 &gravity,
                                         const Math::Vector3 &sceneGravity, const float drag)
    {
        PbdMaterialParams material = preset.material;

        // PBD particles fall with the scene's gravity scaled by the material, so only the part of the wanted
        // gravity along the scene's can be made: its ratio to the scene's, never negative
        const float sceneLengthSq = sceneGravity.x * sceneGravity.x + sceneGravity.y * sceneGravity.y +
                                    sceneGravity.z * sceneGravity.z;
        float ratio = 0.0f;
        if (sceneLengthSq > 1e-12f)
        {
            ratio = (gravity.x * sceneGravity.x + gravity.y * sceneGravity.y + gravity.z * sceneGravity.z) /
                    sceneLengthSq;
        }
        if (!std::isfinite(ratio))
        {
            ratio = 0.0f;
        }
        material.gravityScale = std::max(0.0f, ratio) * preset.material.gravityScale;

        const float extraDamping = std::isfinite(drag) ? std::max(0.0f, drag) : 0.0f;
        material.damping = preset.material.damping + extraDamping;
        return material;
    }
}
