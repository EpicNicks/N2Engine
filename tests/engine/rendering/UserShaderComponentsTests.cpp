#include <gtest/gtest.h>

#include <cstddef>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/Sprite2D.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/rendering/UserShaderSlot.hpp"
#ifdef N2ENGINE_SCRIPTING_ENABLED
#include "engine/scripting/LuaRuntime.hpp"
#endif

// User shaders, stage 8 (orchestrator/proposals/user-shaders.md section 10): Sprite2D, the UI Image and the particle system draw through a
// user shader (UserShaderSlot: a private Material). Pixels are checked on the software renderer, which cannot run a user shader and so draws
// the unlit fallback: that proves the slot's Material reaches the draw with the component's colour, its texture and its cut-out, and that a
// component WITHOUT a shader is untouched (the existing Sprite2D and Image goldens are that proof, and they are unchanged). The GPU
// backends' user-shader pixels are covered at the Material level (UserShaderMaterialTests, the Vulkan and WebGPU .mat goldens): the
// components hand them the same Material. The Image tests are in UserShaderImageTests.cpp (a UI-module file) and the particle system's in SoftwareParticlesGoldenTests.cpp.

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

    bool IsGreen(const Rgb &c) { return c.g > 200 && c.r < 60 && c.b < 60; }
    bool IsBlue(const Rgb &c) { return c.b > 200 && c.r < 60 && c.g < 60; }
    bool IsMagenta(const Rgb &c) { return c.r > 200 && c.b > 200 && c.g < 60; }

    const char *kShaderText =
        "{ \"n2shader\": 1, \"name\": \"Tint\",\n"
        "  \"params\": [\n"
        "    { \"name\": \"u_Gain\", \"type\": \"float\", \"default\": 1, \"min\": 0, \"max\": 4 },\n"
        "    { \"name\": \"u_Color\", \"type\": \"color\", \"default\": [1, 1, 1, 1] },\n"
        "    { \"name\": \"u_Steps\", \"type\": \"int\", \"default\": 2 },\n"
        "    { \"name\": \"baseColorTexture\", \"type\": \"texture\", \"default\": \"white\" }\n"
        "  ] }\n"
        "#stage fragment\n"
        "void main() { n2_FragColor = u_Color * n2_texture(baseColorTexture, n2_UV) * u_Gain; }\n";

    /// A shader nothing is cooked for (so no backend can draw it): the software renderer, which has no user shaders, draws the fallback
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

    /// The scene `build` makes, drawn by an orthographic camera that shows -2 to 2 on both axes (the origin is pixel (32, 32))
    Frame Draw(const std::function<void(Scene &)> &build)
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
            auto scene = Scene::Create("ShaderSprite");
            build(*scene);
            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            renderer.UpdateSceneLighting(scene->CollectLighting(), camera.GetPosition());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            scene.reset();
        }
        renderer.Shutdown();
        return frame;
    }

    Sprite2D *AddSprite(Scene &scene, const Common::Color &color, const int order = 0, const float size = 2.0f)
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

// ===== The slot =====

TEST(UserShaderSlotTest, AShaderMakesAPrivateMaterialHoldingItsParametersAndNullForgetsThem)
{
    UserShaderSlot slot;
    EXPECT_FALSE(slot.HasShader());
    EXPECT_EQ(slot.GetMaterial(), nullptr);
    EXPECT_FALSE(slot.SetFloat("u_Gain", 2.0f)) << "no shader, no parameter";

    const auto shader = MakeShader();
    slot.SetShader(shader);
    ASSERT_TRUE(slot.HasShader());
    ASSERT_NE(slot.GetMaterial(), nullptr);
    EXPECT_EQ(slot.GetMaterial()->GetShader(), shader);
    EXPECT_TRUE(slot.GetMaterial()->IsBlended()) << "blended, like every 2D component's material";
    EXPECT_TRUE(slot.GetMaterial()->IsDoubleSided());

    EXPECT_TRUE(slot.SetParam("u_Gain", 3.0f));
    EXPECT_TRUE(slot.SetParam("u_Gain", 9.0f)) << "clamped to the declared max, not refused";
    ASSERT_TRUE(slot.GetMaterial()->GetParam("u_Gain").has_value());
    EXPECT_EQ(slot.GetMaterial()->GetParam("u_Gain")->value[0], 4.0f);
    EXPECT_TRUE(slot.SetParam("u_Color", Common::Color(0.0f, 1.0f, 0.0f, 1.0f)));
    EXPECT_TRUE(slot.SetParamInt("u_Steps", 5));
    EXPECT_FALSE(slot.SetParam("u_Missing", 1.0f)) << "a name the shader does not declare";
    EXPECT_FALSE(slot.SetParam("u_Gain", Vector2(1.0f, 2.0f))) << "a vec2 for a float";

    // The same shader again keeps the values; another shader or null forgets them
    slot.SetShader(shader);
    EXPECT_EQ(slot.GetMaterial()->GetParam("u_Gain")->value[0], 4.0f);
    slot.SetShader(nullptr);
    EXPECT_FALSE(slot.HasShader());
    EXPECT_EQ(slot.GetMaterial(), nullptr);
    slot.SetShader(shader);
    EXPECT_EQ(slot.GetMaterial()->GetParam("u_Gain")->value[0], 1.0f) << "the shader's default again";
}

