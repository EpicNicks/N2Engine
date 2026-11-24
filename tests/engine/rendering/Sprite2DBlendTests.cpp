// Sprite2D and user shaders, issue #445 item 7 (docs/sprite2d.html#blend-mode, docs/user-shaders.html#engine-params): the sprite's own blend
// mode (alpha, additive, multiply) without a user shader, drawn blended on the headless software renderer (which blends sprites since
// #445); the parameters the engine fills in a user shader (u_Time, u_ScaledTime, u_Saturation, u_Flash); a sprite's saturation and flash
// reaching its user shader; and which blend mode wins when the sprite has a shader.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/software/SWMaterial.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/Sprite2D.hpp"
#ifdef N2ENGINE_SCRIPTING_ENABLED
#include "engine/scripting/LuaRuntime.hpp"
#endif

using namespace N2Engine;
using namespace N2Engine::Rendering;
using Math::Vector2;
using Math::Vector3;
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

    ::testing::AssertionResult Near(const Rgb &actual, const Rgb &expected, const int tolerance = 2)
    {
        if (std::abs(actual.r - expected.r) > tolerance || std::abs(actual.g - expected.g) > tolerance ||
            std::abs(actual.b - expected.b) > tolerance)
        {
            return ::testing::AssertionFailure() << "(" << actual.r << "," << actual.g << "," << actual.b << ") is not (" << expected.r
                                                 << "," << expected.g << "," << expected.b << ")";
        }
        return ::testing::AssertionSuccess();
    }

    /// The scene `build` makes, drawn by an orthographic camera that shows -2 to 2 on both axes (the origin is pixel (32, 32)), on black
    /// `afterRender` runs before the scene goes, while its sprites are still alive
    Frame Draw(const std::function<void(Scene &)> &build, const std::function<void()> &afterRender = {})
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        Frame frame;
        frame.rgba.resize(static_cast<std::size_t>(Size) * Size * 4);
        {
            Camera camera;
            camera.SetOrthographic(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f);
            camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));
            auto scene = Scene::Create("Sprite2DBlend");
            build(*scene);
            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            renderer.UpdateSceneLighting(scene->CollectLighting(), camera.GetPosition());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            if (afterRender)
            {
                afterRender();
            }
            scene.reset();
        }
        renderer.Shutdown();
        return frame;
    }

    Sprite2D *AddSprite(Scene &scene, const Common::Color &color, const int order, const float size)
    {
        auto go = GameObject::Create("Sprite");
        go->CreatePositionable();
        auto *sprite = go->AddComponent<Sprite2D>();
        sprite->SetColor(color);
        sprite->SetSize(Vector2(size, size));
        sprite->SetSortingOrder(order);
        scene.AddRootGameObject(go);
        return sprite;
    }

    /// The middle pixel of `top` (a 2 x 2 sprite, with `mode`) over a 3 x 3 sprite in `back`
    Rgb Over(const Common::Color &back, const Common::Color &top, const SpriteBlend mode)
    {
        const Frame frame = Draw([&](Scene &scene)
        {
            AddSprite(scene, back, -1, 3.0f);
            AddSprite(scene, top, 0, 2.0f)->SetBlendMode(mode);
        });
        return frame.At(32, 32);
    }

    struct ErrorStyleGuard
    {
        UserShaderErrorStyle previous = RenderSettings::GetUserShaderErrorStyle();
        explicit ErrorStyleGuard(const UserShaderErrorStyle style) { RenderSettings::SetUserShaderErrorStyle(style); }
        ~ErrorStyleGuard() { RenderSettings::SetUserShaderErrorStyle(previous); }
    };

    /// A shader with the engine-filled parameters (and one of each name with another type, which stays an ordinary parameter).
    /// Nothing is cooked for it, so the software renderer draws the built-in fallback
    std::shared_ptr<Shader> EngineParamShader(const std::string &stateJson = "")
    {
        std::string text = "{ \"n2shader\": 1, \"name\": \"EngineParams\",\n"
                           "  \"params\": [\n"
                           "    { \"name\": \"u_Time\", \"type\": \"float\" },\n"
                           "    { \"name\": \"u_ScaledTime\", \"type\": \"float\" },\n"
                           "    { \"name\": \"u_Saturation\", \"type\": \"float\", \"default\": 1 },\n"
                           "    { \"name\": \"u_Flash\", \"type\": \"color\", \"default\": [0, 0, 0, 0] },\n"
                           "    { \"name\": \"u_Gain\", \"type\": \"float\", \"default\": 1 }\n"
                           "  ]";
        if (!stateJson.empty())
        {
            text += ", \"state\": " + stateJson;
        }
        text += " }\n#stage fragment\nvoid main() { n2_FragColor = n2_Color * u_Gain + u_Flash * u_Saturation * u_Time * u_ScaledTime; }\n";
        auto shader = Shader::FromSource(text, "engine-params");
        EXPECT_NE(shader, nullptr);
        if (shader)
        {
            shader->SetCookedDirectory(std::filesystem::temp_directory_path() / "n2-no-cooked-data-here");
        }
        return shader;
    }

    /// A backend's user-shader material, as far as ApplyUniforms is concerned: it says it is one and keeps what it is given
    class RecordingUserMaterial final : public Renderer::Software::SWMaterial
    {
    public:
        RecordingUserMaterial() : SWMaterial(nullptr) {}
        [[nodiscard]] bool IsUserShaderMaterial() const override { return true; }
    };
}

