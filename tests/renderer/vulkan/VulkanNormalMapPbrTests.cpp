#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/LightingMath.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/SceneLighting.hpp>

#include "VulkanLitTestUtil.hpp"

// Normal maps and metallic-roughness PBR on the Vulkan backend (#43, stage C): the RENDERED tests of
// tests/renderer/software/SoftwareNormalMapPbrTests.cpp with the same scenes, expectations and tolerances (a normal-mapped
// flat quad against the same quad geometrically tilted; PBR pixels against the closed-form BRDF of LightingMath.hpp), each
// draw also made on a headless SoftwareRenderer and compared with the lit criterion (VulkanLitTestUtil.hpp). The PbrMath
// cases of the software file test CPU helpers and are not mirrored. The handedness of the tangent, a mirrored model matrix,
// a negative scale, a tiny scale and a mesh without tangents are the lit shader's own maths (a faithful port of OpenGL's),
// proven here by drawing it.

using namespace VulkanTesting::Lit;
using Renderer::Common::ColorSpace;
using Renderer::Common::CullMode;

namespace
{
    constexpr float kInvSqrt2 = 0.70710678f;
    constexpr int kCentre = 32;

    /// Rotation about y by 45 degrees (row-major, column vectors): +z turns toward +x
    constexpr float RotateY45[16] = {
        kInvSqrt2, 0, kInvSqrt2, 0,
        0, 1, 0, 0,
        -kInvSqrt2, 0, kInvSqrt2, 0,
        0, 0, 0, 1,
    };

    /// Rotation about z by 90 degrees: +x turns toward +y
    constexpr float RotateZ90[16] = {
        0, -1, 0, 0,
        1, 0, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    /// x mirrored: a negative determinant
    constexpr float MirrorX[16] = {
        -1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    /// The software tests' scene: a glossy lit quad (smoothness 1: a Blinn-Phong highlight so tight that the Blinn-Phong
    /// tests see the diffuse term alone) whose default mesh has the tangent +x with handedness +1
    class VulkanNormalMapBase : public VulkanLitTest
    {
    protected:
        void SetUp() override
        {
            VulkanLitTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            S().lit.SetFloat("uSmoothness", 1.0f);
        }

        Frame Draw(const SceneLightingData& lighting, DualMaterial& material, const DualMesh* which = nullptr,
                   const float* model = Identity, const RenderState& state = RenderState{})
        {
            return S().Draw(lighting, material, which != nullptr ? which : &S().tangentMesh, model, state);
        }

        /// A 1 x 1 data texture (never sRGB, as GpuCache makes normal and metallic-roughness textures)
        DualTexture Texel(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
        {
            return S().Texture({r, g, b, 255}, 1, 1, false);
        }

        /// A texel byte for a component of a unit vector
        static std::uint8_t Encode(const float component)
        {
            return static_cast<std::uint8_t>(std::lround((component * 0.5f + 0.5f) * 255.0f));
        }

        /// A normal map texel for the tangent-space normal (x, y, z), normalised
        DualTexture NormalTexel(const float x, const float y, const float z)
        {
            const float length = std::sqrt(x * x + y * y + z * z);
            return Texel(Encode(x / length), Encode(y / length), Encode(z / length));
        }
    };

    int Byte(const float value)
    {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    /// The reply of a white-lit pixel at normal incidence: light and view along +z, so NdotL = NdotV = NdotH = 1
    /// and VdotH = 1 (the Fresnel weight is 0). `baseColour` and `metallic` as the material's, `smoothness` too.
    float NormalIncidenceReply(const float baseColour, const float metallic, const float smoothness, const float intensity)
    {
        using namespace Renderer::Common;
        const float alpha = PerceptualRoughness(smoothness) * PerceptualRoughness(smoothness);
        const float f0 = kDielectricF0 + (baseColour - kDielectricF0) * metallic;
        const float diffuse = (1.0f - metallic) * (1.0f - kDielectricF0) * baseColour;
        const float specular = f0 * kPi * GgxDistribution(1.0f, alpha) * SmithGgxVisibility(1.0f, 1.0f, alpha);
        return (diffuse + specular) * intensity;
    }

    /// A PBR material: lit shader with uPbr = 1
    void MakePbr(DualMaterial& material, const float metallic, const float smoothness)
    {
        material.SetInt("uPbr", 1);
        material.SetFloat("uMetallic", metallic);
        material.SetFloat("uSmoothness", smoothness);
    }
}

class VulkanNormalMapTest : public VulkanNormalMapBase {};
class VulkanPbrTest : public VulkanNormalMapBase {};

// ============================================================================
// Normal maps
// ============================================================================

TEST_F(VulkanNormalMapTest, AFlatNormalMapChangesNothing)
{
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    const int without = Draw(light, S().lit).At(kCentre, kCentre).r;
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(0, 0, 1));
    const int with = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_GT(without, 40);
    EXPECT_NEAR(with, without, 2) << "a texel of (0.5, 0.5, 1) is the vertex normal";
}

TEST_F(VulkanNormalMapTest, ANormalMapTiltsTheNormalLikeTiltingTheSurface)
{
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);

    // The reference: the same quad turned 45 degrees about y, so its normal is the light's direction, no normal map
    const int tilted = Draw(light, S().lit, nullptr, RotateY45).At(kCentre, kCentre).r;
    const int flat = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_GT(tilted, flat + 30) << "facing the light is much brighter than facing away at 45 degrees";

    // A flat quad whose normal map leans toward +x by 45 degrees: the tangent is +x
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(kInvSqrt2, 0, kInvSqrt2));
    const int mapped = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(mapped, tilted, 4);
}

TEST_F(VulkanNormalMapTest, TheTexelsGreenIsUpTheBitangentAndTheHandednessFlipsIt)
{
    const SceneLightingData fromAbove = DirectionalFrom(Vector3(0, kInvSqrt2, kInvSqrt2), 0.8f);
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(0, kInvSqrt2, kInvSqrt2)); // leaning up (+v)

