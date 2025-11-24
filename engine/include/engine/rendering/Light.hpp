#pragma once

#include <nlohmann/json.hpp>

#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Rendering
{
    enum class LightType
    {
        Directional, // Sun-like, infinite distance, parallel rays
        Point, // Light bulb, radiates in all directions
        Spot // Flashlight, cone of light
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(LightType, {
                                 { LightType::Directional, "Directional" },
                                 { LightType::Point, "Point" },
                                 { LightType::Spot, "Spot" }
                                 })

    class Light : public SerializableComponent
    {
    public:
        explicit Light(GameObject& gameObject);
        [[nodiscard]] std::string GetTypeName() const override;

        [[nodiscard]] Math::Vector3 GetWorldDirection() const;
        [[nodiscard]] Math::Vector3 GetWorldPosition() const;

    public:
        LightType type = LightType::Directional;
        Math::Vector3 color = {1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;

        // Directional light properties
        Math::Vector3 direction = {0.0f, -1.0f, 0.0f}; // Points downward

        // Point light properties
        float range = 10.0f; // Maximum distance light reaches
        float attenuation = 1.0f; // Falloff rate (higher = faster falloff)

        // Spot light properties
        float innerConeAngle = 30.0f; // Inner cone (full brightness) in degrees
        float outerConeAngle = 45.0f; // Outer cone (fades to zero) in degrees
    };
}
