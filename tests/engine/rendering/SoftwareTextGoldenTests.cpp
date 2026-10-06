#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
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

// Golden-image tests through the engine: a TextRenderer drawn by Scene::Render, and a UI Image and UIText
// drawn by the UI pass, rasterized by a headless SoftwareRenderer (no window or GPU) and read back. The checks are
// structural (bounding boxes against the layout, colours, separated glyphs) with pixel tolerances, never
// exact images: Debug and Release may round floats differently.

using namespace N2Engine;
using Renderer::Software::SoftwareRenderer;

namespace
{
    struct Rgb
    {
        std::uint8_t r = 0, g = 0, b = 0;
        bool operator==(const Rgb &) const = default;
    };

    /// A frame read back with ReadFramebuffer: RGBA8, row 0 at the bottom
    struct Frame
    {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;

        /// y counts up from the bottom row
        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        }
    };

    Frame ReadBack(const SoftwareRenderer &renderer, const int width, const int height)
    {
        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4),
                    width, height};
        renderer.ReadFramebuffer(frame.rgba.data(), width, height);
        return frame;
    }

    /// Pixels that aren't the clear colour: how many, and their bounding box
    struct Coverage
    {
        int count = 0;
        int minX = 0, minY = 0, maxX = -1, maxY = -1;
    };

    Coverage CoverageExcept(const Frame &frame, const Rgb background)
    {
        Coverage c;
        c.minX = frame.width;
        c.minY = frame.height;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                if (frame.At(x, y) != background)
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
    std::vector<ColumnRun> ColumnRuns(const Frame &frame, const Rgb background)
    {
        std::vector<ColumnRun> runs;
        bool inRun = false;
        for (int x = 0; x < frame.width; ++x)
        {
            bool covered = false;
            for (int y = 0; y < frame.height && !covered; ++y)
            {
                covered = frame.At(x, y) != background;
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

    constexpr Rgb Black{0, 0, 0};
}

TEST(SoftwareTextGoldenTest, TextRendererDrawsItsGlyphsInsideItsProjectedLayout)
{
    constexpr int width = 160;
    constexpr int height = 96;
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, width, height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);

    const auto scene = Scene::Create("SoftwareTextGolden_Hi");
    auto object = GameObject::Create("Hi");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    ASSERT_NE(text, nullptr);
    text->SetText("Hi");
    text->SetColor(Common::Color::Yellow);
    scene->AddRootGameObject(object);

    const Text::TextLayout layout = text->GetLayout(); // a copy: drawing may lay the text out again
    ASSERT_EQ(layout.quads.size(), 2u) << "one quad per glyph";
    ASSERT_EQ(layout.lines.size(), 1u);
    const Text::Rect bounds = layout.bounds;
    ASSERT_FALSE(bounds.IsEmpty());

    // An orthographic camera looking down -Z at the text (on z = 0), scaled so the layout fills the
    // frame with a margin, and centred on it. World (x, y) lands on pixel ((x - left) * scale, (y - bottom)
    // * scale), y up.
    constexpr float margin = 16.0f;
    const float scale = std::min((width - 2.0f * margin) / bounds.Width(), (height - 2.0f * margin) / bounds.Height());
    const float left = (bounds.minX + bounds.maxX) * 0.5f - width * 0.5f / scale;
    const float bottom = (bounds.minY + bounds.maxY) * 0.5f - height * 0.5f / scale;
    Camera camera;
    camera.SetPosition(Math::Vector3(0.0f, 0.0f, 5.0f));
    camera.SetOrthographic(left, left + width / scale, bottom, bottom + height / scale, 0.1f, 100.0f);
    const auto toPixelX = [&](const float x) { return (x - left) * scale; };
    const auto toPixelY = [&](const float y) { return (y - bottom) * scale; };

    // What Application::Render does for the scene
    renderer.BeginFrame();
    renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
    scene->Render(&renderer, camera);
    renderer.EndFrame();
    renderer.Present();
    const Frame frame = ReadBack(renderer, width, height);

    // Every touched pixel is the text colour: alpha-tested, never blended with the background
    const Coverage c = CoverageExcept(frame, Black);
    ASSERT_GT(c.count, 0) << "the text drew nothing";
    int otherColours = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const Rgb pixel = frame.At(x, y);
            if (pixel != Black && pixel != Rgb{255, 255, 0})
            {
                ++otherColours;
            }
        }
    }
    EXPECT_EQ(otherColours, 0) << "pixels neither background nor the text's yellow";

    // The ink lies inside the layout's box (the lines' advance width, ascent to descent), give or take
    // two pixels, and sits on the baseline
    constexpr float tolerancePx = 2.0f;
    EXPECT_GE(static_cast<float>(c.minX), toPixelX(bounds.minX) - tolerancePx);
    EXPECT_LE(static_cast<float>(c.maxX + 1), toPixelX(bounds.maxX) + tolerancePx);
    EXPECT_GE(static_cast<float>(c.minY), toPixelY(bounds.minY) - tolerancePx);
    EXPECT_LE(static_cast<float>(c.maxY + 1), toPixelY(bounds.maxY) + tolerancePx);
    EXPECT_NEAR(static_cast<float>(c.minY), toPixelY(layout.lines[0].baseline), 3.0f)
        << "H and i stand on the baseline";

    // ...and fills most of it: the side bearings are small, and H's cap height is about half the line
    const float boundsWidthPx = bounds.Width() * scale;
    const float boundsHeightPx = bounds.Height() * scale;
    EXPECT_GE(static_cast<float>(c.maxX - c.minX + 1), 0.7f * boundsWidthPx);
    EXPECT_GE(static_cast<float>(c.maxY - c.minY + 1), 0.4f * boundsHeightPx);

    // Two glyphs, two separate clusters of columns, the H wider than the i
    const std::vector<ColumnRun> runs = ColumnRuns(frame, Black);
    ASSERT_EQ(runs.size(), 2u) << "H and i should be separated by empty columns";
    EXPECT_GT(runs[0].Width(), runs[1].Width());
    EXPECT_GE(runs[1].first - runs[0].last, 3) << "a gap of a few pixels between the glyphs";
    // Each cluster lies within its glyph's quad
    for (std::size_t i = 0; i < 2; ++i)
    {
        const Text::Rect &quad = layout.quads[i].position;
        EXPECT_GE(static_cast<float>(runs[i].first), toPixelX(quad.minX) - tolerancePx) << "glyph " << i;
        EXPECT_LE(static_cast<float>(runs[i].last + 1), toPixelX(quad.maxX) + tolerancePx) << "glyph " << i;
    }

    text->CleanupRenderResources(&renderer);
    renderer.Shutdown();
}

