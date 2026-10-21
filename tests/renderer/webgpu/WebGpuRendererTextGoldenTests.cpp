#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <renderer/webgpu/WebGpuRenderer.hpp>
#include <text/TextLayout.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

#include "common/CrossBackendDiff.hpp"
#include "WebGpuDrawTestUtil.hpp"
#include "WebGpuTestFixture.hpp"
#include "WebGpuTextSceneUtil.hpp"
#include "WebGpuTextTestUtil.hpp"

// Golden-image tests of text through the engine on the WebGPU backend (#43, stage E): the text cases of
// tests/engine/rendering/SoftwareTextGoldenTests.cpp (SoftwareTextGoldenTest.TextRendererDrawsItsGlyphsInsideItsProjectedLayout
// and AChangedColourRedrawsInTheNewColour, SoftwareUIGoldenTest.UITextInkSitsInsideItsRectWithItsAlignment and
// UITextDrawsOverAnImageInHierarchyOrder; the two Image-only UI cases are stage D's, in WebGpuImageGoldenTests.cpp) with the same
// scenes, cameras, sizes and structural expectations: a TextRenderer drawn by Scene::Render, and a UIText drawn by the UI pass
// (UISystem::Render: no depth test, blended), on a WebGPU device (Mesa lavapipe in CI; no window, the offscreen target) and
// read back.
//
// Where the software test encodes alpha testing, the expectation here is OpenGL's (this shader is a port of its):
//   - the software renderer draws a glyph pixel as exactly the text colour or not at all ("every touched pixel is the text's
//     yellow"); WebGPU antialiases the edge over about one pixel and blends it, so an edge pixel is the text colour blended with
//     what is behind it. The tests assert that every pixel lies on the line between the background and the text colour (no
//     other hue) and that antialiased pixels exist, and then read the frame the way the software tests do after THRESHOLDING it
//     at half coverage (the 0.5 iso-line the software alpha test cuts at): the bounding boxes, column runs and counts are those
//     of the thresholded frame;
//   - the text is drawn in the Transparent queue (no depth write) or the UI pass (no depth test): nothing here depends on text
//     writing depth, which the software renderer does.
// A cross-backend comparison closes each family: the same scene on the software renderer and on WebGPU, rendered LARGE (320 x
// 192; 400 x 160) so that the glyph stems are wide enough to have interiors, compared by CrossBackendDiff's STRICT 2/255 on the
// pixels away from the coverage edges (edge border 1: the ramp is about one pixel wide) with a cap on the excluded share, a floor
// on the compared ink interior (so it is never a comparison of the background alone) and the ink-area check (the summed blend
// weights against the software ink pixel count). The measured numbers are printed ([ cross-diff ]) and go in the PR.

