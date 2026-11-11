#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include <renderer/common/ColorEffects.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

using namespace Renderer::Common;
using Renderer::Software::SoftwareRenderer;

// The unlit shader's colour effects, uSaturation and uFlash (Common::ColorEffects; effects design, stage E1: grayscale,
// rainbow and a hit flash on Sprite2D and the UI Image). The software renderer is the reference the GPU shaders follow, and
// the backend of these tests: headless, so CI runs them.
namespace
{
    constexpr int Size = 32;
    constexpr float Identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    using Pixel = std::array<std::uint8_t, 4>;

    struct Frame
    {
        std::vector<std::uint8_t> pixels;
        Pixel At(int x, int y) const
        {
            const auto i = (y * Size + x) * 4;
            return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
        }
    };

    /// A quad over the whole frame with every vertex in `colour` (white: the flat path; anything else: the per-pixel path)
    MeshData Quad(const std::array<float, 4> &colour)
    {
        const auto c = colour;
        MeshData data;
        data.vertices = {
            Vertex{{-1, -1, 0}, {0, 0, 1}, {0, 0}, {c[0], c[1], c[2], c[3]}},
            Vertex{{1, -1, 0}, {0, 0, 1}, {1, 0}, {c[0], c[1], c[2], c[3]}},
            Vertex{{1, 1, 0}, {0, 0, 1}, {1, 1}, {c[0], c[1], c[2], c[3]}},
            Vertex{{-1, 1, 0}, {0, 0, 1}, {0, 1}, {c[0], c[1], c[2], c[3]}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    std::uint8_t To8(const float x)
    {
        return static_cast<std::uint8_t>(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f);
    }

    /// What `effects` make of (r, g, b, a), packed to 8 bits
    Pixel Expected(float r, float g, float b, const float a, const ColorEffects &effects)
    {
        effects.Apply(r, g, b);
        return {To8(r), To8(g), To8(b), To8(a)};
    }

    ::testing::AssertionResult Near(const Pixel &actual, const Pixel &expected)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (std::abs(int(actual[i]) - int(expected[i])) > 1)
            {
                return ::testing::AssertionFailure() << "(" << int(actual[0]) << "," << int(actual[1]) << "," << int(actual[2])
                                                     << "," << int(actual[3]) << ") is not (" << int(expected[0]) << ","
                                                     << int(expected[1]) << "," << int(expected[2]) << "," << int(expected[3])
                                                     << ")";
            }
        }
        return ::testing::AssertionSuccess();
    }

    ColorEffects Effects(const float saturation, const std::array<float, 4> &flash = {0, 0, 0, 0})
    {
        ColorEffects effects;
        effects.saturation = saturation;
        effects.flash = flash;
        return effects;
    }

    class SoftwareColorEffectsTest : public ::testing::Test
    {
      protected:
        SoftwareRenderer renderer;
        IMesh *white = nullptr, *tinted = nullptr;

        void SetUp() override
        {
            ASSERT_TRUE(renderer.Initialize(nullptr, Size, Size));
            white = renderer.CreateMesh(Quad({1, 1, 1, 1}));
            tinted = renderer.CreateMesh(Quad({1, 0.5f, 1, 1}));
            ASSERT_NE(white, nullptr);
            ASSERT_NE(tinted, nullptr);
        }
        void TearDown() override
        {
            renderer.SetStageDim(0.0f);
            renderer.Shutdown();
        }

        IMaterial *Unlit(const float r, const float g, const float b, const float a = 1.f)
        {
            IMaterial *material = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            material->SetColor("uAlbedo", r, g, b, a);
            return material;
        }

        static void Set(IMaterial *material, const ColorEffects &effects)
        {
            material->SetFloat("uSaturation", effects.saturation);
            material->SetVec4("uFlash", effects.flash[0], effects.flash[1], effects.flash[2], effects.flash[3]);
        }

        template <class Draw> Frame Render(Draw draw)
        {
            renderer.Clear(0, 0, 0, 1);
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            draw();
            renderer.EndFrame();
            Frame frame{std::vector<std::uint8_t>(Size * Size * 4)};
            renderer.ReadFramebuffer(frame.pixels.data(), Size, Size);
            return frame;
        }

        Frame Draw(IMesh *mesh, IMaterial *material)
        {
            return Render([&] { renderer.DrawMesh(mesh, Identity, material, RenderState::Opaque()); });
        }
    };
} // namespace

// ---- The maths every backend implements ----------------------------------------------------------------------------

TEST(ColorEffectsMaths, TheDefaultsChangeNothing)
{
    const ColorEffects none;
    EXPECT_TRUE(none.IsDefault());
    float r = 0.9f, g = 0.3f, b = 0.1f;
    none.Apply(r, g, b);
    EXPECT_FLOAT_EQ(r, 0.9f);
    EXPECT_FLOAT_EQ(g, 0.3f);
    EXPECT_FLOAT_EQ(b, 0.1f);
    EXPECT_FALSE(Effects(0.5f).IsDefault());
    EXPECT_FALSE(Effects(1.f, {1, 1, 1, 0.1f}).IsDefault());
    EXPECT_TRUE(Effects(1.f, {1, 1, 1, 0.f}).IsDefault()) << "a flash with alpha 0 is none, whatever its colour";
}

TEST(ColorEffectsMaths, SaturationZeroIsTheLumaGreyAndAboveOnePushesAwayFromIt)
{
    float r = 1.f, g = 0.f, b = 0.f;
    Effects(0.f).Apply(r, g, b);
    EXPECT_FLOAT_EQ(r, 0.299f);
    EXPECT_FLOAT_EQ(g, 0.299f);
    EXPECT_FLOAT_EQ(b, 0.299f);

    float r2 = 0.6f, g2 = 0.4f, b2 = 0.4f;
    const float luma = 0.299f * 0.6f + 0.587f * 0.4f + 0.114f * 0.4f;
    Effects(2.f).Apply(r2, g2, b2);
    EXPECT_NEAR(r2, luma + 2.f * (0.6f - luma), 1e-6f);
    EXPECT_NEAR(0.299f * r2 + 0.587f * g2 + 0.114f * b2, luma, 1e-6f) << "the luma is kept";
}

TEST(ColorEffectsMaths, AboveOneTheResultIsClampedBeforeAnythingElse)
{
    // Red at 4: luma 0.299, so (3.09, -0.9, -0.9) before the clamp. Clamped at once, as the software renderer packs to bytes
    // before the dim and tint, so every backend dims and tints the same in-range colour
    float r = 1.f, g = 0.f, b = 0.f;
    Effects(4.f).Apply(r, g, b);
    EXPECT_FLOAT_EQ(r, 1.f);
    EXPECT_FLOAT_EQ(g, 0.f);
    EXPECT_FLOAT_EQ(b, 0.f);
}

TEST(ColorEffectsMaths, TheClampsGiveTheStoredValues)
{
    EXPECT_FLOAT_EQ(ColorEffects::ClampSaturation(9.f), 4.f);
    EXPECT_FLOAT_EQ(ColorEffects::ClampSaturation(-1.f), 0.f);
    EXPECT_FLOAT_EQ(ColorEffects::ClampSaturation(std::numeric_limits<float>::quiet_NaN()), 1.f);
    EXPECT_FLOAT_EQ(ColorEffects::ClampSaturation(std::numeric_limits<float>::infinity()), 1.f);
    EXPECT_FLOAT_EQ(ColorEffects::ClampFlashComponent(2.f), 1.f);
    EXPECT_FLOAT_EQ(ColorEffects::ClampFlashComponent(-std::numeric_limits<float>::infinity()), 0.f);
}

TEST(ColorEffectsMaths, TheFlashMixesByItsAlphaAfterTheSaturation)
{
    float r = 1.f, g = 0.f, b = 0.f;
    Effects(1.f, {1, 1, 1, 0.5f}).Apply(r, g, b);
    EXPECT_FLOAT_EQ(r, 1.f);
    EXPECT_FLOAT_EQ(g, 0.5f);
    EXPECT_FLOAT_EQ(b, 0.5f);

    // Saturation first: the grey of red, then halfway to blue
    float r2 = 1.f, g2 = 0.f, b2 = 0.f;
    Effects(0.f, {0, 0, 1, 0.5f}).Apply(r2, g2, b2);
    EXPECT_FLOAT_EQ(r2, 0.299f * 0.5f);
    EXPECT_FLOAT_EQ(b2, 0.299f * 0.5f + 0.5f);

    // An alpha over 1 counts as 1, as the shaders' min(uFlash.a, 1.0)
    float r3 = 0.2f, g3 = 0.2f, b3 = 0.2f;
    Effects(1.f, {1, 0, 0, 3.f}).Apply(r3, g3, b3);
    EXPECT_FLOAT_EQ(r3, 1.f);
    EXPECT_FLOAT_EQ(g3, 0.f);
}

// ---- The software renderer --------------------------------------------------------------------------------------------

TEST_F(SoftwareColorEffectsTest, AMaterialThatNeverSetsThemDrawsWhatTheDefaultsDraw)
{
    IMaterial *plain = Unlit(0.8f, 0.4f, 0.2f);
    IMaterial *explicitDefaults = Unlit(0.8f, 0.4f, 0.2f);
    Set(explicitDefaults, ColorEffects{});
    for (IMesh *mesh : {white, tinted})
    {
        EXPECT_EQ(Draw(mesh, plain).pixels, Draw(mesh, explicitDefaults).pixels);
    }
}

TEST_F(SoftwareColorEffectsTest, SaturationZeroDrawsRedAsItsLumaGrey)
{
    IMaterial *red = Unlit(1, 0, 0);
    Set(red, Effects(0.f));
    const Frame frame = Draw(white, red);
    EXPECT_TRUE(Near(frame.At(16, 16), Expected(1, 0, 0, 1, Effects(0.f))));
    EXPECT_EQ(frame.At(16, 16)[0], To8(0.299f)) << "Rec. 601 luma of pure red";
}

TEST_F(SoftwareColorEffectsTest, AWhiteFlashAtHalfIsHalfwayToWhiteAndKeepsTheAlpha)
{
    IMaterial *red = Unlit(1, 0, 0, 0.75f);
    Set(red, Effects(1.f, {1, 1, 1, 0.5f}));
    const Frame frame = Draw(white, red);
    EXPECT_TRUE(Near(frame.At(16, 16), Expected(1, 0, 0, 0.75f, Effects(1.f, {1, 1, 1, 0.5f}))));
    EXPECT_EQ(frame.At(16, 16)[3], To8(0.75f)) << "the flash does not change alpha";
}

TEST_F(SoftwareColorEffectsTest, ThePerPixelPathAppliesThemAfterTheVertexColour)
{
    // The vertex colour (1, 0.5, 1) makes the draw shade per pixel instead of with one flat colour
    IMaterial *material = Unlit(0.5f, 1, 0);
    const ColorEffects effects = Effects(0.3f, {0, 0, 1, 0.25f});
    Set(material, effects);
    const Frame frame = Draw(tinted, material);
    EXPECT_TRUE(Near(frame.At(16, 16), Expected(0.5f, 0.5f, 0, 1, effects)));
}

TEST_F(SoftwareColorEffectsTest, TheyComeBeforeTheStageDim)
{
    // Saturation 4 too: the clamp comes before the dim, as on the GPU backends
    for (const ColorEffects &effects : {Effects(0.f, {0, 1, 0, 0.5f}), Effects(4.f)})
    {
        IMaterial *red = Unlit(1, 0, 0);
        Set(red, effects);
        const Frame frame = Render([&] {
            renderer.SetStageDim(0.5f);
            renderer.DrawMesh(white, Identity, red, RenderState::Opaque());
            renderer.SetStageDim(0.0f);
        });
        float r = 1, g = 0, b = 0;
        effects.Apply(r, g, b);
        RenderState::StageDimmed(r, g, b, 0.5f);
        EXPECT_TRUE(Near(frame.At(16, 16), Pixel{To8(r), To8(g), To8(b), 255})) << effects.saturation;
    }
}

TEST_F(SoftwareColorEffectsTest, TheLitShaderIgnoresThem)
{
    IMaterial *lit = renderer.CreateMaterial(renderer.GetStandardLitShader());
    lit->SetColor("uAlbedo", 1, 0, 0, 1);
    const Frame before = Draw(white, lit);
    Set(lit, Effects(0.f, {1, 1, 1, 1}));
    EXPECT_EQ(Draw(white, lit).pixels, before.pixels) << "unlit only, as on the GPU backends";
}

TEST_F(SoftwareColorEffectsTest, AChangeAfterDrawMeshOnlyAffectsLaterDraws)
{
    // Like every material value: the draw keeps the snapshot DrawMesh took
    IMaterial *red = Unlit(1, 0, 0);
    const Frame frame = Render([&] {
        renderer.DrawMesh(white, Identity, red, RenderState::Opaque());
        Set(red, Effects(0.f));
    });
    EXPECT_EQ(frame.At(16, 16), (Pixel{255, 0, 0, 255}));
}