    // w = +1: the bitangent cross(N, T) is +y, so the normal leans up, toward the light
    const int lit = Draw(fromAbove, S().lit, &S().tangentMesh).At(kCentre, kCentre).r;
    EXPECT_GT(lit, 150);
    // w = -1: it leans down, away from the light
    const int dark = Draw(fromAbove, S().lit, &S().mirroredTangentMesh).At(kCentre, kCentre).r;
    EXPECT_LT(dark, 12);
}

TEST_F(VulkanNormalMapTest, TheTangentTurnsWithTheModelMatrix)
{
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(kInvSqrt2, 0, kInvSqrt2)); // leaning along +T
    const SceneLightingData fromAbove = DirectionalFrom(Vector3(0, kInvSqrt2, kInvSqrt2), 0.8f);
    const SceneLightingData fromTheRight = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);

    // Turned 90 degrees about z, +T is world +y: the normal leans up
    const int up = Draw(fromAbove, S().lit, nullptr, RotateZ90).At(kCentre, kCentre).r;
    const int right = Draw(fromTheRight, S().lit, nullptr, RotateZ90).At(kCentre, kCentre).r;
    EXPECT_GT(up, 150) << "toward a light above";
    EXPECT_LT(right, up - 40) << "and not toward a light on the right";
}

TEST_F(VulkanNormalMapTest, AMirroringModelMatrixFlipsTheHandednessSoUpStaysUp)
{
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(0, kInvSqrt2, kInvSqrt2)); // leaning up (+v)
    const SceneLightingData fromAbove = DirectionalFrom(Vector3(0, kInvSqrt2, kInvSqrt2), 0.8f);
    RenderState bothFaces;
    bothFaces.cull = CullMode::None; // mirroring reverses the winding

    // The mirror reverses x only: v still grows up the quad, so the normal still leans up
    const int mirrored = Draw(fromAbove, S().lit, nullptr, MirrorX, bothFaces).At(kCentre, kCentre).r;
    EXPECT_GT(mirrored, 150);
}

TEST_F(VulkanNormalMapTest, ANegativeScaleFlipsTheMapsXAndY)
{
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(kInvSqrt2, 0, kInvSqrt2));
    S().lit.SetFloat("uNormalScale", 1.0f);
    const int toward = Draw(light, S().lit).At(kCentre, kCentre).r;
    S().lit.SetFloat("uNormalScale", -1.0f);
    const int away = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_GT(toward, 150);
    EXPECT_LT(away, 12) << "leaning along -x: away from a light on the right";
}

TEST_F(VulkanNormalMapTest, ATinyModelScaleDoesNotSwitchNormalMappingOff)
{
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(kInvSqrt2, 0, kInvSqrt2));
    const int full = Draw(light, S().lit).At(kCentre, kCentre).r;
    ASSERT_GT(full, 150);

    // The quad scaled down by 1e-4 (its tangent in world space is 1e-4 long) and the projection scaled up to match
    constexpr float tiny = 1e-4f;
    constexpr float model[16] = {tiny, 0, 0, 0, 0, tiny, 0, 0, 0, 0, tiny, 0, 0, 0, 0, 1};
    constexpr float projection[16] = {1.0f / tiny, 0, 0, 0, 0, 1.0f / tiny, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const Frame frame = S().Draw(light, S().lit, &S().tangentMesh, model, RenderState{}, Vector3(0, 0, 100), projection);
    EXPECT_NEAR(frame.At(kCentre, kCentre).r, full, 3) << "the normal map still applies";
}

