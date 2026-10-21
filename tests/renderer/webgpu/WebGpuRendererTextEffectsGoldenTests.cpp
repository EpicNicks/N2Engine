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
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "engine/common/Color.hpp"
#include "engine/rendering/TextDrawing.hpp"
#include "engine/text/Font.hpp"
#include "engine/text/TextEffects.hpp"

#include "common/CrossBackendDiff.hpp"
#include "WebGpuDrawTestUtil.hpp"
#include "WebGpuTestFixture.hpp"
#include "WebGpuTextSceneUtil.hpp"
#include "WebGpuTextTestUtil.hpp"

// Golden-image tests of the text effects (outline, shadow, softness) on the WebGPU backend (#43, stage E): the six cases of
// tests/engine/rendering/SoftwareTextEffectsGoldenTests.cpp with the same scene ("Hi" in yellow through a TextRenderer, 160 x 96,
// spaced out so no effect reaches the next glyph), the same effect values and the same structural expectations, on a WebGPU
// device (Mesa lavapipe in CI; no window, the offscreen target).
//
// The software text shader alpha-tests, so every software pixel is exactly the background, the face, the outline or the shadow
// colour, and the software tests compare those exact pixel sets. The WebGPU shader (a port of OpenGL's) antialiases and
// composites the outline and shadow UNDER the face in one draw, so an edge pixel is a blend. Each mirror therefore keeps the
// software assertion's meaning and changes its encoding, said at the assertion:
//   - 'only these colours' becomes 'no hue that these layers cannot make' (the face is yellow, the outline blue, the shadow red,
//     over black: red equals green unless a shadow shows, blue is the outline);
//   - 'the same set of face pixels' / 'every outline pixel is still outline' compare the frames CLASSIFIED to the nearest layer
//     colour (Ink::Nearest: halfway between two layers is the 0.5 iso-line each software alpha test cuts at), with a small
//     allowance for the pixels that sit exactly on it, where the software tests are exact;
//   - 'equals' for two renders that the software renderer makes identical because of alpha testing (effects off, clamped effects)
//     is exact here too: the same uniforms give the same bytes (WebGPU frame against WebGPU frame, both antialiased);
//   - SOFTNESS: the software renderer ignores it (so its 'effects off' case sets it); here it is an effect that widens the face's
//     ramp, so the 'off' case leaves it out and a separate case checks it.
// The cross-backend comparison (the last test) renders outline plus shadow large (320 x 192) on both backends: STRICT 2/255 on the
// pixels away from the edges of any layer (a pixel is an edge pixel when its colour differs from a neighbour's, either frame,
// edge border 1), a cap on the excluded share, a floor on the compared ink, and the per-layer pixel counts of the classified
// WebGPU frame against the software frame's (each layer's area). The numbers are printed ([ cross-diff ]) and go in the PR.

using namespace N2Engine;
using namespace WebGpuTesting;
using namespace WebGpuTesting::Draw;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr Rgb kFace{255, 255, 0};    // Color::Yellow
    constexpr Rgb kOutline{0, 0, 255};   // Color::Blue
    constexpr Rgb kShadow{255, 0, 0};    // Color::Red

    const std::vector<Rgb>& Palette()
    {
        static const std::vector<Rgb> palette = {kBlack, kFace, kOutline, kShadow};
        return palette;
    }

    Frame Classified(const Frame& frame)
    {
        return Ink::Nearest(frame, Palette());
    }

    bool Inside(const Frame& frame, const int x, const int y)
    {
        return x >= 0 && y >= 0 && x < frame.width && y < frame.height;
    }

    class WebGpuTextEffectsGoldenTest : public WebGpuTest
    {
    protected:
        void SetUp() override
        {
            WebGpuTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            ASSERT_TRUE(R().SetRenderTargetSize(kWidth, kHeight));
        }

        static constexpr int kWidth = 160;
        static constexpr int kHeight = 96;
    };

    /// Outline pixels (of a classified frame) with no face pixel within widthPx (rounded up) plus 2 pixels, in x and y
    int OutlinePixelsFartherThan(const Frame& frame, const float widthPx)
    {
        const int reach = static_cast<int>(std::ceil(widthPx)) + 2;
        int farPixels = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                if (!(frame.At(x, y) == kOutline))
                {
                    continue;
                }
                bool nearFace = false;
                for (int oy = -reach; oy <= reach && !nearFace; ++oy)
                {
                    for (int ox = -reach; ox <= reach && !nearFace; ++ox)
                    {
                        nearFace = Inside(frame, x + ox, y + oy) && frame.At(x + ox, y + oy) == kFace;
                    }
                }
                farPixels += nearFace ? 0 : 1;
            }
        }
        return farPixels;
    }

    Text::TextEffects WithOutline(const float width)
    {
        Text::TextEffects effects;
        effects.outlineWidth = width;
        effects.outlineColor = Common::Color::Blue;
        return effects;
    }

    /// No pixel of the frame has the shadow's red: with a yellow face, a blue outline and a black background, red equals green
    int PixelsWithAShadowHue(const Frame& frame)
    {
        int count = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                count += Ink::IsRedFree(frame.At(x, y)) ? 0 : 1;
            }
        }
        return count;
    }
}