using namespace N2Engine;
using namespace WebGpuTesting;
using namespace WebGpuTesting::Draw;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr Rgb kCyanText{0, 255, 255};

    /// Pixels that are not `background`: how many, and their bounding box
    struct Extent
    {
        int count = 0;
        int minX = 0, minY = 0, maxX = -1, maxY = -1;
    };

    Extent CoverageExcept(const Frame& frame, const Rgb background)
    {
        Extent c;
        c.minX = frame.width;
        c.minY = frame.height;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                if (!(frame.At(x, y) == background))
                {
                    ++c.count;
                    c.minX = std::min(c.minX, x);
                    c.maxX = std::max(c.maxX, x);
                    c.minY = std::min(c.minY, y);
                    c.maxY = std::max(c.maxY, y);
                }
            }
        }
        return c;
    }

    struct ColumnRun
    {
        int first = 0;
        int last = 0;
        [[nodiscard]] int Width() const { return last - first + 1; }
    };

    /// The runs of consecutive columns holding any covered pixel: separated glyphs give separate runs
    std::vector<ColumnRun> ColumnRuns(const Frame& frame, const Rgb background)
    {
        std::vector<ColumnRun> runs;
        bool inRun = false;
        for (int x = 0; x < frame.width; ++x)
        {
            bool covered = false;
            for (int y = 0; y < frame.height && !covered; ++y)
            {
                covered = !(frame.At(x, y) == background);
            }
            if (covered && !inRun)
            {
                runs.push_back(ColumnRun{x, x});
            }
            else if (covered)
            {
                runs.back().last = x;
            }
            inRun = covered;
        }
        return runs;
    }

    class WebGpuTextGoldenTest : public WebGpuTest
    {
    protected:
        void SetUp() override
        {
            WebGpuTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
        }
    };

    // ---- "Hi" through a TextRenderer, on any renderer --------------------------------------------------------------------

    struct HiScene
    {
        Frame frame;
        Text::TextLayout layout;
        float scale = 1.0f;
        float left = 0.0f;
        float bottom = 0.0f;
    };

    /// The software test's scene: "Hi" in yellow, the layout fitted into the frame with a 16 px margin (at 160 x 96; the
    /// margin scales with the frame) by an orthographic camera, drawn by Scene::Render, read back. `renderer` is initialized at
    /// width x height already.
    HiScene RenderHi(Renderer::Common::IRenderer& renderer, const int width, const int height)
    {
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);

        const auto scene = Scene::Create("WebGPUTextGolden_Hi");
        auto object = GameObject::Create("Hi");
        auto* text = object->AddComponent<Rendering::TextRenderer>();
        EXPECT_NE(text, nullptr);
        text->SetText("Hi");
        text->SetColor(Common::Color::Yellow);
        scene->AddRootGameObject(object);

        HiScene result;
        result.layout = text->GetLayout(); // a copy: drawing may lay the text out again
        const Text::Rect bounds = result.layout.bounds;

        // An orthographic camera looking down -Z at the text (on z = 0), scaled so the layout fills the frame with a margin,
        // and centred on it. World (x, y) lands on pixel ((x - left) * scale, (y - bottom) * scale), y up.
        const float margin = 16.0f * static_cast<float>(width) / 160.0f;
        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        result.scale = std::min((w - 2.0f * margin) / bounds.Width(), (h - 2.0f * margin) / bounds.Height());
        result.left = (bounds.minX + bounds.maxX) * 0.5f - w * 0.5f / result.scale;
        result.bottom = (bounds.minY + bounds.maxY) * 0.5f - h * 0.5f / result.scale;
        Camera camera;
        camera.SetPosition(Math::Vector3(0.0f, 0.0f, 5.0f));
        camera.SetOrthographic(result.left, result.left + w / result.scale, result.bottom, result.bottom + h / result.scale, 0.1f,
                               100.0f);

        // What Application::Render does for the scene
        renderer.BeginFrame();
        renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
        scene->Render(&renderer, camera);
        renderer.EndFrame();
        renderer.Present();
        result.frame = Ink::ReadBack(renderer, width, height);

        text->CleanupRenderResources(&renderer);
        return result;
    }

    // ---- UI text, on any renderer ----------------------------------------------------------------------------------------

    // The default font's metrics (Noto Sans Regular, font units): what the UI text checks below are derived from. "Hi" has no
    // kerning pair, so its line is H's advance plus i's.
    constexpr float kUnitsPerEm = 1000.0f;
    constexpr float kAscent = 1069.0f;
    constexpr float kDescent = -293.0f;
    constexpr float kAdvanceH = 741.0f;
    constexpr float kInkLeftH = 97.0f; // H's ink spans 97..643
    constexpr float kAdvanceI = 258.0f;
    constexpr float kInkRightI = 181.0f; // i's ink spans 78..181

    /// A canvas in a new scene; elements are pinned to its bottom-left corner, so their rects are exact
    struct UIScene
    {
        std::unique_ptr<Scene> scene;
        GameObject::Ptr canvas;
    };

    UIScene MakeUIScene(const std::string& name)
    {
        UIScene result{Scene::Create(name), UI::UISystem::CreateCanvas(name + "_Canvas")};
        result.scene->AddRootGameObject(result.canvas);
        return result;
    }

    GameObject::Ptr AddElement(const GameObject::Ptr& parent, const std::string& name, const float x, const float y,
                               const float width, const float height)
    {
        auto element = UI::UISystem::CreateElement(name);
        auto* rectTransform = element->GetComponent<UI::RectTransform>();
        rectTransform->SetAnchorMin(Math::Vector2{0.0f, 0.0f});
        rectTransform->SetAnchorMax(Math::Vector2{0.0f, 0.0f});
        rectTransform->SetPivot(Math::Vector2{0.0f, 0.0f});
        rectTransform->SetAnchoredPosition(Math::Vector2{x, y});
        rectTransform->SetSizeDelta(Math::Vector2{width, height});
        parent->AddChild(element, false);
        return element;
    }

    Frame RenderUIPass(Renderer::Common::IRenderer& renderer, const Scene& scene, const int width, const int height)
    {
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        renderer.BeginFrame();
        UI::UISystem::Render(scene, &renderer, Vector2i{width, height});
        renderer.EndFrame();
        renderer.Present();
        return Ink::ReadBack(renderer, width, height);
    }

    /// A UIText "Hi" in yellow in a rect (canvas space, y up) of a width x height frame, centred
    Frame RenderUiHi(Renderer::Common::IRenderer& renderer, const int width, const int height, const float rectX,
                     const float rectY, const float rectW, const float rectH, const float fontSize)
    {
        UIScene ui = MakeUIScene("WebGPUTextGolden_UiHi");
        const auto element = AddElement(ui.canvas, "Label", rectX, rectY, rectW, rectH);
        auto* text = element->AddComponent<UI::UIText>();
        EXPECT_NE(text, nullptr);
        text->SetText("Hi");
        text->SetFontSize(fontSize);
        text->SetColor(Common::Color::Yellow);
        text->SetHorizontalAlign(Text::HorizontalAlign::Center);
        text->SetVerticalAlign(Text::VerticalAlign::Middle);
        const Frame frame = RenderUIPass(renderer, *ui.scene, width, height);
        ui.scene.reset(); // the scene and its UI (and the font atlas texture) go before the renderer does
        return frame;
    }

    int CountColour(const Frame& frame, const Rgb colour)
    {
        return frame.Count(colour);
    }

    constexpr Rgb kRedPanel{255, 0, 0};
    constexpr Rgb kWhiteText{255, 255, 255};
}

