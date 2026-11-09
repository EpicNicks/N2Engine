#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

using namespace Renderer::Common;
using Renderer::Software::SoftwareRenderer;

// RenderState::tint and IRenderer::SetScreenTint (effects design, stage E2: the time-stop circle and full-screen grey): a
// saturation change inside or outside a circle on the screen, applied by every built-in shader to the pixels it writes. The
// software renderer is the reference the GPU backends' shaders implement (ScreenTint::Covers and ScreenTint::Saturated),
// and the backend of these tests: headless, so CI runs them.
namespace
{
    constexpr int Size = 64;
    constexpr float Identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    using Pixel = std::array<std::uint8_t, 4>;
    constexpr Pixel Red{255, 0, 0, 255}, Green{0, 255, 0, 255}, Blue{0, 0, 255, 255};

    struct Frame
    {
        std::vector<std::uint8_t> pixels;
        /// y counts up from the bottom row (ReadFramebuffer's order)
        Pixel At(int x, int y) const
        {
            const auto i = (y * Size + x) * 4;
            return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
        }
    };

    /// A quad over the whole frame at `depth` (clip space z), facing the camera
    MeshData Quad(const float depth)
    {
        MeshData data;
        data.vertices = {
            Vertex{{-1, -1, depth}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
            Vertex{{1, -1, depth}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
            Vertex{{1, 1, depth}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}},
            Vertex{{-1, 1, depth}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        return data;
    }

    std::uint8_t To8(const float x)
    {
        return static_cast<std::uint8_t>(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f);
    }

    /// What the dim and then a tint of `saturation` make of an 8-bit colour, packed as the renderer packs it
    Pixel Tinted(const Pixel &pixel, const float saturation, const float dim = 0.f)
    {
        float r = pixel[0] / 255.f, g = pixel[1] / 255.f, b = pixel[2] / 255.f;
        RenderState::StageDimmed(r, g, b, dim);
        ScreenTint::Saturated(r, g, b, saturation);
        return {To8(r), To8(g), To8(b), pixel[3]};
    }

    /// Every channel within one unit (the frames are rounded to 8 bits at different points)
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

    /// A tint of `saturation` in a circle at (x, y) (0 to 1 of the screen) of `radius` half screen heights
    ScreenTint Circle(const float saturation, const float x, const float y, const float radius, const bool inside = true)
    {
        ScreenTint tint;
        tint.saturation = saturation;
        tint.centreX = x;
        tint.centreY = y;
        tint.radius = radius;
        tint.inside = inside;
        return tint;
    }

    class SoftwareScreenTintTest : public ::testing::Test
    {
      protected:
        SoftwareRenderer renderer;
        IMaterial *red = nullptr, *green = nullptr, *halfGreen = nullptr;
        IMesh *near = nullptr, *far = nullptr;

        void SetUp() override
        {
            ASSERT_TRUE(renderer.Initialize(nullptr, Size, Size));
            renderer.Clear(0, 0, 0, 1);
            red = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            green = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            halfGreen = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            near = renderer.CreateMesh(Quad(-0.5f));
            far = renderer.CreateMesh(Quad(0.5f));
            ASSERT_NE(red, nullptr);
            ASSERT_NE(green, nullptr);
            ASSERT_NE(halfGreen, nullptr);
            ASSERT_NE(near, nullptr);
            ASSERT_NE(far, nullptr);
            red->SetColor("uAlbedo", 1, 0, 0, 1);
            green->SetColor("uAlbedo", 0, 1, 0, 1);
            halfGreen->SetColor("uAlbedo", 0, 1, 0, 0.5f);
        }
        void TearDown() override
        {
            renderer.SetScreenTint(ScreenTint{});
            renderer.SetStageDim(0.0f);
            renderer.Shutdown();
        }

        /// One frame of `draw`, cleared to `clearBlue` blue (0: black) first
        template <class Draw> Frame Render(Draw draw, const float clearBlue = 0.f)
        {
            renderer.Clear(0, 0, clearBlue, 1);
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            draw();
            renderer.EndFrame();
            Frame frame{std::vector<std::uint8_t>(Size * Size * 4)};
            renderer.ReadFramebuffer(frame.pixels.data(), Size, Size);
            return frame;
        }

        /// The full-screen red quad, drawn while `tint` is set
        Frame RedWith(const ScreenTint &tint)
        {
            return Render([&] {
                renderer.SetScreenTint(tint);
                renderer.DrawMesh(far, Identity, red);
                renderer.SetScreenTint(ScreenTint{});
            });
        }
    };
} // namespace

// ---- The maths every backend implements ----------------------------------------------------------------------------

TEST(ScreenTintMaths, SaturationOneChangesNothingAndZeroIsTheLumaGrey)
{
    float r = 1.f, g = 0.5f, b = 0.25f;
    ScreenTint::Saturated(r, g, b, 1.f);
    EXPECT_FLOAT_EQ(r, 1.f);
    EXPECT_FLOAT_EQ(g, 0.5f);
    EXPECT_FLOAT_EQ(b, 0.25f);

    ScreenTint::Saturated(r, g, b, 0.f);
    const float luma = RenderState::DimLumaR * 1.f + RenderState::DimLumaG * 0.5f + RenderState::DimLumaB * 0.25f;
    EXPECT_FLOAT_EQ(r, luma);
    EXPECT_FLOAT_EQ(g, luma);
    EXPECT_FLOAT_EQ(b, luma);

    // Halfway: halfway between the colour and its grey, and the grey keeps its luma
    float r2 = 1.f, g2 = 0.f, b2 = 0.f;
    ScreenTint::Saturated(r2, g2, b2, 0.5f);
    EXPECT_NEAR(r2, (1.f + 0.299f) / 2.f, 1e-6f);
    EXPECT_NEAR(g2, 0.299f / 2.f, 1e-6f);
    EXPECT_NEAR(0.299f * r2 + 0.587f * g2 + 0.114f * b2, 0.299f, 1e-6f);
}

TEST(ScreenTintMaths, TintingEachDrawThenBlendingIsTintingTheBlendedFrame)
{
    // The linearity the design rests on: saturation is a linear map of the colour, and normal and additive blending mix
    // colours linearly, so tint(blend(src, dst)) == blend(tint(src), tint(dst)) before the target clamps
    const std::array<std::array<float, 3>, 4> colours{{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.2f, 0.4f, 0.9f}, {0.7f, 0.7f, 0.1f}}};
    for (const float saturation : {0.f, 0.2f, 0.5f, 0.9f})
    {
        for (const auto &src : colours)
        {
            for (const auto &dst : colours)
            {
                for (const float alpha : {0.25f, 0.5f, 1.f})
                {
                    for (const bool additive : {false, true})
                    {
                        std::array<float, 3> blended{}, tintedSrc = src, tintedDst = dst;
                        for (int i = 0; i < 3; ++i)
                        {
                            blended[i] = alpha * src[i] + (additive ? 1.f : 1.f - alpha) * dst[i];
                        }
                        ScreenTint::Saturated(blended[0], blended[1], blended[2], saturation);
                        ScreenTint::Saturated(tintedSrc[0], tintedSrc[1], tintedSrc[2], saturation);
                        ScreenTint::Saturated(tintedDst[0], tintedDst[1], tintedDst[2], saturation);
                        for (int i = 0; i < 3; ++i)
                        {
                            const float perDraw = alpha * tintedSrc[i] + (additive ? 1.f : 1.f - alpha) * tintedDst[i];
                            EXPECT_NEAR(perDraw, blended[i], 1e-5f) << "saturation " << saturation << ", alpha " << alpha
                                                                    << (additive ? ", additive" : ", normal");
                        }
                    }
                }
            }
        }
    }
}

TEST(ScreenTintMaths, TheDefaultAndAnEmptyInsideCircleAreNoTint)
{
    EXPECT_FALSE(ScreenTint{}.IsActive()) << "saturation 1";
    EXPECT_FALSE(Circle(0.f, 0.5f, 0.5f, 0.f, true).IsActive()) << "the inside of a circle of radius 0 is empty";
    EXPECT_TRUE(Circle(0.f, 0.5f, 0.5f, 0.f, false).IsActive()) << "its outside is the whole screen";
    EXPECT_TRUE(Circle(0.99f, 0.5f, 0.5f, 1.f).IsActive());
}

TEST(ScreenTintMaths, TheCircleIsPlacedInPixelsForEitherRowOrder)
{
    const ScreenTint tint = Circle(0.f, 0.25f, 0.75f, 0.5f);
    const ScreenTintPixels up = tint.InPixels(200.f, 100.f, false);
    EXPECT_FLOAT_EQ(up.centreX, 50.f);
    EXPECT_FLOAT_EQ(up.centreY, 75.f) << "rows counted up from the bottom (OpenGL, software)";
    EXPECT_FLOAT_EQ(up.radius, 25.f) << "half screen heights: 0.5 x 50";
    const ScreenTintPixels down = tint.InPixels(200.f, 100.f, true);
    EXPECT_FLOAT_EQ(down.centreX, 50.f);
    EXPECT_FLOAT_EQ(down.centreY, 25.f) << "rows counted down from the top (Vulkan, WebGPU)";
    EXPECT_FLOAT_EQ(down.radius, 25.f);

    EXPECT_TRUE(tint.Covers(up, 50.5f, 75.5f));
    EXPECT_FALSE(tint.Covers(up, 50.5f, 25.5f));
    EXPECT_FALSE(Circle(0.f, 0.25f, 0.75f, 0.5f, false).Covers(up, 50.5f, 75.5f)) << "outside: the reverse";
    EXPECT_TRUE(Circle(0.f, 0.25f, 0.75f, 0.5f, false).Covers(up, 50.5f, 25.5f));
}

TEST(ScreenTintMaths, CoversWholeTargetDecidesWhetherTheClearColourIsTinted)
{
    EXPECT_TRUE(ScreenTint{.saturation = 0.f}.CoversWholeTarget(160.f, 90.f)) << "the default radius covers any screen";
    EXPECT_FALSE(ScreenTint{}.CoversWholeTarget(160.f, 90.f)) << "no tint";
    // 16:9 from the middle: the corners are sqrt((16/9)^2 + 1) = 2.04 half heights away
    EXPECT_TRUE(Circle(0.f, 0.5f, 0.5f, 2.1f).CoversWholeTarget(160.f, 90.f));
    EXPECT_FALSE(Circle(0.f, 0.5f, 0.5f, 2.0f).CoversWholeTarget(160.f, 90.f));
    // Outside: only when the circle does not reach the screen at all
    EXPECT_TRUE(Circle(0.f, 0.5f, 0.5f, 0.f, false).CoversWholeTarget(160.f, 90.f));
    EXPECT_FALSE(Circle(0.f, 0.5f, 0.5f, 0.1f, false).CoversWholeTarget(160.f, 90.f));
    EXPECT_TRUE(Circle(0.f, 3.f, 0.5f, 0.5f, false).CoversWholeTarget(160.f, 90.f)) << "a circle off to the right";
}

TEST(ScreenTintMaths, TheRenderersModifierIsSanitisedAndOnlyAnActiveTintReplacesADrawsOwn)
{
    SoftwareRenderer renderer;
    EXPECT_TRUE(renderer.SupportsScreenTint());
    EXPECT_EQ(renderer.WithFrameModifiers(RenderState{}), RenderState{}) << "nothing set: the state as it is";

    renderer.SetScreenTint(Circle(-1.f, 0.5f, 0.5f, -2.f));
    EXPECT_FLOAT_EQ(renderer.GetScreenTint().saturation, 0.f);
    EXPECT_FLOAT_EQ(renderer.GetScreenTint().radius, 0.f);
    renderer.SetScreenTint(Circle(3.f, 0.5f, 0.5f, 1.f));
    EXPECT_FLOAT_EQ(renderer.GetScreenTint().saturation, 1.f);
    renderer.SetScreenTint(Circle(0.f, std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.f));
    EXPECT_EQ(renderer.GetScreenTint(), ScreenTint{}) << "a value that is not finite is no tint";

    const ScreenTint grey = Circle(0.2f, 0.3f, 0.4f, 1.5f, false);
    renderer.SetScreenTint(grey);
    EXPECT_EQ(renderer.WithFrameModifiers(RenderState{}).tint, grey);
    RenderState own;
    own.tint = Circle(0.5f, 0.5f, 0.5f, 1.f);
    EXPECT_EQ(renderer.WithFrameModifiers(own).tint, grey) << "the frame's tint, while one is set";
    renderer.SetScreenTint(ScreenTint{});
    EXPECT_EQ(renderer.WithFrameModifiers(own).tint, own.tint) << "the draw's own tint, while none is";
}

// ---- Software pixels ---------------------------------------------------------------------------------------------

TEST_F(SoftwareScreenTintTest, NoTintLeavesEveryPixelAsItWas)
{
    const Frame plain = Render([&] { renderer.DrawMesh(far, Identity, red); });
    EXPECT_EQ(plain.At(32, 32), Red);
    EXPECT_EQ(RedWith(ScreenTint{}).pixels, plain.pixels) << "the default";
    EXPECT_EQ(RedWith(Circle(1.f, 0.5f, 0.5f, 1.f)).pixels, plain.pixels) << "saturation 1";
    EXPECT_EQ(RedWith(Circle(0.f, 0.5f, 0.5f, 0.f)).pixels, plain.pixels) << "inside a circle of radius 0";
}

TEST_F(SoftwareScreenTintTest, InsideTheCircleIsTintedAndOutsideIsNot)
{
    // Centre (32, 32), radius 0.5 half heights = 16 pixels
    const Frame frame = RedWith(Circle(0.f, 0.5f, 0.5f, 0.5f));
    EXPECT_EQ(frame.At(32, 32), Tinted(Red, 0.f));
    EXPECT_EQ(frame.At(32, 32)[0], To8(0.299f)) << "the grey of red is its luma";
    EXPECT_EQ(frame.At(2, 2), Red);
    EXPECT_EQ(frame.At(61, 61), Red);
    // The edge on row 32 (pixel centres at y 32.5): x 47 is 15.5 away (inside), x 48 16.5 (outside); the same to the left
    EXPECT_EQ(frame.At(47, 32), Tinted(Red, 0.f));
    EXPECT_EQ(frame.At(48, 32), Red);
    EXPECT_EQ(frame.At(16, 32), Tinted(Red, 0.f));
    EXPECT_EQ(frame.At(15, 32), Red);

    // A partial saturation
    const Frame partly = RedWith(Circle(0.2f, 0.5f, 0.5f, 0.5f));
    EXPECT_EQ(partly.At(32, 32), Tinted(Red, 0.2f));
    EXPECT_NE(partly.At(32, 32), Tinted(Red, 0.f));
}

TEST_F(SoftwareScreenTintTest, InsideFalseTintsOutsideTheCircle)
{
    const Frame frame = RedWith(Circle(0.f, 0.5f, 0.5f, 0.5f, false));
    EXPECT_EQ(frame.At(32, 32), Red);
    EXPECT_EQ(frame.At(2, 2), Tinted(Red, 0.f));
    EXPECT_EQ(frame.At(48, 32), Tinted(Red, 0.f));
    EXPECT_EQ(frame.At(47, 32), Red);
}

TEST_F(SoftwareScreenTintTest, AnOffCentreCircleIsWhereItsCentreSaysCountedFromTheBottomLeft)
{
    // Centre (0.25, 0.75): pixel (16, 48) counted up from the bottom; radius 0.25 half heights = 8 pixels
    const Frame frame = RedWith(Circle(0.f, 0.25f, 0.75f, 0.25f));
    EXPECT_EQ(frame.At(16, 48), Tinted(Red, 0.f));
    EXPECT_EQ(frame.At(16, 16), Red) << "not mirrored top to bottom";
    EXPECT_EQ(frame.At(48, 48), Red) << "not mirrored left to right";
    EXPECT_EQ(frame.At(16, 56), Red) << "9 rows up: outside";
    EXPECT_EQ(frame.At(16, 55), Tinted(Red, 0.f)) << "7.5 rows up: inside";
}

TEST_F(SoftwareScreenTintTest, TheWholeScreenTintGreysEveryPixelADrawWritesButNotTheClearColour)
{
    ScreenTint whole;
    whole.saturation = 0.f;
    const Frame frame = Render([&] {
        renderer.SetScreenTint(whole);
        // A quad over the left half only: the right half stays the clear colour
        MeshData left = Quad(0.f);
        left.vertices[1].position[0] = 0.f;
        left.vertices[2].position[0] = 0.f;
        IMesh *half = renderer.CreateMesh(left);
        renderer.DrawMesh(half, Identity, red);
        renderer.SetScreenTint(ScreenTint{});
    }, 1.f);
    EXPECT_EQ(frame.At(8, 32), Tinted(Red, 0.f));
    EXPECT_EQ(frame.At(56, 32), Blue) << "the clear colour is no draw, so no shader tints it (Application does, on the CPU)";
}

TEST_F(SoftwareScreenTintTest, TheDimIsAppliedFirstThenTheTint)
{
    const Frame frame = Render([&] {
        renderer.SetStageDim(0.65f);
        renderer.SetScreenTint(Circle(0.2f, 0.5f, 0.5f, 0.5f));
        renderer.DrawMesh(far, Identity, red);
        renderer.SetScreenTint(ScreenTint{});
        renderer.SetStageDim(0.f);
    });
    EXPECT_TRUE(Near(frame.At(32, 32), Tinted(Red, 0.2f, 0.65f)));
    EXPECT_TRUE(Near(frame.At(2, 2), Tinted(Red, 1.f, 0.65f))) << "outside the circle: dimmed only";
}

TEST_F(SoftwareScreenTintTest, ADrawOverAnotherIsTheTintedFrame)
{
    // The software renderer cannot blend (every draw is opaque), so the near draw covers the far one whatever its state;
    // tinting each draw gives the frame that tinting the finished frame gives (the linearity for blended draws on the GPU
    // backends is ScreenTintMaths.TintingEachDrawThenBlendingIsTintingTheBlendedFrame and VulkanScreenTintTest)
    const auto scene = [&] {
        renderer.DrawMesh(far, Identity, red);
        renderer.DrawMesh(near, Identity, halfGreen, RenderState::Transparent());
    };
    const Frame plain = Render(scene);
    const ScreenTint tint = Circle(0.3f, 0.5f, 0.5f, 0.5f);
    const Frame tinted = Render([&] {
        renderer.SetScreenTint(tint);
        scene();
        renderer.SetScreenTint(ScreenTint{});
    });
    const ScreenTintPixels circle = tint.InPixels(Size, Size, false);
    for (int y = 0; y < Size; y += 3)
    {
        for (int x = 0; x < Size; x += 3)
        {
            const Pixel expected = tint.Covers(circle, x + 0.5f, y + 0.5f) ? Tinted(plain.At(x, y), 0.3f) : plain.At(x, y);
            ASSERT_TRUE(Near(tinted.At(x, y), expected)) << "pixel (" << x << ", " << y << ")";
        }
    }
}

TEST_F(SoftwareScreenTintTest, TheTintStaysWithItsDrawWhenTheRendererReordersDraws)
{
    // Two depth-writing draws in one run: the software renderer sorts them front to back (early-Z). The far one is
    // recorded first with the tint on, the near one after with it off; the near one covers the frame and must come out
    // untinted although it is rasterized first.
    const ScreenTint grey = Circle(0.f, 0.5f, 0.5f, 0.5f);
    const Frame frame = Render([&] {
        renderer.SetScreenTint(grey);
        renderer.DrawMesh(far, Identity, red);
        renderer.SetScreenTint(ScreenTint{});
        renderer.DrawMesh(near, Identity, green);
    });
    EXPECT_EQ(frame.At(32, 32), Green);

    // The other way round: the tinted draw is the near one
    const Frame other = Render([&] {
        renderer.DrawMesh(far, Identity, green);
        renderer.SetScreenTint(grey);
        renderer.DrawMesh(near, Identity, red);
        renderer.SetScreenTint(ScreenTint{});
    });
    EXPECT_EQ(other.At(32, 32), Tinted(Red, 0.f));
    EXPECT_EQ(other.At(2, 2), Red);
}

TEST_F(SoftwareScreenTintTest, ADrawsOwnTintIsAppliedWithoutTheModifier)
{
    RenderState state;
    state.tint = Circle(0.f, 0.5f, 0.5f, 0.5f);
    const Frame frame = Render([&] { renderer.DrawMesh(far, Identity, red, state); });
    EXPECT_EQ(frame.At(32, 32), Tinted(Red, 0.f));
    EXPECT_EQ(frame.At(2, 2), Red);
}
