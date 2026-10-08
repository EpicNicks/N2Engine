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
#include <renderer/common/RenderState.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

// The software renderer's normal maps and metallic-roughness PBR (#3 P4b): real rasterizations of a quad facing +z,
// read back headless. A normal-mapped flat quad is compared with the same quad geometrically tilted (they must agree),
// and PBR pixels with the closed-form BRDF of LightingMath.hpp. Checks use tolerances of a few 8-bit levels, never
// exact images, because Debug and Release may round floats differently.

using Renderer::Common::AuxTexture;
using Renderer::Common::ColorSpace;
using Renderer::Common::CullMode;
using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::ITexture;
using Renderer::Common::MeshData;
using Renderer::Common::RenderState;
using Renderer::Common::SceneLightingData;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::Vertex;
using Renderer::Software::SoftwareRenderer;
using N2Engine::Math::Vector3;

namespace
{
    constexpr int Size = 64;
    constexpr float kInvSqrt2 = 0.70710678f;

    constexpr float Identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

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

    /// A quad over x, y in -1..1 at z = 0, facing +z, with u across x and v up y (tangent +x, handedness `w`, so
    /// the bitangent cross(N, T) * w is +y for w = +1); `tangent` false leaves the tangent zero (none)
    MeshData Quad(const bool tangent, const float w = 1.0f)
    {
        const float t = tangent ? 1.0f : 0.0f;
        const float h = tangent ? w : 0.0f;
        MeshData data;
        data.vertices = {
            Vertex{{-1, -1, 0}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {t, 0, 0, h}},
            Vertex{{1, -1, 0}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}, {t, 0, 0, h}},
            Vertex{{1, 1, 0}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}, {t, 0, 0, h}},
            Vertex{{-1, 1, 0}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}, {t, 0, 0, h}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    struct Scene
    {
        SoftwareRenderer renderer;
        IMesh *mesh = nullptr;         // tangent +x, w = +1
        IMesh *mirroredMesh = nullptr; // tangent +x, w = -1
        IMesh *bareMesh = nullptr;     // no tangent
        IMaterial *lit = nullptr;

        Scene()
        {
            EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
            renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
            mesh = renderer.CreateMesh(Quad(true, 1.0f));
            mirroredMesh = renderer.CreateMesh(Quad(true, -1.0f));
            bareMesh = renderer.CreateMesh(Quad(false));
            lit = renderer.CreateMaterial(renderer.GetStandardLitShader());
            // Glossy: Blinn-Phong highlight so tight that the Blinn-Phong tests see the diffuse term alone
            lit->SetFloat("uSmoothness", 1.0f);
        }

        ~Scene() { renderer.Shutdown(); }

        Frame Draw(const SceneLightingData &lighting, IMaterial *material, IMesh *which = nullptr,
                   const float *model = Identity, const RenderState &state = RenderState{},
                   const Vector3 &camera = Vector3(0, 0, 100))
        {
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            renderer.UpdateSceneLighting(lighting, camera);
            renderer.DrawMesh(which ? which : mesh, model, material, state);
            renderer.EndFrame();
            renderer.Present();
            Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            return frame;
        }

        /// A 1 x 1 data texture (never sRGB, as GpuCache makes normal and metallic-roughness textures)
        ITexture *Texel(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
        {
            TextureOptions nearest;
            nearest.filter = TextureFilter::Nearest;
            nearest.srgb = false;
            const std::vector<std::uint8_t> rgba = {r, g, b, 255};
            ITexture *texture = renderer.CreateTexture(rgba.data(), 1, 1, 4, nearest);
            EXPECT_NE(texture, nullptr);
            return texture;
        }
    };

    /// A texel byte for a component of a unit vector
    std::uint8_t Encode(const float component)
    {
        return static_cast<std::uint8_t>(std::lround((component * 0.5f + 0.5f) * 255.0f));
    }

    /// A normal map texel for the tangent-space normal (x, y, z), normalised
    ITexture *NormalTexel(Scene &scene, const float x, const float y, const float z)
    {
        const float length = std::sqrt(x * x + y * y + z * z);
        return scene.Texel(Encode(x / length), Encode(y / length), Encode(z / length));
    }

    SceneLightingData AmbientOnly(const float grey)
    {
        SceneLightingData lighting;
        lighting.ambientColor = Vector3(grey, grey, grey);
        return lighting;
    }

    /// One directional light travelling along `direction` (so lighting from -direction), no ambient
    SceneLightingData DirectionalFrom(const Vector3 &toLight, const float intensity)
    {
        SceneLightingData lighting = AmbientOnly(0.0f);
        Renderer::Common::DirectionalLightData light;
        light.direction = Vector3(-toLight.x, -toLight.y, -toLight.z);
        light.intensity = intensity;
        lighting.directionalLights.push_back(light);
        return lighting;
    }

    constexpr int kCentre = 32;
}

// ============================================================================
// Normal maps
// ============================================================================

TEST(SoftwareNormalMapTest, AFlatNormalMapChangesNothing)
{
    Scene scene;
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    const int without = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, 0, 0, 1));
    const int with = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_GT(without, 40);
    EXPECT_NEAR(with, without, 2) << "a texel of (0.5, 0.5, 1) is the vertex normal";
}