TEST_F(WebGpuTextEffectsGoldenTest, EffectsOffDrawsExactlyThePlainText)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Frame plain = scene.Render(Text::TextEffects{});
    ASSERT_GT(plain.Count(kFace), 50) << "the text drew nothing";
    // GL SEMANTICS: plain text is the face colour blended with the background (the software renderer's 'only the face colour'
    // is alpha testing): every pixel is on the line between black and yellow, and the antialiased edge exists
    EXPECT_EQ(Ink::OffTheLine(plain, kFace, kBlack), 0) << "plain text is only the face colour over the background";
    EXPECT_GT(Ink::InBetween(plain, kFace, kBlack), 20);

    // Effects that are off, however their other settings are set: an outline without width, a shadow with a transparent
    // colour (and the shadow's softness, which does nothing without a shadow). The software test also sets `softness` here
    // because the software renderer ignores it; OpenGL and WebGPU do not (it widens the face's ramp), so it is not 'off' and is
    // checked below instead. Both frames are WebGPU frames from the same uniforms, so they are exactly equal
    Text::TextEffects off;
    off.outlineWidth = 0.0f;
    off.outlineColor = Common::Color::Blue;
    off.shadowOffset = Math::Vector2(0.05f, -0.05f);
    off.shadowColor = Common::Color{1.0f, 0.0f, 0.0f, 0.0f};
    off.shadowSoftness = 0.05f;
    const Frame offFrame = scene.Render(off);
    EXPECT_EQ(offFrame.rgba, plain.rgba) << "effects that are off changed the image";

    // Softness is an effect here: the ramp widens (more in-between pixels) around the same iso-line (the same ink) and area
    Text::TextEffects soft;
    soft.softness = 0.05f;
    const Frame softFrame = scene.Render(soft);
    EXPECT_NE(softFrame.rgba, plain.rgba) << "softness changes nothing: it should widen the ramp on this backend";
    EXPECT_GT(Ink::InBetween(softFrame, kFace, kBlack), Ink::InBetween(plain, kFace, kBlack));
    EXPECT_NEAR(Ink::InkPixels(softFrame, kFace, kBlack), Ink::InkPixels(plain, kFace, kBlack),
                0.06 * Ink::InkPixels(plain, kFace, kBlack));
    EXPECT_NEAR(Ink::InkArea(softFrame, kFace, kBlack), Ink::InkArea(plain, kFace, kBlack), 0.06 * Ink::InkArea(plain, kFace, kBlack));
}

