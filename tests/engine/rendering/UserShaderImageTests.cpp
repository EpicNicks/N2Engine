#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/UISystem.hpp"

// User shaders, stage 8: the UI Image drawn through a user shader (UserShaderSlot), on the software renderer, which has no user shaders and so
// draws the unlit fallback: the slot's Material reaches the draw with the image's colour, its sprite and its cut-out. Image without a shader is
// untouched (SoftwareImageGoldenTests, ImageCutOutTests and ImageColorEffectsTests are that proof). UI module file (N2ENGINE_UI_TESTS).

using namespace N2Engine;
using namespace N2Engine::Rendering;
using Renderer::Software::SoftwareRenderer;

namespace
{
    constexpr int Size = 64;

    struct Rgb
    {
        int r = 0, g = 0, b = 0;
    };

    struct Frame
    {
        std::vector<std::uint8_t> rgba;
        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * Size + static_cast<std::size_t>(x)) * 4;
            return {rgba[i], rgba[i + 1], rgba[i + 2]};
        }
    };

    bool IsGreen(const Rgb &c) { return c.g > 200 && c.r < 60 && c.b < 60; }
    bool IsMagenta(const Rgb &c) { return c.r > 200 && c.b > 200 && c.g < 60; }

    const char *kShaderText = R"({ "n2shader": 1, "name": "Tint",
  "params": [
    { "name": "u_Gain", "type": "float", "default": 1, "min": 0, "max": 4 },
    { "name": "baseColorTexture", "type": "texture", "default": "white" }
  ] }
#stage fragment
void main() { n2_FragColor = n2_texture(baseColorTexture, n2_UV) * u_Gain; }
)";

    std::shared_ptr<Shader> MakeShader()
    {
        auto shader = Shader::FromSource(kShaderText, "tint");
        EXPECT_NE(shader, nullptr);
        if (shader)
        {
            shader->SetCookedDirectory(std::filesystem::temp_directory_path() / "n2-no-cooked-data-here");
        }
        return shader;
    }

    struct ErrorStyleGuard
    {
        UserShaderErrorStyle previous = RenderSettings::GetUserShaderErrorStyle();
        explicit ErrorStyleGuard(const UserShaderErrorStyle style) { RenderSettings::SetUserShaderErrorStyle(style); }
        ~ErrorStyleGuard() { RenderSettings::SetUserShaderErrorStyle(previous); }
    };

    /// Left half transparent, right half opaque green
    std::shared_ptr<Texture> HalfTexture()
    {
        const std::vector<std::uint8_t> pixels = {255, 0, 0, 0, 0, 255, 0, 255};
        Renderer::Common::TextureOptions options;
        options.filter = Renderer::Common::TextureFilter::Nearest;
        options.mipmaps = false;
        return Texture::Create(2, 1, pixels, options);
    }
}

// ===== UI Image =====

namespace
{
    constexpr float Identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    /// A full-target Image (identity view and projection, so its rect is clip space) drawn on a software renderer cleared to black
    /// and read back; `setup` runs on the Image first
    Frame DrawImage(const std::function<void(UI::Image &)> &setup)
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        Frame frame;
        frame.rgba.resize(static_cast<std::size_t>(Size) * Size * 4);
        const auto go = GameObject::Create("Image");
        auto *image = go->AddComponent<UI::Image>();
        setup(*image);
        renderer.BeginFrame();
        renderer.SetViewProjection(Identity, Identity);
        image->RenderUI(&renderer, UI::Rect{-1.0f, -1.0f, 2.0f, 2.0f}, UI::UISystem::OverlayState());
        renderer.EndFrame();
        renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
        image->OnDestroy(); // its quad, material and texture share, before the renderer goes
        renderer.Shutdown();
        return frame;
    }
}

TEST(ImageShaderTest, AnImageWithAShaderShowsItsColourWhereTheFallbackIsDrawn)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Unlit);
    const Frame frame = DrawImage([](UI::Image &image)
    {
        image.SetColor(Common::Color::Green);
        image.SetShader(MakeShader());
        EXPECT_TRUE(image.SetShaderParam("u_Gain", 2.0f));
        EXPECT_FALSE(image.SetShaderParam("nope", 1.0f));
        ASSERT_NE(image.GetShaderMaterial(), nullptr);
    });
    EXPECT_TRUE(IsGreen(frame.At(32, 32)));
    EXPECT_TRUE(IsGreen(frame.At(5, 5)));
}

TEST(ImageShaderTest, TheEditorsMagentaAndTheCutOutOfASpriteWork)
{
    {
        const ErrorStyleGuard style(UserShaderErrorStyle::Magenta);
        const Frame frame = DrawImage([](UI::Image &image) { image.SetShader(MakeShader()); });
        EXPECT_TRUE(IsMagenta(frame.At(32, 32)));
    }
    {
        const ErrorStyleGuard style(UserShaderErrorStyle::Unlit);
        const auto texture = HalfTexture();
        ASSERT_NE(texture, nullptr);
        const Frame frame = DrawImage([&](UI::Image &image)
        {
            image.SetColor(Common::Color::White);
            image.SetSprite(texture);
            image.SetShader(MakeShader());
        });
        EXPECT_TRUE(IsGreen(frame.At(50, 32))) << "the opaque half";
        const Rgb left = frame.At(10, 32);
        EXPECT_TRUE(left.r < 30 && left.g < 30 && left.b < 30) << "the transparent half is cut out (nothing behind it: black)";
    }
}

TEST(ImageShaderTest, ClearingTheShaderGoesBackToTheStandardDraw)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Magenta);
    const Frame cleared = DrawImage([](UI::Image &image)
    {
        image.SetColor(Common::Color::Green);
        image.SetShader(MakeShader());
        image.SetShader(nullptr);
    });
    EXPECT_TRUE(IsGreen(cleared.At(32, 32)));
}