TEST(SoftwareNormalMapTest, ANormalMapTiltsTheNormalLikeTiltingTheSurface)
{
    Scene scene;
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);

    // The reference: the same quad turned 45 degrees about y, so its normal is the light's direction, no normal map
    const int tilted = scene.Draw(light, scene.lit, nullptr, RotateY45).At(kCentre, kCentre).r;
    const int flat = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_GT(tilted, flat + 30) << "facing the light is much brighter than facing away at 45 degrees";

    // A flat quad whose normal map leans toward +x by 45 degrees: the tangent is +x
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, kInvSqrt2, 0, kInvSqrt2));
    const int mapped = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(mapped, tilted, 4);
}

TEST(SoftwareNormalMapTest, TheTexelsGreenIsUpTheBitangentAndTheHandednessFlipsIt)
{
    Scene scene;
    const SceneLightingData fromAbove = DirectionalFrom(Vector3(0, kInvSqrt2, kInvSqrt2), 0.8f);
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, 0, kInvSqrt2, kInvSqrt2)); // leaning up (+v)

    // w = +1: the bitangent cross(N, T) is +y, so the normal leans up, toward the light
    const int lit = scene.Draw(fromAbove, scene.lit, scene.mesh).At(kCentre, kCentre).r;
    EXPECT_GT(lit, 150);
    // w = -1: it leans down, away from the light
    const int dark = scene.Draw(fromAbove, scene.lit, scene.mirroredMesh).At(kCentre, kCentre).r;
    EXPECT_LT(dark, 12);
}

TEST(SoftwareNormalMapTest, TheTangentTurnsWithTheModelMatrix)
{
    Scene scene;
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, kInvSqrt2, 0, kInvSqrt2)); // leaning along +T
    const SceneLightingData fromAbove = DirectionalFrom(Vector3(0, kInvSqrt2, kInvSqrt2), 0.8f);
    const SceneLightingData fromTheRight = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);

    // Turned 90 degrees about z, +T is world +y: the normal leans up
    const int up = scene.Draw(fromAbove, scene.lit, nullptr, RotateZ90).At(kCentre, kCentre).r;
    const int right = scene.Draw(fromTheRight, scene.lit, nullptr, RotateZ90).At(kCentre, kCentre).r;
    EXPECT_GT(up, 150) << "toward a light above";
    EXPECT_LT(right, up - 40) << "and not toward a light on the right";
}