// ===== The sprite's own blend mode =====

TEST(Sprite2DBlendTest, AlphaIsTheDefaultAndMixesByAlphaOnTheSoftwareRenderer)
{
    auto go = GameObject::Create("Sprite");
    EXPECT_EQ(go->AddComponent<Sprite2D>()->GetBlendMode(), SpriteBlend::Alpha);
    // Red at half alpha over blue: half of each, as a GPU blends, where the software renderer used to draw it opaque
    EXPECT_TRUE(Near(Over(Common::Color::Blue, Common::Color{1.0f, 0.0f, 0.0f, 0.5f}, SpriteBlend::Alpha), {128, 0, 127}));
    EXPECT_TRUE(Near(Over(Common::Color::Blue, Common::Color::Red, SpriteBlend::Alpha), {255, 0, 0})) << "opaque covers";
}

TEST(Sprite2DBlendTest, AdditiveAddsTheColourWithoutAUserShader)
{
    EXPECT_TRUE(Near(Over(Common::Color::Blue, Common::Color::Red, SpriteBlend::Additive), {255, 0, 255}));
    EXPECT_TRUE(Near(Over(Common::Color::Blue, Common::Color{1.0f, 0.0f, 0.0f, 0.5f}, SpriteBlend::Additive), {128, 0, 255}))
        << "the alpha scales what is added";
    const Frame frame = Draw([](Scene &scene)
    {
        AddSprite(scene, Common::Color::Red, 0, 2.0f)->SetBlendMode(SpriteBlend::Additive);
    });
    EXPECT_TRUE(Near(frame.At(32, 32), {255, 0, 0})) << "added to the black clear colour";
    EXPECT_TRUE(Near(frame.At(5, 5), {0, 0, 0})) << "only inside the shape";
}

TEST(Sprite2DBlendTest, MultiplyTimesWhatIsBehindAndIgnoresAlpha)
{
    const Common::Color grey{0.5f, 0.5f, 0.5f, 1.0f};
    EXPECT_TRUE(Near(Over(grey, Common::Color{1.0f, 0.0f, 1.0f, 0.25f}, SpriteBlend::Multiply), {128, 0, 128}));
    EXPECT_TRUE(Near(Over(grey, Common::Color::White, SpriteBlend::Multiply), {128, 128, 128})) << "white leaves the picture as it is";
}

TEST(Sprite2DBlendTest, TheColourEffectsApplyBeforeTheBlend)
{
    // Red at saturation 0 is its grey (76); added over blue, the grey is added
    const Frame frame = Draw([](Scene &scene)
    {
        AddSprite(scene, Common::Color::Blue, -1, 3.0f);
        auto *glow = AddSprite(scene, Common::Color::Red, 0, 2.0f);
        glow->SetBlendMode(SpriteBlend::Additive);
        glow->SetSaturation(0.0f);
    });
    EXPECT_TRUE(Near(frame.At(32, 32), {76, 76, 255}));
}

