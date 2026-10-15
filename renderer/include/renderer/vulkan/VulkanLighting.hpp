#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <math/Vector3.hpp>

#include "renderer/common/SceneLighting.hpp"
#include "renderer/vulkan/VulkanShaderInterface.hpp"

// The lit shader's lighting block (#43, stage C): the 752-byte std140 block that UpdateSceneLighting fills from a
// SceneLightingData, as a pure function with no Vulkan in it (so a test needs no device). Every offset comes from the
// tables of VulkanShaderInterface.hpp at compile time (LightingOffset, MemberOffset: a name that is not in a table does
// not compile), so there is no second hand-typed copy; VulkanShaderParityTests.cpp checks the tables against lit.frag.
//
// It mirrors OpenGLRenderer::UpdateSceneLighting, which sets the same values on the lit program by name:
//   uEncodeOutput           1 for linear lighting, else 0. OpenGL encodes in the shader only when its target is not sRGB (or
//                           forceShaderEncode is set); Vulkan's colour target is always UNORM, so linear lighting ALWAYS
//                           encodes in the shader and forceShaderEncode changes nothing
//   uAmbientLight           the ambient colour
//   uCameraPos              the camera position
//   uNumDirectionalLights   min(count, 2), and the directional lights: direction, color, intensity
//   uNumPointLights         min(count, 8), and the point lights: position, color, intensity, range, attenuation
//   uNumSpotLights          the first min(count, 4) spot lights whose direction is not (nearly) zero, in order, packed from
//                           the start (a spot light with no axis lights nothing and is left out: normalising it would give
//                           NaN); direction is normalised; cosInner / cosOuter are cos(innerConeAngle) / cos(outerConeAngle),
//                           with no special case for an inner angle at or past the outer one (as OpenGL; the shader's
//                           smoothstep decides what that means)
// Everything else in the block (the unused light slots) is zero.

namespace Renderer::Vulkan
{
    using LightingBlock = std::array<std::uint8_t, ShaderInterface::kLightingBlockSize>;

    namespace Detail
    {
        inline void PutFloat(LightingBlock& block, const std::uint32_t offset, const float value)
        {
            std::memcpy(block.data() + offset, &value, sizeof(value));
        }

        inline void PutInt(LightingBlock& block, const std::uint32_t offset, const int value)
        {
            const std::int32_t stored = value;
            std::memcpy(block.data() + offset, &stored, sizeof(stored));
        }

        inline void PutVec3(LightingBlock& block, const std::uint32_t offset, const float x, const float y, const float z)
        {
            PutFloat(block, offset, x);
            PutFloat(block, offset + 4, y);
            PutFloat(block, offset + 8, z);
        }
    }