TEST(SoftwareTextGoldenTest, AChangedColourRedrawsInTheNewColour)
{
    constexpr int width = 96;
    constexpr int height = 64;
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, width, height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);

    auto object = GameObject::Create("Colour");
    auto *text = object->AddComponent<Rendering::TextRenderer>();
    text->SetText("T");
    text->SetHorizontalAlign(Text::HorizontalAlign::Center);
    text->SetVerticalAlign(Text::VerticalAlign::Middle);

    Camera camera;
    camera.SetPosition(Math::Vector3(0.0f, 0.0f, 5.0f));
    camera.SetOrthographic(-1.5f, 1.5f, -1.0f, 1.0f, 0.1f, 100.0f);

    const auto render = [&](const Common::Color &colour)
    {
        text->SetColor(colour);
        renderer.BeginFrame();
        renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
        text->Render(&renderer);
        renderer.EndFrame();
        renderer.Present();
        return ReadBack(renderer, width, height);
    };

    const Frame cyan = render(Common::Color::Cyan);
    const Frame red = render(Common::Color::Red);
    const Coverage cyanCoverage = CoverageExcept(cyan, Black);
    const Coverage redCoverage = CoverageExcept(red, Black);
    ASSERT_GT(cyanCoverage.count, 0);
    EXPECT_EQ(redCoverage.count, cyanCoverage.count) << "the same glyph, only recoloured";
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (cyan.At(x, y) != Black)
            {
                EXPECT_EQ(cyan.At(x, y), (Rgb{0, 255, 255})) << x << ", " << y;
                EXPECT_EQ(red.At(x, y), (Rgb{255, 0, 0})) << x << ", " << y;
            }
        }
    }

    text->CleanupRenderResources(&renderer);
    renderer.Shutdown();
}