TEST_F(VulkanNormalMapTest, AMeshWithoutTangentsIsDrawnWithoutNormalMapping)
{
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    const Frame plain = Draw(light, S().lit, &S().mesh);
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(kInvSqrt2, 0, kInvSqrt2));
    const Frame mapped = Draw(light, S().lit, &S().mesh);
    EXPECT_EQ(plain.rgba, mapped.rgba) << "no tangent: the vertex normal is used";
}

TEST_F(VulkanNormalMapTest, TheScaleMultipliesTheTexelsXAndY)
{
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(kInvSqrt2, 0, kInvSqrt2));

    S().lit.SetFloat("uNormalScale", 0.0f);
    const int zero = Draw(light, S().lit).At(kCentre, kCentre).r;
    S().lit.SetAuxTexture(AuxTexture::Normal, DualTexture{});
    const int none = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(zero, none, 2) << "scale 0 is a flat surface";

    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(kInvSqrt2, 0, kInvSqrt2));
    S().lit.SetFloat("uNormalScale", 0.5f);
    const int half = Draw(light, S().lit).At(kCentre, kCentre).r;
    S().lit.SetFloat("uNormalScale", 1.0f);
    const int full = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_GT(half, zero + 20) << "half the lean already faces the light more";

    // Half of x scales the leaning normal (0.71, 0, 0.71) to (0.45, 0, 0.89) after normalising: the surface tilted by
    // atan(0.5) = 26.57 degrees, which the same quad turned that far shows too; and the full scale is the 45 degree turn
    constexpr float s = 0.4472136f;
    constexpr float c = 0.8944272f;
    constexpr float tiltedHalf[16] = {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1};
    S().lit.SetAuxTexture(AuxTexture::Normal, DualTexture{});
    EXPECT_NEAR(half, Draw(light, S().lit, nullptr, tiltedHalf).At(kCentre, kCentre).r, 4);
    EXPECT_NEAR(full, Draw(light, S().lit, nullptr, RotateY45).At(kCentre, kCentre).r, 4);
}

TEST_F(VulkanNormalMapTest, TheNormalMapIsDataNeverDecodedAsSrgb)
{
    // A texel of (128, 128, 255) is the flat normal. If it were decoded as sRGB, x and y would read as -0.78
    S().lit.SetAuxTexture(AuxTexture::Normal, Texel(128, 128, 255));
    const SceneLightingData light = DirectionalFrom(Vector3(0, 0, 1), 0.5f);
    const int mapped = Draw(light, S().lit).At(kCentre, kCentre).r;
    S().lit.SetAuxTexture(AuxTexture::Normal, DualTexture{});
    EXPECT_NEAR(mapped, Draw(light, S().lit).At(kCentre, kCentre).r, 2);
}

TEST_F(VulkanNormalMapTest, AnUnlitMaterialIgnoresTheNormalMap)
{
    S().unlit.SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    const Frame before = Draw(AmbientOnly(0.0f), S().unlit);
    // Set on the materials directly: an unlit material has no uHasNormalTexture to set
    const DualTexture normal = NormalTexel(kInvSqrt2, 0, kInvSqrt2);
    S().unlit.vulkan->SetAuxTexture(AuxTexture::Normal, normal.vulkan);
    S().unlit.software->SetAuxTexture(AuxTexture::Normal, normal.software);
    EXPECT_EQ(before.rgba, Draw(AmbientOnly(0.0f), S().unlit).rgba);
}

// ============================================================================
// PBR
// ============================================================================

TEST_F(VulkanPbrTest, ADielectricIsADiffuseSurfaceWithAFaintHighlight)
{
    MakePbr(S().lit, 0.0f, 0.5f);
    const Frame frame = Draw(DirectionalFrom(Vector3(0, 0, 1), 0.3f), S().lit);
    EXPECT_NEAR(frame.At(kCentre, kCentre).r, Byte(NormalIncidenceReply(1.0f, 0.0f, 0.5f, 0.3f)), 2);
    EXPECT_NEAR(frame.At(kCentre, kCentre).g, frame.At(kCentre, kCentre).r, 1) << "white stays grey";
}