TEST(SoftwareNormalMapTest, AMirroringModelMatrixFlipsTheHandednessSoUpStaysUp)
{
    Scene scene;
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, 0, kInvSqrt2, kInvSqrt2)); // leaning up (+v)
    const SceneLightingData fromAbove = DirectionalFrom(Vector3(0, kInvSqrt2, kInvSqrt2), 0.8f);
    RenderState bothFaces;
    bothFaces.cull = CullMode::None; // mirroring reverses the winding

    // The mirror reverses x only: v still grows up the quad, so the normal still leans up
    const int mirrored = scene.Draw(fromAbove, scene.lit, nullptr, MirrorX, bothFaces).At(kCentre, kCentre).r;
    EXPECT_GT(mirrored, 150);
}

TEST(SoftwareNormalMapTest, ANegativeScaleFlipsTheMapsXAndY)
{
    Scene scene;
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, kInvSqrt2, 0, kInvSqrt2));
    scene.lit->SetFloat("uNormalScale", 1.0f);
    const int toward = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    scene.lit->SetFloat("uNormalScale", -1.0f);
    const int away = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_GT(toward, 150);
    EXPECT_LT(away, 12) << "leaning along -x: away from a light on the right";
}

TEST(SoftwareNormalMapTest, ATinyModelScaleDoesNotSwitchNormalMappingOff)
{
    Scene scene;
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, kInvSqrt2, 0, kInvSqrt2));
    const int full = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    ASSERT_GT(full, 150);

    // The quad scaled down by 1e-4 (its tangent in world space is 1e-4 long) and the projection scaled up to match
    constexpr float tiny = 1e-4f;
    constexpr float model[16] = {tiny, 0, 0, 0, 0, tiny, 0, 0, 0, 0, tiny, 0, 0, 0, 0, 1};
    constexpr float projection[16] = {1.0f / tiny, 0, 0, 0, 0, 1.0f / tiny, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    scene.renderer.BeginFrame();
    scene.renderer.SetViewProjection(Identity, projection);
    scene.renderer.UpdateSceneLighting(light, Vector3(0, 0, 100));
    scene.renderer.DrawMesh(scene.mesh, model, scene.lit, RenderState{});
    scene.renderer.EndFrame();
    scene.renderer.Present();
    Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(Size) * Size * 4)};
    scene.renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
    EXPECT_NEAR(frame.At(kCentre, kCentre).r, full, 3) << "the normal map still applies";
}

TEST(SoftwareNormalMapTest, AMeshWithoutTangentsIsDrawnWithoutNormalMapping)
{
    Scene scene;
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    const Frame plain = scene.Draw(light, scene.lit, scene.bareMesh);
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, kInvSqrt2, 0, kInvSqrt2));
    const Frame mapped = scene.Draw(light, scene.lit, scene.bareMesh);
    EXPECT_EQ(plain.rgba, mapped.rgba) << "no tangent: the vertex normal is used";
}

TEST(SoftwareNormalMapTest, TheScaleMultipliesTheTexelsXAndY)
{
    Scene scene;
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, kInvSqrt2, 0, kInvSqrt2));

    scene.lit->SetFloat("uNormalScale", 0.0f);
    const int zero = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    scene.lit->SetAuxTexture(AuxTexture::Normal, nullptr);
    const int none = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(zero, none, 2) << "scale 0 is a flat surface";

    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, kInvSqrt2, 0, kInvSqrt2));
    scene.lit->SetFloat("uNormalScale", 0.5f);
    const int half = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    scene.lit->SetFloat("uNormalScale", 1.0f);
    const int full = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_GT(half, zero + 20) << "half the lean already faces the light more";

    // Half of x scales the leaning normal (0.71, 0, 0.71) to (0.45, 0, 0.89) after normalising: the surface tilted by
    // atan(0.5) = 26.57 degrees, which the same quad turned that far shows too; and the full scale is the 45 degree turn
    constexpr float s = 0.4472136f;
    constexpr float c = 0.8944272f;
    constexpr float tiltedHalf[16] = {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1};
    scene.lit->SetAuxTexture(AuxTexture::Normal, nullptr);
    EXPECT_NEAR(half, scene.Draw(light, scene.lit, nullptr, tiltedHalf).At(kCentre, kCentre).r, 4);
    EXPECT_NEAR(full, scene.Draw(light, scene.lit, nullptr, RotateY45).At(kCentre, kCentre).r, 4);
}

