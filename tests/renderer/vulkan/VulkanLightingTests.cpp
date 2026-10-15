#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/LightingMath.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/vulkan/VulkanLighting.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>
#include <renderer/vulkan/VulkanShaderInterface.hpp>

#include "VulkanLitTestUtil.hpp"

// Lit shading on the Vulkan backend (#43, stage C): the RENDERED tests of tests/renderer/software/SoftwareLightingTests.cpp
// with the same scenes and structural expectations (spot lights, emissive, occlusion, colour space), each draw also made on
// a headless SoftwareRenderer and compared with the lit criterion (VulkanLitTestUtil.hpp). The LightingMath cases of the
// software file test CPU helpers and are not mirrored. Then the checks that only Vulkan has: UpdateSceneLighting called
// twice in a frame (a draw sees the last update before it), lighting that changes every frame over more than the two
// frames in flight, more lights than the maxima, forceShaderEncode (which changes nothing here), and the defaults and
// aux-texture fallbacks of the lit material. Every test runs with validation on (zero errors and warnings, no leaks).

using namespace VulkanTesting::Lit;
using Renderer::Common::ColorSpace;
using Renderer::Common::PointLightData;
using Renderer::Common::SpotLightData;
namespace Vk = Renderer::Vulkan::ShaderInterface;
namespace Diff = VulkanTesting::Diff;

namespace
{
    constexpr int kCentre = Size / 2;

    /// A spot light one unit in front of the quad, shining at it along -z
    SpotLightData SpotAtTheQuad(const float innerDegrees, const float outerDegrees, const float intensity = 0.5f)
    {
        SpotLightData spot;
        spot.position = Vector3(0.0f, 0.0f, 1.0f);
        spot.direction = Vector3(0.0f, 0.0f, -1.0f);
        spot.intensity = intensity;
        spot.range = 10.0f;
        spot.innerConeAngle = Degrees(innerDegrees);
        spot.outerConeAngle = Degrees(outerDegrees);
        return spot;
    }

    SceneLightingData WithSpot(const SpotLightData& spot)
    {
        SceneLightingData lighting = AmbientOnly(0.0f);
        lighting.spotLights.push_back(spot);
        return lighting;
    }

    /// The software tests' scene: a lit white quad with smoothness 0
    class VulkanLightingBase : public VulkanLitTest
    {
    protected:
        void SetUp() override
        {
            VulkanLitTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            S().lit.SetFloat("uSmoothness", 0.0f);
        }
    };
}

class VulkanSpotLightTest : public VulkanLightingBase {};
class VulkanEmissiveTest : public VulkanLightingBase {};
class VulkanOcclusionTest : public VulkanLightingBase {};
class VulkanColorSpaceTest : public VulkanLightingBase {};
class VulkanLightingUpdateTest : public VulkanLightingBase {};
class VulkanLitMaterialTest : public VulkanLightingBase {};

// ============================================================================
// Spot lights
// ============================================================================

TEST_F(VulkanSpotLightTest, LightsInsideTheConeAndNothingOutsideIt)
{
    // 20 and 40 degrees at one unit: the cone reaches x = tan(40) = 0.84 on the quad, full strength to 0.36
    const Frame frame = S().Draw(WithSpot(SpotAtTheQuad(20.0f, 40.0f)), S().lit);
    const Rgb centre = frame.At(32, 32);
    EXPECT_GT(centre.r, 60) << "lit on the axis";
    EXPECT_EQ(centre.r, centre.g);

    // Past the outer cone (x = 0.9 and beyond): no ambient, so nothing at all
    for (int px = 32 + 29; px < Size; ++px)
    {
        ASSERT_GT(WorldX(px), 0.88f);
        const Rgb outside = frame.At(px, 32);
        EXPECT_EQ(outside.r, 0) << "column " << px;
        EXPECT_EQ(outside.g, 0);
        EXPECT_EQ(outside.b, 0);
    }
    // ...and on the other side and in the corner
    EXPECT_EQ(frame.At(2, 32).r, 0);
    EXPECT_EQ(frame.At(2, 2).r, 0);
    EXPECT_EQ(frame.At(61, 61).r, 0);
}

TEST_F(VulkanSpotLightTest, TheLightFallsOffBetweenTheInnerAndOuterCone)
{
    const Frame soft = S().Draw(WithSpot(SpotAtTheQuad(20.0f, 40.0f)), S().lit);
    // The same light with a hard edge at 40 degrees: full strength all the way out, so its pixel is the unscaled one
    const Frame hard = S().Draw(WithSpot(SpotAtTheQuad(40.0f, 40.0f)), S().lit);

    // Outward along the row the soft light never brightens (the cone, distance and N.L all fall)
    int previous = 255;
    for (int px = 32; px < Size; ++px)
    {
        const int value = soft.At(px, 32).r;
        EXPECT_LE(value, previous + 1) << "column " << px;
        previous = value;
    }

    // Between the cones the soft light is the hard light times the cone factor
    for (const int px : {46, 50, 52})
    {
        const float x = WorldX(px);
        const float cosTheta = 1.0f / std::sqrt(1.0f + x * x);
        const float factor = Renderer::Common::SpotConeFactor(cosTheta, Degrees(20.0f), Degrees(40.0f));
        ASSERT_GT(factor, 0.1f) << "column " << px << " should be between the cones";
        ASSERT_LT(factor, 0.9f);
        const float hardValue = static_cast<float>(hard.At(px, 32).r);
        ASSERT_GT(hardValue, 60.0f);
        EXPECT_NEAR(static_cast<float>(soft.At(px, 32).r) / hardValue, factor, 0.04f) << "column " << px;
    }

    // Inside the inner cone the two agree (full strength both)
    EXPECT_NEAR(soft.At(32, 32).r, hard.At(32, 32).r, 1);
    EXPECT_NEAR(soft.At(36, 32).r, hard.At(36, 32).r, 1);
}