TEST_F(VulkanPbrTest, AMetalHasNoDiffuseAndTintsItsHighlightWithTheBaseColour)
{
    S().lit.SetColor("uAlbedo", 1.0f, 0.5f, 0.25f, 1.0f);
    MakePbr(S().lit, 1.0f, 0.5f);
    const SceneLightingData light = DirectionalFrom(Vector3(0, 0, 1), 0.1f);
    const Frame metal = Draw(light, S().lit);
    EXPECT_NEAR(metal.At(kCentre, kCentre).r, Byte(NormalIncidenceReply(1.0f, 1.0f, 0.5f, 0.1f)), 2);
    EXPECT_NEAR(metal.At(kCentre, kCentre).g, Byte(NormalIncidenceReply(0.5f, 1.0f, 0.5f, 0.1f)), 2);
    EXPECT_NEAR(metal.At(kCentre, kCentre).b, Byte(NormalIncidenceReply(0.25f, 1.0f, 0.5f, 0.1f)), 2);
    EXPECT_GT(metal.At(kCentre, kCentre).r, metal.At(kCentre, kCentre).g + 20);

    // The same colour as a dielectric is a different, diffuse look: red much dimmer here, where the metal reflects
    MakePbr(S().lit, 0.0f, 0.5f);
    const Frame dielectric = Draw(light, S().lit);
    EXPECT_GT(metal.At(kCentre, kCentre).r, dielectric.At(kCentre, kCentre).r + 40);
}

TEST_F(VulkanPbrTest, UnderAmbientLightAMetalReflectsItsColourAndADielectricScattersIt)
{
    S().lit.SetColor("uAlbedo", 0.8f, 0.8f, 0.8f, 1.0f);
    const SceneLightingData ambient = AmbientOnly(1.0f);
    const auto env = Renderer::Common::EnvironmentBrdf(Renderer::Common::PerceptualRoughness(0.5f), 1.0f);

    MakePbr(S().lit, 1.0f, 0.5f);
    EXPECT_NEAR(Draw(ambient, S().lit).At(kCentre, kCentre).r, Byte(0.8f * env.scale + env.bias), 3)
        << "a metal: F0 = the base colour";
    MakePbr(S().lit, 0.0f, 0.5f);
    const float f0 = Renderer::Common::kDielectricF0;
    const float reflected = f0 * env.scale + env.bias;
    EXPECT_NEAR(Draw(ambient, S().lit).At(kCentre, kCentre).r, Byte(0.8f * (1.0f - reflected) + reflected), 3)
        << "a dielectric: its diffuse colour with what the faint reflection did not take, plus the reflection";

    // No light, no ambient: nothing to reflect
    MakePbr(S().lit, 1.0f, 0.5f);
    EXPECT_EQ(Draw(AmbientOnly(0.0f), S().lit).At(kCentre, kCentre).r, 0);
}

TEST_F(VulkanPbrTest, RoughnessSpreadsTheHighlightSoASmoothSurfaceHasABrighterPeak)
{
    MakePbr(S().lit, 1.0f, 0.9f); // roughness 0.1
    const SceneLightingData light = DirectionalFrom(Vector3(0, 0, 1), 0.0001f);
    const int smooth = Draw(light, S().lit).At(kCentre, kCentre).r;
    MakePbr(S().lit, 1.0f, 0.5f);
    const int rough = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_GT(smooth, rough + 30);
    EXPECT_NEAR(smooth, Byte(NormalIncidenceReply(1.0f, 1.0f, 0.9f, 0.0001f)), 2);
}

TEST_F(VulkanPbrTest, TheMetallicRoughnessTextureMultipliesTheFactors)
{
    const SceneLightingData light = DirectionalFrom(Vector3(0, 0, 1), 0.1f);

    // Blue is the metallic factor: a texel with none makes a metal a dielectric
    MakePbr(S().lit, 1.0f, 0.5f);
    S().lit.SetAuxTexture(AuxTexture::MetallicRoughness, Texel(0, 255, 0));
    const int notMetal = Draw(light, S().lit).At(kCentre, kCentre).r;
    MakePbr(S().lit, 0.0f, 0.5f);
    S().lit.SetAuxTexture(AuxTexture::MetallicRoughness, DualTexture{});
    const int dielectric = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(notMetal, dielectric, 2) << "roughness 1 x 0.5 of the texel's green is 0.5, as the factor alone";

    // A full blue texel leaves the factor alone
    MakePbr(S().lit, 1.0f, 0.5f);
    S().lit.SetAuxTexture(AuxTexture::MetallicRoughness, Texel(0, 255, 255));
    const int metalWithTexel = Draw(light, S().lit).At(kCentre, kCentre).r;
    S().lit.SetAuxTexture(AuxTexture::MetallicRoughness, DualTexture{});
    const int metal = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(metalWithTexel, metal, 2);
    EXPECT_GT(metal, notMetal + 40);

    // Green is the roughness: half of it makes the surface smoother, its highlight brighter
    MakePbr(S().lit, 1.0f, 0.0f); // roughness 1
    const SceneLightingData faint = DirectionalFrom(Vector3(0, 0, 1), 0.02f);
    S().lit.SetAuxTexture(AuxTexture::MetallicRoughness, DualTexture{});
    const int roughMetal = Draw(faint, S().lit).At(kCentre, kCentre).r;
    S().lit.SetAuxTexture(AuxTexture::MetallicRoughness, Texel(0, 128, 255));
    const int smoother = Draw(faint, S().lit).At(kCentre, kCentre).r;
    EXPECT_GT(smoother, roughMetal + 10);
}

