#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <math/Vector3.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/vulkan/VulkanLighting.hpp>
#include <renderer/vulkan/VulkanShaderInterface.hpp>

// The lit shader's 752-byte lighting block as UpdateSceneLighting fills it (#43, stage C), checked without a device:
// BuildLightingBlock is a pure function of a SceneLightingData and the camera position. The expectations are what
// OpenGLRenderer::UpdateSceneLighting sets on the lit program (VulkanShaderParityTests.cpp checks that every name GL sets
// has an entry in the tables the block is filled from, and the tables against the GLSL).

using Renderer::Common::ColorSpace;
using Renderer::Common::DirectionalLightData;
using Renderer::Common::PointLightData;
using Renderer::Common::SceneLightingData;
using Renderer::Common::SpotLightData;
using Renderer::Vulkan::BuildLightingBlock;
using Renderer::Vulkan::LightingBlock;
using N2Engine::Math::Vector3;
namespace Vk = Renderer::Vulkan::ShaderInterface;

namespace
{
    float FloatAt(const LightingBlock& block, const std::uint32_t offset)
    {
        float value = 0.0f;
        std::memcpy(&value, block.data() + offset, sizeof(value));
        return value;
    }

    int IntAt(const LightingBlock& block, const std::uint32_t offset)
    {
        std::int32_t value = 0;
        std::memcpy(&value, block.data() + offset, sizeof(value));
        return static_cast<int>(value);
    }

    Vector3 Vec3At(const LightingBlock& block, const std::uint32_t offset)
    {
        return Vector3(FloatAt(block, offset), FloatAt(block, offset + 4), FloatAt(block, offset + 8));
    }

    std::uint32_t Loose(const char* name)
    {
        for (const Vk::Uniform& uniform : Vk::kLitLighting)
        {
            if (uniform.name == name)
            {
                return uniform.offset;
            }
        }
        ADD_FAILURE() << "no lighting uniform " << name;
        return 0;
    }

    SpotLightData Spot(const Vector3& direction)
    {
        SpotLightData spot;
        spot.position = Vector3(1.0f, 2.0f, 3.0f);
        spot.direction = direction;
        spot.color = Vector3(0.25f, 0.5f, 0.75f);
        spot.intensity = 2.0f;
        spot.range = 7.0f;
        spot.innerConeAngle = 0.3f;
        spot.outerConeAngle = 0.6f;
        return spot;
    }
}

TEST(VulkanLightingBlockTest, TheBlockIsTheStd140SizeAndASceneWithNothingFillsItWithTheDefaults)
{
    static_assert(sizeof(LightingBlock) == 752);
    const SceneLightingData defaults;
    const LightingBlock block = BuildLightingBlock(defaults, Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(IntAt(block, Loose("uEncodeOutput")), 0) << "gamma lighting does not encode";
    EXPECT_EQ(IntAt(block, Loose("uNumDirectionalLights")), 0);
    EXPECT_EQ(IntAt(block, Loose("uNumPointLights")), 0);
    EXPECT_EQ(IntAt(block, Loose("uNumSpotLights")), 0);
    const Vector3 ambient = Vec3At(block, Loose("uAmbientLight"));
    EXPECT_FLOAT_EQ(ambient.x, 0.2f);
    EXPECT_FLOAT_EQ(ambient.y, 0.2f);
    EXPECT_FLOAT_EQ(ambient.z, 0.2f);
    const Vector3 camera = Vec3At(block, Loose("uCameraPos"));
    EXPECT_FLOAT_EQ(camera.x, 0.0f);
    EXPECT_FLOAT_EQ(camera.y, 0.0f);
    EXPECT_FLOAT_EQ(camera.z, 0.0f);
    // The light arrays (everything from the first one on) are zero
    for (std::uint32_t i = Loose("uDirectionalLights"); i < Vk::kLightingBlockSize; ++i)
    {
        EXPECT_EQ(block[i], 0) << "byte " << i;
    }
}

TEST(VulkanLightingBlockTest, TheCameraTheAmbientColourAndTheColourSpaceAreWrittenAtTheirOffsets)
{
    SceneLightingData lighting;
    lighting.ambientColor = Vector3(0.1f, 0.2f, 0.3f);
    lighting.colorSpace = ColorSpace::Linear;
    const LightingBlock block = BuildLightingBlock(lighting, Vector3(4.0f, 5.0f, 6.0f));
    const Vector3 camera = Vec3At(block, Loose("uCameraPos"));
    EXPECT_FLOAT_EQ(camera.x, 4.0f);
    EXPECT_FLOAT_EQ(camera.y, 5.0f);
    EXPECT_FLOAT_EQ(camera.z, 6.0f);
    const Vector3 ambient = Vec3At(block, Loose("uAmbientLight"));
    EXPECT_FLOAT_EQ(ambient.x, 0.1f);
    EXPECT_FLOAT_EQ(ambient.y, 0.2f);
    EXPECT_FLOAT_EQ(ambient.z, 0.3f);
    EXPECT_EQ(IntAt(block, Loose("uEncodeOutput")), 1) << "linear lighting encodes in the shader";
}

TEST(VulkanLightingBlockTest, ForceShaderEncodeChangesNothingBecauseTheTargetIsAlwaysUnorm)
{
    for (const ColorSpace space : {ColorSpace::Gamma, ColorSpace::Linear})
    {
        SceneLightingData plain;
        plain.colorSpace = space;
        SceneLightingData forced = plain;
        forced.forceShaderEncode = true;
        EXPECT_EQ(BuildLightingBlock(plain, Vector3(1.0f, 2.0f, 3.0f)), BuildLightingBlock(forced, Vector3(1.0f, 2.0f, 3.0f)));
    }
}

TEST(VulkanLightingBlockTest, DirectionalLightsAreWrittenAtTheStructStrideAndClampedToTwo)
{
    SceneLightingData lighting;
    for (int i = 0; i < 5; ++i)
    {
        DirectionalLightData light;
        light.direction = Vector3(static_cast<float>(i), -1.0f, 0.5f);
        light.color = Vector3(0.1f * static_cast<float>(i + 1), 0.5f, 0.25f);
        light.intensity = 1.5f + static_cast<float>(i);
        lighting.directionalLights.push_back(light);
    }
    const LightingBlock block = BuildLightingBlock(lighting, Vector3());
    EXPECT_EQ(IntAt(block, Loose("uNumDirectionalLights")), 2);
    const std::uint32_t array = Loose("uDirectionalLights");
    for (std::uint32_t i = 0; i < 2; ++i)
    {
        const std::uint32_t base = array + i * Vk::kDirectionalLightStride;
        const Vector3 direction = Vec3At(block, base + Vk::MemberOffset(Vk::kDirectionalLightMembers, "direction"));
        EXPECT_FLOAT_EQ(direction.x, static_cast<float>(i));
        EXPECT_FLOAT_EQ(direction.y, -1.0f);
        EXPECT_FLOAT_EQ(direction.z, 0.5f);
        const Vector3 color = Vec3At(block, base + Vk::MemberOffset(Vk::kDirectionalLightMembers, "color"));
        EXPECT_FLOAT_EQ(color.x, 0.1f * static_cast<float>(i + 1));
        EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kDirectionalLightMembers, "intensity")),
                        1.5f + static_cast<float>(i));
    }
    // Nothing of the third and later lights, and the arrays after this one are untouched
    for (std::uint32_t i = array + 2 * Vk::kDirectionalLightStride; i < Vk::kLightingBlockSize; ++i)
    {
        EXPECT_EQ(block[i], 0) << "byte " << i;
    }
}