TEST(UserShaderSlotTest, TwoSlotsOfOneShaderNeverShareValues)
{
    const auto shader = MakeShader();
    UserShaderSlot a;
    UserShaderSlot b;
    a.SetShader(shader);
    b.SetShader(shader);
    ASSERT_TRUE(a.SetParam("u_Gain", 3.0f));
    EXPECT_NE(a.GetMaterial(), b.GetMaterial());
    EXPECT_EQ(b.GetMaterial()->GetParam("u_Gain")->value[0], 1.0f) << "one object's value never reaches another's";
}

TEST(UserShaderSlotTest, TheDrawsInputsAreSetOnlyWhenTheyChange)
{
    UserShaderSlot slot;
    slot.SetShader(MakeShader());
    const std::uint64_t first = slot.GetMaterial()->GetVersion();
    slot.Sync(nullptr, 0.0f);
    EXPECT_EQ(slot.GetMaterial()->GetVersion(), first) << "nothing changed: no new version, so no new GPU material";
    const auto texture = HalfTexture();
    slot.Sync(texture, 0.5f);
    EXPECT_EQ(slot.GetMaterial()->GetBaseColorTexture(), texture);
    EXPECT_EQ(slot.GetMaterial()->GetBlendDiscardBelow(), 0.5f);
    const std::uint64_t second = slot.GetMaterial()->GetVersion();
    slot.Sync(texture, 0.5f);
    EXPECT_EQ(slot.GetMaterial()->GetVersion(), second);
}

// ===== Sprite2D =====

TEST(Sprite2DShaderTest, AShaderThatCannotBeDrawnShowsTheSpritesColourOnTheSoftwareRenderer)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Unlit);
    const Frame frame = Draw([&](Scene &scene)
    {
        auto *sprite = AddSprite(scene, Common::Color::Green);
        sprite->SetShader(MakeShader());
        EXPECT_TRUE(sprite->SetShaderParam("u_Gain", 2.0f));
        EXPECT_TRUE(sprite->SetShaderParam("u_Color", Common::Color::Red));
        EXPECT_TRUE(sprite->SetShaderInt("u_Steps", 3));
        EXPECT_TRUE(sprite->SetShaderVec4("u_Color", 1.0f, 0.0f, 0.0f, 1.0f));
        EXPECT_FALSE(sprite->SetShaderParam("nope", 1.0f));
    });
    EXPECT_TRUE(IsGreen(frame.At(32, 32))) << "the unlit fallback: the sprite's own colour";
    EXPECT_TRUE(IsGreen(frame.At(40, 40)));
}

TEST(Sprite2DShaderTest, InTheEditorTheFallbackIsMagentaSoTheFailureIsObvious)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Magenta);
    const Frame frame = Draw([&](Scene &scene) { AddSprite(scene, Common::Color::Green)->SetShader(MakeShader()); });
    EXPECT_TRUE(IsMagenta(frame.At(32, 32)));
}

TEST(Sprite2DShaderTest, TheTexturesCutOutOnARendererThatCannotBlendSurvivesTheShader)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Unlit);
    const auto texture = HalfTexture();
    ASSERT_NE(texture, nullptr);
    const Frame frame = Draw([&](Scene &scene)
    {
        AddSprite(scene, Common::Color::Blue, -1, 3.0f);
        auto *sprite = AddSprite(scene, Common::Color::White);
        sprite->SetTexture(texture);
        sprite->SetShader(MakeShader());
    });
    EXPECT_TRUE(IsBlue(frame.At(20, 32))) << "the transparent half shows what is behind it, as with the built-in shader";
    EXPECT_TRUE(IsGreen(frame.At(44, 32)));
}

