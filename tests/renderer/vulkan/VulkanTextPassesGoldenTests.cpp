#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "engine/common/Color.hpp"
#include "engine/rendering/TextDrawing.hpp"
#include "engine/text/Font.hpp"
#include "engine/text/TextEffects.hpp"

#include "common/CrossBackendDiff.hpp"
#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"
#include "VulkanTextSceneUtil.hpp"
#include "VulkanTextTestUtil.hpp"

// Multi-pass text effects on the Vulkan backend (#43, stage E): the ten RENDERED cases of SoftwareTextPassesGoldenTest in
// tests/engine/rendering/SoftwareTextPassesGoldenTests.cpp, with the same scene ("Hi" in yellow through a TextRenderer, 160 x 96,
// spaced out so no effect reaches the next glyph), passes, orders and structural expectations, on a Vulkan device (Mesa lavapipe
// in CI; no window, the offscreen target). The TextPassesTest and TextPassesSaveTest cases of that file are CPU and
// serialisation tests and are not mirrored (they run once, against the engine, in tests/engine).
//
// What the passes do on the GPU: TextDrawing::DrawResources::DrawPasses draws every resolved effect pass with the SAME material
// and mesh, changing uAlbedo (alpha 0: the shadow of a face that is not drawn), uOutline (the pass's width), uShadowColor,
// uShadowOffset and uShadowSoftness between DrawMesh calls, then the face last. Each DrawMesh takes a snapshot of the material's
// block into the frame's uniform ring, so every pass draws with its own values within one frame (VulkanTextShaderTests.cpp tests
// that at renderer level); here the whole path runs through the engine and the frames are read.
//
// As in VulkanTextEffectsGoldenTests.cpp, the software tests' exact colour sets (it alpha-tests) become the OpenGL ones:
//   - where the software test compares frames that its alpha test makes identical, the Vulkan frames are compared with Vulkan
//     frames from the same uniforms (byte-equal) or, where the two are the same image drawn in different ways (the settings in one
//     draw against the same list as passes), within a few levels of 8-bit blend rounding;
//   - 'where both shapes cover, the later pass wins' reads the frames classified to the nearest layer colour (Ink::Nearest), with
//     a small allowance for the pixels on the 0.5 iso-line;
//   - 'a pass in the face's own shape is a copy of the face' is the one that differs in substance: with blending, a pass behind
//     the face shows at the face's antialiased edge (the face covers a fraction c of the pixel, so (1 - c) of the pass's colour
//     stays: a red fringe at most a quarter red), which on the software renderer, alpha-testing, is hidden exactly. The test
//     asserts what OpenGL does: identical where the face is solid or absent, and a fringe only of the pass's colour, bounded, at
//     the edge pixels.

using namespace N2Engine;
using namespace VulkanTesting;
using namespace VulkanTesting::Draw;

namespace
{
    constexpr Rgb kFace{255, 255, 0};  // Color::Yellow (kBlue, kRed and kGreen are Draw's: Color::Blue, Red and Green)

    const std::vector<Rgb>& Palette()
    {
        static const std::vector<Rgb> palette = {kBlack, kFace, kBlue, kRed, kGreen};
        return palette;
    }

    Frame Classified(const Frame& frame)
    {
        return Ink::Nearest(frame, Palette());
    }

    class VulkanTextPassesGoldenTest : public VulkanTest
    {
    protected:
        void SetUp() override
        {
            VulkanTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            N2_REQUIRE_BUILT_IN_SHADERS();
            ASSERT_TRUE(R().SetRenderTargetSize(kWidth, kHeight));
        }

        static constexpr int kWidth = 160;
        static constexpr int kHeight = 96;
    };

    Text::TextPass Pass(const Common::Color& color, const float offsetX, const float offsetY, const float width = 0.0f,
                        const float softness = 0.0f, const int order = 0)
    {
        return Text::TextPass{color, Math::Vector2(offsetX, offsetY), width, softness, order};
    }

    Text::TextEffects WithPasses(const std::vector<Text::TextPass>& passes)
    {
        Text::TextEffects effects;
        effects.passes = passes;
        return effects;
    }

    /// Of the frames' pixels that are `colour` in every one of `frames` (classified) and not the face in `plain`: how many, and
    /// how many of those are `expected` in `result`
    struct Overlap
    {
        int count = 0;
        int wrong = 0;
    };