TEST(VulkanLightingBlockTest, PointLightsAreWrittenAtTheStructStrideAndClampedToEight)
{
    SceneLightingData lighting;
    for (int i = 0; i < 11; ++i)
    {
        PointLightData light;
        light.position = Vector3(static_cast<float>(i), 1.0f, 2.0f);
        light.color = Vector3(0.5f, 0.25f, 0.125f);
        light.intensity = 3.0f;
        light.range = 4.0f + static_cast<float>(i);
        light.attenuation = 0.5f;
        lighting.pointLights.push_back(light);
    }
    const LightingBlock block = BuildLightingBlock(lighting, Vector3());
    EXPECT_EQ(IntAt(block, Loose("uNumPointLights")), 8);
    const std::uint32_t array = Loose("uPointLights");
    for (std::uint32_t i = 0; i < 8; ++i)
    {
        const std::uint32_t base = array + i * Vk::kPointLightStride;
        EXPECT_FLOAT_EQ(Vec3At(block, base + Vk::MemberOffset(Vk::kPointLightMembers, "position")).x, static_cast<float>(i));
        EXPECT_FLOAT_EQ(Vec3At(block, base + Vk::MemberOffset(Vk::kPointLightMembers, "color")).z, 0.125f);
        EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kPointLightMembers, "intensity")), 3.0f);
        EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kPointLightMembers, "range")), 4.0f + static_cast<float>(i));
        EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kPointLightMembers, "attenuation")), 0.5f);
    }
}

TEST(VulkanLightingBlockTest, SpotLightsGetANormalisedAxisAndTheConeCosinesAsOpenGlComputesThem)
{
    SceneLightingData lighting;
    lighting.spotLights.push_back(Spot(Vector3(0.0f, 0.0f, -7.0f)));
    const LightingBlock block = BuildLightingBlock(lighting, Vector3());
    EXPECT_EQ(IntAt(block, Loose("uNumSpotLights")), 1);
    const std::uint32_t base = Loose("uSpotLights");
    const Vector3 direction = Vec3At(block, base + Vk::MemberOffset(Vk::kSpotLightMembers, "direction"));
    EXPECT_FLOAT_EQ(direction.x, 0.0f);
    EXPECT_FLOAT_EQ(direction.y, 0.0f);
    EXPECT_FLOAT_EQ(direction.z, -1.0f) << "a non-unit axis is normalised";
    EXPECT_FLOAT_EQ(Vec3At(block, base + Vk::MemberOffset(Vk::kSpotLightMembers, "position")).y, 2.0f);
    EXPECT_FLOAT_EQ(Vec3At(block, base + Vk::MemberOffset(Vk::kSpotLightMembers, "color")).z, 0.75f);
    EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kSpotLightMembers, "intensity")), 2.0f);
    EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kSpotLightMembers, "range")), 7.0f);
    EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kSpotLightMembers, "cosInner")), std::cos(0.3f));
    EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kSpotLightMembers, "cosOuter")), std::cos(0.6f));
}

