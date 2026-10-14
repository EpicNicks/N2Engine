#pragma once

#include "engine/serialization/FieldInfo.hpp" // N2_SERIALIZE_ENUM

namespace N2Engine::Physics
{
    /**
     * What a particle system is made of, for the PhysX GPU backend: each preset is a set of PBD material
     * parameters (see Particles::GetPbdPreset). The CPU backend ignores it (its particles are all the same
     * ballistic sprites).
     */
    enum class ParticleMaterial
    {
        /// A fluid: viscosity, surface tension and cohesion; particles flow, pool and splash
        Water,
        /// Granular: friction and no cohesion; particles pile up
        Sand,
        /// Light and drifting: strong damping, no cohesion
        Smoke
    };

    N2_SERIALIZE_ENUM(ParticleMaterial, {
                      { ParticleMaterial::Water, "Water" },
                      { ParticleMaterial::Sand, "Sand" },
                      { ParticleMaterial::Smoke, "Smoke" }
                      })
}