    Overlap CountOverlap(const Frame& plain, const std::vector<const Frame*>& frames, const std::vector<Rgb>& colours,
                         const Frame& result, const Rgb& expected)
    {
        Overlap overlap;
        for (int y = 0; y < plain.height; ++y)
        {
            for (int x = 0; x < plain.width; ++x)
            {
                if (plain.At(x, y) == kFace)
                {
                    continue;
                }
                bool all = true;
                for (std::size_t i = 0; i < frames.size(); ++i)
                {
                    all = all && frames[i]->At(x, y) == colours[i];
                }
                if (!all)
                {
                    continue;
                }
                ++overlap.count;
                overlap.wrong += result.At(x, y) == expected ? 0 : 1;
            }
        }
        return overlap;
    }

    /// The allowance for pixels on the 0.5 iso-line that fall either side of the classification: 10% of the overlap, at least 2
    int Allowance(const int count)
    {
        return std::max(2, count / 10);
    }

    /// No pixel of the frame has a green hue: with a yellow face, a red and a blue pass over black, green never exceeds red
    int PixelsWithAGreenHue(const Frame& frame)
    {
        int count = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                const Rgb pixel = frame.At(x, y);
                count += static_cast<int>(pixel.g) > static_cast<int>(pixel.r) + 2 ? 1 : 0;
            }
        }
        return count;
    }
}

TEST_F(VulkanTextPassesGoldenTest, TheShadowAndOutlineSettingsDrawAsTheirPassEquivalents)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    // The settings (one shader draw)...
    Text::TextEffects settings;
    settings.outlineWidth = 0.04f;
    settings.outlineColor = Common::Color::Blue;
    settings.shadowOffset = Math::Vector2(0.08f, -0.08f);
    settings.shadowColor = Common::Color::Red;
    const Frame fromSettings = scene.Render(settings);
    ASSERT_GT(Classified(fromSettings).Count(kRed), 0);
    ASSERT_GT(Classified(fromSettings).Count(kBlue), 0);

    // ...and the same list written out as passes (one draw each, then the face). The two agree here because the glyphs are far
    // enough apart that no glyph's effect reaches its neighbour's; where they do overlap the single draw goes glyph by glyph and
    // the pass list pass by pass, so the images differ there.
    // The software renderer makes the two byte-identical (alpha testing). Here the single draw composites face, outline and
    // shadow in the shader and blends once, while the passes blend one after the other into the 8-bit target: the same
    // equation, rounded after each pass, so the frames agree to within 3/255 (the measured maximum is printed)
    const Frame fromPasses = scene.Render(
        WithPasses({Pass(Common::Color::Red, 0.08f, -0.08f, 0.04f), Pass(Common::Color::Blue, 0.0f, 0.0f, 0.04f)}));
    const Ink::FrameDifference difference = Ink::Difference(fromSettings, fromPasses, 3);
    std::cout << "[ passes ] the settings against the same list as passes: the largest channel difference " << difference.maxChannel
              << "/255, " << difference.overTolerance << " pixels over 3/255" << std::endl;
    EXPECT_FALSE(difference.sizesDiffer);
    EXPECT_EQ(difference.overTolerance, 0) << "mapping the settings onto the pass list changed the image (largest difference "
                                           << difference.maxChannel << "/255)";
}

TEST_F(VulkanTextPassesGoldenTest, LaterPassesDrawOverEarlierOnesAndTheFaceIsLast)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Frame plain = Classified(scene.Render(Text::TextEffects{}));
    const Text::TextPass red = Pass(Common::Color::Red, 0.06f, -0.06f, 0.03f);
    const Text::TextPass blue = Pass(Common::Color::Blue, 0.0f, 0.0f, 0.03f);
    const Frame redOnly = Classified(scene.Render(WithPasses({red})));
    const Frame blueOnly = Classified(scene.Render(WithPasses({blue})));
    const Frame redThenBlue = Classified(scene.Render(WithPasses({red, blue})));
    const Frame blueThenRed = Classified(scene.Render(WithPasses({blue, red})));

    // Face, red, blue and the background only (no green hue), and the face is the plain text's: it is drawn last
    EXPECT_EQ(PixelsWithAGreenHue(scene.Render(WithPasses({red, blue}))), 0);
    Ink::ExpectSameFaceApprox(plain, redThenBlue, kFace);
    Ink::ExpectSameFaceApprox(plain, blueThenRed, kFace);

    // Where both shapes cover (and the face doesn't), the later pass wins
    const Overlap forward = CountOverlap(plain, {&redOnly, &blueOnly}, {kRed, kBlue}, redThenBlue, kBlue);
    const Overlap reverse = CountOverlap(plain, {&redOnly, &blueOnly}, {kRed, kBlue}, blueThenRed, kRed);
    EXPECT_GT(forward.count, 20) << "the two shapes never overlap, so the test shows nothing";
    EXPECT_LE(forward.wrong, Allowance(forward.count)) << "the second pass didn't draw over the first";
    EXPECT_LE(reverse.wrong, Allowance(reverse.count)) << "reversing the passes didn't reverse which one is on top";
}