// ============================================================================
// TextRenderer
// ============================================================================

TEST_F(WebGpuTextGoldenTest, TextRendererDrawsItsGlyphsInsideItsProjectedLayout)
{
    constexpr int width = 160;
    constexpr int height = 96;
    ASSERT_TRUE(R().SetRenderTargetSize(width, height));

    const HiScene scene = RenderHi(R(), width, height);
    ASSERT_EQ(scene.layout.quads.size(), 2u) << "one quad per glyph";
    ASSERT_EQ(scene.layout.lines.size(), 1u);
    const Text::Rect bounds = scene.layout.bounds;
    ASSERT_FALSE(bounds.IsEmpty());
    const auto toPixelX = [&](const float x) { return (x - scene.left) * scene.scale; };
    const auto toPixelY = [&](const float y) { return (y - scene.bottom) * scene.scale; };

    // GL SEMANTICS. The software test asserts that every touched pixel is exactly the text colour (alpha-tested, never
    // blended with the background). WebGPU antialiases and blends: every pixel is the text's yellow blended with the black
    // background (on the line between them, no other hue), and the edge pixels in between exist
    const Frame& raw = scene.frame;
    ASSERT_GT(CoverageExcept(raw, kBlack).count, 0) << "the text drew nothing";
    EXPECT_EQ(Ink::OffTheLine(raw, kYellow, kBlack), 0) << "pixels that are not yellow blended with the background";
    EXPECT_GT(Ink::InBetween(raw, kYellow, kBlack), 20) << "no antialiased edge pixels: the text was alpha-tested";
    EXPECT_GT(raw.Count(kYellow), 50) << "glyph interiors are exactly the text colour";

    // The software test's structure, on the frame thresholded at half coverage (what its alpha test draws)
    const Frame frame = Ink::Thresholded(raw, kYellow, kBlack);
    const Extent c = CoverageExcept(frame, kBlack);
    ASSERT_GT(c.count, 0);

    // The ink lies inside the layout's box (the lines' advance width, ascent to descent), give or take two pixels, and sits on
    // the baseline
    constexpr float tolerancePx = 2.0f;
    EXPECT_GE(static_cast<float>(c.minX), toPixelX(bounds.minX) - tolerancePx);
    EXPECT_LE(static_cast<float>(c.maxX + 1), toPixelX(bounds.maxX) + tolerancePx);
    EXPECT_GE(static_cast<float>(c.minY), toPixelY(bounds.minY) - tolerancePx);
    EXPECT_LE(static_cast<float>(c.maxY + 1), toPixelY(bounds.maxY) + tolerancePx);
    EXPECT_NEAR(static_cast<float>(c.minY), toPixelY(scene.layout.lines[0].baseline), 3.0f) << "H and i stand on the baseline";

    // ...and fills most of it: the side bearings are small, and H's cap height is about half the line
    const float boundsWidthPx = bounds.Width() * scene.scale;
    const float boundsHeightPx = bounds.Height() * scene.scale;
    EXPECT_GE(static_cast<float>(c.maxX - c.minX + 1), 0.7f * boundsWidthPx);
    EXPECT_GE(static_cast<float>(c.maxY - c.minY + 1), 0.4f * boundsHeightPx);

    // Two glyphs, two separate clusters of columns, the H wider than the i
    const std::vector<ColumnRun> runs = ColumnRuns(frame, kBlack);
    ASSERT_EQ(runs.size(), 2u) << "H and i should be separated by empty columns";
    EXPECT_GT(runs[0].Width(), runs[1].Width());
    EXPECT_GE(runs[1].first - runs[0].last, 3) << "a gap of a few pixels between the glyphs";
    // Each cluster lies within its glyph's quad
    for (std::size_t i = 0; i < 2; ++i)
    {
        const Text::Rect& quad = scene.layout.quads[i].position;
        EXPECT_GE(static_cast<float>(runs[i].first), toPixelX(quad.minX) - tolerancePx) << "glyph " << i;
        EXPECT_LE(static_cast<float>(runs[i].last + 1), toPixelX(quad.maxX) + tolerancePx) << "glyph " << i;
    }
}