TEST(VulkanLightingBlockTest, AnInnerAngleAtOrPastTheOuterOneIsWrittenAsItIsLikeOpenGl)
{
    SceneLightingData lighting;
    SpotLightData spot = Spot(Vector3(0.0f, -1.0f, 0.0f));
    spot.innerConeAngle = 0.9f; // past the outer one
    spot.outerConeAngle = 0.5f;
    lighting.spotLights.push_back(spot);
    const LightingBlock block = BuildLightingBlock(lighting, Vector3());
    const std::uint32_t base = Loose("uSpotLights");
    EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kSpotLightMembers, "cosInner")), std::cos(0.9f));
    EXPECT_FLOAT_EQ(FloatAt(block, base + Vk::MemberOffset(Vk::kSpotLightMembers, "cosOuter")), std::cos(0.5f));
}

TEST(VulkanLightingBlockTest, OnlyTheFirstFourSpotLightsAreConsideredAndOnesWithoutAnAxisAreLeftOutAndTheRestPackedDown)
{
    SceneLightingData lighting;
    lighting.spotLights.push_back(Spot(Vector3(0.0f, 0.0f, 0.0f)));  // no axis: skipped
    lighting.spotLights.push_back(Spot(Vector3(1.0f, 0.0f, 0.0f)));  // becomes slot 0
    lighting.spotLights.push_back(Spot(Vector3(0.0f, 0.0f, 0.0f)));  // skipped
    lighting.spotLights.push_back(Spot(Vector3(0.0f, 2.0f, 0.0f)));  // slot 1
    lighting.spotLights.push_back(Spot(Vector3(0.0f, 0.0f, 3.0f)));  // the fifth: past the cap of four candidates
    const LightingBlock block = BuildLightingBlock(lighting, Vector3());
    EXPECT_EQ(IntAt(block, Loose("uNumSpotLights")), 2);
    const std::uint32_t array = Loose("uSpotLights");
    const std::uint32_t directionOffset = Vk::MemberOffset(Vk::kSpotLightMembers, "direction");
    EXPECT_FLOAT_EQ(Vec3At(block, array + 0 * Vk::kSpotLightStride + directionOffset).x, 1.0f);
    EXPECT_FLOAT_EQ(Vec3At(block, array + 1 * Vk::kSpotLightStride + directionOffset).y, 1.0f) << "normalised (0, 2, 0)";
    for (std::uint32_t i = array + 2 * Vk::kSpotLightStride; i < Vk::kLightingBlockSize; ++i)
    {
        EXPECT_EQ(block[i], 0) << "byte " << i << ": the fifth light never reaches the block";
    }

    // Seven spot lights: four considered, as on OpenGL
    SceneLightingData many;
    for (int i = 0; i < 7; ++i)
    {
        many.spotLights.push_back(Spot(Vector3(0.0f, 0.0f, -1.0f)));
    }
    EXPECT_EQ(IntAt(BuildLightingBlock(many, Vector3()), Loose("uNumSpotLights")), 4);
}

TEST(VulkanLightingBlockTest, EveryFilledLightSlotStaysInsideItsOwnArray)
{
    // Maximal lighting: every array full. The block is exactly the table's size and no array writes past its end
    SceneLightingData lighting;
    for (int i = 0; i < 2; ++i) lighting.directionalLights.emplace_back();
    for (int i = 0; i < 8; ++i) lighting.pointLights.emplace_back();
    for (int i = 0; i < 4; ++i) lighting.spotLights.push_back(Spot(Vector3(0.0f, -1.0f, 0.0f)));
    const LightingBlock block = BuildLightingBlock(lighting, Vector3(1.0f, 1.0f, 1.0f));
    EXPECT_EQ(IntAt(block, Loose("uNumDirectionalLights")), 2);
    EXPECT_EQ(IntAt(block, Loose("uNumPointLights")), 8);
    EXPECT_EQ(IntAt(block, Loose("uNumSpotLights")), 4);
    // The padding bytes of a std140 struct (after the last member of an element) stay zero
    const std::uint32_t spotArray = Loose("uSpotLights");
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        const std::uint32_t element = spotArray + i * Vk::kSpotLightStride;
        for (std::uint32_t byte = Vk::MemberOffset(Vk::kSpotLightMembers, "cosOuter") + 4; byte < Vk::kSpotLightStride; ++byte)
        {
            EXPECT_EQ(block[element + byte], 0) << "spot " << i << " byte " << byte;
        }
    }
}