TEST(SoftwareNormalMapTest, TheNormalMapIsDataNeverDecodedAsSrgb)
{
    Scene scene;
    // A texel of (128, 128, 255) is the flat normal. If it were decoded as sRGB, x and y would read as -0.78
    scene.lit->SetAuxTexture(AuxTexture::Normal, scene.Texel(128, 128, 255));
    const SceneLightingData light = DirectionalFrom(Vector3(0, 0, 1), 0.5f);
    const int mapped = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    scene.lit->SetAuxTexture(AuxTexture::Normal, nullptr);
    EXPECT_NEAR(mapped, scene.Draw(light, scene.lit).At(kCentre, kCentre).r, 2);
}

TEST(SoftwareNormalMapTest, AnUnlitMaterialIgnoresTheNormalMap)
{
    Scene scene;
    IMaterial *unlit = scene.renderer.CreateMaterial(scene.renderer.GetStandardUnlitShader());
    unlit->SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    const Frame before = scene.Draw(AmbientOnly(0.0f), unlit);
    unlit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, kInvSqrt2, 0, kInvSqrt2));
    EXPECT_EQ(before.rgba, scene.Draw(AmbientOnly(0.0f), unlit).rgba);
}

// ============================================================================
// PBR
// ============================================================================

namespace
{
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