TEST_F(WebGpuTextGoldenTest, TextRendererMatchesTheSoftwareRendererAwayFromTheGlyphEdges)
{
    // The same scene on both backends, rendered large (320 x 192: the stems of H and i are about 8 pixels wide, so they have
    // interiors), and compared as described at the top: strict 2/255 away from the coverage edges, the ink area within 5%
    constexpr int width = 320;
    constexpr int height = 192;
    ASSERT_TRUE(R().SetRenderTargetSize(width, height));
    const HiScene webgpu = RenderHi(R(), width, height);

    SoftwareRenderer software;
    ASSERT_TRUE(software.Initialize(nullptr, width, height));
    const HiScene reference = RenderHi(software, width, height);
    software.Shutdown();

    // The software frame is alpha-tested: exactly yellow or black
    EXPECT_EQ(Ink::InBetween(reference.frame, kYellow, kBlack), 0);
    EXPECT_GT(Ink::InBetween(webgpu.frame, kYellow, kBlack), 40);

    // Edge border 1: the antialiasing ramp is about one pixel wide (two at 45 degrees). At most 25% of the frame may be
    // left out (the glyphs are about a tenth of it), and at least 300 ink pixels must be compared
    Ink::AntialiasedRule rule;
    rule.edgeBorder = 1;
    rule.maxExcludedFraction = 0.25;
    rule.maxAreaError = 0.05;
    rule.minInteriorPixels = 300;
    EXPECT_TRUE(Ink::CompareAntialiased("TextRenderer 'Hi' 320x192", webgpu.frame, reference.frame, kYellow, kBlack, rule));
}