TEST_F(WebGpuTextEffectsGoldenTest, AnOutlineIsARingAroundTheFaceThatGrowsWithItsWidth)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Frame plain = Classified(scene.Render(Text::TextEffects{}));
    const float narrowEms = 0.05f;
    const float wideEms = 0.12f;
    const Frame narrowRaw = scene.Render(WithOutline(narrowEms));
    const Frame wideRaw = scene.Render(WithOutline(wideEms));
    const Frame narrow = Classified(narrowRaw);
    const Frame wide = Classified(wideRaw);

    // Only face, outline and background: no pixel has the shadow's hue (the outline is composited under the face and blended,
    // so its edge pixels are blends of the three, which classification reads as the layer they are mostly)
    EXPECT_EQ(PixelsWithAShadowHue(narrowRaw), 0);
    EXPECT_EQ(PixelsWithAShadowHue(wideRaw), 0);
    // The face is the plain text's (the software test: exactly; here to a few pixels on the iso-line)
    Ink::ExpectSameFaceApprox(plain, narrow, kFace);
    Ink::ExpectSameFaceApprox(plain, wide, kFace);

    // There is an outline, and a wider one covers more
    const int narrowCount = narrow.Count(kOutline);
    const int wideCount = wide.Count(kOutline);
    EXPECT_GT(narrowCount, 0) << "no outline drawn";
    EXPECT_GT(wideCount, narrowCount) << "a wider outline covers more pixels";

    // A ring: no face pixel touches the background (every face edge is outlined)...
    int faceNextToBackground = 0;
    for (int y = 0; y < narrow.height; ++y)
    {
        for (int x = 0; x < narrow.width; ++x)
        {
            if (!(narrow.At(x, y) == kFace))
            {
                continue;
            }
            constexpr int dx[] = {1, -1, 0, 0};
            constexpr int dy[] = {0, 0, 1, -1};
            for (int i = 0; i < 4; ++i)
            {
                const int nx = x + dx[i];
                const int ny = y + dy[i];
                if (Inside(narrow, nx, ny) && narrow.At(nx, ny) == kBlack)
                {
                    ++faceNextToBackground;
                }
            }
        }
    }
    EXPECT_EQ(faceNextToBackground, 0) << "face pixels next to the background: the outline has gaps";

    // ...and no outline pixel is further from the face than the outline's width (plus rounding)
    EXPECT_EQ(OutlinePixelsFartherThan(narrow, narrowEms * scene.Scale()), 0);
    EXPECT_EQ(OutlinePixelsFartherThan(wide, wideEms * scene.Scale()), 0);
}

