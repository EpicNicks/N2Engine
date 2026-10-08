#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/LightingMath.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SWTexture.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

// The software renderer's lit shading beyond directional and point lights: spot lights (cone, falloff, range and the
// four-light cap, matching the OpenGL shader), the emissive term, the occlusion texture and the linear colour
// space. Frames are real rasterizations of a quad facing +z, read back headless. Checks use tolerances of a few
// 8-bit levels, never exact images, because Debug and Release may round floats differently.

using Renderer::Common::AuxTexture;
using Renderer::Common::ColorSpace;
using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::PointLightData;
using Renderer::Common::SceneLightingData;
using Renderer::Common::SpotLightData;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::Vertex;
using Renderer::Software::SoftwareRenderer;
using N2Engine::Math::Vector3;

namespace
{
    constexpr int Size = 64;

    constexpr float Identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    struct Rgb
    {
        int r = 0, g = 0, b = 0;
    };

    /// A frame read back with ReadFramebuffer: RGBA8, row 0 at the bottom
    struct Frame
    {
        std::vector<std::uint8_t> rgba;

        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * Size + static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        }
    };

    /// The world x of pixel column px at the middle row: the quad spans -1..1 over the frame
    float WorldX(const int px)
    {
        return (static_cast<float>(px) + 0.5f - static_cast<float>(Size) * 0.5f) / (static_cast<float>(Size) * 0.5f);
    }

    /// A quad over x, y in -1..1 at z = 0, facing +z, with u 0..1 across it
    MeshData Quad()
    {
        MeshData data;
        data.vertices = {
            Vertex{{-1, -1, 0}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{1, -1, 0}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{1, 1, 0}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}},
            Vertex{{-1, 1, 0}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    /// A headless renderer with a lit white material on the quad. Identity matrices: world space is the quad's.
    struct Scene
    {
        SoftwareRenderer renderer;
        IMesh *mesh = nullptr;
        IMaterial *lit = nullptr;
        IMaterial *unlit = nullptr;

        Scene()
        {
            EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
            renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
            mesh = renderer.CreateMesh(Quad());
            lit = renderer.CreateMaterial(renderer.GetStandardLitShader());
            unlit = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            lit->SetFloat("uSmoothness", 0.0f);
        }

        ~Scene() { renderer.Shutdown(); }

        Frame Draw(const SceneLightingData &lighting, IMaterial *material, const Vector3 &camera = Vector3(0, 0, 100))
        {
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            renderer.UpdateSceneLighting(lighting, camera);
            renderer.DrawMesh(mesh, Identity, material);
            renderer.EndFrame();
            renderer.Present();
            Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            return frame;
        }

        ITexture *Texture(const std::vector<std::uint8_t> &rgba, const std::uint32_t width, const std::uint32_t height)
        {
            TextureOptions nearest;
            nearest.filter = TextureFilter::Nearest;
            ITexture *texture = renderer.CreateTexture(rgba.data(), width, height, 4, nearest);
            EXPECT_NE(texture, nullptr);
            return texture;
        }
    };

    constexpr float Degrees(const float degrees) { return degrees * 3.14159265f / 180.0f; }

    /// Ambient only, no lights, of the given grey
    SceneLightingData AmbientOnly(const float grey)
    {
        SceneLightingData lighting;
        lighting.ambientColor = Vector3(grey, grey, grey);
        return lighting;
    }

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

    SceneLightingData WithSpot(const SpotLightData &spot)
    {
        SceneLightingData lighting = AmbientOnly(0.0f);
        lighting.spotLights.push_back(spot);
        return lighting;
    }

    SceneLightingData WithDirectional(const float intensity)
    {
        SceneLightingData lighting = AmbientOnly(0.0f);
        Renderer::Common::DirectionalLightData light;
        light.direction = Vector3(0.0f, 0.0f, -1.0f); // travelling into the quad
        light.intensity = intensity;
        lighting.directionalLights.push_back(light);
        return lighting;
    }
}

// ============================================================================
// The shared formulas
// ============================================================================

TEST(LightingMathTest, TheSpotConeIsOneInsideZeroOutsideAndARampBetween)
{
    using Renderer::Common::SpotConeFactor;
    const float inner = Degrees(20.0f);
    const float outer = Degrees(40.0f);
    EXPECT_FLOAT_EQ(SpotConeFactor(1.0f, inner, outer), 1.0f) << "on the axis";
    EXPECT_FLOAT_EQ(SpotConeFactor(std::cos(Degrees(15.0f)), inner, outer), 1.0f) << "inside the inner cone";
    EXPECT_FLOAT_EQ(SpotConeFactor(std::cos(Degrees(45.0f)), inner, outer), 0.0f) << "outside the outer cone";
    EXPECT_FLOAT_EQ(SpotConeFactor(-1.0f, inner, outer), 0.0f) << "behind the light";
    EXPECT_FLOAT_EQ(SpotConeFactor(std::cos(outer), inner, outer), 0.0f) << "at the outer edge";
    EXPECT_FLOAT_EQ(SpotConeFactor(std::cos(inner), inner, outer), 1.0f) << "at the inner edge";

    // Halfway in the cosine between the two edges
    const float mid = 0.5f * (std::cos(inner) + std::cos(outer));
    EXPECT_NEAR(SpotConeFactor(mid, inner, outer), 0.5f, 1e-4f);

    // Falling off: never rises as the angle grows
    float previous = 1.0f;
    for (int degrees = 0; degrees <= 60; ++degrees)
    {
        const float factor = SpotConeFactor(std::cos(Degrees(static_cast<float>(degrees))), inner, outer);
        EXPECT_LE(factor, previous + 1e-6f) << degrees << " degrees";
        previous = factor;
    }
}

TEST(LightingMathTest, AnInnerAngleAtOrPastTheOuterOneIsAHardEdgeAtTheOuter)
{
    using Renderer::Common::SpotConeFactor;
    const float outer = Degrees(30.0f);
    for (const float inner : {outer, Degrees(50.0f)})
    {
        EXPECT_FLOAT_EQ(SpotConeFactor(std::cos(Degrees(25.0f)), inner, outer), 1.0f);
        EXPECT_FLOAT_EQ(SpotConeFactor(std::cos(Degrees(35.0f)), inner, outer), 0.0f);
    }
}

TEST(LightingMathTest, SrgbConversionsRoundTripAndMatchKnownValues)
{
    using Renderer::Common::LinearToSrgb;
    using Renderer::Common::SrgbByteToLinear;
    using Renderer::Common::SrgbToLinear;
    EXPECT_FLOAT_EQ(SrgbToLinear(0.0f), 0.0f);
    EXPECT_NEAR(SrgbToLinear(1.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(SrgbToLinear(0.5f), 0.2140f, 1e-3f);
    EXPECT_NEAR(LinearToSrgb(0.25f), 0.5371f, 1e-3f);
    EXPECT_FLOAT_EQ(SrgbToLinear(0.02f), 0.02f / 12.92f) << "the linear segment near black";
    EXPECT_FLOAT_EQ(LinearToSrgb(-1.0f), 0.0f);
    EXPECT_FLOAT_EQ(SrgbToLinear(-1.0f), 0.0f);
    for (int i = 0; i <= 255; ++i)
    {
        const float encoded = static_cast<float>(i) / 255.0f;
        EXPECT_NEAR(LinearToSrgb(SrgbToLinear(encoded)), encoded, 1e-4f) << i;
        EXPECT_NEAR(SrgbByteToLinear(static_cast<unsigned char>(i)), SrgbToLinear(encoded), 1e-6f) << i;
    }
}

// ============================================================================
// Spot lights
// ============================================================================

TEST(SoftwareSpotLightTest, LightsInsideTheConeAndNothingOutsideIt)
{
    Scene scene;
    // 20 and 40 degrees at one unit: the cone reaches x = tan(40) = 0.84 on the quad, full strength to 0.36
    const Frame frame = scene.Draw(WithSpot(SpotAtTheQuad(20.0f, 40.0f)), scene.lit);
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

TEST(SoftwareSpotLightTest, TheLightFallsOffBetweenTheInnerAndOuterCone)
{
    Scene scene;
    const Frame soft = scene.Draw(WithSpot(SpotAtTheQuad(20.0f, 40.0f)), scene.lit);
    // The same light with a hard edge at 40 degrees: full strength all the way out, so its pixel is the unscaled one
    const Frame hard = scene.Draw(WithSpot(SpotAtTheQuad(40.0f, 40.0f)), scene.lit);

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

TEST(SoftwareSpotLightTest, AWideConeLightsLikeAPointLightOfTheSameRange)
{
    Scene scene;
    const Frame spot = scene.Draw(WithSpot(SpotAtTheQuad(80.0f, 85.0f, 0.7f)), scene.lit);

    SceneLightingData pointLighting = AmbientOnly(0.0f);
    PointLightData point;
    point.position = Vector3(0.0f, 0.0f, 1.0f);
    point.intensity = 0.7f;
    point.range = 10.0f;
    point.attenuation = 1.0f; // the spot's own attenuation
    pointLighting.pointLights.push_back(point);
    const Frame reference = scene.Draw(pointLighting, scene.lit);

    ASSERT_GT(reference.At(32, 32).r, 60);
    for (const int px : {32, 36, 42, 50, 58})
    {
        EXPECT_NEAR(spot.At(px, 32).r, reference.At(px, 32).r, 1) << "column " << px;
    }
    EXPECT_NEAR(spot.At(40, 50).g, reference.At(40, 50).g, 1);
}

TEST(SoftwareSpotLightTest, ALightFacingAwayOrOutOfRangeLightsNothing)
{
    Scene scene;
    SpotLightData away = SpotAtTheQuad(20.0f, 40.0f);
    away.direction = Vector3(0.0f, 0.0f, 1.0f);
    EXPECT_EQ(scene.Draw(WithSpot(away), scene.lit).At(32, 32).r, 0);

    SpotLightData tooShort = SpotAtTheQuad(20.0f, 40.0f);
    tooShort.range = 0.5f; // the quad is one unit away
    EXPECT_EQ(scene.Draw(WithSpot(tooShort), scene.lit).At(32, 32).r, 0);

    SpotLightData noRange = SpotAtTheQuad(20.0f, 40.0f);
    noRange.range = 0.0f;
    EXPECT_EQ(scene.Draw(WithSpot(noRange), scene.lit).At(32, 32).r, 0);

    SpotLightData noAxis = SpotAtTheQuad(20.0f, 40.0f);
    noAxis.direction = Vector3(0.0f, 0.0f, 0.0f);
    EXPECT_EQ(scene.Draw(WithSpot(noAxis), scene.lit).At(32, 32).r, 0);

    // A non-unit axis is normalised
    SpotLightData scaled = SpotAtTheQuad(20.0f, 40.0f);
    const Frame unit = scene.Draw(WithSpot(scaled), scene.lit);
    scaled.direction = Vector3(0.0f, 0.0f, -7.0f);
    const Frame big = scene.Draw(WithSpot(scaled), scene.lit);
    EXPECT_EQ(big.At(32, 32).r, unit.At(32, 32).r);
    EXPECT_EQ(big.At(48, 32).r, unit.At(48, 32).r);
}

TEST(SoftwareSpotLightTest, OnlyTheFirstFourSpotLightsCountAsOnOpenGL)
{
    Scene scene;
    SceneLightingData lighting = AmbientOnly(0.0f);
    SpotLightData away = SpotAtTheQuad(20.0f, 40.0f);
    away.direction = Vector3(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < SceneLightingData::MAX_SPOT_LIGHTS; ++i)
    {
        lighting.spotLights.push_back(away);
    }
    lighting.spotLights.push_back(SpotAtTheQuad(20.0f, 40.0f)); // the fifth, which would light the quad
    EXPECT_EQ(scene.Draw(lighting, scene.lit).At(32, 32).r, 0);

    // As the last of four it counts
    lighting.spotLights.erase(lighting.spotLights.begin());
    EXPECT_GT(scene.Draw(lighting, scene.lit).At(32, 32).r, 60);
}

TEST(SoftwareSpotLightTest, TwoSpotLightsAdd)
{
    Scene scene;
    // A dim surface, so that the sum stays below 255 (the specular term is not scaled by the intensity)
    scene.lit->SetColor("uAlbedo", 0.3f, 0.3f, 0.3f, 1.0f);
    const int one = scene.Draw(WithSpot(SpotAtTheQuad(20.0f, 40.0f, 0.3f)), scene.lit).At(32, 32).r;
    SceneLightingData both = WithSpot(SpotAtTheQuad(20.0f, 40.0f, 0.3f));
    both.spotLights.push_back(SpotAtTheQuad(20.0f, 40.0f, 0.3f));
    const int two = scene.Draw(both, scene.lit).At(32, 32).r;
    ASSERT_GT(one, 15);
    ASSERT_LT(two, 200);
    EXPECT_NEAR(two, 2 * one, 2);
}

// ============================================================================
// Emissive
// ============================================================================

TEST(SoftwareEmissiveTest, EmissiveIsAddedAfterLightingAndNotChangedByIt)
{
    Scene scene;
    scene.lit->SetColor("uAlbedo", 0.0f, 0.0f, 0.0f, 1.0f); // black: lights add nothing to it
    scene.lit->SetVec3("uEmissive", 0.2f, 0.4f, 0.6f);

    const Frame dark = scene.Draw(AmbientOnly(0.0f), scene.lit);
    EXPECT_NEAR(dark.At(32, 32).r, 51, 1);
    EXPECT_NEAR(dark.At(32, 32).g, 102, 1);
    EXPECT_NEAR(dark.At(32, 32).b, 153, 1);

    // A strong light and ambient change nothing on a black surface: the glow is the same
    SceneLightingData bright = WithDirectional(1.0f);
    bright.ambientColor = Vector3(1.0f, 1.0f, 1.0f);
    const Frame lit = scene.Draw(bright, scene.lit);
    EXPECT_EQ(lit.At(32, 32).r, dark.At(32, 32).r);
    EXPECT_EQ(lit.At(32, 32).g, dark.At(32, 32).g);
    EXPECT_EQ(lit.At(32, 32).b, dark.At(32, 32).b);
    EXPECT_EQ(lit.At(5, 50).g, dark.At(5, 50).g);
}

TEST(SoftwareEmissiveTest, EmissiveAddsToTheLitSurface)
{
    Scene scene;
    scene.lit->SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    const int without = scene.Draw(AmbientOnly(0.4f), scene.lit).At(32, 32).r; // 0.5 * 0.4 = 0.2
    EXPECT_NEAR(without, 51, 1);
    scene.lit->SetVec3("uEmissive", 0.3f, 0.0f, 0.0f);
    const Frame with = scene.Draw(AmbientOnly(0.4f), scene.lit);
    EXPECT_NEAR(with.At(32, 32).r, 128, 1); // 0.2 + 0.3
    EXPECT_NEAR(with.At(32, 32).g, 51, 1);
}

TEST(SoftwareEmissiveTest, TheEmissiveTextureMultipliesTheColour)
{
    Scene scene;
    scene.lit->SetColor("uAlbedo", 0.0f, 0.0f, 0.0f, 1.0f);
    // Left texel red, right texel green
    ITexture *glow = scene.Texture({255, 0, 0, 255, 0, 255, 0, 255}, 2, 1);
    scene.lit->SetAuxTexture(AuxTexture::Emissive, glow);
    ASSERT_EQ(scene.lit->GetAuxTexture(AuxTexture::Emissive), glow);
    scene.lit->SetInt("uHasEmissiveTexture", 1);

    // The colour is black by default: the texture alone shows nothing
    Frame frame = scene.Draw(AmbientOnly(0.0f), scene.lit);
    EXPECT_EQ(frame.At(10, 32).r, 0);
    EXPECT_EQ(frame.At(54, 32).g, 0);

    scene.lit->SetVec3("uEmissive", 1.0f, 1.0f, 1.0f);
    frame = scene.Draw(AmbientOnly(0.0f), scene.lit);
    EXPECT_NEAR(frame.At(10, 32).r, 255, 1);
    EXPECT_EQ(frame.At(10, 32).g, 0);
    EXPECT_EQ(frame.At(54, 32).r, 0);
    EXPECT_NEAR(frame.At(54, 32).g, 255, 1);

    scene.lit->SetVec3("uEmissive", 0.5f, 0.5f, 0.5f);
    frame = scene.Draw(AmbientOnly(0.0f), scene.lit);
    EXPECT_NEAR(frame.At(10, 32).r, 128, 1);
}

TEST(SoftwareEmissiveTest, AnInvalidTextureIsStoredAsNoneSoTheHasTextureFlagsAgreeWithWhatIsDrawn)
{
    Scene scene;
    Renderer::Software::SWTexture empty; // no pixels: not valid
    scene.lit->SetAuxTexture(AuxTexture::Emissive, &empty);
    scene.lit->SetAuxTexture(AuxTexture::Occlusion, &empty);
    EXPECT_EQ(scene.lit->GetAuxTexture(AuxTexture::Emissive), nullptr);
    EXPECT_EQ(scene.lit->GetAuxTexture(AuxTexture::Occlusion), nullptr);
}

TEST(SoftwareEmissiveTest, UnlitMaterialsIgnoreEmissive)
{
    Scene scene;
    scene.unlit->SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    scene.unlit->SetVec3("uEmissive", 0.4f, 0.4f, 0.4f);
    EXPECT_NEAR(scene.Draw(AmbientOnly(0.0f), scene.unlit).At(32, 32).r, 128, 1);
}

// ============================================================================
// Occlusion
// ============================================================================

TEST(SoftwareOcclusionTest, TheOcclusionTextureScalesTheAmbientLightOnly)
{
    Scene scene;
    // Left texel fully open (255), right texel half occluded (128)
    ITexture *occlusion = scene.Texture({255, 255, 255, 255, 128, 128, 128, 255}, 2, 1);
    scene.lit->SetAuxTexture(AuxTexture::Occlusion, occlusion);
    scene.lit->SetInt("uHasOcclusionTexture", 1);

    const Frame ambient = scene.Draw(AmbientOnly(1.0f), scene.lit);
    EXPECT_NEAR(ambient.At(10, 32).r, 255, 1) << "open";
    EXPECT_NEAR(ambient.At(54, 32).r, 128, 2) << "half occluded";

    // Half the strength: 1 + 0.5 * (0.502 - 1) = 0.751
    scene.lit->SetFloat("uOcclusionStrength", 0.5f);
    EXPECT_NEAR(scene.Draw(AmbientOnly(1.0f), scene.lit).At(54, 32).r, 191, 2);
    scene.lit->SetFloat("uOcclusionStrength", 0.0f);
    EXPECT_NEAR(scene.Draw(AmbientOnly(1.0f), scene.lit).At(54, 32).r, 255, 1) << "strength 0 is no occlusion";
    scene.lit->SetFloat("uOcclusionStrength", 1.0f);

    // A direct light is not occluded: the same pixel as without the texture
    const Frame direct = scene.Draw(WithDirectional(0.6f), scene.lit);
    scene.lit->SetInt("uHasOcclusionTexture", 0);
    scene.lit->SetAuxTexture(AuxTexture::Occlusion, nullptr);
    const Frame reference = scene.Draw(WithDirectional(0.6f), scene.lit);
    EXPECT_GT(reference.At(54, 32).r, 60);
    EXPECT_EQ(direct.At(54, 32).r, reference.At(54, 32).r);
    EXPECT_EQ(direct.At(10, 32).r, reference.At(10, 32).r);
}

// ============================================================================
// Colour space
// ============================================================================

TEST(SoftwareColorSpaceTest, GammaIsTheDefaultAndLeavesLitOutputAsItWas)
{
    Scene scene;
    scene.lit->SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    SceneLightingData lighting = AmbientOnly(0.5f);
    EXPECT_EQ(lighting.colorSpace, ColorSpace::Gamma);
    const Frame frame = scene.Draw(lighting, scene.lit);
    EXPECT_NEAR(frame.At(32, 32).r, 64, 1) << "0.5 x 0.5, written as it is";

    // The sRGB flags are read only in linear lighting
    scene.lit->SetInt("uBaseColorSrgb", 1);
    scene.lit->SetInt("uEmissiveTextureSrgb", 1);
    const Frame flagged = scene.Draw(lighting, scene.lit);
    EXPECT_EQ(flagged.At(32, 32).r, frame.At(32, 32).r);

    // An sRGB texture is multiplied as it is
    ITexture *grey = scene.Texture({128, 128, 128, 255}, 1, 1);
    scene.lit->SetTexture(grey);
    scene.lit->SetColor("uAlbedo", 1.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(scene.Draw(AmbientOnly(1.0f), scene.lit).At(32, 32).r, 128, 1);
}

TEST(SoftwareColorSpaceTest, LinearLightingEncodesTheResultForDisplay)
{
    Scene scene;
    scene.lit->SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    SceneLightingData lighting = AmbientOnly(0.5f);
    lighting.colorSpace = ColorSpace::Linear;
    // 0.5 x 0.5 = 0.25 linear, which displays as 0.537
    const Frame frame = scene.Draw(lighting, scene.lit);
    EXPECT_NEAR(frame.At(32, 32).r, 137, 1);
    EXPECT_NEAR(frame.At(32, 32).b, 137, 1);

    // Black and full white stay put
    scene.lit->SetColor("uAlbedo", 0.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(scene.Draw(lighting, scene.lit).At(32, 32).r, 0);
    scene.lit->SetColor("uAlbedo", 1.0f, 1.0f, 1.0f, 1.0f);
    lighting.ambientColor = Vector3(1.0f, 1.0f, 1.0f);
    EXPECT_EQ(scene.Draw(lighting, scene.lit).At(32, 32).r, 255);
}

TEST(SoftwareColorSpaceTest, LinearLightingDecodesSrgbTexturesOnly)
{
    Scene scene;
    ITexture *grey = scene.Texture({128, 128, 128, 255}, 1, 1);
    scene.lit->SetTexture(grey);
    SceneLightingData lighting = AmbientOnly(1.0f);
    lighting.colorSpace = ColorSpace::Linear;

    // An sRGB texture is decoded then encoded again: the colour it shows is its own
    scene.lit->SetInt("uBaseColorSrgb", 1);
    EXPECT_NEAR(scene.Draw(lighting, scene.lit).At(32, 32).r, 128, 1);

    // A texture of plain numbers (not sRGB) is taken as linear, so it displays brighter
    scene.lit->SetInt("uBaseColorSrgb", 0);
    EXPECT_NEAR(scene.Draw(lighting, scene.lit).At(32, 32).r, 188, 2);
}

TEST(SoftwareColorSpaceTest, LinearLightingEncodesTheEmissiveSumAndDecodesItsTexture)
{
    Scene scene;
    scene.lit->SetColor("uAlbedo", 0.0f, 0.0f, 0.0f, 1.0f);
    SceneLightingData lighting = AmbientOnly(0.0f);
    lighting.colorSpace = ColorSpace::Linear;

    scene.lit->SetVec3("uEmissive", 0.25f, 0.25f, 0.25f);
    EXPECT_NEAR(scene.Draw(lighting, scene.lit).At(32, 32).g, 137, 1);

    ITexture *glow = scene.Texture({128, 128, 128, 255}, 1, 1);
    scene.lit->SetAuxTexture(AuxTexture::Emissive, glow);
    scene.lit->SetVec3("uEmissive", 1.0f, 1.0f, 1.0f);
    scene.lit->SetInt("uEmissiveTextureSrgb", 1);
    EXPECT_NEAR(scene.Draw(lighting, scene.lit).At(32, 32).g, 128, 1) << "decoded, then encoded again";
    scene.lit->SetInt("uEmissiveTextureSrgb", 0);
    EXPECT_NEAR(scene.Draw(lighting, scene.lit).At(32, 32).g, 188, 2);
}

TEST(SoftwareColorSpaceTest, UnlitDrawsTheSameInBothColourSpaces)
{
    Scene scene;
    scene.unlit->SetColor("uAlbedo", 0.5f, 0.25f, 1.0f, 1.0f);
    SceneLightingData gamma = AmbientOnly(0.2f);
    SceneLightingData linear = gamma;
    linear.colorSpace = ColorSpace::Linear;
    const Frame a = scene.Draw(gamma, scene.unlit);
    const Frame b = scene.Draw(linear, scene.unlit);
    EXPECT_EQ(a.rgba, b.rgba);
    EXPECT_NEAR(a.At(32, 32).r, 128, 1);
}

TEST(SoftwareColorSpaceTest, OcclusionIsDataAndNeverDecoded)
{
    Scene scene;
    ITexture *occlusion = scene.Texture({128, 128, 128, 255}, 1, 1);
    scene.lit->SetAuxTexture(AuxTexture::Occlusion, occlusion);
    scene.lit->SetInt("uHasOcclusionTexture", 1);
    scene.lit->SetInt("uBaseColorSrgb", 1);
    scene.lit->SetInt("uEmissiveTextureSrgb", 1);
    SceneLightingData lighting = AmbientOnly(1.0f);
    lighting.colorSpace = ColorSpace::Linear;
    // 0.502 of the ambient in linear light, encoded: 0.502 -> 0.7366 -> 188
    EXPECT_NEAR(scene.Draw(lighting, scene.lit).At(32, 32).r, 188, 2);
}