TEST_F(VulkanTextPassesGoldenTest, ExtraPassesDrawAfterTheSettingsOutlineAndShadow)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    Text::TextEffects outlined;
    outlined.outlineWidth = 0.03f;
    outlined.outlineColor = Common::Color::Blue;
    const Frame outlineOnly = Classified(scene.Render(outlined));

    // A wider red pass after the blue outline setting
    Text::TextEffects both = outlined;
    both.passes = {Pass(Common::Color::Red, 0.0f, 0.0f, 0.06f)};
    const Frame withPassRaw = scene.Render(both);
    const Frame withPass = Classified(withPassRaw);
    EXPECT_EQ(PixelsWithAGreenHue(withPassRaw), 0);
    EXPECT_GT(withPass.Count(kRed), 0);

    // The settings shadow and outline are the first entries of the list and the extra passes follow them, so the red pass is
    // drawn after the outline and covers it where it reaches
    int outlineLost = 0;
    for (int y = 0; y < withPass.height; ++y)
    {
        for (int x = 0; x < withPass.width; ++x)
        {
            outlineLost += (withPass.At(x, y) == kRed && outlineOnly.At(x, y) == kBlue) ? 1 : 0;
        }
    }
    EXPECT_GT(outlineLost, 0) << "extra passes are drawn after the settings outline, so they cover it";
}

TEST_F(VulkanTextPassesGoldenTest, EffectPassesNeverCoverTheFace)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Frame plainRaw = scene.Render(Text::TextEffects{});
    const Frame plain = Classified(plainRaw);
    ASSERT_GT(plain.Count(kFace), 0);

    // A fat pass right on the text, a shadow-like one and a hidden one: the face pixels never change
    const Frame frameRaw = scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, 0.1f),
                                                    Pass(Common::Color::Blue, 0.05f, 0.05f, 0.05f),
                                                    Pass(Common::Color{0.0f, 1.0f, 0.0f, 0.0f}, 0.0f, 0.0f)}));
    const Frame frame = Classified(frameRaw);
    Ink::ExpectSameFaceApprox(plain, frame, kFace);
    // The face is drawn last with alpha 1 where it is solid: those pixels are exactly the face colour whatever is behind
    int solidChanged = 0;
    for (int y = 0; y < plainRaw.height; ++y)
    {
        for (int x = 0; x < plainRaw.width; ++x)
        {
            solidChanged += (plainRaw.At(x, y) == kFace && !(frameRaw.At(x, y) == kFace)) ? 1 : 0;
        }
    }
    EXPECT_EQ(solidChanged, 0) << "a pass covered the solid face";
    EXPECT_EQ(PixelsWithAGreenHue(frameRaw), 0) << "the hidden (alpha 0) pass drew";
    EXPECT_GT(frame.Count(kRed), 0);
    EXPECT_GT(frame.Count(kBlue), 0);
}