TEST_F(VulkanSpotLightTest, AWideConeLightsLikeAPointLightOfTheSameRange)
{
    const Frame spot = S().Draw(WithSpot(SpotAtTheQuad(80.0f, 85.0f, 0.7f)), S().lit);

    SceneLightingData pointLighting = AmbientOnly(0.0f);
    PointLightData point;
    point.position = Vector3(0.0f, 0.0f, 1.0f);
    point.intensity = 0.7f;
    point.range = 10.0f;
    point.attenuation = 1.0f; // the spot's own attenuation
    pointLighting.pointLights.push_back(point);
    const Frame reference = S().Draw(pointLighting, S().lit);

    ASSERT_GT(reference.At(32, 32).r, 60);
    for (const int px : {32, 36, 42, 50, 58})
    {
        EXPECT_NEAR(spot.At(px, 32).r, reference.At(px, 32).r, 1) << "column " << px;
    }
    EXPECT_NEAR(spot.At(40, 50).g, reference.At(40, 50).g, 1);
}

TEST_F(VulkanSpotLightTest, ALightFacingAwayOrOutOfRangeLightsNothing)
{
    SpotLightData away = SpotAtTheQuad(20.0f, 40.0f);
    away.direction = Vector3(0.0f, 0.0f, 1.0f);
    EXPECT_EQ(S().Draw(WithSpot(away), S().lit).At(32, 32).r, 0);

    SpotLightData tooShort = SpotAtTheQuad(20.0f, 40.0f);
    tooShort.range = 0.5f; // the quad is one unit away
    EXPECT_EQ(S().Draw(WithSpot(tooShort), S().lit).At(32, 32).r, 0);

    SpotLightData noRange = SpotAtTheQuad(20.0f, 40.0f);
    noRange.range = 0.0f;
    EXPECT_EQ(S().Draw(WithSpot(noRange), S().lit).At(32, 32).r, 0);

    SpotLightData noAxis = SpotAtTheQuad(20.0f, 40.0f);
    noAxis.direction = Vector3(0.0f, 0.0f, 0.0f);
    EXPECT_EQ(S().Draw(WithSpot(noAxis), S().lit).At(32, 32).r, 0);

    // A non-unit axis is normalised
    SpotLightData scaled = SpotAtTheQuad(20.0f, 40.0f);
    const Frame unit = S().Draw(WithSpot(scaled), S().lit);
    scaled.direction = Vector3(0.0f, 0.0f, -7.0f);
    const Frame big = S().Draw(WithSpot(scaled), S().lit);
    EXPECT_EQ(big.At(32, 32).r, unit.At(32, 32).r);
    EXPECT_EQ(big.At(48, 32).r, unit.At(48, 32).r);
}

TEST_F(VulkanSpotLightTest, OnlyTheFirstFourSpotLightsCountAsOnOpenGL)
{
    SceneLightingData lighting = AmbientOnly(0.0f);
    SpotLightData away = SpotAtTheQuad(20.0f, 40.0f);
    away.direction = Vector3(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < SceneLightingData::MAX_SPOT_LIGHTS; ++i)
    {
        lighting.spotLights.push_back(away);
    }
    lighting.spotLights.push_back(SpotAtTheQuad(20.0f, 40.0f)); // the fifth, which would light the quad
    EXPECT_EQ(S().Draw(lighting, S().lit).At(32, 32).r, 0);

    // As the last of four it counts
    lighting.spotLights.erase(lighting.spotLights.begin());
    EXPECT_GT(S().Draw(lighting, S().lit).At(32, 32).r, 60);
}

TEST_F(VulkanSpotLightTest, TwoSpotLightsAdd)
{
    // A dim surface, so that the sum stays below 255 (the specular term is not scaled by the intensity)
    S().lit.SetColor("uAlbedo", 0.3f, 0.3f, 0.3f, 1.0f);
    const int one = S().Draw(WithSpot(SpotAtTheQuad(20.0f, 40.0f, 0.3f)), S().lit).At(32, 32).r;
    SceneLightingData both = WithSpot(SpotAtTheQuad(20.0f, 40.0f, 0.3f));
    both.spotLights.push_back(SpotAtTheQuad(20.0f, 40.0f, 0.3f));
    const int two = S().Draw(both, S().lit).At(32, 32).r;
    ASSERT_GT(one, 15);
    ASSERT_LT(two, 200);
    EXPECT_NEAR(two, 2 * one, 2);
}

// ============================================================================
// Emissive
// ============================================================================

