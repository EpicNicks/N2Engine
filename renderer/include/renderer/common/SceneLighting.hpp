#pragma once

#include <cstdint>
#include <math/Vector3.hpp>
#include <vector>

namespace Renderer::Common
{
    /**
     * How the lit shaders work with colour. Gamma (the default) is what the engine always did: colours and
     * lighting are used as they are and the result is written as it is. Linear decodes sRGB textures to linear
     * light, lights in linear light and encodes the result to sRGB for display. Only the standard lit shader
     * differs; unlit and text draw the same in both.
     */
    enum class ColorSpace : std::uint8_t
    {
        Gamma,
        Linear
    };

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
        /// Set from the project's rendering settings by Scene::CollectLighting; read by the lit shaders
        ColorSpace colorSpace = ColorSpace::Gamma;
        /// Linear lighting on OpenGL: have the lit shader encode its own output even when the target says it is
        /// sRGB (an escape hatch for a driver that reports the wrong colour encoding). The software renderer
        /// ignores it.
        bool forceShaderEncode = false;

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