#pragma once

#include <math/Vector3.hpp>
#include <vector>

namespace Renderer::Common
{
    struct DirectionalLightData
    {
        N2Engine::Math::Vector3 direction;
        N2Engine::Math::Vector3 color;
        float intensity;

        DirectionalLightData()
            : direction(0.0f, -1.0f, 0.0f)
            , color(1.0f, 1.0f, 1.0f)
            , intensity(1.0f)
        {}
    };

    struct PointLightData
    {
        N2Engine::Math::Vector3 position;
        N2Engine::Math::Vector3 color;
        float intensity;
        float range;
        float attenuation;

        PointLightData()
            : position(0.0f, 0.0f, 0.0f)
            , color(1.0f, 1.0f, 1.0f)
            , intensity(1.0f)
            , range(10.0f)
            , attenuation(1.0f)
        {}
    };

    struct SpotLightData
    {
        N2Engine::Math::Vector3 position;
        N2Engine::Math::Vector3 direction;
        N2Engine::Math::Vector3 color;
        float intensity;
        float range;
        float innerConeAngle;  // In radians
        float outerConeAngle;  // In radians

        SpotLightData()
            : position(0.0f, 0.0f, 0.0f)
            , direction(0.0f, -1.0f, 0.0f)
            , color(1.0f, 1.0f, 1.0f)
            , intensity(1.0f)
            , range(10.0f)
            , innerConeAngle(0.523599f)  // 30 degrees
            , outerConeAngle(0.785398f)  // 45 degrees
        {}
    };

    struct SceneLightingData
    {
        N2Engine::Math::Vector3 ambientColor;

        std::vector<DirectionalLightData> directionalLights;
        std::vector<PointLightData> pointLights;
        std::vector<SpotLightData> spotLights;

        static constexpr int MAX_DIRECTIONAL_LIGHTS = 2;
        static constexpr int MAX_POINT_LIGHTS = 8;
        static constexpr int MAX_SPOT_LIGHTS = 4;

        SceneLightingData()
            : ambientColor(0.2f, 0.2f, 0.2f)
        {}
    };
}