TEST(Sprite2DBlendTest, TheBlendModeSurvivesASceneLoadAndAnOlderSceneIsAlpha)
{
    const auto go = GameObject::Create("Sprite");
    go->CreatePositionable();
    go->AddComponent<Sprite2D>()->SetBlendMode(SpriteBlend::Multiply);
    const auto scene = Scene::Create("Sprite2DBlendSave");
    scene->AddRootGameObject(go);
    nlohmann::json json = scene->Serialize();
    EXPECT_NE(json.dump().find("\"Multiply\""), std::string::npos) << "saved by name, so a renamed constant does not change files";

    const auto loaded = Scene::FromJSON(json);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->FindGameObject("Sprite")->GetComponent<Sprite2D>()->GetBlendMode(), SpriteBlend::Multiply);

    bool erased = false;
    const std::function<void(nlohmann::json &)> erase = [&](nlohmann::json &node)
    {
        if (node.is_object())
        {
            erased = node.erase("_blendMode") > 0 || erased;
            erased = node.erase("blendMode") > 0 || erased;
        }
        if (node.is_object() || node.is_array())
        {
            for (auto &child : node)
            {
                erase(child);
            }
        }
    };
    erase(json);
    EXPECT_TRUE(erased);
    const auto older = Scene::FromJSON(json);
    ASSERT_NE(older, nullptr);
    EXPECT_EQ(older->FindGameObject("Sprite")->GetComponent<Sprite2D>()->GetBlendMode(), SpriteBlend::Alpha);
}

TEST(Sprite2DBlendTest, TheBlendModeIsAnInspectorFieldWithItsThreeOptions)
{
    auto go = GameObject::Create("Sprite");
    auto *sprite = go->AddComponent<Sprite2D>();
    bool found = false;
    for (const FieldInfo &field : sprite->DescribeFields())
    {
        if (field.name.find("blendMode") != std::string::npos)
        {
            found = true;
            EXPECT_EQ(field.enumOptions, (std::vector<std::string>{"Alpha", "Additive", "Multiply"}));
        }
    }
    EXPECT_TRUE(found);
}

// ===== Parameters the engine fills in a user shader =====

TEST(EngineShaderParamsTest, TimeSaturationAndFlashAreFilledOnEveryDrawWhateverWasSet)
{
    auto material = Material::Create(ShadingModel::Unlit);
    material->SetShader(EngineParamShader());
    material->SetSaturation(0.25f);
    material->SetFlash(Common::Color{1.0f, 0.5f, 0.0f, 0.75f});
    EXPECT_TRUE(material->SetFloat("u_Time", 99.0f)) << "a declared parameter can still be set";
    EXPECT_TRUE(material->SetFloat("u_Gain", 3.0f));

    RecordingUserMaterial target;
    material->ApplyUniforms(target);
    EXPECT_FLOAT_EQ(target.GetFloat("u_Time", -1.0f), Time::GetUnscaledTime()) << "the engine's clock, not the 99 that was set";
    EXPECT_FLOAT_EQ(target.GetFloat("u_ScaledTime", -1.0f), Time::GetTime());
    EXPECT_FLOAT_EQ(target.GetFloat("u_Saturation", -1.0f), 0.25f);
    EXPECT_EQ(target.GetVec4("u_Flash", {9, 9, 9, 9}), (std::array<float, 4>{1.0f, 0.5f, 0.0f, 0.75f}));
    EXPECT_FLOAT_EQ(target.GetFloat("u_Gain", -1.0f), 3.0f) << "an ordinary parameter is what was set";
}

TEST(EngineShaderParamsTest, ANameWithAnotherTypeIsAnOrdinaryParameter)
{
    const auto shader = Shader::FromSource("{ \"n2shader\": 1, \"params\": [ { \"name\": \"u_Time\", \"type\": \"vec2\", \"default\": [3, 4] },"
                                           " { \"name\": \"u_Flash\", \"type\": \"float\", \"default\": 0.5 } ] }\n"
                                           "#stage fragment\nvoid main() { n2_FragColor = vec4(u_Time, u_Flash, 1.0); }\n",
                                           "typed");
    ASSERT_NE(shader, nullptr);
    auto material = Material::Create(ShadingModel::Unlit);
    material->SetShader(shader);
    material->SetFlash(Common::Color::White);
    RecordingUserMaterial target;
    material->ApplyUniforms(target);
    EXPECT_EQ(target.GetVec4("u_Time", {0, 0, 0, 0}), (std::array<float, 4>{3.0f, 4.0f, 0.0f, 0.0f}));
    EXPECT_FLOAT_EQ(target.GetFloat("u_Flash", -1.0f), 0.5f);
}