TEST_F(WebGpuTextGoldenTest, AChangedColourRedrawsInTheNewColour)
{
    constexpr int width = 96;
    constexpr int height = 64;
    ASSERT_TRUE(R().SetRenderTargetSize(width, height));
    R().Clear(0.0f, 0.0f, 0.0f, 1.0f);

    auto object = GameObject::Create("Colour");
    auto* text = object->AddComponent<Rendering::TextRenderer>();
    text->SetText("T");
    text->SetHorizontalAlign(Text::HorizontalAlign::Center);
    text->SetVerticalAlign(Text::VerticalAlign::Middle);

    Camera camera;
    camera.SetPosition(Math::Vector3(0.0f, 0.0f, 5.0f));
    camera.SetOrthographic(-1.5f, 1.5f, -1.0f, 1.0f, 0.1f, 100.0f);

    const auto render = [&](const Common::Color& colour)
    {
        text->SetColor(colour);
        R().BeginFrame();
        R().SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
        text->Render(&R());
        R().EndFrame();
        R().Present();
        return Ink::ReadBack(R(), width, height);
    };

    const Frame cyan = render(Common::Color::Cyan);
    const Frame red = render(Common::Color::Red);
    ASSERT_GT(Ink::InkPixels(cyan, kCyanText, kBlack), 0);
    EXPECT_NEAR(Ink::InkPixels(red, kRedPanel, kBlack), Ink::InkPixels(cyan, kCyanText, kBlack), 3) << "the same glyph, only recoloured";
    EXPECT_NEAR(Ink::InkArea(red, kRedPanel, kBlack), Ink::InkArea(cyan, kCyanText, kBlack), 1.0);
    EXPECT_EQ(Ink::OffTheLine(cyan, kCyanText, kBlack), 0);
    EXPECT_EQ(Ink::OffTheLine(red, kRedPanel, kBlack), 0);
    // Every pixel is the same blend of the two colours: where the cyan frame is w * cyan, the red one is w * red
    int interior = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const Rgb c = cyan.At(x, y);
            const Rgb r = red.At(x, y);
            EXPECT_EQ(c.r, 0) << x << ", " << y;
            EXPECT_NEAR(static_cast<int>(c.g), static_cast<int>(r.r), 1) << "the same coverage at " << x << ", " << y;
            EXPECT_NEAR(static_cast<int>(c.b), static_cast<int>(r.r), 1) << x << ", " << y;
            EXPECT_EQ(r.g, 0);
            EXPECT_EQ(r.b, 0);
            if (c == kCyanText)
            {
                ++interior;
                EXPECT_EQ(r, kRedPanel) << "the interior is exactly the new colour at " << x << ", " << y;
            }
        }
    }
    EXPECT_GT(interior, 30) << "the glyph has an interior";

    text->CleanupRenderResources(&R());
}

// ============================================================================
// UIText
// ============================================================================