TEST(SoftwareUIGoldenTest, AnImageFillsItsRect)
{
    constexpr int width = 64;
    constexpr int height = 48;
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, width, height));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);

    auto scene = Scene::Create("SoftwareUIGolden_Image");
    const auto canvas = UI::UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto element = UI::UISystem::CreateElement("Panel");
    auto *rectTransform = element->GetComponent<UI::RectTransform>();
    ASSERT_NE(rectTransform, nullptr);
    // A 20 x 10 rect with its bottom-left corner at (8, 4), canvas space (y up)
    rectTransform->SetAnchorMin(Math::Vector2{0.0f, 0.0f});
    rectTransform->SetAnchorMax(Math::Vector2{0.0f, 0.0f});
    rectTransform->SetPivot(Math::Vector2{0.0f, 0.0f});
    rectTransform->SetAnchoredPosition(Math::Vector2{8.0f, 4.0f});
    rectTransform->SetSizeDelta(Math::Vector2{20.0f, 10.0f});
    auto *image = element->AddComponent<UI::Image>();
    ASSERT_NE(image, nullptr);
    image->SetColor(Common::Color::Red);
    canvas->AddChild(element, false);

    // The UI pass on its own, as Application::Render runs it after the scene
    renderer.BeginFrame();
    UI::UISystem::Render(*scene, &renderer, Vector2i{width, height});
    renderer.EndFrame();
    renderer.Present();
    const Frame frame = ReadBack(renderer, width, height);

    const Coverage c = CoverageExcept(frame, Black);
    EXPECT_NEAR(c.count, 200, 30) << "about 20 x 10 pixels";
    EXPECT_NEAR(c.minX, 8, 1);
    EXPECT_NEAR(c.maxX, 27, 1);
    EXPECT_NEAR(c.minY, 4, 1);
    EXPECT_NEAR(c.maxY, 13, 1);
    if (c.count > 0)
    {
        EXPECT_EQ(frame.At((c.minX + c.maxX) / 2, (c.minY + c.maxY) / 2), (Rgb{255, 0, 0}));
    }

    scene.reset(); // the scene and its UI go before the renderer shuts down
    renderer.Shutdown();
}

TEST(SoftwareUIGoldenTest, APressedButtonDrawsItsImageWithThePressedTint)
{
    constexpr int width = 32;
    constexpr int height = 32;
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, width, height));

    auto scene = Scene::Create("SoftwareUIGolden_Button");
    const auto canvas = UI::UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto element = UI::UISystem::CreateElement("Button");
    element->GetComponent<UI::RectTransform>()->StretchToParent();
    auto *image = element->AddComponent<UI::Image>();
    image->SetColor(Common::Color{0.0f, 1.0f, 1.0f, 1.0f}); // cyan, so the tint shows in two channels
    auto *button = element->AddComponent<UI::Button>();
    button->SetFadeDuration(0.0f);
    button->SetPressedColor(Common::Color{0.5f, 0.5f, 0.5f, 1.0f});
    canvas->AddChild(element, false);

    const auto draw = [&]
    {
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        renderer.BeginFrame();
        UI::UISystem::Render(*scene, &renderer, Vector2i{width, height});
        renderer.EndFrame();
        renderer.Present();
        return ReadBack(renderer, width, height).At(width / 2, height / 2);
    };

    EXPECT_EQ(draw(), (Rgb{0, 255, 255})) << "normal: the image's own colour";

    // Pressed with the pointer over it, as the dispatcher would send it
    button->OnMouseEnter();
    button->OnMouseDown();
    ASSERT_EQ(button->GetState(), UI::Button::State::Pressed);
    const Rgb pressed = draw();
    EXPECT_EQ(pressed.r, 0);
    EXPECT_NEAR(pressed.g, 128, 2) << "cyan times the pressed grey";
    EXPECT_NEAR(pressed.b, 128, 2);

    button->OnMouseUp();
    button->OnMouseExit();
    EXPECT_EQ(draw(), (Rgb{0, 255, 255})) << "released: back to the image's own colour";

    scene.reset(); // the scene and its UI go before the renderer shuts down
    renderer.Shutdown();
}