TEST_F(WebGpuTextEffectsGoldenTest, AShadowIsOffsetInItsDirectionAndStaysUnderTheFace)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Frame plain = Classified(scene.Render(Text::TextEffects{}));

    // Down and to the right (+x right, +y up), by a whole number of pixels so moved pixels line up
    constexpr int shiftPx = 3;
    const float offsetEms = static_cast<float>(shiftPx) / scene.Scale();
    ASSERT_LT(offsetEms, 0.1f) << "the offset should be well inside the default font's limit";
    Text::TextEffects effects;
    effects.shadowOffset = Math::Vector2(offsetEms, -offsetEms);
    effects.shadowColor = Common::Color::Red;
    const Frame shadowedRaw = scene.Render(effects);
    const Frame shadowed = Classified(shadowedRaw);

    // No blue (no outline is set): red equals... the face is yellow and the shadow red over black, so blue is never there
    int blue = 0;
    for (int y = 0; y < shadowedRaw.height; ++y)
    {
        for (int x = 0; x < shadowedRaw.width; ++x)
        {
            blue += shadowedRaw.At(x, y).b > 2 ? 1 : 0;
        }
    }
    EXPECT_EQ(blue, 0) << "only the face, the shadow and the background";
    Ink::ExpectSameFaceApprox(plain, shadowed, kFace); // the shadow is under the face: it never changes the face
    ASSERT_GT(shadowed.Count(kShadow), 0) << "no shadow drawn";

    // The shadow is the face moved by the offset: almost every face pixel, moved, is face or shadow, and the shadow reaches
    // further right and further down than the face. (The software test needs 95% of the moved face pixels to be covered; on
    // an antialiased frame the pixels on the 0.5 iso-line may fall either side of the classification, so 90%)
    const int shiftX = shiftPx;
    const int shiftY = -shiftPx;
    int faceCount = 0;
    int movedCovered = 0;
    int faceMaxX = -1, faceMinY = shadowed.height, shadowMaxX = -1, shadowMinY = shadowed.height;
    for (int y = 0; y < shadowed.height; ++y)
    {
        for (int x = 0; x < shadowed.width; ++x)
        {
            const Rgb pixel = shadowed.At(x, y);
            if (pixel == kShadow)
            {
                shadowMaxX = std::max(shadowMaxX, x);
                shadowMinY = std::min(shadowMinY, y);
            }
            if (!(plain.At(x, y) == kFace))
            {
                continue;
            }
            ++faceCount;
            faceMaxX = std::max(faceMaxX, x);
            faceMinY = std::min(faceMinY, y);
            const int mx = x + shiftX;
            const int my = y + shiftY;
            if (Inside(shadowed, mx, my) && (shadowed.At(mx, my) == kFace || shadowed.At(mx, my) == kShadow))
            {
                ++movedCovered;
            }
        }
    }
    ASSERT_GT(faceCount, 0);
    EXPECT_GE(movedCovered, faceCount * 90 / 100) << "the shadow isn't the face moved by the offset";
    EXPECT_NEAR(shadowMaxX - faceMaxX, shiftX, 2) << "the shadow should reach right by the offset";
    EXPECT_NEAR(faceMinY - shadowMinY, -shiftY, 2) << "the shadow should reach down by the offset";

    // The opposite offset puts it on the other side
    effects.shadowOffset = Math::Vector2(-offsetEms, offsetEms);
    const Frame opposite = Classified(scene.Render(effects));
    int leftOfFace = 0;
    int rightOfFace = 0;
    int faceMinX = opposite.width;
    for (int y = 0; y < plain.height; ++y)
    {
        for (int x = 0; x < plain.width; ++x)
        {
            if (plain.At(x, y) == kFace)
            {
                faceMinX = std::min(faceMinX, x);
            }
        }
    }
    for (int y = 0; y < opposite.height; ++y)
    {
        for (int x = 0; x < opposite.width; ++x)
        {
            if (opposite.At(x, y) == kShadow)
            {
                leftOfFace += x < faceMinX ? 1 : 0;
                rightOfFace += x > faceMaxX ? 1 : 0;
            }
        }
    }
    EXPECT_GT(leftOfFace, 0) << "a shadow moved left should show left of the text";
    EXPECT_EQ(rightOfFace, 0) << "a shadow moved left shouldn't show right of the text";
}

TEST_F(WebGpuTextEffectsGoldenTest, AnOutlinedShadowIsDrawnUnderTheOutline)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const Frame plain = Classified(scene.Render(Text::TextEffects{}));
    Text::TextEffects effects = WithOutline(0.04f);
    const Frame outlined = Classified(scene.Render(effects));
    effects.shadowOffset = Math::Vector2(0.08f, -0.08f);
    effects.shadowColor = Common::Color::Red;
    const Frame bothRaw = scene.Render(effects);
    const Frame both = Classified(bothRaw);

    EXPECT_GT(both.Count(kShadow), 0);
    Ink::ExpectSameFaceApprox(plain, both, kFace);
    // The shadow goes under the outline: every outline pixel of the outlined text is still outline. (Exact on the software
    // renderer; here the outline's own half-coverage pixels, now over the shadow instead of the background, may fall either
    // side of the classification, so a few are allowed)
    int lost = 0;
    for (int y = 0; y < both.height; ++y)
    {
        for (int x = 0; x < both.width; ++x)
        {
            lost += (outlined.At(x, y) == kOutline && !(both.At(x, y) == kOutline)) ? 1 : 0;
        }
    }
    EXPECT_LE(lost, std::max(6, outlined.Count(kOutline) * 3 / 100)) << "the shadow overwrote outline pixels (" << lost << " of "
                                                                     << outlined.Count(kOutline) << ")";
}

