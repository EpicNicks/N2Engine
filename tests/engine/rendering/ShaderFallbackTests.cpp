#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/Sprite2D.hpp"

// Issue #445: what stands in for a user shader where it cannot be drawn, shown on the software renderer (which has no user
// shaders): the .n2shader header's "fallback" (tint, opacity, hidden), the blend mode, and the header's validation

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

    bool Near(const Rgb &a, const Rgb &b, const int tolerance = 2)
    {
        return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance;
    }

    std::string ShaderText(const std::string &extra)
    {
        return "{ \"n2shader\": 1, \"name\": \"Fallback\",\n"
               "  \"params\": [\n"
               "    { \"name\": \"u_Tint\", \"type\": \"color\", \"default\": [1, 1, 1, 1] },\n"
               "    { \"name\": \"u_Fade\", \"type\": \"float\", \"default\": 1 },\n"
               "    { \"name\": \"u_Offset\", \"type\": \"vec2\", \"default\": [0, 0] }\n"
               "  ]" +
               (extra.empty() ? std::string{} : ", " + extra) +
               " }\n#stage fragment\nvoid main() { n2_FragColor = u_Tint * u_Fade; }\n";
    }

    std::shared_ptr<Shader> MakeShader(const std::string &extra)
    {
        auto shader = Shader::FromSource(ShaderText(extra), "fallback");
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

    /// The middle pixel of a white sprite with `shader` (set up by `setup`) over a blue background sprite
    Rgb OverBlue(const std::shared_ptr<Shader> &shader, const std::function<void(Sprite2D &)> &setup = {})
    {
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(Size) * Size * 4);
        {
            Camera camera;
            camera.SetOrthographic(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f);
            camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));
            auto scene = Scene::Create("ShaderFallback");
            const auto add = [&scene](const Common::Color &color, const int order, const float size)
            {
                auto go = GameObject::Create("Sprite");
                go->CreatePositionable();
                auto *sprite = go->AddComponent<Sprite2D>();
                sprite->SetColor(color);
                sprite->SetSize(Vector2(size, size));
                sprite->SetSortingOrder(order);
                scene->AddRootGameObject(go);
                return sprite;
            };
            add(Common::Color::Blue, -1, 3.0f);
            Sprite2D *top = add(Common::Color::White, 0, 2.0f);
            top->SetShader(shader);
            if (setup)
            {
                setup(*top);
            }
            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            renderer.UpdateSceneLighting(scene->CollectLighting(), camera.GetPosition());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(rgba.data(), Size, Size);
            scene.reset();
        }
        renderer.Shutdown();
        const std::size_t i = (static_cast<std::size_t>(32) * Size + 32) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2]};
    }
}

TEST(ShaderFallbackTest, TheHeaderIsReadAndChecked)
{
    const auto plain = MakeShader("");
    ASSERT_NE(plain, nullptr);
    EXPECT_TRUE(plain->GetFallback().tint.empty());
    EXPECT_FALSE(plain->GetFallback().hidden);
    const auto full = MakeShader(R"("fallback": { "tint": "u_Tint", "opacity": "u_Fade", "hidden": false })");
    ASSERT_NE(full, nullptr);
    EXPECT_EQ(full->GetFallback().tint, "u_Tint");
    EXPECT_EQ(full->GetFallback().opacity, "u_Fade");
    for (const char *bad : {R"("fallback": [])", R"("fallback": { "tint": "u_Missing" })",
                            R"("fallback": { "tint": "u_Fade" })", R"("fallback": { "opacity": "u_Offset" })",
                            R"("fallback": { "tint": 3 })", R"("fallback": { "hidden": 1 })",
                            R"("fallback": { "glow": true })"})
    {
        EXPECT_EQ(MakeShader(bad), nullptr) << bad;
    }
}

TEST(ShaderFallbackTest, TheTintAndOpacityParametersColourTheFallback)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Unlit);
    const auto shader = MakeShader(R"("fallback": { "tint": "u_Tint", "opacity": "u_Fade" })");
    ASSERT_NE(shader, nullptr);
    EXPECT_TRUE(Near(OverBlue(shader), {255, 255, 255})) << "the defaults change nothing";
    EXPECT_TRUE(Near(OverBlue(shader, [](Sprite2D &sprite)
    {
        EXPECT_TRUE(sprite.SetShaderParam("u_Tint", Common::Color{1.0f, 0.0f, 0.0f, 1.0f}));
    }), {255, 0, 0}));
    // Red at half opacity over blue
    EXPECT_TRUE(Near(OverBlue(shader, [](Sprite2D &sprite)
    {
        EXPECT_TRUE(sprite.SetShaderParam("u_Tint", Common::Color{1.0f, 0.0f, 0.0f, 1.0f}));
        EXPECT_TRUE(sprite.SetShaderParam("u_Fade", 0.5f));
    }), {128, 0, 127}));
    EXPECT_TRUE(Near(OverBlue(shader, [](Sprite2D &sprite) { EXPECT_TRUE(sprite.SetShaderParam("u_Fade", 0.0f)); }),
                     {0, 0, 255}))
        << "faded out";
}

TEST(ShaderFallbackTest, AnUnnamedParameterChangesNothing)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Unlit);
    const auto shader = MakeShader("");
    ASSERT_NE(shader, nullptr);
    EXPECT_TRUE(Near(OverBlue(shader, [](Sprite2D &sprite)
    {
        EXPECT_TRUE(sprite.SetShaderParam("u_Tint", Common::Color{1.0f, 0.0f, 0.0f, 1.0f}));
    }), {255, 255, 255}));
}

TEST(ShaderFallbackTest, AHiddenFallbackDrawsNothingExceptTheEditorsMagenta)
{
    const auto shader = MakeShader(R"("fallback": { "hidden": true })");
    ASSERT_NE(shader, nullptr);
    {
        const ErrorStyleGuard style(UserShaderErrorStyle::Unlit);
        EXPECT_TRUE(Near(OverBlue(shader), {0, 0, 255}));
    }
    const ErrorStyleGuard style(UserShaderErrorStyle::Magenta);
    EXPECT_TRUE(Near(OverBlue(shader), {255, 0, 255})) << "the failure still shows in the editor";
}

TEST(ShaderFallbackTest, TheFallbackBlendsByTheShadersMode)
{
    const ErrorStyleGuard style(UserShaderErrorStyle::Unlit);
    const auto additive = MakeShader(R"("state": { "blend": "additive" }, "fallback": { "tint": "u_Tint" })");
    ASSERT_NE(additive, nullptr);
    EXPECT_TRUE(Near(OverBlue(additive, [](Sprite2D &sprite)
    {
        EXPECT_TRUE(sprite.SetShaderParam("u_Tint", Common::Color{1.0f, 0.0f, 0.0f, 1.0f}));
    }), {255, 0, 255})) << "red added to blue";
}