TEST_F(VulkanEmissiveTest, EmissiveIsAddedAfterLightingAndNotChangedByIt)
{
    S().lit.SetColor("uAlbedo", 0.0f, 0.0f, 0.0f, 1.0f); // black: lights add nothing to it
    S().lit.SetVec3("uEmissive", 0.2f, 0.4f, 0.6f);

    const Frame dark = S().Draw(AmbientOnly(0.0f), S().lit);
    EXPECT_NEAR(dark.At(32, 32).r, 51, 1);
    EXPECT_NEAR(dark.At(32, 32).g, 102, 1);
    EXPECT_NEAR(dark.At(32, 32).b, 153, 1);

    // A strong light and ambient change nothing on a black surface: the glow is the same
    SceneLightingData bright = WithDirectional(1.0f);
    bright.ambientColor = Vector3(1.0f, 1.0f, 1.0f);
    const Frame lit = S().Draw(bright, S().lit);
    EXPECT_EQ(lit.At(32, 32).r, dark.At(32, 32).r);
    EXPECT_EQ(lit.At(32, 32).g, dark.At(32, 32).g);
    EXPECT_EQ(lit.At(32, 32).b, dark.At(32, 32).b);
    EXPECT_EQ(lit.At(5, 50).g, dark.At(5, 50).g);
}

TEST_F(VulkanEmissiveTest, EmissiveAddsToTheLitSurface)
{
    S().lit.SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    const int without = S().Draw(AmbientOnly(0.4f), S().lit).At(32, 32).r; // 0.5 * 0.4 = 0.2
    EXPECT_NEAR(without, 51, 1);
    S().lit.SetVec3("uEmissive", 0.3f, 0.0f, 0.0f);
    const Frame with = S().Draw(AmbientOnly(0.4f), S().lit);
    EXPECT_NEAR(with.At(32, 32).r, 128, 1); // 0.2 + 0.3
    EXPECT_NEAR(with.At(32, 32).g, 51, 1);
}

TEST_F(VulkanEmissiveTest, TheEmissiveTextureMultipliesTheColour)
{
    S().lit.SetColor("uAlbedo", 0.0f, 0.0f, 0.0f, 1.0f);
    // Left texel red, right texel green
    const DualTexture glow = S().Texture({255, 0, 0, 255, 0, 255, 0, 255}, 2, 1);
    S().lit.SetAuxTexture(AuxTexture::Emissive, glow);
    ASSERT_EQ(S().lit.GetAuxTexture(AuxTexture::Emissive), glow.vulkan);
    S().lit.SetInt("uHasEmissiveTexture", 1);

    // The colour is black by default: the texture alone shows nothing
    Frame frame = S().Draw(AmbientOnly(0.0f), S().lit);
    EXPECT_EQ(frame.At(10, 32).r, 0);
    EXPECT_EQ(frame.At(54, 32).g, 0);

    S().lit.SetVec3("uEmissive", 1.0f, 1.0f, 1.0f);
    frame = S().Draw(AmbientOnly(0.0f), S().lit);
    EXPECT_NEAR(frame.At(10, 32).r, 255, 1);
    EXPECT_EQ(frame.At(10, 32).g, 0);
    EXPECT_EQ(frame.At(54, 32).r, 0);
    EXPECT_NEAR(frame.At(54, 32).g, 255, 1);

    S().lit.SetVec3("uEmissive", 0.5f, 0.5f, 0.5f);
    frame = S().Draw(AmbientOnly(0.0f), S().lit);
    EXPECT_NEAR(frame.At(10, 32).r, 128, 1);
}

TEST_F(VulkanEmissiveTest, AnInvalidOrDestroyedTextureDrawsTheDefaultSoTheHasTextureFlagsStillAgreeWithWhatIsDrawn)
{
    // The software renderer stores an invalid texture as none, so its flags follow what is drawn. Here the renderer does not
    // infer the flags (as OpenGL: Material::ApplyUniforms sets them), so the same guarantee comes from the 1x1 defaults: a
    // slot whose texture is not a live texture of this renderer (a foreign pointer, or one destroyed since) samples the
    // default, and each default is the value that makes the flag a no-op. GetAuxTexture returns what was set.
    S().lit.SetColor("uAlbedo", 0.4f, 0.4f, 0.4f, 1.0f);
    const SceneLightingData lighting = DirectionalFrom(Vector3(0.0f, 0.0f, 1.0f), 0.6f);
    const Frame plain = S().Draw(lighting, S().lit);
    ASSERT_GT(plain.At(kCentre, kCentre).r, 30);

    // A pointer that is not a texture of this renderer (the software renderer's texture: only ever compared)
    const DualTexture foreignMaker = S().Texture({10, 200, 30, 255}, 1, 1);
    ITexture* foreign = foreignMaker.software;
    IMaterial* material = S().lit.vulkan;
    for (const AuxTexture which : {AuxTexture::Emissive, AuxTexture::Occlusion, AuxTexture::Normal, AuxTexture::MetallicRoughness})
    {
        material->SetAuxTexture(which, foreign);
        EXPECT_EQ(material->GetAuxTexture(which), foreign);
    }
    for (const char* flag : {"uHasEmissiveTexture", "uHasOcclusionTexture", "uHasNormalTexture", "uHasMetallicRoughnessTexture"})
    {
        material->SetInt(flag, 1);
    }
    material->SetFloat("uOcclusionStrength", 1.0f);
    EXPECT_TRUE(Diff::Agree(S().DrawVulkanOnly(lighting, material, S().mesh.vulkan).AsImage(), plain.AsImage(), Diff::Options{}))
        << "every flag on with no usable texture draws exactly what every flag off does";

    // The same with a texture that was live and was destroyed (Vulkan only: the software renderer is not asked)
    const DualTexture glow = S().Texture({255, 0, 0, 255}, 1, 1);
    material->SetAuxTexture(AuxTexture::Emissive, glow.vulkan);
    material->SetVec3("uEmissive", 1.0f, 1.0f, 1.0f);
    const Frame withGlow = S().DrawVulkanOnly(lighting, material, S().mesh.vulkan);
    EXPECT_GT(withGlow.At(kCentre, kCentre).r, plain.At(kCentre, kCentre).r + 100) << "the live texture glows";
    R().DestroyTexture(glow.vulkan);
    const Frame afterDestroy = S().DrawVulkanOnly(lighting, material, S().mesh.vulkan);
    EXPECT_EQ(afterDestroy.At(kCentre, kCentre).r, plain.At(kCentre, kCentre).r)
        << "a destroyed emissive texture is the black default: no glow";
}