    int Byte(const float value)
    {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    /// A PBR material: lit shader with uPbr = 1
    void MakePbr(IMaterial *material, const float metallic, const float smoothness)
    {
        material->SetInt("uPbr", 1);
        material->SetFloat("uMetallic", metallic);
        material->SetFloat("uSmoothness", smoothness);
    }
}

TEST(PbrMathTest, TheDistributionVisibilityAndFresnelHaveTheirKnownValues)
{
    using namespace Renderer::Common;
    // D at the peak is 1 / (pi * alpha^2)
    EXPECT_NEAR(GgxDistribution(1.0f, 0.5f), 1.0f / (kPi * 0.25f), 1e-3f);
    EXPECT_NEAR(GgxDistribution(1.0f, 1.0f), 1.0f / kPi, 1e-5f) << "alpha 1 is a uniform lobe";
    EXPECT_LT(GgxDistribution(0.5f, 0.25f), GgxDistribution(1.0f, 0.25f)) << "falls off away from the half vector";
    // Normal incidence: G / (4 NdotL NdotV) = 1 / 4 for any roughness
    for (const float alpha : {0.05f, 0.25f, 0.5f, 1.0f})
    {
        EXPECT_NEAR(SmithGgxVisibility(1.0f, 1.0f, alpha), 0.25f, 1e-5f) << "alpha " << alpha;
    }
    EXPECT_GT(SmithGgxVisibility(0.3f, 0.3f, 0.25f), 0.25f) << "grazing angles: the denominator shrinks faster than G";
    EXPECT_FLOAT_EQ(SchlickWeight(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(SchlickWeight(0.0f), 1.0f);
    EXPECT_NEAR(SchlickWeight(0.5f), 0.03125f, 1e-6f);
    // Clamps
    EXPECT_FLOAT_EQ(PerceptualRoughness(1.0f), kMinPerceptualRoughness);
    EXPECT_FLOAT_EQ(PerceptualRoughness(0.0f), 1.0f);
    EXPECT_FLOAT_EQ(PerceptualRoughness(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(PerceptualRoughness(0.0f, 0.5f), 0.5f) << "times the texture's green";
    EXPECT_FLOAT_EQ(PerceptualRoughness(0.5f, 0.0f), kMinPerceptualRoughness);
}

TEST(PbrMathTest, TheEnvironmentBrdfReflectsEverythingAtNormalIncidenceOnAMirrorAndLessWhenRough)
{
    using namespace Renderer::Common;
    const EnvBrdf mirror = EnvironmentBrdf(0.0f, 1.0f);
    EXPECT_NEAR(mirror.scale + mirror.bias, 1.0f, 0.01f) << "a white mirror reflects the whole environment";
    const EnvBrdf rough = EnvironmentBrdf(1.0f, 1.0f);
    EXPECT_LT(rough.scale + rough.bias, mirror.scale + mirror.bias);
    EXPECT_GT(rough.scale + rough.bias, 0.3f);
    // At grazing angles the reflectance rises toward 1 even for F0 = 0 (the bias)
    EXPECT_GT(EnvironmentBrdf(0.5f, 0.05f).bias, EnvironmentBrdf(0.5f, 1.0f).bias);
}

TEST(SoftwarePbrTest, ADielectricIsADiffuseSurfaceWithAFaintHighlight)
{
    Scene scene;
    MakePbr(scene.lit, 0.0f, 0.5f);
    const Frame frame = scene.Draw(DirectionalFrom(Vector3(0, 0, 1), 0.3f), scene.lit);
    EXPECT_NEAR(frame.At(kCentre, kCentre).r, Byte(NormalIncidenceReply(1.0f, 0.0f, 0.5f, 0.3f)), 2);
    EXPECT_NEAR(frame.At(kCentre, kCentre).g, frame.At(kCentre, kCentre).r, 1) << "white stays grey";
}

TEST(SoftwarePbrTest, AMetalHasNoDiffuseAndTintsItsHighlightWithTheBaseColour)
{
    Scene scene;
    scene.lit->SetColor("uAlbedo", 1.0f, 0.5f, 0.25f, 1.0f);
    MakePbr(scene.lit, 1.0f, 0.5f);
    const SceneLightingData light = DirectionalFrom(Vector3(0, 0, 1), 0.1f);
    const Frame metal = scene.Draw(light, scene.lit);
    EXPECT_NEAR(metal.At(kCentre, kCentre).r, Byte(NormalIncidenceReply(1.0f, 1.0f, 0.5f, 0.1f)), 2);
    EXPECT_NEAR(metal.At(kCentre, kCentre).g, Byte(NormalIncidenceReply(0.5f, 1.0f, 0.5f, 0.1f)), 2);
    EXPECT_NEAR(metal.At(kCentre, kCentre).b, Byte(NormalIncidenceReply(0.25f, 1.0f, 0.5f, 0.1f)), 2);
    EXPECT_GT(metal.At(kCentre, kCentre).r, metal.At(kCentre, kCentre).g + 20);

    // The same colour as a dielectric is a different, diffuse look: red much dimmer here, where the metal reflects
    MakePbr(scene.lit, 0.0f, 0.5f);
    const Frame dielectric = scene.Draw(light, scene.lit);
    EXPECT_GT(metal.At(kCentre, kCentre).r, dielectric.At(kCentre, kCentre).r + 40);
}

TEST(SoftwarePbrTest, UnderAmbientLightAMetalReflectsItsColourAndADielectricScattersIt)
{
    Scene scene;
    scene.lit->SetColor("uAlbedo", 0.8f, 0.8f, 0.8f, 1.0f);
    const SceneLightingData ambient = AmbientOnly(1.0f);
    const auto env = Renderer::Common::EnvironmentBrdf(Renderer::Common::PerceptualRoughness(0.5f), 1.0f);

    MakePbr(scene.lit, 1.0f, 0.5f);
    EXPECT_NEAR(scene.Draw(ambient, scene.lit).At(kCentre, kCentre).r, Byte(0.8f * env.scale + env.bias), 3)
        << "a metal: F0 = the base colour";
    MakePbr(scene.lit, 0.0f, 0.5f);
    const float f0 = Renderer::Common::kDielectricF0;
    const float reflected = f0 * env.scale + env.bias;
    EXPECT_NEAR(scene.Draw(ambient, scene.lit).At(kCentre, kCentre).r, Byte(0.8f * (1.0f - reflected) + reflected), 3)
        << "a dielectric: its diffuse colour with what the faint reflection did not take, plus the reflection";

    // No light, no ambient: nothing to reflect
    MakePbr(scene.lit, 1.0f, 0.5f);
    EXPECT_EQ(scene.Draw(AmbientOnly(0.0f), scene.lit).At(kCentre, kCentre).r, 0);
}

TEST(SoftwarePbrTest, RoughnessSpreadsTheHighlightSoASmoothSurfaceHasABrighterPeak)
{
    Scene scene;
    MakePbr(scene.lit, 1.0f, 0.9f); // roughness 0.1
    const SceneLightingData light = DirectionalFrom(Vector3(0, 0, 1), 0.0001f);
    const int smooth = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    MakePbr(scene.lit, 1.0f, 0.5f);
    const int rough = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_GT(smooth, rough + 30);
    EXPECT_NEAR(smooth, Byte(NormalIncidenceReply(1.0f, 1.0f, 0.9f, 0.0001f)), 2);
}

TEST(SoftwarePbrTest, TheMetallicRoughnessTextureMultipliesTheFactors)
{
    Scene scene;
    const SceneLightingData light = DirectionalFrom(Vector3(0, 0, 1), 0.1f);

    // Blue is the metallic factor: a texel with none makes a metal a dielectric
    MakePbr(scene.lit, 1.0f, 0.5f);
    scene.lit->SetAuxTexture(AuxTexture::MetallicRoughness, scene.Texel(0, 255, 0));
    const int notMetal = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    MakePbr(scene.lit, 0.0f, 0.5f);
    scene.lit->SetAuxTexture(AuxTexture::MetallicRoughness, nullptr);
    const int dielectric = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(notMetal, dielectric, 2) << "roughness 1 x 0.5 of the texel's green is 0.5, as the factor alone";

    // A full blue texel leaves the factor alone
    MakePbr(scene.lit, 1.0f, 0.5f);
    scene.lit->SetAuxTexture(AuxTexture::MetallicRoughness, scene.Texel(0, 255, 255));
    const int metalWithTexel = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    scene.lit->SetAuxTexture(AuxTexture::MetallicRoughness, nullptr);
    const int metal = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(metalWithTexel, metal, 2);
    EXPECT_GT(metal, notMetal + 40);

    // Green is the roughness: half of it makes the surface smoother, its highlight brighter
    MakePbr(scene.lit, 1.0f, 0.0f); // roughness 1
    const SceneLightingData faint = DirectionalFrom(Vector3(0, 0, 1), 0.02f);
    scene.lit->SetAuxTexture(AuxTexture::MetallicRoughness, nullptr);
    const int roughMetal = scene.Draw(faint, scene.lit).At(kCentre, kCentre).r;
    scene.lit->SetAuxTexture(AuxTexture::MetallicRoughness, scene.Texel(0, 128, 255));
    const int smoother = scene.Draw(faint, scene.lit).At(kCentre, kCentre).r;
    EXPECT_GT(smoother, roughMetal + 10);
}

TEST(SoftwarePbrTest, TheMetallicRoughnessTextureIsIgnoredWithoutPbr)
{
    Scene scene;
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.6f);
    const Frame plain = scene.Draw(light, scene.lit);
    scene.lit->SetAuxTexture(AuxTexture::MetallicRoughness, scene.Texel(0, 0, 255));
    scene.lit->SetFloat("uMetallic", 1.0f); // Blinn-Phong has no metal
    EXPECT_EQ(plain.rgba, scene.Draw(light, scene.lit).rgba);
}

TEST(SoftwarePbrTest, TheNormalMapShadesPbrAsItDoesBlinnPhong)
{
    Scene scene;
    MakePbr(scene.lit, 0.0f, 0.3f);
    const SceneLightingData light = DirectionalFrom(Vector3(kInvSqrt2, 0, kInvSqrt2), 0.8f);
    const int tilted = scene.Draw(light, scene.lit, nullptr, RotateY45).At(kCentre, kCentre).r;
    scene.lit->SetAuxTexture(AuxTexture::Normal, NormalTexel(scene, kInvSqrt2, 0, kInvSqrt2));
    const int mapped = scene.Draw(light, scene.lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(mapped, tilted, 4);
    scene.lit->SetAuxTexture(AuxTexture::Normal, nullptr);
    EXPECT_LT(scene.Draw(light, scene.lit).At(kCentre, kCentre).r, tilted - 30);
}

TEST(SoftwarePbrTest, EmissiveAndOcclusionWorkAsTheyDoWithBlinnPhong)
{
    Scene scene;
    scene.lit->SetColor("uAlbedo", 0.5f, 0.5f, 0.5f, 1.0f);
    MakePbr(scene.lit, 0.0f, 0.5f);
    const int base = scene.Draw(AmbientOnly(0.4f), scene.lit).At(kCentre, kCentre).r;

    // Emissive adds after the lighting
    scene.lit->SetVec3("uEmissive", 0.3f, 0.0f, 0.0f);
    const Frame glowing = scene.Draw(AmbientOnly(0.4f), scene.lit);
    EXPECT_NEAR(glowing.At(kCentre, kCentre).r, base + Byte(0.3f), 2);
    EXPECT_NEAR(glowing.At(kCentre, kCentre).g, base, 1);
    scene.lit->SetVec3("uEmissive", 0.0f, 0.0f, 0.0f);

    // Occlusion scales the ambient light, diffuse and specular alike
    scene.lit->SetAuxTexture(AuxTexture::Occlusion, scene.Texel(128, 128, 128));
    scene.lit->SetFloat("uOcclusionStrength", 1.0f);
    const int occluded = scene.Draw(AmbientOnly(0.4f), scene.lit).At(kCentre, kCentre).r;
    EXPECT_NEAR(occluded, static_cast<int>(std::lround(static_cast<float>(base) * 0.502f)), 3);
    scene.lit->SetFloat("uOcclusionStrength", 0.0f);
    EXPECT_NEAR(scene.Draw(AmbientOnly(0.4f), scene.lit).At(kCentre, kCentre).r, base, 1);
}

TEST(SoftwarePbrTest, LinearLightingEncodesThePbrResultToo)
{
    Scene scene;
    MakePbr(scene.lit, 0.0f, 0.5f);
    SceneLightingData gamma = DirectionalFrom(Vector3(0, 0, 1), 0.2f);
    SceneLightingData linear = gamma;
    linear.colorSpace = ColorSpace::Linear;
    const int plain = scene.Draw(gamma, scene.lit).At(kCentre, kCentre).r;
    const int encoded = scene.Draw(linear, scene.lit).At(kCentre, kCentre).r;
    EXPECT_GT(plain, 20);
    EXPECT_NEAR(encoded, Byte(Renderer::Common::LinearToSrgb(static_cast<float>(plain) / 255.0f)), 3);
    EXPECT_GT(encoded, plain + 20) << "the encode lifts the dark values";
}

TEST(SoftwarePbrTest, ALightBehindTheSurfaceGivesNoPbrLight)
{
    Scene scene;
    MakePbr(scene.lit, 0.5f, 0.5f);
    const Frame frame = scene.Draw(DirectionalFrom(Vector3(0, 0, -1), 1.0f), scene.lit);
    EXPECT_EQ(frame.At(kCentre, kCentre).r, 0);
}
