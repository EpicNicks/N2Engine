#include "engine/rendering/Light.hpp"

#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Rendering
{
    REGISTER_COMPONENT(Light)

    Light::Light(GameObject& gameObject)
        : SerializableComponent(gameObject)
    {
        RegisterMember(NAMEOF(type), type);
        RegisterMember(NAMEOF(color), color);
        RegisterMember(NAMEOF(intensity), intensity);

        RegisterMember(NAMEOF(direction), direction);

        RegisterMember(NAMEOF(attenuation), attenuation);
        RegisterMember(NAMEOF(range), range);

        RegisterMember(NAMEOF(innerConeAngle), innerConeAngle);
        RegisterMember(NAMEOF(outerConeAngle), outerConeAngle);
    }

    std::string Light::GetTypeName() const
    {
        return NAMEOF(Light);
    }
}