TEST(Sprite2DShaderTest, ClearingTheShaderGoesBackToTheBuiltInDraw)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Magenta);
    Sprite2D *kept = nullptr;
    const Frame frame = Draw([&](Scene &scene)
    {
        kept = AddSprite(scene, Common::Color::Green);
        kept->SetShader(MakeShader());
        kept->SetShader(nullptr);
        EXPECT_EQ(kept->GetShader(), nullptr);
        EXPECT_EQ(kept->GetShaderMaterial(), nullptr);
    });
    EXPECT_TRUE(IsGreen(frame.At(32, 32))) << "no shader: the built-in unlit draw, not the editor's magenta";
}

// ===== Stage 9g: the shader and its values as scene and prefab data =====

namespace
{
    /// A shader as a project asset would be found: registered, so a saved UUID resolves to it in this process
    std::shared_ptr<Shader> RegisteredShader()
    {
        auto shader = MakeShader();
        IO::Resources::Instance().RegisterAsset(shader);
        return shader;
    }

    nlohmann::json SaveOf(Sprite2D &sprite) { return sprite.Serialize(); }

    std::shared_ptr<GameObject> NewSpriteObject(Sprite2D *&sprite)
    {
        auto go = GameObject::Create("S");
        go->CreatePositionable();
        sprite = go->AddComponent<Sprite2D>();
        return go;
    }
}

TEST(Sprite2DShaderTest, ASpriteWithoutAShaderSavesNothingAboutOne)
{
    Sprite2D *sprite = nullptr;
    const auto go = NewSpriteObject(sprite);
    const nlohmann::json saved = SaveOf(*sprite);
    EXPECT_EQ(saved.dump().find("shader"), std::string::npos) << "every scene file written before 9g is what it was";
    // And a file that never had the key loads as no shader (an old scene)
    sprite->SetShader(RegisteredShader());
    ASSERT_TRUE(sprite->GetShader() != nullptr);
    sprite->Deserialize(saved);
    EXPECT_FALSE(sprite->GetShader() != nullptr) << "a file with no shader key clears the slot, as a null asset reference clears a reference";
}

TEST(Sprite2DShaderTest, TheShaderAndItsValuesAreSavedPerInstanceAndLoadBack)
{
    const auto shader = RegisteredShader();
    Sprite2D *first = nullptr;
    Sprite2D *second = nullptr;
    const auto a = NewSpriteObject(first);
    const auto b = NewSpriteObject(second);
    first->SetShader(shader);
    second->SetShader(shader);
    first->SetShaderParam("u_Gain", 2.5f);
    first->SetShaderParam("u_Color", Common::Color(0.0f, 1.0f, 0.0f, 1.0f));
    first->SetShaderInt("u_Steps", 5);
    second->SetShaderParam("u_Gain", 0.5f);

    const nlohmann::json savedFirst = SaveOf(*first);
    const nlohmann::json savedSecond = SaveOf(*second);
    ASSERT_TRUE(savedFirst.contains("shaderState"));
    EXPECT_EQ(savedFirst.at("shaderState").at("shader"), shader->GetUUID().ToString()) << "named by UUID, so a moved asset still resolves";
    EXPECT_EQ(savedFirst.at("shaderState").at("params").at("u_Gain"), 2.5f);
    EXPECT_EQ(savedSecond.at("shaderState").at("params").at("u_Gain"), 0.5f) << "each instance holds its own values";
    EXPECT_FALSE(savedSecond.at("shaderState").at("params").contains("u_Steps"));

    // A new component loads what was saved, whichever order the files come in
    Sprite2D *loaded = nullptr;
    const auto c = NewSpriteObject(loaded);
    loaded->Deserialize(savedFirst);
    ASSERT_TRUE(loaded->GetShader() != nullptr);
    EXPECT_EQ(loaded->GetShader(), shader);
    EXPECT_EQ(loaded->GetShaderMaterial()->GetParam("u_Gain")->value[0], 2.5f);
    EXPECT_EQ(loaded->GetShaderMaterial()->GetParam("u_Color")->value, (std::array<float, 4>{0, 1, 0, 1}));
    EXPECT_EQ(loaded->GetShaderMaterial()->GetParam("u_Steps")->value[0], 5.0f);
    EXPECT_EQ(SaveOf(*loaded).at("shaderState"), savedFirst.at("shaderState")) << "a round trip is stable";
    // The same shader again keeps the values the file did not mention at their defaults
    loaded->Deserialize(savedSecond);
    EXPECT_EQ(loaded->GetShaderMaterial()->GetParam("u_Gain")->value[0], 0.5f);
    EXPECT_EQ(loaded->GetShaderMaterial()->GetParam("u_Steps")->value[0], 2.0f);
}