    /// The lighting block for `lighting` seen from `cameraPosition` (see the header comment)
    [[nodiscard]] inline LightingBlock BuildLightingBlock(const Common::SceneLightingData& lighting,
                                                          const N2Engine::Math::Vector3& cameraPosition)
    {
        using namespace ShaderInterface;
        using namespace Detail;

        constexpr std::uint32_t cameraPos = LightingOffset("uCameraPos");
        constexpr std::uint32_t encodeOutput = LightingOffset("uEncodeOutput");
        constexpr std::uint32_t ambient = LightingOffset("uAmbientLight");
        constexpr std::uint32_t numDirectional = LightingOffset("uNumDirectionalLights");
        constexpr std::uint32_t numPoint = LightingOffset("uNumPointLights");
        constexpr std::uint32_t numSpot = LightingOffset("uNumSpotLights");
        constexpr std::uint32_t directionalArray = LightingOffset("uDirectionalLights");
        constexpr std::uint32_t pointArray = LightingOffset("uPointLights");
        constexpr std::uint32_t spotArray = LightingOffset("uSpotLights");

        // The caps are the shader's array sizes (the table), and SceneLightingData's own maxima agree with them
        static_assert(Common::SceneLightingData::MAX_DIRECTIONAL_LIGHTS == static_cast<int>(kDirectionalLightCount));
        static_assert(Common::SceneLightingData::MAX_POINT_LIGHTS == static_cast<int>(kPointLightCount));
        static_assert(Common::SceneLightingData::MAX_SPOT_LIGHTS == static_cast<int>(kSpotLightCount));

        LightingBlock block{};

        PutVec3(block, cameraPos, cameraPosition.x, cameraPosition.y, cameraPosition.z);
        PutInt(block, encodeOutput, lighting.colorSpace == Common::ColorSpace::Linear ? 1 : 0);
        PutVec3(block, ambient, lighting.ambientColor.x, lighting.ambientColor.y, lighting.ambientColor.z);

        const int directionalCount = std::min<int>(static_cast<int>(lighting.directionalLights.size()),
                                                   Common::SceneLightingData::MAX_DIRECTIONAL_LIGHTS);
        PutInt(block, numDirectional, directionalCount);
        for (int i = 0; i < directionalCount; ++i)
        {
            const Common::DirectionalLightData& light = lighting.directionalLights[static_cast<std::size_t>(i)];
            const std::uint32_t base = directionalArray + static_cast<std::uint32_t>(i) * kDirectionalLightStride;
            PutVec3(block, base + MemberOffset(kDirectionalLightMembers, "direction"), light.direction.x, light.direction.y,
                    light.direction.z);
            PutVec3(block, base + MemberOffset(kDirectionalLightMembers, "color"), light.color.x, light.color.y, light.color.z);
            PutFloat(block, base + MemberOffset(kDirectionalLightMembers, "intensity"), light.intensity);
        }

        const int pointCount = std::min<int>(static_cast<int>(lighting.pointLights.size()),
                                             Common::SceneLightingData::MAX_POINT_LIGHTS);
        PutInt(block, numPoint, pointCount);
        for (int i = 0; i < pointCount; ++i)
        {
            const Common::PointLightData& light = lighting.pointLights[static_cast<std::size_t>(i)];
            const std::uint32_t base = pointArray + static_cast<std::uint32_t>(i) * kPointLightStride;
            PutVec3(block, base + MemberOffset(kPointLightMembers, "position"), light.position.x, light.position.y,
                    light.position.z);
            PutVec3(block, base + MemberOffset(kPointLightMembers, "color"), light.color.x, light.color.y, light.color.z);
            PutFloat(block, base + MemberOffset(kPointLightMembers, "intensity"), light.intensity);
            PutFloat(block, base + MemberOffset(kPointLightMembers, "range"), light.range);
            PutFloat(block, base + MemberOffset(kPointLightMembers, "attenuation"), light.attenuation);
        }

        const int spotCandidates = std::min<int>(static_cast<int>(lighting.spotLights.size()),
                                                 Common::SceneLightingData::MAX_SPOT_LIGHTS);
        int spotCount = 0;
        for (int candidate = 0; candidate < spotCandidates; ++candidate)
        {
            const Common::SpotLightData& light = lighting.spotLights[static_cast<std::size_t>(candidate)];
            const float length = std::sqrt(light.direction.x * light.direction.x + light.direction.y * light.direction.y +
                                           light.direction.z * light.direction.z);
            if (!(length > 1e-6f))
            {
                continue;
            }
            const std::uint32_t base = spotArray + static_cast<std::uint32_t>(spotCount++) * kSpotLightStride;
            PutVec3(block, base + MemberOffset(kSpotLightMembers, "position"), light.position.x, light.position.y,
                    light.position.z);
            PutVec3(block, base + MemberOffset(kSpotLightMembers, "direction"), light.direction.x / length,
                    light.direction.y / length, light.direction.z / length);
            PutVec3(block, base + MemberOffset(kSpotLightMembers, "color"), light.color.x, light.color.y, light.color.z);
            PutFloat(block, base + MemberOffset(kSpotLightMembers, "intensity"), light.intensity);
            PutFloat(block, base + MemberOffset(kSpotLightMembers, "range"), light.range);
            PutFloat(block, base + MemberOffset(kSpotLightMembers, "cosInner"), std::cos(light.innerConeAngle));
            PutFloat(block, base + MemberOffset(kSpotLightMembers, "cosOuter"), std::cos(light.outerConeAngle));
        }
        PutInt(block, numSpot, spotCount);

        return block;
    }
}
