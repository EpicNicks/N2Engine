#include "engine/rendering/Light.hpp"

#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/MathSerialization.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"

namespace N2Engine::Rendering
{
    REGISTER_COMPONENT(Light)

    Light::Light(GameObject &gameObject)
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

    Math::Vector3 Light::GetWorldDirection() const
    {
        if (type == LightType::Directional)
        {
            // Directional lights use their direction property directly
            return direction.Normalized();
        }
        if (type == LightType::Spot)
        {
            // Spot lights use their GameObject's forward direction
            if (auto positionable = _gameObject.GetPositionable())
            {
                // Get forward vector from rotation
                // Assuming -Z is forward
                const Math::Vector3 forward(0.0f, 0.0f, -1.0f);
                // TODO: Transform by GameObject's rotation when quaternion->vector conversion is available
                return forward.Normalized();
            }
        }
        return direction.Normalized();
    }

    Math::Vector3 Light::GetWorldPosition() const
    {
        if (!_gameObject.GetPositionable())
        {
            _gameObject.CreatePositionable();
        }
        return _gameObject.GetPositionable()->GetPosition();
    }
}