TEST_F(VulkanPbrTest, TheMetallicRoughnessTextureIsIgnoredWithoutPbr)
{
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.6f);
    const Frame plain = Draw(light, S().lit);
    S().lit.SetAuxTexture(AuxTexture::MetallicRoughness, Texel(0, 0, 255));
    S().lit.SetFloat("uMetallic", 1.0f); // Blinn-Phong has no metal
    EXPECT_EQ(plain.rgba, Draw(light, S().lit).rgba);
}

TEST_F(VulkanPbrTest, TheNormalMapShadesPbrAsItDoesBlinnPhong)
{
    MakePbr(S().lit, 0.0f, 0.3f);
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    const int tilted = Draw(light, S().lit, nullptr, RotateY45).At(kCentre, kCentre).r;
    S().lit.SetAuxTexture(AuxTexture::Normal, NormalTexel(kInvSqrt2, 0, kInvSqrt2));
    const int mapped = Draw(light, S().lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(mapped, tilted, 4);
    S().lit.SetAuxTexture(AuxTexture::Normal, DualTexture{});
    EXPECT_LT(Draw(light, S().lit).At(kCentre, kCentre).r, tilted - 30);
}

TEST_F(VulkanPbrTest, EmissiveAndOcclusionWorkAsTheyDoWithBlinnPhong)
{
    S().lit.SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    MakePbr(S().lit, 0.0f, 0.5f);
    const int base = Draw(AmbientOnly(0.4f), S().lit).At(kCentre, kCentre).r;

    // Emissive adds after the lighting
    S().lit.SetVec3("uEmissive", 0.3f, 0.0f, 0.0f);
    const Frame glowing = Draw(AmbientOnly(0.4f), S().lit);
    EXPECT_NEAR(glowing.At(kCentre, kCentre).r, base + Byte(0.3f), 2);
    EXPECT_NEAR(glowing.At(kCentre, kCentre).g, base, 1);
    S().lit.SetVec3("uEmissive", 0.0f, 0.0f, 0.0f);

    // Occlusion scales the ambient light, diffuse and specular alike
    S().lit.SetAuxTexture(AuxTexture::Occlusion, Texel(128, 128, 128));
    S().lit.SetFloat("uOcclusionStrength", 1.0f);
    const int occluded = Draw(AmbientOnly(0.4f), S().lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(occluded, static_cast<int>(std::lround(static_cast<float>(base) * 0.502f)), 3);
    S().lit.SetFloat("uOcclusionStrength", 0.0f);
    EXPECT_NEAR(Draw(AmbientOnly(0.4f), S().lit).At(kCentre, kCentre).r, base, 1);
}

TEST_F(VulkanPbrTest, LinearLightingEncodesThePbrResultToo)
{
    MakePbr(S().lit, 0.0f, 0.5f);
    SceneLightingData gamma = DirectionalFrom(Vector3(0, 0, 1), 0.2f);
    SceneLightingData linear = gamma;
    linear.colorSpace = ColorSpace::Linear;
    const int plain = Draw(gamma, S().lit).At(kCentre, kCentre).r;
    const int encoded = Draw(linear, S().lit).At(kCentre, kCentre).r;
    EXPECT_GT(plain, 20);
    EXPECT_NEAR(encoded, Byte(Renderer::Common::LinearToSrgb(static_cast<float>(plain) / 255.0f)), 3);
    EXPECT_GT(encoded, plain + 20) << "the encode lifts the dark values";
}

TEST_F(VulkanPbrTest, ALightBehindTheSurfaceGivesNoPbrLight)
{
    MakePbr(S().lit, 0.5f, 0.5f);
    const Frame frame = Draw(DirectionalFrom(Vector3(0, 0, -1), 1.0f), S().lit);
    EXPECT_EQ(frame.At(kCentre, kCentre).r, 0);
}