TEST_F(VulkanTextPassesGoldenTest, APassWithoutOffsetOrWidthIsHiddenByTheSolidFaceAndOnlyFringesItsAntialiasedEdge)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    // Behind the face, a pass in the face's own shape. SOFTWARE: fully hidden by the face (alpha-tested), the frames are equal.
    // GL AND VULKAN: hidden where the face is solid, and the edge pixels, where the face covers a fraction c, keep (1 - c) of
    // the pass, which itself covers c: a red fringe of at most (1 - c) c, a quarter. So: the same wherever the plain text is
    // solid or absent, and elsewhere only the red channel rose, by at most a quarter of full
    const Frame frame = scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f)}));
    int fringe = 0;
    for (int y = 0; y < plain.height; ++y)
    {
        for (int x = 0; x < plain.width; ++x)
        {
            const Rgb before = plain.At(x, y);
            const Rgb after = frame.At(x, y);
            if (before == kFace || before == kBlack)
            {
                EXPECT_EQ(after, before) << "the pass showed where the face is solid or absent at " << x << ", " << y;
                continue;
            }
            ++fringe;
            EXPECT_NEAR(static_cast<int>(after.g), static_cast<int>(before.g), 1) << x << ", " << y;
            EXPECT_NEAR(static_cast<int>(after.b), static_cast<int>(before.b), 1) << x << ", " << y;
            EXPECT_GE(static_cast<int>(after.r), static_cast<int>(before.r) - 1) << x << ", " << y;
            EXPECT_LE(static_cast<int>(after.r), static_cast<int>(before.r) + 66) << "more than a quarter red at " << x << ", " << y;
        }
    }
    EXPECT_GT(fringe, 20) << "the edge pixels exist";
    // The picture is the plain text's to the classification: the same face, no red body
    // (the fringe pixels at about a third coverage may classify as red, so the allowance is 10%)
    Ink::ExpectSameFaceApprox(Classified(plain), Classified(frame), kFace, 10.0, 8);
    EXPECT_LE(Classified(frame).Count(kRed), fringe / 2) << "a copy of the face behind the face is no more than a fringe";
}

TEST_F(VulkanTextPassesGoldenTest, AWiderPassIsARingAroundTheFace)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Frame plain = Classified(scene.Render(Text::TextEffects{}));
    const Frame thin = Classified(scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, 0.02f)})));
    const Frame wide = Classified(scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, 0.06f)})));
    Ink::ExpectSameFaceApprox(plain, wide, kFace);
    EXPECT_GT(thin.Count(kRed), 0);
    EXPECT_GT(wide.Count(kRed), thin.Count(kRed)) << "a wider pass covers more";
}

TEST_F(VulkanTextPassesGoldenTest, PassesBeyondTheSpreadAreClampedNotDrawnAsBoxes)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Frame plain = Classified(scene.Render(Text::TextEffects{}));
    const Frame hugeRaw = scene.Render(WithPasses({Pass(Common::Color::Red, 3.0f, -3.0f, 3.0f, 3.0f)}));
    const Frame huge = Classified(hugeRaw);
    Ink::ExpectSameFaceApprox(plain, huge, kFace);
    int blue = 0;
    for (int y = 0; y < hugeRaw.height; ++y)
    {
        for (int x = 0; x < hugeRaw.width; ++x)
        {
            blue += hugeRaw.At(x, y).b > 2 ? 1 : 0;
        }
    }
    EXPECT_EQ(blue, 0) << "only the face, the pass and the background";

    // The clamp gives exactly the widest pass the font allows, not a box over the glyph quads: the same uniforms, so the same
    // bytes (the software test asserts the same equality)
    const auto font = Text::Font::GetDefault();
    ASSERT_NE(font, nullptr);
    const float widest = Rendering::TextDrawing::MaxEffectEms(font->GetSdfFont().GetAtlas().GetSettings());
    const Frame atMax = scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, widest)}));
    EXPECT_EQ(hugeRaw.rgba, atMax.rgba);

    // Not boxes: no red pixel is further from the face than the widest pass reaches (plus rounding)
    const int reach = static_cast<int>(std::ceil(widest * scene.Scale())) + 2;
    int farPixels = 0;
    for (int y = 0; y < huge.height; ++y)
    {
        for (int x = 0; x < huge.width; ++x)
        {
            if (!(huge.At(x, y) == kRed))
            {
                continue;
            }
            bool nearFace = false;
            for (int oy = -reach; oy <= reach && !nearFace; ++oy)
            {
                for (int ox = -reach; ox <= reach && !nearFace; ++ox)
                {
                    const int nx = x + ox;
                    const int ny = y + oy;
                    nearFace = nx >= 0 && ny >= 0 && nx < huge.width && ny < huge.height && huge.At(nx, ny) == kFace;
                }
            }
            farPixels += nearFace ? 0 : 1;
        }
    }
    EXPECT_EQ(farPixels, 0) << "pass pixels far from the face: the glyph quads were filled";
    EXPECT_GT(huge.Count(kRed), 0);
}

