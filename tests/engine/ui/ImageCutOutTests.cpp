// The UI Image on a renderer that cannot blend (the software renderer): a textured image's pixels under half its alpha are
// cut out, as Sprite2D's are, so a sprite's transparent parts are not drawn as opaque boxes. A flat colour is never cut out.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/GameObject.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/UISystem.hpp"

using namespace N2Engine;
using N2Engine::UI::Image;
using N2Engine::UI::Rect;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr int Size = 16;
    constexpr float Identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    struct Rgb
    {
        int r = 0, g = 0, b = 0;
    };

    bool IsBlue(const Rgb &c) { return c.b > 200 && c.r < 60 && c.g < 60; }
    bool IsGreen(const Rgb &c) { return c.g > 200 && c.r < 60 && c.b < 60; }

    struct Row
    {
        Rgb left;  // a pixel in the left half
        Rgb right; // a pixel in the right half
    };

    /// The middle row of a frame cleared to blue in which `image` covers the whole target (identity view and projection,
    /// so the rect is in clip space)
    Row DrawOverBlue(Image &image)
    {
        SoftwareRenderer renderer;
        EXPECT_FALSE(renderer.SupportsBlending()) << "this test is about the renderer that cannot blend";
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 1.0f, 1.0f);
        renderer.BeginFrame();
        renderer.SetViewProjection(Identity, Identity);
        image.RenderUI(&renderer, Rect{-1.0f, -1.0f, 2.0f, 2.0f}, UI::UISystem::OverlayState());
        renderer.EndFrame();
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(Size) * Size * 4);
        renderer.ReadFramebuffer(rgba.data(), Size, Size);
        image.OnDestroy(); // its quad, material and texture share, before the renderer goes
        renderer.Shutdown();
        const auto at = [&](const int x)
        {
            const std::size_t i = (static_cast<std::size_t>(Size / 2) * Size + static_cast<std::size_t>(x)) * 4;
            return Rgb{rgba[i], rgba[i + 1], rgba[i + 2]};
        };
        return {at(Size / 4), at(Size * 3 / 4)};
    }

    /// Two texels: the left one transparent red, the right one opaque green
    std::shared_ptr<Rendering::Texture> HalfTransparentTexture()
    {
        const std::vector<std::uint8_t> pixels = {255, 0, 0, 0, 0, 255, 0, 255};
        Renderer::Common::TextureOptions options;
        options.filter = Renderer::Common::TextureFilter::Nearest;
        options.mipmaps = false;
        return Rendering::Texture::Create(2, 1, pixels, options);
    }
}

TEST(ImageCutOutTest, ATexturesTransparentPixelsAreCutOutOnTheSoftwareRenderer)
{
    const auto texture = HalfTransparentTexture();
    ASSERT_NE(texture, nullptr);
    const auto go = GameObject::Create("Image");
    auto *image = go->AddComponent<Image>();
    image->SetSprite(texture);
    const Row row = DrawOverBlue(*image);
    EXPECT_TRUE(IsBlue(row.left)) << "the transparent half shows what is behind it, not a box";
    EXPECT_TRUE(IsGreen(row.right));
}

TEST(ImageCutOutTest, AFadedImageKeepsItsOpaquePixels)
{
    // The test is on the final alpha with half the colour's alpha as the threshold, so fading does not make it vanish
    const auto texture = HalfTransparentTexture();
    ASSERT_NE(texture, nullptr);
    const auto go = GameObject::Create("Image");
    auto *image = go->AddComponent<Image>();
    image->SetSprite(texture);
    image->SetColor(Common::Color{1.0f, 1.0f, 1.0f, 0.3f});
    const Row row = DrawOverBlue(*image);
    EXPECT_TRUE(IsBlue(row.left));
    EXPECT_TRUE(IsGreen(row.right)) << "an opaque texel of a faded image is still drawn";
}

TEST(ImageCutOutTest, AFlatTranslucentColourIsStillDrawnOpaque)
{
    // No texture: nothing to cut out, so the behaviour from before is kept (the software renderer draws it opaque)
    const auto go = GameObject::Create("Image");
    auto *image = go->AddComponent<Image>();
    image->SetColor(Common::Color{0.0f, 1.0f, 0.0f, 0.2f});
    const Row row = DrawOverBlue(*image);
    EXPECT_TRUE(IsGreen(row.left));
    EXPECT_TRUE(IsGreen(row.right));
}
