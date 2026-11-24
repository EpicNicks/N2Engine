#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"

#include "TextureTestSupport.hpp"

// Golden-image tests of UI Image sprites: drawn by the UI pass on a headless SoftwareRenderer (no window or GPU)
// and read back. A sprite decoded from an image file must show upright (the source's top-left corner at the top
// left of the screen), and a Linear texture must be filtered bilinearly. The checks are structural, with
// tolerances, never exact images.

using namespace N2Engine;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using Renderer::Software::SoftwareRenderer;
using TextureTestSupport::MakeBmp;

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

    /// A full-screen Image showing `sprite`, drawn by the UI pass alone (as Application::Render runs it after
    /// the scene), and read back
    Frame RenderSprite(const std::shared_ptr<Rendering::Texture> &sprite, const int width, const int height,
                       const std::function<void(UI::Image &)> &configure = {})
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, static_cast<uint32_t>(width), static_cast<uint32_t>(height)));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);

        Frame frame{std::vector<std::uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4),
                    width, height};
        {
            auto scene = Scene::Create("SoftwareImageGolden");
            const auto canvas = UI::UISystem::CreateCanvas("Canvas");
            scene->AddRootGameObject(canvas);
            const auto element = UI::UISystem::CreateElement("Sprite");
            element->GetComponent<UI::RectTransform>()->StretchToParent();
            auto *image = element->AddComponent<UI::Image>();
            image->SetColor(Common::Color::White); // the sprite's own colours
            image->SetSprite(sprite);
            if (configure)
            {
                configure(*image);
            }
            canvas->AddChild(element, false);

            renderer.BeginFrame();
            UI::UISystem::Render(*scene, &renderer, Vector2i{width, height});
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), width, height);
            scene.reset(); // the scene and its UI go before the renderer shuts down
        }
        renderer.Shutdown();
        return frame;
    }

    constexpr Rgb kRed{255, 0, 0};
    constexpr Rgb kGreen{0, 255, 0};
    constexpr Rgb kBlue{0, 0, 255};
    constexpr Rgb kYellow{255, 255, 0};
}

TEST(SoftwareImageGoldenTest, ASpriteFromAnImageFileShowsUpright)
{
    // The source image, as an editor shows it: red | green over blue | yellow. BMP stores it bottom-up; the
    // decoder (flipY on, the default) stores it bottom row first, so v = 0 (the bottom of the rect) is its
    // bottom row.
    const auto bmp = MakeBmp(2, 2, {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}});
    Rendering::TextureSettings settings;
    settings.filter = TextureFilter::Nearest;
    const auto sprite = Rendering::Texture::CreateFromEncoded(bmp, settings, "checker");
    ASSERT_NE(sprite, nullptr);

    constexpr int width = 40;
    constexpr int height = 40;
    const Frame frame = RenderSprite(sprite, width, height);

    // Quadrant centres; frame y counts up from the bottom of the screen
    EXPECT_EQ(frame.At(10, 30), kRed) << "the source's top-left is at the top left";
    EXPECT_EQ(frame.At(30, 30), kGreen) << "top right";
    EXPECT_EQ(frame.At(10, 10), kBlue) << "bottom left";
    EXPECT_EQ(frame.At(30, 10), kYellow) << "bottom right";

    // Nearest: each quadrant is one solid colour, give or take the pixel on its edge
    int wrong = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (x == 19 || x == 20 || y == 19 || y == 20 || x == 0 || y == 0 || x == width - 1 || y == height - 1)
            {
                continue; // the boundary between texels, and the rect's edge pixels
            }
            const Rgb expected = y >= 20 ? (x < 20 ? kRed : kGreen) : (x < 20 ? kBlue : kYellow);
            wrong += frame.At(x, y) == expected ? 0 : 1;
        }
    }
    EXPECT_EQ(wrong, 0) << "pixels off their quadrant's colour";
}