TEST_F(WebGpuTextGoldenTest, UITextInkSitsInsideItsRectWithItsAlignment)
{
    constexpr int width = 200;
    constexpr int height = 80;
    // The rect (canvas space, y up) and the text size: 32 px per em, so one font unit is 0.032 px
    constexpr float rectX = 20.0f;
    constexpr float rectY = 10.0f;
    constexpr float rectW = 160.0f;
    constexpr float rectH = 60.0f;
    constexpr float fontSize = 32.0f;
    constexpr float s = fontSize / kUnitsPerEm;
    // Generous: the SDF edge, bilinear sampling and pixel centres each move an edge by under a pixel
    constexpr float tolerancePx = 2.0f;
    ASSERT_TRUE(R().SetRenderTargetSize(width, height));

    struct Case
    {
        Text::HorizontalAlign horizontal;
        Text::VerticalAlign vertical;
        const char* name;
    };
    for (const Case& alignment : {Case{Text::HorizontalAlign::Left, Text::VerticalAlign::Top, "Left/Top"},
                                  Case{Text::HorizontalAlign::Center, Text::VerticalAlign::Middle, "Center/Middle"},
                                  Case{Text::HorizontalAlign::Right, Text::VerticalAlign::Bottom, "Right/Bottom"}})
    {
        UIScene ui = MakeUIScene("WebGPUTextGolden_UiText");
        const auto element = AddElement(ui.canvas, "Label", rectX, rectY, rectW, rectH);
        auto* text = element->AddComponent<UI::UIText>();
        ASSERT_NE(text, nullptr);
        text->SetText("Hi");
        text->SetFontSize(fontSize);
        text->SetColor(Common::Color::Yellow);
        text->SetHorizontalAlign(alignment.horizontal);
        text->SetVerticalAlign(alignment.vertical);

        const Frame raw = RenderUIPass(R(), *ui.scene, width, height);
        ASSERT_GT(CoverageExcept(raw, kBlack).count, 0) << alignment.name << ": the text drew nothing";

        // GL SEMANTICS: yellow blended with the background (the UI pass blends), not alpha-tested pixels of exactly the text's
        // yellow. Everything below reads the frame thresholded at half coverage, as the software test reads its own
        EXPECT_EQ(Ink::OffTheLine(raw, kYellow, kBlack), 0) << alignment.name << ": pixels that are not yellow blended with black";
        const Frame frame = Ink::Thresholded(raw, kYellow, kBlack);
        const Extent c = CoverageExcept(frame, kBlack);
        ASSERT_GT(c.count, 0) << alignment.name;
        EXPECT_EQ(CountColour(frame, kYellow), c.count) << alignment.name;

        // The ink is inside the rect
        EXPECT_GE(static_cast<float>(c.minX), rectX - 1.0f) << alignment.name;
        EXPECT_LE(static_cast<float>(c.maxX + 1), rectX + rectW + 1.0f) << alignment.name;
        EXPECT_GE(static_cast<float>(c.minY), rectY - 1.0f) << alignment.name;
        EXPECT_LE(static_cast<float>(c.maxY + 1), rectY + rectH + 1.0f) << alignment.name;
        // Both glyphs are there: the ink is about as wide as H's left edge to i's right edge
        EXPECT_NEAR(static_cast<float>(c.maxX + 1 - c.minX), (kAdvanceH + kInkRightI - kInkLeftH) * s, 2.0f * tolerancePx)
            << alignment.name;

        // Horizontally: the line (H's advance plus i's) is placed by the alignment, and the ink sits in it
        const float lineWidth = (kAdvanceH + kAdvanceI) * s;
        float lineLeft = rectX;
        switch (alignment.horizontal)
        {
        case Text::HorizontalAlign::Left:
            lineLeft = rectX;
            break;
        case Text::HorizontalAlign::Center:
            lineLeft = rectX + (rectW - lineWidth) * 0.5f;
            break;
        case Text::HorizontalAlign::Right:
            lineLeft = rectX + rectW - lineWidth;
            break;
        }
        EXPECT_NEAR(static_cast<float>(c.minX), lineLeft + kInkLeftH * s, tolerancePx) << alignment.name << ": H's left edge";
        EXPECT_NEAR(static_cast<float>(c.maxX + 1), lineLeft + (kAdvanceH + kInkRightI) * s, tolerancePx)
            << alignment.name << ": i's right edge";

        // Vertically: H and i stand on the baseline, which the alignment places
        const float blockHeight = (kAscent - kDescent) * s;
        float baseline = 0.0f;
        switch (alignment.vertical)
        {
        case Text::VerticalAlign::Top:
            baseline = rectY + rectH - kAscent * s;
            break;
        case Text::VerticalAlign::Middle:
            baseline = rectY + rectH * 0.5f + blockHeight * 0.5f - kAscent * s;
            break;
        case Text::VerticalAlign::Bottom:
        case Text::VerticalAlign::Baseline:
            baseline = rectY - kDescent * s;
            break;
        }
        EXPECT_NEAR(static_cast<float>(c.minY), baseline, tolerancePx) << alignment.name << ": the baseline";
        // ...and reach well up towards the ascent (H's cap height is about two thirds of an em)
        EXPECT_GE(static_cast<float>(c.maxY + 1) - baseline, 0.5f * kAscent * s) << alignment.name;
        EXPECT_LE(static_cast<float>(c.maxY + 1) - baseline, kAscent * s + tolerancePx) << alignment.name;

        ui.scene.reset();
    }
}

