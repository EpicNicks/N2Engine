#pragma once

#include "engine/serialization/FieldInfo.hpp" // N2_SERIALIZE_ENUM

namespace N2Engine::Physics
{
    /// The material model of the PhysX soft-body backend (the CPU backend ignores it). An engine enum: no PhysX type
    /// appears here, the real driver maps it to PxDeformableVolumeMaterialModel.
    enum class SoftMaterialModel
    {
        /// PhysX's default: fast, well suited for high stiffness, needs well-shaped tetrahedra
        CoRotational,
        /// Robust to bad tetrahedra, well suited for lower stiffness, slower
        NeoHookean
    };

    N2_SERIALIZE_ENUM(SoftMaterialModel, {
                      { SoftMaterialModel::CoRotational, "CoRotational" },
                      { SoftMaterialModel::NeoHookean, "NeoHookean" }
                      })
}