TEST(Sprite2DShaderTest, ABadShaderInAFileClearsTheSlotWithAWarningAndNeverThrows)
{
    Sprite2D *sprite = nullptr;
    const auto go = NewSpriteObject(sprite);
    sprite->SetShader(RegisteredShader());
    nlohmann::json saved = SaveOf(*sprite);
    saved["shaderState"] = nlohmann::json{{"shader", "00000000-0000-0000-0000-00000000dead"}, {"params", {{"u_Gain", 2}}}};
    EXPECT_NO_THROW(sprite->Deserialize(saved));
    EXPECT_FALSE(sprite->GetShader() != nullptr);
    for (const nlohmann::json &junk : {nlohmann::json(5), nlohmann::json("shader"), nlohmann::json::array(), nlohmann::json::object(),
                                       nlohmann::json{{"shader", 7}}, nlohmann::json{{"shader", nullptr}}})
    {
        saved["shaderState"] = junk;
        sprite->SetShader(RegisteredShader());
        EXPECT_NO_THROW(sprite->Deserialize(saved)) << junk.dump();
        EXPECT_FALSE(sprite->GetShader() != nullptr) << junk.dump();
    }
}

TEST(Sprite2DShaderTest, ValuesThatDoNotFitTheShadersParametersAreDroppedAsInAMatFile)
{
    const auto shader = RegisteredShader();
    Sprite2D *sprite = nullptr;
    const auto go = NewSpriteObject(sprite);
    sprite->SetShader(shader);
    nlohmann::json saved = SaveOf(*sprite);
    saved["shaderState"] = nlohmann::json{{"shader", shader->GetUUID().ToString()},
                                          {"params", {{"u_Gain", "fast"}, {"nope", 1}, {"u_Steps", 3}, {"u_Color", {1, 2}}}}};
    sprite->Deserialize(saved);
    ASSERT_TRUE(sprite->GetShader() != nullptr);
    EXPECT_EQ(sprite->GetShaderMaterial()->GetParam("u_Steps")->value[0], 3.0f);
    EXPECT_EQ(sprite->GetShaderMaterial()->GetParam("u_Gain")->value[0], 1.0f) << "the default: the wrong type is ignored";
    EXPECT_EQ(sprite->GetShaderMaterial()->GetParam("u_Color")->value, (std::array<float, 4>{1, 1, 1, 1}));
}

TEST(Sprite2DShaderTest, TheShaderStateIsNotAFieldAnEditorScriptOrTheNetworkKnows)
{
    Sprite2D *sprite = nullptr;
    const auto go = NewSpriteObject(sprite);
    sprite->SetShader(RegisteredShader());
    ASSERT_TRUE(SaveOf(*sprite).contains("shaderState"));
    for (const FieldInfo &field : sprite->DescribeFields())
    {
        EXPECT_NE(field.name, "shaderState") << "not in a schema, so not in an inspector, a script's field list or the replication fingerprint";
        EXPECT_FALSE(field.replicated && field.name.find("shader") != std::string::npos);
    }
}

#ifdef N2ENGINE_SCRIPTING_ENABLED
TEST(UserShaderComponentsLuaTest, ComponentsHaveNoShaderUntilOneIsSetAndABadPathIsAnError)
{
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    sol::state &lua = Scripting::LuaRuntime::Instance().GetState();
    const sol::protected_function_result result = lua.safe_script(R"(
        local go = GameObject.Create('ShaderLua')
        local s = go:AddComponent('Sprite2D')
        assert(s:GetShader() == nil)
        assert(s:GetMaterialInstance() == nil)
        local ok, err = pcall(function() s:SetShader('res://does-not-exist.n2shader') end)
        assert(not ok)
        assert(tostring(err):find("can't load shader", 1, true), tostring(err))
        s:SetShader(nil)
        assert(s:GetShader() == nil)
    )", sol::script_pass_on_error);
    if (!result.valid())
    {
        const sol::error error = result;
        FAIL() << error.what();
    }
}
#endif