TEST_F(VulkanTextPassesGoldenTest, APassBelowTheShadowSettingDrawsBehindIt)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    Text::TextEffects shadowOnly;
    shadowOnly.shadowOffset = Math::Vector2(0.04f, -0.04f);
    shadowOnly.shadowColor = Common::Color::Blue;
    const Frame shadowFrame = Classified(scene.Render(shadowOnly));
    const Frame redFrame = Classified(scene.Render(WithPasses({Pass(Common::Color::Red, 0.0f, 0.0f, 0.05f)})));

    const auto withRed = [&shadowOnly](const int order)
    {
        Text::TextEffects effects = shadowOnly;
        effects.passes = {Pass(Common::Color::Red, 0.0f, 0.0f, 0.05f, 0.0f, order)};
        return effects;
    };
    const Frame behind = Classified(scene.Render(withRed(-300)));
    const Frame inFront = Classified(scene.Render(withRed(0)));
    const Frame plain = Classified(scene.Render(Text::TextEffects{}));
    Ink::ExpectSameFaceApprox(plain, behind, kFace);
    Ink::ExpectSameFaceApprox(plain, inFront, kFace);

    // Where the shadow and the red pass both cover (the face doesn't): the one drawn later is on top
    const Overlap overBehind = CountOverlap(plain, {&shadowFrame, &redFrame}, {kBlue, kRed}, behind, kBlue);
    const Overlap overFront = CountOverlap(plain, {&shadowFrame, &redFrame}, {kBlue, kRed}, inFront, kRed);
    EXPECT_GT(overBehind.count, 20) << "the shapes never overlap, so the test shows nothing";
    EXPECT_LE(overBehind.wrong, Allowance(overBehind.count)) << "an order below the shadow's didn't draw behind it";
    EXPECT_LE(overFront.wrong, Allowance(overFront.count)) << "the default order didn't draw in front of the shadow";
}

TEST_F(VulkanTextPassesGoldenTest, APassBetweenTheShadowAndTheOutlineCoversOnlyTheShadow)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    Text::TextEffects settings;
    settings.shadowOffset = Math::Vector2(0.04f, -0.04f);
    settings.shadowColor = Common::Color::Blue;
    settings.outlineWidth = 0.03f;
    settings.outlineColor = Common::Color::Green;
    const Frame base = Classified(scene.Render(settings));
    ASSERT_GT(base.Count(kBlue), 0);
    ASSERT_GT(base.Count(kGreen), 0);

    Text::TextEffects mixed = settings;
    mixed.passes = {Pass(Common::Color::Red, 0.0f, 0.0f, 0.06f, 0.0f, -150)};
    const Frame frame = Classified(scene.Render(mixed));

    // The outline is drawn after the red pass, so every outline pixel of the base image is still there (to the iso-line
    // pixels); the red pass took over some of the shadow's
    int outlineLost = 0;
    int shadowCovered = 0;
    for (int y = 0; y < base.height; ++y)
    {
        for (int x = 0; x < base.width; ++x)
        {
            outlineLost += (base.At(x, y) == kGreen && !(frame.At(x, y) == kGreen)) ? 1 : 0;
            shadowCovered += (base.At(x, y) == kBlue && frame.At(x, y) == kRed) ? 1 : 0;
        }
    }
    EXPECT_LE(outlineLost, std::max(4, base.Count(kGreen) * 3 / 100)) << "the red pass covered the outline";
    EXPECT_GT(shadowCovered, 0);
}

TEST_F(VulkanTextPassesGoldenTest, AnOrderAboveZeroDrawsLikeOrderZero)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Text::TextPass atZero = Pass(Common::Color::Red, 0.0f, 0.0f, 0.04f, 0.0f, 0);
    Text::TextPass high = atZero;
    high.order = 9;
    const Frame zero = scene.Render(WithPasses({atZero}));
    const Frame above = scene.Render(WithPasses({high}));
    // The order clamps to the highest there is, so the uniforms and the draw order are the same: the same bytes
    EXPECT_EQ(zero.rgba, above.rgba);
    EXPECT_GT(Classified(above).Count(kRed), 0);
    EXPECT_GT(Classified(above).Count(kFace), 0) << "the face is still drawn, and on top";
}