TEST_F(WebGpuTextGoldenTest, UITextMatchesTheSoftwareRendererAwayFromTheGlyphEdges)
{
    // A UIText "Hi" at 96 px (the stems are about 9 pixels wide) in a 400 x 160 frame, on both backends; compared as the
    // TextRenderer scene above is
    constexpr int width = 400;
    constexpr int height = 160;
    ASSERT_TRUE(R().SetRenderTargetSize(width, height));
    const Frame webgpu = RenderUiHi(R(), width, height, 20.0f, 10.0f, 360.0f, 140.0f, 96.0f);

    SoftwareRenderer software;
    ASSERT_TRUE(software.Initialize(nullptr, width, height));
    const Frame reference = RenderUiHi(software, width, height, 20.0f, 10.0f, 360.0f, 140.0f, 96.0f);
    software.Shutdown();

    EXPECT_EQ(Ink::InBetween(reference, kYellow, kBlack), 0) << "the software frame is alpha-tested";
    EXPECT_GT(Ink::InBetween(webgpu, kYellow, kBlack), 40);
    Ink::AntialiasedRule rule;
    rule.edgeBorder = 1;
    rule.maxExcludedFraction = 0.25;
    rule.maxAreaError = 0.05;
    rule.minInteriorPixels = 300;
    EXPECT_TRUE(Ink::CompareAntialiased("UIText 'Hi' 400x160", webgpu, reference, kYellow, kBlack, rule));
}