namespace
{
    // The default font's metrics (Noto Sans Regular, font units): what the UI text checks below are derived
    // from. "Hi" has no kerning pair, so its line is H's advance plus i's.
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

    UIScene MakeUIScene(const std::string &name)
    {
        UIScene result{Scene::Create(name), UI::UISystem::CreateCanvas(name + "_Canvas")};
        result.scene->AddRootGameObject(result.canvas);
        return result;
    }

    GameObject::Ptr AddElement(const GameObject::Ptr &parent, const std::string &name, const float x, const float y,
                               const float width, const float height)
    {
        auto element = UI::UISystem::CreateElement(name);
        auto *rectTransform = element->GetComponent<UI::RectTransform>();
        rectTransform->SetAnchorMin(Math::Vector2{0.0f, 0.0f});
        rectTransform->SetAnchorMax(Math::Vector2{0.0f, 0.0f});
        rectTransform->SetPivot(Math::Vector2{0.0f, 0.0f});
        rectTransform->SetAnchoredPosition(Math::Vector2{x, y});
        rectTransform->SetSizeDelta(Math::Vector2{width, height});
        parent->AddChild(element, false);
        return element;
    }

    Frame RenderUIPass(SoftwareRenderer &renderer, const Scene &scene, const int width, const int height)
    {
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        renderer.BeginFrame();
        UI::UISystem::Render(scene, &renderer, Vector2i{width, height});
        renderer.EndFrame();
        renderer.Present();
        return ReadBack(renderer, width, height);
    }

    int CountColour(const Frame &frame, const Rgb colour)
    {
        int count = 0;
        for (int y = 0; y < frame.height; ++y)
        {
            for (int x = 0; x < frame.width; ++x)
            {
                count += frame.At(x, y) == colour ? 1 : 0;
            }
        }
        return count;
    }

    constexpr Rgb Yellow{255, 255, 0};
    constexpr Rgb Red{255, 0, 0};
    constexpr Rgb White{255, 255, 255};
}

TEST(SoftwareUIGoldenTest, UITextInkSitsInsideItsRectWithItsAlignment)
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

    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, width, height));

    struct Case
    {
        Text::HorizontalAlign horizontal;
        Text::VerticalAlign vertical;
        const char *name;
    };
    for (const Case &alignment : {Case{Text::HorizontalAlign::Left, Text::VerticalAlign::Top, "Left/Top"},
                                  Case{Text::HorizontalAlign::Center, Text::VerticalAlign::Middle, "Center/Middle"},
                                  Case{Text::HorizontalAlign::Right, Text::VerticalAlign::Bottom, "Right/Bottom"}})
    {
        UIScene ui = MakeUIScene("SoftwareUIGolden_Text");
        const auto element = AddElement(ui.canvas, "Label", rectX, rectY, rectW, rectH);
        auto *text = element->AddComponent<UI::UIText>();
        ASSERT_NE(text, nullptr);
        text->SetText("Hi");
        text->SetFontSize(fontSize);
        text->SetColor(Common::Color::Yellow);
        text->SetHorizontalAlign(alignment.horizontal);
        text->SetVerticalAlign(alignment.vertical);

        const Frame frame = RenderUIPass(renderer, *ui.scene, width, height);
        const Coverage c = CoverageExcept(frame, Black);
        ASSERT_GT(c.count, 0) << alignment.name << ": the text drew nothing";

        // Alpha-tested: every touched pixel is the text's colour
        EXPECT_EQ(CountColour(frame, Yellow), c.count) << alignment.name << ": pixels that aren't the text's yellow";

        // The ink is inside the rect
        EXPECT_GE(static_cast<float>(c.minX), rectX - 1.0f) << alignment.name;
        EXPECT_LE(static_cast<float>(c.maxX + 1), rectX + rectW + 1.0f) << alignment.name;
        EXPECT_GE(static_cast<float>(c.minY), rectY - 1.0f) << alignment.name;
        EXPECT_LE(static_cast<float>(c.maxY + 1), rectY + rectH + 1.0f) << alignment.name;
        // Both glyphs are there: the ink is about as wide as H's left edge to i's right edge
        EXPECT_NEAR(static_cast<float>(c.maxX + 1 - c.minX), (kAdvanceH + kInkRightI - kInkLeftH) * s,
                    2.0f * tolerancePx) << alignment.name;

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
        EXPECT_NEAR(static_cast<float>(c.minX), lineLeft + kInkLeftH * s, tolerancePx)
            << alignment.name << ": H's left edge";
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

    renderer.Shutdown();
}