TEST_F(VulkanEmissiveTest, UnlitMaterialsIgnoreEmissive)
{
    S().unlit.SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    S().unlit.SetVec3("uEmissive", 0.4f, 0.4f, 0.4f); // not in the unlit block: logged once, ignored
    EXPECT_NEAR(S().Draw(AmbientOnly(0.0f), S().unlit).At(32, 32).r, 128, 1);
}

// ============================================================================
// Occlusion
// ============================================================================

TEST_F(VulkanOcclusionTest, TheOcclusionTextureScalesTheAmbientLightOnly)
{
    // Left texel fully open (255), right texel half occluded (128)
    const DualTexture occlusion = S().Texture({255, 255, 255, 255, 128, 128, 128, 255}, 2, 1);
    S().lit.SetAuxTexture(AuxTexture::Occlusion, occlusion);
    S().lit.SetInt("uHasOcclusionTexture", 1);

    const Frame ambient = S().Draw(AmbientOnly(1.0f), S().lit);
    EXPECT_NEAR(ambient.At(10, 32).r, 255, 1) << "open";
    EXPECT_NEAR(ambient.At(54, 32).r, 128, 2) << "half occluded";

    // Half the strength: 1 + 0.5 * (0.502 - 1) = 0.751
    S().lit.SetFloat("uOcclusionStrength", 0.5f);
    EXPECT_NEAR(S().Draw(AmbientOnly(1.0f), S().lit).At(54, 32).r, 191, 2);
    S().lit.SetFloat("uOcclusionStrength", 0.0f);
    EXPECT_NEAR(S().Draw(AmbientOnly(1.0f), S().lit).At(54, 32).r, 255, 1) << "strength 0 is no occlusion";
    S().lit.SetFloat("uOcclusionStrength", 1.0f);

    // A direct light is not occluded: the same pixel as without the texture
    const Frame direct = S().Draw(WithDirectional(0.6f), S().lit);
    S().lit.SetInt("uHasOcclusionTexture", 0);
    S().lit.SetAuxTexture(AuxTexture::Occlusion, DualTexture{});
    const Frame reference = S().Draw(WithDirectional(0.6f), S().lit);
    EXPECT_GT(reference.At(54, 32).r, 60);
    EXPECT_EQ(direct.At(54, 32).r, reference.At(54, 32).r);
    EXPECT_EQ(direct.At(10, 32).r, reference.At(10, 32).r);
}

// ============================================================================
// Colour space
// ============================================================================