TEST_F(WebGpuTextEffectsGoldenTest, EffectsBeyondTheSpreadAreClampedNotDrawnAsBoxes)
{
    Ink::EffectsScene scene(R(), kWidth, kHeight);
    ASSERT_TRUE(scene.Ok());

    const auto font = Text::Font::GetDefault();
    ASSERT_NE(font, nullptr);
    const float maxEms = Rendering::TextDrawing::MaxEffectEms(font->GetAtlasSettings());
    ASSERT_GT(maxEms, 0.0f);

    // Far past the spread: drawn as the largest outline the spread allows, never filling the glyph quads. The clamp gives the
    // same uniforms, so the two WebGPU frames are byte-equal (as the software ones are)
    const Frame atMax = scene.Render(WithOutline(maxEms));
    const Frame huge = scene.Render(WithOutline(10.0f));
    EXPECT_EQ(huge.rgba, atMax.rgba) << "an outline past the limit should draw as the limit";

    // A quad filled edge to edge would put outline pixels in the quad's corners, far from any face pixel
    EXPECT_EQ(OutlinePixelsFartherThan(Classified(huge), maxEms * scene.Scale()), 0)
        << "outline pixels far from the face: the glyph quads were filled";
    EXPECT_GT(Classified(huge).Count(kOutline), 0);
}

TEST_F(WebGpuTextEffectsGoldenTest, AGlyphsOutlineNeverPunchesHolesInItsNeighboursFace)
{
    // Tightly spaced, so each glyph's outline reaches into its neighbours' quads. The software test guards a depth problem
    // (an outline pixel that wrote depth made the next glyph's coplanar face fail the depth test). The Transparent state
    // writes no depth, so on WebGPU the question is the draw order inside one mesh: the glyph quads of one text are drawn in one
    // DrawMesh, in order, each blended over the last, so a later glyph's outline falls on an earlier glyph's face wherever the
    // outline reaches it. The face interior must be untouched: a hole is a pixel deep inside the plain text's face (all four
    // neighbours are face too) that is not face with the outline on
    Ink::EffectsScene scene(R(), kWidth, kHeight, "HIH", -0.08f);
    ASSERT_TRUE(scene.Ok());

    const Frame plain = Classified(scene.Render(Text::TextEffects{}));
    ASSERT_GT(plain.Count(kFace), 0);
    const Frame outlinedRaw = scene.Render(WithOutline(0.1f));
    const Frame outlined = Classified(outlinedRaw);
    ASSERT_GT(outlined.Count(kOutline), 0);

    int holes = 0;
    int interior = 0;
    for (int y = 1; y + 1 < plain.height; ++y)
    {
        for (int x = 1; x + 1 < plain.width; ++x)
        {
            if (!(plain.At(x, y) == kFace) || !(plain.At(x - 1, y) == kFace) || !(plain.At(x + 1, y) == kFace) ||
                !(plain.At(x, y - 1) == kFace) || !(plain.At(x, y + 1) == kFace))
            {
                continue;
            }
            ++interior;
            holes += outlined.At(x, y) == kFace ? 0 : 1;
        }
    }
    EXPECT_GT(interior, 100) << "the plain text has an interior";
    EXPECT_EQ(holes, 0) << "outline pixels inside a neighbouring glyph's face";
    // And the face itself is the plain text's, to the iso-line pixels
    Ink::ExpectSameFaceApprox(plain, outlined, kFace, 3.0, 8);
}

// ============================================================================
// Against the software renderer
// ============================================================================

namespace
{
    /// Outline, shadow and face over black, 320 x 192, on `renderer` (initialized at that size already)
    Frame RenderOutlinedShadow(Renderer::Common::IRenderer& renderer)
    {
        Ink::EffectsScene scene(renderer, 320, 192);
        EXPECT_TRUE(scene.Ok());
        Text::TextEffects effects = WithOutline(0.04f);
        effects.shadowOffset = Math::Vector2(0.08f, -0.08f);
        effects.shadowColor = Common::Color::Red;
        return scene.Render(effects);
    }
}