TEST(SoftwareImageGoldenTest, ALinearSpriteIsFilteredIntoAGradient)
{
    // Two texels, red then blue, clamped at the edges. Texel centres are a quarter and three quarters across.
    TextureOptions linear;
    linear.filter = TextureFilter::Linear;
    linear.wrap = TextureWrap::ClampToEdge;
    linear.mipmaps = false;
    const std::vector<std::uint8_t> pixels = {255, 0, 0, 255, 0, 0, 255, 255};
    const auto sprite = Rendering::Texture::Create(2, 1, pixels, linear);
    ASSERT_NE(sprite, nullptr);

    constexpr int width = 64;
    constexpr int height = 8;
    const Frame frame = RenderSprite(sprite, width, height);
    constexpr int row = height / 2;

    // Pure colours outside the texel centres (clamped), a blend between them
    EXPECT_EQ(frame.At(2, row), kRed);
    EXPECT_EQ(frame.At(width - 3, row), kBlue);
    const Rgb middle = frame.At(width / 2, row);
    EXPECT_NEAR(middle.r, 128, 12) << "halfway: half red";
    EXPECT_NEAR(middle.b, 128, 12) << "halfway: half blue";
    EXPECT_EQ(middle.g, 0);

    // Red falls and blue rises steadily from one texel centre to the other
    std::set<std::tuple<int, int, int>> distinct;
    for (int x = width / 4; x + 1 < 3 * width / 4; ++x)
    {
        const Rgb here = frame.At(x, row);
        const Rgb next = frame.At(x + 1, row);
        EXPECT_GE(here.r, next.r) << x;
        EXPECT_LE(here.b, next.b) << x;
        EXPECT_NEAR(static_cast<int>(here.r) + static_cast<int>(here.b), 255, 2) << x << ": the weights sum to one";
        distinct.emplace(here.r, here.g, here.b);
    }
    EXPECT_GE(distinct.size(), 20u) << "a gradient, not two flat halves";

    // The same texture sampled Nearest has only its two colours
    TextureOptions nearest = linear;
    nearest.filter = TextureFilter::Nearest;
    const Frame flat = RenderSprite(Rendering::Texture::Create(2, 1, pixels, nearest), width, height);
    for (int x = 1; x + 1 < width; ++x)
    {
        const Rgb pixel = flat.At(x, row);
        EXPECT_TRUE(pixel == kRed || pixel == kBlue) << x;
    }
    EXPECT_EQ(flat.At(width / 4, row), kRed);
    EXPECT_EQ(flat.At(3 * width / 4, row), kBlue);
}

TEST(SoftwareImageGoldenTest, ANineSlicedSpriteKeepsItsCornersAndStretchesTheRest)
{
    // Issue #445: a 3 x 3 sprite with a 1-pixel border, drawn 60 x 60 with each border pixel 20 canvas units (sliceScale 20), so the
    // corners are 20 x 20 and the edges and centre stretch over the middle 20 x 20. A nine-colour source: one colour a cell.
    const std::vector<TextureTestSupport::Rgb> cells = {{255, 0, 0},   {0, 255, 0},     {0, 0, 255},
                                                        {255, 255, 0}, {255, 255, 255}, {0, 255, 255},
                                                        {255, 0, 255}, {128, 128, 128}, {64, 0, 0}};
    Rendering::TextureSettings settings;
    settings.filter = TextureFilter::Nearest;
    const auto sprite = Rendering::Texture::CreateFromEncoded(MakeBmp(3, 3, cells), settings, "nine");
    ASSERT_NE(sprite, nullptr);

    const auto sliced = [](UI::Image &image)
    {
        image.SetSliceBorder(1.0f, 1.0f, 1.0f, 1.0f);
        image.SetSliceScale(20.0f);
    };
    constexpr int size = 60;
    const Frame frame = RenderSprite(sprite, size, size, sliced);
    // Cell centres; frame y counts up from the bottom, the source's top row is the screen's top
    EXPECT_EQ(frame.At(10, 50), kRed) << "top-left corner";
    EXPECT_EQ(frame.At(30, 50), kGreen) << "top edge";
    EXPECT_EQ(frame.At(50, 50), kBlue) << "top-right corner";
    EXPECT_EQ(frame.At(10, 30), kYellow) << "left edge";
    EXPECT_EQ(frame.At(30, 30), (Rgb{255, 255, 255})) << "centre";
    EXPECT_EQ(frame.At(50, 30), (Rgb{0, 255, 255})) << "right edge";
    EXPECT_EQ(frame.At(10, 10), (Rgb{255, 0, 255})) << "bottom-left corner";
    EXPECT_EQ(frame.At(30, 10), (Rgb{128, 128, 128})) << "bottom edge";
    EXPECT_EQ(frame.At(50, 10), (Rgb{64, 0, 0})) << "bottom-right corner";

    // A larger rect stretches only the middle: the corners are still 20 x 20, so a pixel 25 in from the corner is the edge
    const Frame wide = RenderSprite(sprite, 120, 60, sliced);
    EXPECT_EQ(wide.At(10, 50), kRed);
    EXPECT_EQ(wide.At(60, 50), kGreen) << "the top edge stretched across the middle";
    EXPECT_EQ(wide.At(25, 50), kGreen) << "just past the corner";
    EXPECT_EQ(wide.At(110, 50), kBlue);
    EXPECT_EQ(wide.At(60, 30), (Rgb{255, 255, 255}));

    // fillCenter false leaves the clear colour in the middle
    const Frame frameOnly = RenderSprite(sprite, size, size, [&sliced](UI::Image &image)
    {
        sliced(image);
        image.SetFillCenter(false);
    });
    EXPECT_EQ(frameOnly.At(30, 30), (Rgb{0, 0, 0})) << "no centre";
    EXPECT_EQ(frameOnly.At(30, 50), kGreen) << "the edges remain";
}
