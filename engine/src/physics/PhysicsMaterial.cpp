#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/common/ScriptUtils.hpp"

namespace N2Engine::Physics
{
    void to_json(nlohmann::json &j, const PhysicsMaterial &physicsMaterial)
    {
        j = nlohmann::json{
            {NAMEOF_FIELD(physicsMaterial.staticFriction), physicsMaterial.staticFriction},
            {NAMEOF_FIELD(physicsMaterial.dynamicFriction), physicsMaterial.dynamicFriction},
            {NAMEOF_FIELD(physicsMaterial.restitution), physicsMaterial.restitution}
        };
    }

    void from_json(const nlohmann::json &j, PhysicsMaterial &physicsMaterial)
    {
        physicsMaterial.staticFriction = j.value("staticFriction", 0.0f);
        physicsMaterial.dynamicFriction = j.value("dynamicFriction", 0.0f);
        physicsMaterial.restitution = j.value("restitution", 0.0f);
    }
}