TEST_F(WebGpuTextEffectsGoldenTest, OutlineAndShadowMatchTheSoftwareRendererAwayFromTheLayerEdges)
{
    ASSERT_TRUE(R().SetRenderTargetSize(320, 192));
    const Frame webgpu = RenderOutlinedShadow(R());

    SoftwareRenderer software;
    ASSERT_TRUE(software.Initialize(nullptr, 320, 192));
    const Frame reference = RenderOutlinedShadow(software);
    software.Shutdown();

    // The software frame is alpha-tested: only the four exact colours
    int foreign = 0;
    for (int y = 0; y < reference.height; ++y)
    {
        for (int x = 0; x < reference.width; ++x)
        {
            const Rgb pixel = reference.At(x, y);
            foreign += (pixel == kBlack || pixel == kFace || pixel == kOutline || pixel == kShadow) ? 0 : 1;
        }
    }
    EXPECT_EQ(foreign, 0) << "the software frame is alpha-tested";
    EXPECT_GT(Ink::InBetween(webgpu, kFace, kBlack), 100) << "the WebGPU frame is antialiased";

    // STRICT 2/255 on the pixels away from the edges of every layer: a pixel is an edge pixel when its colour differs from a
    // neighbour's in either frame (the strict definition of CrossBackendDiff.hpp), edge border 1. Three layers' boundaries are
    // about a third of the frame at most, so the cap is 40%; at least 500 ink pixels (any layer) are compared
    Ink::AntialiasedRule rule;
    rule.edgeBorder = 1;
    rule.coverageEdges = false;
    rule.checkArea = false;
    rule.maxExcludedFraction = 0.4;
    rule.minInteriorPixels = 500;
    EXPECT_TRUE(Ink::CompareAntialiased("text outline + shadow 320x192", webgpu, reference, kFace, kBlack, rule));

    // The area of each layer: the WebGPU frame classified to the nearest layer colour against the software layers. The iso-lines
    // each layer is cut at are the same on both backends (0.5 for the face, 0.5 - outline for the outline and the shadow). The
    // shadow and the face-plus-outline (the whole inked shape inside the outline's edge, which does not depend on where the
    // face and the outline meet) are compared within 5% (or 10 pixels): that is the area check, the one that fails a glyph
    // that is too thin or too fat. The face and the outline alone are compared within 12%: they share a boundary whose
    // classification, a pixel that is half face and half outline, is sensitive to how the CPU driver rounds a filtered sample
    // (measured: the x86_64 lavapipe leg gives 0.97 and 1.03 of the software's face and outline, the arm64 one 0.91 and 1.08,
    // while their sum agrees with the software's to 0.2% to 0.4% on both)
    const Frame classified = Classified(webgpu);
    const auto countOf = [](const Frame& frame, const Rgb layer) { return frame.Count(layer); };
    const auto report = [&](const std::string& what, const int webgpuPixels, const int softwarePixels, const double allowedShare)
    {
        const double ratio = softwarePixels > 0 ? static_cast<double>(webgpuPixels) / softwarePixels : 0.0;
        std::cout << "[ cross-diff ] text outline + shadow 320x192, " << what << ": " << webgpuPixels
                  << " WebGPU pixels (nearest layer) against " << softwarePixels << " software pixels, ratio " << ratio
                  << std::endl;
        Diff::RecordedLines().push_back("text outline + shadow 320x192, " + what + ": " + std::to_string(webgpuPixels) +
                                        " against " + std::to_string(softwarePixels));
        EXPECT_GT(softwarePixels, 0) << what;
        EXPECT_NEAR(webgpuPixels, softwarePixels, std::max(10.0, allowedShare * softwarePixels)) << what;
    };
    report("face and outline together", countOf(classified, kFace) + countOf(classified, kOutline),
           countOf(reference, kFace) + countOf(reference, kOutline), 0.05);
    report("shadow", countOf(classified, kShadow), countOf(reference, kShadow), 0.05);
    report("face alone", countOf(classified, kFace), countOf(reference, kFace), 0.12);
    report("outline alone", countOf(classified, kOutline), countOf(reference, kOutline), 0.12);
}