// ===== A sprite with a user shader =====

TEST(Sprite2DShaderEffectsTest, TheSpritesSaturationAndFlashReachItsShaderMaterial)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Unlit);
    Sprite2D *grey = nullptr;
    Sprite2D *flashed = nullptr;
    const Frame frame = Draw([&](Scene &scene)
    {
        grey = AddSprite(scene, Common::Color::Red, 0, 2.0f);
        grey->SetShader(EngineParamShader());
        grey->SetSaturation(0.0f);
        flashed = AddSprite(scene, Common::Color::Red, 1, 0.5f);
        flashed->SetShader(EngineParamShader());
        flashed->SetSaturation(0.5f);
        flashed->SetFlash(Common::Color{1.0f, 1.0f, 1.0f, 0.8f});
    }, [&]
    {
        ASSERT_NE(flashed->GetShaderMaterial(), nullptr);
        EXPECT_FLOAT_EQ(flashed->GetShaderMaterial()->GetSaturation(), 0.5f);
        EXPECT_FLOAT_EQ(flashed->GetShaderMaterial()->GetFlash().a, 0.8f);
        RecordingUserMaterial target;
        flashed->GetShaderMaterial()->ApplyUniforms(target);
        EXPECT_FLOAT_EQ(target.GetFloat("u_Saturation", -1.0f), 0.5f) << "what a shader that declares it reads";
        EXPECT_EQ(target.GetVec4("u_Flash", {0, 0, 0, 0}), (std::array<float, 4>{1.0f, 1.0f, 1.0f, 0.8f}));
    });
    // The software renderer cannot run the shader: its built-in fallback applies the effects itself, so the big sprite is red's grey
    EXPECT_TRUE(Near(frame.At(20, 20), {76, 76, 76}));
}

TEST(Sprite2DShaderEffectsTest, TheSpritesBlendModeAppliesUnlessTheShaderHasABlendHint)
{
    Sprite2D *plain = nullptr;
    Sprite2D *hinted = nullptr;
    const Frame frame = Draw([&](Scene &scene)
    {
        plain = AddSprite(scene, Common::Color::Red, 0, 1.0f);
        plain->SetShader(EngineParamShader());
        plain->SetBlendMode(SpriteBlend::Additive);
        hinted = AddSprite(scene, Common::Color::Red, 0, 1.0f);
        hinted->SetShader(EngineParamShader("{ \"blend\": \"premultiplied\" }"));
        hinted->SetBlendMode(SpriteBlend::Additive);
    }, [&]
    {
        EXPECT_EQ(plain->GetShaderMaterial()->GetBlendMode(), BlendMode::Additive) << "no hint: the sprite's mode";
        EXPECT_EQ(hinted->GetShaderMaterial()->GetBlendMode(), BlendMode::Premultiplied) << "the shader's hint wins";
    });
    (void)frame;
}


#ifdef N2ENGINE_SCRIPTING_ENABLED
TEST(Sprite2DBlendTest, AScriptSetsTheBlendModeByNameAndAnUnknownNameIsAnError)
{
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    sol::state &lua = Scripting::LuaRuntime::Instance().GetState();
    const sol::protected_function_result result = lua.safe_script(R"(
        local go = GameObject.Create('LuaSpriteBlend')
        local s = go:AddComponent('Sprite2D')
        assert(s:GetBlendMode() == 'Alpha')
        s:SetBlendMode('Additive')
        assert(s:GetBlendMode() == 'Additive')
        s:SetBlendMode('Multiply')
        assert(s:GetBlendMode() == 'Multiply')
        local ok, err = pcall(function() s:SetBlendMode('Screen') end)
        assert(not ok)
        assert(tostring(err):find('Sprite2D:SetBlendMode', 1, true), tostring(err))
        assert(s:GetBlendMode() == 'Multiply', 'a bad name changes nothing')
    )", sol::script_pass_on_error);
    if (!result.valid())
    {
        const sol::error error = result;
        FAIL() << error.what();
    }
}
#endif