TEST(SoftwareUIGoldenTest, UITextDrawsOverAnImageInHierarchyOrder)
{
    constexpr int width = 120;
    constexpr int height = 60;
    SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, width, height));

    const auto addPanel = [](const GameObject::Ptr &parent, const std::string &name)
    {
        auto panel = AddElement(parent, name, 10.0f, 10.0f, 100.0f, 40.0f);
        panel->AddComponent<UI::Image>()->SetColor(Common::Color::Red);
        return panel;
    };
    const auto addLabel = [](const GameObject::Ptr &parent, const std::string &name)
    {
        auto label = AddElement(parent, name, 10.0f, 10.0f, 100.0f, 40.0f);
        auto *text = label->AddComponent<UI::UIText>();
        text->SetText("HH");
        text->SetFontSize(32.0f);
        text->SetColor(Common::Color::White);
        text->SetHorizontalAlign(Text::HorizontalAlign::Center);
        text->SetVerticalAlign(Text::VerticalAlign::Middle);
        return label;
    };

    // The text after the image (a later sibling) draws on top of it
    {
        UIScene ui = MakeUIScene("SoftwareUIGolden_TextOnTop");
        addPanel(ui.canvas, "Panel");
        addLabel(ui.canvas, "Label");
        const Frame frame = RenderUIPass(renderer, *ui.scene, width, height);

        const int white = CountColour(frame, White);
        const int red = CountColour(frame, Red);
        EXPECT_GT(white, 50) << "the text shows over the panel";
        EXPECT_GT(red, 100 * 40 / 2) << "most of the panel is still red";
        EXPECT_NEAR(white + red, 100 * 40, 100 * 40 / 10) << "text pixels replace panel pixels";
        EXPECT_EQ(white + red, CoverageExcept(frame, Black).count) << "only the panel's and the text's colours";
        // Every white pixel is inside the panel
        const Coverage whiteInk = [&]
        {
            Coverage c;
            c.minX = width;
            c.minY = height;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    if (frame.At(x, y) == White)
                    {
                        c.minX = std::min(c.minX, x);
                        c.maxX = std::max(c.maxX, x);
                        c.minY = std::min(c.minY, y);
                        c.maxY = std::max(c.maxY, y);
                    }
                }
            }
            return c;
        }();
        EXPECT_GE(whiteInk.minX, 9);
        EXPECT_LE(whiteInk.maxX, 110);
        EXPECT_GE(whiteInk.minY, 9);
        EXPECT_LE(whiteInk.maxY, 50);
        ui.scene.reset();
    }

    // A child of the image draws on top too
    {
        UIScene ui = MakeUIScene("SoftwareUIGolden_TextChild");
        const auto panel = addPanel(ui.canvas, "Panel");
        const auto label = addLabel(panel, "Label");
        label->GetComponent<UI::RectTransform>()->StretchToParent(); // the panel's rect
        const Frame frame = RenderUIPass(renderer, *ui.scene, width, height);
        EXPECT_GT(CountColour(frame, White), 50) << "a child draws over its parent";
        ui.scene.reset();
    }

    // The text before the image (an earlier sibling) is covered by it
    {
        UIScene ui = MakeUIScene("SoftwareUIGolden_TextUnder");
        addLabel(ui.canvas, "Label");
        addPanel(ui.canvas, "Panel");
        const Frame frame = RenderUIPass(renderer, *ui.scene, width, height);
        EXPECT_EQ(CountColour(frame, White), 0) << "the panel drawn later covers the text";
        EXPECT_NEAR(CountColour(frame, Red), 100 * 40, 100 * 40 / 10);
        ui.scene.reset();
    }

    renderer.Shutdown();
}