TEST_F(WebGpuTextGoldenTest, UITextDrawsOverAnImageInHierarchyOrder)
{
    constexpr int width = 120;
    constexpr int height = 60;
    ASSERT_TRUE(R().SetRenderTargetSize(width, height));

    const auto addPanel = [](const GameObject::Ptr& parent, const std::string& name)
    {
        auto panel = AddElement(parent, name, 10.0f, 10.0f, 100.0f, 40.0f);
        panel->AddComponent<UI::Image>()->SetColor(Common::Color::Red);
        return panel;
    };
    const auto addLabel = [](const GameObject::Ptr& parent, const std::string& name)
    {
        auto label = AddElement(parent, name, 10.0f, 10.0f, 100.0f, 40.0f);
        auto* text = label->AddComponent<UI::UIText>();
        text->SetText("HH");
        text->SetFontSize(32.0f);
        text->SetColor(Common::Color::White);
        text->SetHorizontalAlign(Text::HorizontalAlign::Center);
        text->SetVerticalAlign(Text::VerticalAlign::Middle);
        return label;
    };
    // Black, the panel's red and the text's white, every pixel the nearest of them: the antialiased edge of the white text is a
    // blend of white and red (the UI pass blends), which the software renderer, alpha-testing, never has
    const std::vector<Rgb> palette = {kBlack, kRedPanel, kWhiteText};

    // The text after the image (a later sibling) draws on top of it
    {
        UIScene ui = MakeUIScene("WebGPUTextGolden_TextOnTop");
        addPanel(ui.canvas, "Panel");
        addLabel(ui.canvas, "Label");
        const Frame raw = RenderUIPass(R(), *ui.scene, width, height);
        const Frame frame = Ink::Nearest(raw, palette);

        const int white = CountColour(frame, kWhiteText);
        const int red = CountColour(frame, kRedPanel);
        EXPECT_GT(white, 50) << "the text shows over the panel";
        EXPECT_GT(red, 100 * 40 / 2) << "most of the panel is still red";
        EXPECT_NEAR(white + red, 100 * 40, 100 * 40 / 10) << "text pixels replace panel pixels";
        EXPECT_EQ(white + red, CoverageExcept(frame, kBlack).count) << "only the panel's and the text's colours";
        // GL SEMANTICS: an edge pixel is white blended with the panel's red (on the line between them), and the panel's own
        // edge, aligned to pixels, is sharp. Nothing is another hue
        EXPECT_EQ(Ink::OffTheLine(raw, kWhiteText, kRedPanel) - CountColour(raw, kBlack), 0)
            << "pixels that are not the panel's red blended with the text's white (black outside the panel does not count)";
        EXPECT_GT(Ink::InBetween(raw, kWhiteText, kRedPanel) - CountColour(raw, kBlack), 20) << "no antialiased text edge";
        // Every white pixel is inside the panel
        Extent whiteInk;
        whiteInk.minX = width;
        whiteInk.minY = height;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if (frame.At(x, y) == kWhiteText)
                {
                    ++whiteInk.count;
                    whiteInk.minX = std::min(whiteInk.minX, x);
                    whiteInk.maxX = std::max(whiteInk.maxX, x);
                    whiteInk.minY = std::min(whiteInk.minY, y);
                    whiteInk.maxY = std::max(whiteInk.maxY, y);
                }
            }
        }
        EXPECT_GE(whiteInk.minX, 9);
        EXPECT_LE(whiteInk.maxX, 110);
        EXPECT_GE(whiteInk.minY, 9);
        EXPECT_LE(whiteInk.maxY, 50);
        ui.scene.reset();
    }

    // A child of the image draws on top too
    {
        UIScene ui = MakeUIScene("WebGPUTextGolden_TextChild");
        const auto panel = addPanel(ui.canvas, "Panel");
        const auto label = addLabel(panel, "Label");
        label->GetComponent<UI::RectTransform>()->StretchToParent(); // the panel's rect
        const Frame frame = Ink::Nearest(RenderUIPass(R(), *ui.scene, width, height), palette);
        EXPECT_GT(CountColour(frame, kWhiteText), 50) << "a child draws over its parent";
        ui.scene.reset();
    }

    // The text before the image (an earlier sibling) is covered by it: the panel (alpha 1) blends to exactly its colour over
    // whatever is under it, so there is no trace of the text, as on the software renderer
    {
        UIScene ui = MakeUIScene("WebGPUTextGolden_TextUnder");
        addLabel(ui.canvas, "Label");
        addPanel(ui.canvas, "Panel");
        const Frame frame = RenderUIPass(R(), *ui.scene, width, height);
        EXPECT_EQ(CountColour(frame, kWhiteText), 0) << "the panel drawn later covers the text";
        EXPECT_NEAR(CountColour(frame, kRedPanel), 100 * 40, 100 * 40 / 10);
        EXPECT_EQ(Ink::InBetween(frame, kRedPanel, kBlack), 0) << "nothing of the text shows through the panel";
        ui.scene.reset();
    }
}
