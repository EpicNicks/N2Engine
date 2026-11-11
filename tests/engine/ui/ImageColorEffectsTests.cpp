// The UI Image's colour effects, saturation and flash (effects design, stage E1), as on Sprite2D: drawn on a headless software
// renderer (the reference of the GPU shaders), serialized with the scene, and set from Lua.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/GameObject.hpp"
#include "engine/common/Color.hpp"
#include "engine/scripting/LuaRuntime.hpp"
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

    /// The middle pixel of a frame in which `image` covers the whole target (identity view and projection, so the rect is
    /// in clip space)
    Rgb DrawMiddle(Image &image)
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        renderer.BeginFrame();
        renderer.SetViewProjection(Identity, Identity);
        image.RenderUI(&renderer, Rect{-1.0f, -1.0f, 2.0f, 2.0f}, UI::UISystem::OverlayState());
        renderer.EndFrame();
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(Size) * Size * 4);
        renderer.ReadFramebuffer(rgba.data(), Size, Size);
        image.OnDestroy(); // its quad and material, before the renderer goes
        renderer.Shutdown();
        const std::size_t i = (static_cast<std::size_t>(Size / 2) * Size + Size / 2) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2]};
    }
}

TEST(ImageColorEffectsTest, TheDefaultsDrawTheColourAsItIs)
{
    const auto go = GameObject::Create("Image");
    auto *image = go->AddComponent<Image>();
    image->SetColor(Common::Color::Red);
    EXPECT_FLOAT_EQ(image->GetSaturation(), 1.0f);
    EXPECT_FLOAT_EQ(image->GetFlash().a, 0.0f);
    const Rgb pixel = DrawMiddle(*image);
    EXPECT_EQ(pixel.r, 255);
    EXPECT_EQ(pixel.g, 0);
}

TEST(ImageColorEffectsTest, SaturationZeroIsTheLumaGreyAndAFlashMixesOverIt)
{
    const auto go = GameObject::Create("Image");
    auto *image = go->AddComponent<Image>();
    image->SetColor(Common::Color::Red);
    image->SetSaturation(0.0f);
    const Rgb grey = DrawMiddle(*image);
    EXPECT_NEAR(grey.r, 76, 1) << "0.299 x 255";
    EXPECT_NEAR(grey.g, 76, 1);
    EXPECT_NEAR(grey.b, 76, 1);

    image->SetSaturation(1.0f);
    image->SetFlash(Common::Color{1.0f, 1.0f, 1.0f, 0.5f});
    const Rgb flashed = DrawMiddle(*image);
    EXPECT_NEAR(flashed.r, 255, 1);
    EXPECT_NEAR(flashed.g, 128, 1);
    EXPECT_NEAR(flashed.b, 128, 1);
}

TEST(ImageColorEffectsTest, OutOfRangeValuesAreClampedWhenDrawn)
{
    const auto go = GameObject::Create("Image");
    auto *image = go->AddComponent<Image>();
    image->SetColor(Common::Color::Red);
    image->SetSaturation(std::numeric_limits<float>::infinity()); // not finite: 1, no change
    image->SetFlash(Common::Color{0.0f, 2.0f, 0.0f, 5.0f});       // clamped to green at alpha 1
    const Rgb pixel = DrawMiddle(*image);
    EXPECT_EQ(pixel.r, 0);
    EXPECT_EQ(pixel.g, 255);
}

TEST(ImageColorEffectsTest, TheEffectsAreSavedAndAnOlderImageLoadsWithoutThem)
{
    const auto source = GameObject::Create("Source");
    auto *image = source->AddComponent<Image>();
    image->SetSaturation(0.5f);
    image->SetFlash(Common::Color{0.0f, 0.0f, 1.0f, 0.25f});
    nlohmann::json json = image->Serialize();

    const auto target = GameObject::Create("Target");
    auto *copy = target->AddComponent<Image>();
    copy->Deserialize(json);
    EXPECT_FLOAT_EQ(copy->GetSaturation(), 0.5f);
    EXPECT_FLOAT_EQ(copy->GetFlash().b, 1.0f);
    EXPECT_FLOAT_EQ(copy->GetFlash().a, 0.25f);

    // Saved before the fields existed
    json.erase("saturation");
    json.erase("flash");
    const auto older = GameObject::Create("Older");
    auto *plain = older->AddComponent<Image>();
    plain->Deserialize(json);
    EXPECT_FLOAT_EQ(plain->GetSaturation(), 1.0f);
    EXPECT_FLOAT_EQ(plain->GetFlash().a, 0.0f);
}

TEST(ImageColorEffectsTest, AScriptCanSetTheEffects)
{
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    sol::state &lua = Scripting::LuaRuntime::Instance().GetState();
    const sol::protected_function_result result = lua.safe_script(R"(
        local go = GameObject.Create('LuaImageEffects')
        local image = go:AddComponent('Image')
        assert(image:GetSaturation() == 1)
        image:SetSaturation(0.25)
        assert(image:GetSaturation() == 0.25)
        image:SetFlash(Color.new(1, 0, 0, 0.5))
        assert(image:GetFlash().r == 1)
        assert(image:GetFlash().a == 0.5)
    )", sol::script_pass_on_error);
    if (!result.valid())
    {
        const sol::error error = result;
        FAIL() << error.what();
    }
}