TEST_F(VulkanColorSpaceTest, GammaIsTheDefaultAndLeavesLitOutputAsItWas)
{
    S().lit.SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    SceneLightingData lighting = AmbientOnly(0.5f);
    EXPECT_EQ(lighting.colorSpace, ColorSpace::Gamma);
    const Frame frame = S().Draw(lighting, S().lit);
    EXPECT_NEAR(frame.At(32, 32).r, 64, 1) << "0.5 x 0.5, written as it is";

    // A plain texture is multiplied as it is
    const DualTexture plainGrey = S().Texture({128, 128, 128, 255}, 1, 1, false);
    S().lit.SetTexture(plainGrey);
    S().lit.SetColor("uAlbedo", 1.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(S().Draw(AmbientOnly(1.0f), S().lit).At(32, 32).r, 128, 1);

    // An sRGB texture is decoded whatever the colour space, as OpenGL's SRGB8_ALPHA8 is (the engine only makes
    // one for a lit material in linear lighting): 128 reads as 0.216
    S().lit.SetTexture(S().Texture({128, 128, 128, 255}, 1, 1, true));
    EXPECT_NEAR(S().Draw(AmbientOnly(1.0f), S().lit).At(32, 32).r, 55, 1);

    // and an unlit draw too, written unencoded
    S().unlit.SetTexture(S().Texture({128, 128, 128, 255}, 1, 1, true));
    EXPECT_NEAR(S().Draw(AmbientOnly(1.0f), S().unlit).At(32, 32).r, 55, 1);
    S().unlit.SetTexture(S().Texture({128, 128, 128, 255}, 1, 1, false));
    EXPECT_NEAR(S().Draw(AmbientOnly(1.0f), S().unlit).At(32, 32).r, 128, 1);
}

TEST_F(VulkanColorSpaceTest, LinearLightingEncodesTheResultForDisplay)
{
    S().lit.SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    SceneLightingData lighting = AmbientOnly(0.5f);
    lighting.colorSpace = ColorSpace::Linear;
    // 0.5 x 0.5 = 0.25 linear, which displays as 0.537
    const Frame frame = S().Draw(lighting, S().lit);
    EXPECT_NEAR(frame.At(32, 32).r, 137, 1);
    EXPECT_NEAR(frame.At(32, 32).b, 137, 1);

    // Black and full white stay put
    S().lit.SetColor("uAlbedo", 0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(S().Draw(lighting, S().lit).At(32, 32).r, 0);
    S().lit.SetColor("uAlbedo", 1.0f, 1.0f, 1.0f, 1.0f);
    lighting.ambientColor = Vector3(1.0f, 1.0f, 1.0f);
    EXPECT_EQ(S().Draw(lighting, S().lit).At(32, 32).r, 255);
}

TEST_F(VulkanColorSpaceTest, LinearLightingDecodesSrgbTexturesOnly)
{
    const DualTexture srgbGrey = S().Texture({128, 128, 128, 255}, 1, 1, true);
    const DualTexture plainGrey = S().Texture({128, 128, 128, 255}, 1, 1, false);
    SceneLightingData lighting = AmbientOnly(1.0f);
    lighting.colorSpace = ColorSpace::Linear;

    // An sRGB texture is decoded then encoded again: the colour it shows is its own
    S().lit.SetTexture(srgbGrey);
    EXPECT_NEAR(S().Draw(lighting, S().lit).At(32, 32).r, 128, 1);

    // A texture of plain numbers (not sRGB) is taken as linear, so it displays brighter
    S().lit.SetTexture(plainGrey);
    EXPECT_NEAR(S().Draw(lighting, S().lit).At(32, 32).r, 188, 2);
}

TEST_F(VulkanColorSpaceTest, LinearLightingEncodesTheEmissiveSumAndDecodesItsTexture)
{
    S().lit.SetColor("uAlbedo", 0.0f, 0.0f, 0.0f, 1.0f);
    SceneLightingData lighting = AmbientOnly(0.0f);
    lighting.colorSpace = ColorSpace::Linear;

    S().lit.SetVec3("uEmissive", 0.25f, 0.25f, 0.25f);
    EXPECT_NEAR(S().Draw(lighting, S().lit).At(32, 32).g, 137, 1);

    S().lit.SetVec3("uEmissive", 1.0f, 1.0f, 1.0f);
    S().lit.SetAuxTexture(AuxTexture::Emissive, S().Texture({128, 128, 128, 255}, 1, 1, true));
    EXPECT_NEAR(S().Draw(lighting, S().lit).At(32, 32).g, 128, 1) << "decoded, then encoded again";
    S().lit.SetAuxTexture(AuxTexture::Emissive, S().Texture({128, 128, 128, 255}, 1, 1, false));
    EXPECT_NEAR(S().Draw(lighting, S().lit).At(32, 32).g, 188, 2);
}

TEST_F(VulkanColorSpaceTest, UnlitDrawsTheSameInBothColourSpaces)
{
    S().unlit.SetColor("uAlbedo", 0.5f, 0.25f, 1.0f, 1.0f);
    SceneLightingData gamma = AmbientOnly(0.2f);
    SceneLightingData linear = gamma;
    linear.colorSpace = ColorSpace::Linear;
    const Frame a = S().Draw(gamma, S().unlit);
    const Frame b = S().Draw(linear, S().unlit);
    EXPECT_EQ(a.rgba, b.rgba);
    EXPECT_NEAR(a.At(32, 32).r, 128, 1);
}

TEST_F(VulkanColorSpaceTest, OcclusionIsDataAndNeverDecoded)
{
    // The occlusion texture is read as it is (the engine never makes it an sRGB texture)
    const DualTexture occlusion = S().Texture({128, 128, 128, 255}, 1, 1, false);
    S().lit.SetAuxTexture(AuxTexture::Occlusion, occlusion);
    S().lit.SetInt("uHasOcclusionTexture", 1);
    SceneLightingData lighting = AmbientOnly(1.0f);
    lighting.colorSpace = ColorSpace::Linear;
    // 0.502 of the ambient in linear light, encoded: 0.502 -> 0.7366 -> 188
    EXPECT_NEAR(S().Draw(lighting, S().lit).At(32, 32).r, 188, 2);
}

// ============================================================================
// What only Vulkan has: when the lighting is read, and how it is kept
// ============================================================================

namespace
{
    /// Two quads on the same mesh: the left half (x -1..0) and the right half (x 0..1)
    MeshData LeftAndRightHalves()
    {
        MeshData data = Quad(false);
        for (Vertex& vertex : data.vertices)
        {
            vertex.position[0] = (vertex.position[0] - 1.0f) * 0.5f; // -1..1 -> -1..0
        }
        MeshData right = Quad(false);
        for (Vertex& vertex : right.vertices)
        {
            vertex.position[0] = (vertex.position[0] + 1.0f) * 0.5f; // -1..1 -> 0..1
        }
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        data.vertices.insert(data.vertices.end(), right.vertices.begin(), right.vertices.end());
        for (const std::uint32_t index : right.indices)
        {
            data.indices.push_back(base + index);
        }
        return data;
    }

    constexpr Renderer::Common::IndexRange kLeft{0, 6};
    constexpr Renderer::Common::IndexRange kRight{6, 6};
}

TEST_F(VulkanLightingUpdateTest, ADrawUsesTheLightingSetByTheLastUpdateBeforeIt)
{
    IMesh* halves = R().CreateMesh(LeftAndRightHalves());
    ASSERT_NE(halves, nullptr);
    IMaterial* lit = S().lit.vulkan;

    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().UpdateSceneLighting(AmbientOnly(0.2f), Vector3(0, 0, 100));
    R().DrawMesh(halves, Identity, lit, RenderState{}, kLeft); // draw A: the first lighting
    R().UpdateSceneLighting(AmbientOnly(0.8f), Vector3(0, 0, 100));
    R().DrawMesh(halves, Identity, lit, RenderState{}, kRight); // draw B: the second
    R().UpdateSceneLighting(AmbientOnly(0.0f), Vector3(0, 0, 100)); // after the last draw: changes nothing in this frame
    R().EndFrame();

    Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
    R().ReadFramebuffer(frame.rgba.data(), Size, Size);
    EXPECT_NEAR(frame.At(16, 32).r, 51, 1) << "A (left) was drawn under the ambient 0.2";
    EXPECT_NEAR(frame.At(48, 32).r, 204, 1) << "B (right) under 0.8";

    // The next frame starts from the last update (0.0): lighting set outside a frame is kept for the next frame's draws
    R().UpdateSceneLighting(AmbientOnly(0.5f), Vector3(0, 0, 100)); // outside BeginFrame..EndFrame
    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().DrawMesh(halves, Identity, lit, RenderState{}, kLeft);
    R().EndFrame();
    R().ReadFramebuffer(frame.rgba.data(), Size, Size);
    EXPECT_NEAR(frame.At(16, 32).r, 128, 1) << "an update outside a frame is used by the next frame";
}

TEST_F(VulkanLightingUpdateTest, EveryFrameShowsItsOwnLightingAcrossMoreFramesThanAreInFlight)
{
    IMaterial* lit = S().lit.vulkan;
    IMesh* quad = S().mesh.vulkan;
    constexpr int frames = 9; // more than the 2 frames in flight, so every slot is reused several times
    for (int i = 0; i < frames; ++i)
    {
        const float grey = 0.1f * static_cast<float>(i + 1);
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        R().UpdateSceneLighting(AmbientOnly(grey), Vector3(0, 0, 100));
        R().DrawMesh(quad, Identity, lit);
        R().EndFrame();
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
        R().ReadFramebuffer(frame.rgba.data(), Size, Size);
        EXPECT_NEAR(frame.At(32, 32).r, static_cast<int>(std::lround(grey * 255.0f)), 1) << "frame " << i;
    }

    // And without waiting for a frame before the next one (the GPU may still be reading the earlier ones): only the last is
    // read, but the validation layer sees every write
    for (int i = 0; i < 12; ++i)
    {
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        R().UpdateSceneLighting(AmbientOnly(i % 2 == 0 ? 0.25f : 0.75f), Vector3(0, 0, 100));
        R().DrawMesh(quad, Identity, lit);
        R().EndFrame();
    }
    Frame last{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
    R().ReadFramebuffer(last.rgba.data(), Size, Size);
    EXPECT_NEAR(last.At(32, 32).r, 191, 1) << "the last of the twelve (i = 11) is 0.75";
}

TEST_F(VulkanLightingUpdateTest, ManyDrawsAcrossRingChunksEachSeeTheLightingOfTheirTime)
{
    IMesh* halves = R().CreateMesh(LeftAndRightHalves());
    ASSERT_NE(halves, nullptr);
    IMaterial* lit = S().lit.vulkan;

    // Enough lit draws that the frame's uniform ring must grow beyond its first chunk, with the lighting changing in the
    // middle: every chunk holds its own copy of the lighting current when its first lit draw was made
    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().UpdateSceneLighting(AmbientOnly(0.2f), Vector3(0, 0, 100));
    for (int i = 0; i < 2000; ++i)
    {
        R().DrawMesh(halves, Identity, lit, RenderState{}, kLeft);
    }
    R().UpdateSceneLighting(AmbientOnly(0.8f), Vector3(0, 0, 100));
    for (int i = 0; i < 2000; ++i)
    {
        R().DrawMesh(halves, Identity, lit, RenderState{}, kRight);
    }
    R().EndFrame();
    EXPECT_GE(R().GetRingChunkCountForTest(), 2u) << "the ring grew: draws spanned more than one chunk";

    Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
    R().ReadFramebuffer(frame.rgba.data(), Size, Size);
    EXPECT_NEAR(frame.At(16, 32).r, 51, 1);
    EXPECT_NEAR(frame.At(48, 32).r, 204, 1);
}

TEST_F(VulkanLightingUpdateTest, MoreLightsThanTheMaximaAreClamped)
{
    SceneLightingData lighting = AmbientOnly(0.0f);
    for (int i = 0; i < 5; ++i)
    {
        Renderer::Common::DirectionalLightData light;
        light.direction = Vector3(0.0f, 0.0f, -1.0f);
        light.intensity = 0.1f;
        lighting.directionalLights.push_back(light);
    }
    for (int i = 0; i < 12; ++i)
    {
        PointLightData point;
        point.position = Vector3(0.0f, 0.0f, 1.0f + static_cast<float>(i));
        lighting.pointLights.push_back(point);
    }
    for (int i = 0; i < 7; ++i)
    {
        lighting.spotLights.push_back(SpotAtTheQuad(20.0f, 40.0f));
    }
    R().UpdateSceneLighting(lighting, Vector3(1, 2, 3));
    const std::vector<std::uint8_t> block = R().GetLightingBlockForTest();
    ASSERT_EQ(block.size(), static_cast<std::size_t>(Vk::kLightingBlockSize));
    const auto intAt = [&block](const char* name)
    {
        for (const Vk::Uniform& uniform : Vk::kLitLighting)
        {
            if (uniform.name == name)
            {
                std::int32_t value = 0;
                std::memcpy(&value, block.data() + uniform.offset, sizeof(value));
                return static_cast<int>(value);
            }
        }
        return -1;
    };
    EXPECT_EQ(intAt("uNumDirectionalLights"), 2);
    EXPECT_EQ(intAt("uNumPointLights"), 8);
    EXPECT_EQ(intAt("uNumSpotLights"), 4);

    // And in the picture: five identical directional lights light the quad like the first two
    SceneLightingData five = AmbientOnly(0.0f);
    SceneLightingData two = AmbientOnly(0.0f);
    for (int i = 0; i < 5; ++i)
    {
        Renderer::Common::DirectionalLightData light;
        light.direction = Vector3(0.0f, 0.0f, -1.0f);
        light.intensity = 0.1f;
        five.directionalLights.push_back(light);
        if (i < 2)
        {
            two.directionalLights.push_back(light);
        }
    }
    // Vulkan only: the software renderer does not clamp the directional lights, so a cross diff of the five would disagree
    const Frame fiveFrame = S().DrawVulkanOnly(five, S().lit.vulkan, S().mesh.vulkan);
    const Frame twoFrame = S().DrawVulkanOnly(two, S().lit.vulkan, S().mesh.vulkan);
    EXPECT_EQ(fiveFrame.rgba, twoFrame.rgba) << "lights past the two directional ones never reach the shader";
    EXPECT_GT(twoFrame.At(32, 32).r, 20);
}

TEST_F(VulkanLightingUpdateTest, ForceShaderEncodeChangesNothingOnVulkan)
{
    // OpenGL encodes in the shader when its target is not sRGB or forceShaderEncode is set. Vulkan's colour target is UNORM,
    // so linear lighting always encodes in the shader and the flag has nothing to decide
    S().lit.SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    for (const ColorSpace space : {ColorSpace::Gamma, ColorSpace::Linear})
    {
        SceneLightingData plain = AmbientOnly(0.5f);
        plain.colorSpace = space;
        SceneLightingData forced = plain;
        forced.forceShaderEncode = true;
        const Frame a = S().DrawVulkanOnly(plain, S().lit.vulkan, S().mesh.vulkan);
        const Frame b = S().DrawVulkanOnly(forced, S().lit.vulkan, S().mesh.vulkan);
        EXPECT_EQ(a.rgba, b.rgba) << (space == ColorSpace::Linear ? "linear" : "gamma");
        EXPECT_NEAR(a.At(32, 32).r, space == ColorSpace::Linear ? 137 : 64, 1);
    }
}

// ============================================================================
// The lit material: GL defaults, the std140 name table, the aux-texture defaults
// ============================================================================

TEST_F(VulkanLitMaterialTest, ANewLitMaterialHoldsExactlyTheOpenGlDefaultsInItsStd140Block)
{
    for (const bool withTexture : {false, true})
    {
        const DualTexture texture = S().Texture({1, 2, 3, 255}, 1, 1);
        IMaterial* material = withTexture ? R().CreateMaterial(R().GetStandardLitShader(), texture.vulkan)
                                          : R().CreateMaterial(R().GetStandardLitShader());
        ASSERT_NE(material, nullptr);
        std::vector<std::uint8_t> block;
        ASSERT_TRUE(R().GetMaterialBlockForTest(material, block));
        ASSERT_EQ(block.size(), static_cast<std::size_t>(Vk::kMaterialBlockRange));

        const auto floatAt = [&block](const char* name, const int component = 0)
        {
            const Vk::Uniform* uniform = Vk::FindMaterialUniform(Vk::BuiltInShader::Lit, name);
            EXPECT_NE(uniform, nullptr) << name;
            float value = -1.0f;
            if (uniform != nullptr)
            {
                std::memcpy(&value, block.data() + uniform->offset + 4 * component, sizeof(value));
            }
            return value;
        };
        const auto intAt = [&block](const char* name)
        {
            const Vk::Uniform* uniform = Vk::FindMaterialUniform(Vk::BuiltInShader::Lit, name);
            EXPECT_NE(uniform, nullptr) << name;
            std::int32_t value = -1;
            if (uniform != nullptr)
            {
                std::memcpy(&value, block.data() + uniform->offset, sizeof(value));
            }
            return static_cast<int>(value);
        };
        for (int component = 0; component < 4; ++component)
        {
            EXPECT_FLOAT_EQ(floatAt("uAlbedo", component), 1.0f) << "white albedo, component " << component;
        }
        EXPECT_EQ(intAt("uHasTexture"), withTexture ? 1 : 0);
        EXPECT_FLOAT_EQ(floatAt("uMetallic"), 0.0f);
        EXPECT_FLOAT_EQ(floatAt("uSmoothness"), 0.5f);
        EXPECT_FLOAT_EQ(floatAt("uAlphaCutoff"), 0.0f);
        for (int component = 0; component < 3; ++component)
        {
            EXPECT_FLOAT_EQ(floatAt("uEmissive", component), 0.0f);
        }
        EXPECT_EQ(intAt("uHasEmissiveTexture"), 0);
        EXPECT_EQ(intAt("uHasOcclusionTexture"), 0);
        EXPECT_FLOAT_EQ(floatAt("uOcclusionStrength"), 1.0f);
        EXPECT_EQ(intAt("uHasNormalTexture"), 0);
        EXPECT_FLOAT_EQ(floatAt("uNormalScale"), 1.0f);
        EXPECT_EQ(intAt("uPbr"), 0);
        EXPECT_EQ(intAt("uHasMetallicRoughnessTexture"), 0);

        // Nothing past the block's end is written
        for (std::size_t i = Vk::MaterialBlockSize(Vk::BuiltInShader::Lit); i < block.size(); ++i)
        {
            EXPECT_EQ(block[i], 0) << "byte " << i;
        }

        // Written through the name table, with its types: a Vec3 into uEmissive, an Int into uPbr, a Float into
        // uMetallic; a name or type that does not fit is ignored (the block does not change)
        material->SetVec3("uEmissive", 0.25f, 0.5f, 0.75f);
        material->SetInt("uPbr", 1);
        material->SetFloat("uMetallic", 0.75f);
        std::vector<std::uint8_t> changed;
        ASSERT_TRUE(R().GetMaterialBlockForTest(material, changed));
        float e[3];
        std::memcpy(e, changed.data() + Vk::FindMaterialUniform(Vk::BuiltInShader::Lit, "uEmissive")->offset, sizeof(e));
        EXPECT_FLOAT_EQ(e[0], 0.25f);
        EXPECT_FLOAT_EQ(e[1], 0.5f);
        EXPECT_FLOAT_EQ(e[2], 0.75f);
        material->SetInt("uAlbedo", 3);        // wrong type
        material->SetFloat("uNoSuchName", 1.0f); // unknown name
        std::vector<std::uint8_t> same;
        ASSERT_TRUE(R().GetMaterialBlockForTest(material, same));
        EXPECT_EQ(same, changed);
        R().DestroyMaterial(material);
    }
    R().DestroyMaterial(nullptr);
    std::vector<std::uint8_t> none;
    EXPECT_FALSE(R().GetMaterialBlockForTest(nullptr, none));
}

TEST_F(VulkanLitMaterialTest, AbsentAuxTexturesDrawLikeTheirDefaultsAndTheDefaultsAreNeutral)
{
    // Every flag on with the aux textures absent: each slot samples its 1x1 default (emissive black, occlusion white,
    // normal flat, metallic-roughness white). With uPbr set too, the metallic-roughness white leaves the factors alone, so
    // the picture is the one with every flag off
    S().lit.SetColor("uAlbedo", 0.6f, 0.6f, 0.6f, 1.0f);
    const SceneLightingData lighting = DirectionalFrom(Vector3(0.3f, 0.2f, 1.0f), 0.7f);
    const Frame off = S().DrawVulkanOnly(lighting, S().lit.vulkan, S().tangentMesh.vulkan);
    IMaterial* material = S().lit.vulkan;
    for (const char* flag : {"uHasEmissiveTexture", "uHasOcclusionTexture", "uHasNormalTexture"})
    {
        material->SetInt(flag, 1);
    }
    const Frame on = S().DrawVulkanOnly(lighting, material, S().tangentMesh.vulkan);
    EXPECT_TRUE(Diff::Agree(on.AsImage(), off.AsImage(), Diff::Options{})) << "flags on, textures absent: the defaults are neutral";

    material->SetInt("uHasMetallicRoughnessTexture", 1);
    material->SetInt("uPbr", 1);
    const Frame pbrOn = S().DrawVulkanOnly(lighting, material, S().tangentMesh.vulkan);
    material->SetInt("uHasMetallicRoughnessTexture", 0);
    const Frame pbrOff = S().DrawVulkanOnly(lighting, material, S().tangentMesh.vulkan);
    EXPECT_TRUE(Diff::Agree(pbrOn.AsImage(), pbrOff.AsImage(), Diff::Options{}))
        << "the white metallic-roughness default leaves the factors alone";
}
