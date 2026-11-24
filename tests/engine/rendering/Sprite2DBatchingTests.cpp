// Sprite2D batching, issue #445 item 4 (docs/sprite2d.html#batching): runs of alike sprites in the Transparent queue drawn as one draw call
// (SpriteBatcher), on the headless software renderer. The draw calls are counted by the renderer (IRenderer::GetLastFrameCounters) and
// by the batcher (SpriteBatcher::GetFrameStats); the batched picture is compared pixel by pixel with the same scene drawn unbatched.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/common/RenderCounters.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/RenderSettings.hpp"
#include "engine/rendering/Shader.hpp"
#include "engine/rendering/Sprite2D.hpp"
#include "engine/rendering/SpriteBatcher.hpp"
#include "engine/rendering/Texture.hpp"
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
    constexpr int Size = 128;

    struct Result
    {
        std::vector<std::uint8_t> rgba;
        Renderer::Common::RenderCounters counters;
        SpriteBatchStats stats;
    };

    /// Batching on or off for one test, put back afterwards
    struct BatchingGuard
    {
        bool previous = SpriteBatcher::IsEnabled();
        explicit BatchingGuard(const bool enabled) { SpriteBatcher::SetEnabled(enabled); }
        ~BatchingGuard() { SpriteBatcher::SetEnabled(previous); }
    };

    /// The scene `build` makes, drawn once by an orthographic camera showing -4 to 4 on both axes (16 pixels a unit, the origin at the
    /// pixel (64, 64)), on black, with batching on or off
    Result Draw(const std::function<void(Scene &)> &build, const bool batching)
    {
        const BatchingGuard guard(batching);
        SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, Size, Size));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        Result result;
        result.rgba.resize(static_cast<std::size_t>(Size) * Size * 4);
        {
            Camera camera;
            camera.SetOrthographic(-4.0f, 4.0f, -4.0f, 4.0f, 0.1f, 100.0f);
            camera.SetPosition(Vector3(0.0f, 0.0f, 10.0f));
            auto scene = Scene::Create("Sprite2DBatching");
            build(*scene);
            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            renderer.UpdateSceneLighting(scene->CollectLighting(), camera.GetPosition());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(result.rgba.data(), Size, Size);
            result.counters = renderer.GetLastFrameCounters();
            result.stats = SpriteBatcher::GetFrameStats(&renderer);
            scene.reset();
        }
        renderer.Shutdown();
        return result;
    }

    Sprite2D *AddSprite(Scene &scene, const Vector3 &position, const Common::Color &color, const int order = 0,
                        const Vector2 &size = Vector2(0.5f, 0.5f))
    {
        auto go = GameObject::Create("Sprite");
        go->CreatePositionable();
        go->GetPositionable()->SetPosition(position);
        auto *sprite = go->AddComponent<Sprite2D>();
        sprite->SetColor(color);
        sprite->SetSize(size);
        sprite->SetSortingOrder(order);
        scene.AddRootGameObject(go);
        return sprite;
    }

    /// The number of pixels where the two pictures differ by more than one step in a channel (8-bit rounding of the same colour reached
    /// by the uniform or by the vertex colour may differ by one)
    int DifferingPixels(const Result &a, const Result &b)
    {
        int differing = 0;
        for (std::size_t i = 0; i + 3 < a.rgba.size(); i += 4)
        {
            for (std::size_t c = 0; c < 3; ++c)
            {
                if (std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])) > 1)
                {
                    ++differing;
                    break;
                }
            }
        }
        return differing;
    }

    int Lit(const Result &result)
    {
        int lit = 0;
        for (std::size_t i = 0; i + 3 < result.rgba.size(); i += 4)
        {
            lit += (result.rgba[i] | result.rgba[i + 1] | result.rgba[i + 2]) != 0 ? 1 : 0;
        }
        return lit;
    }

    std::shared_ptr<Texture> Checker()
    {
        Renderer::Common::TextureOptions options;
        options.filter = Renderer::Common::TextureFilter::Nearest;
        options.mipmaps = false;
        const std::vector<std::uint8_t> pixels = {255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255};
        return Texture::Create(2, 2, pixels, options);
    }

    /// A mixed scene: shapes on a 1/16-unit grid (so no edge passes through a pixel centre), colours, a half-transparent and an additive
    /// run, two textures' worth of runs, flips and rotations, in several sorting orders that interleave
    void MixedScene(Scene &scene, const std::shared_ptr<Texture> &texture)
    {
        const Common::Color palette[] = {Common::Color::Red, Common::Color::Green, Common::Color::Blue, Common::Color::White,
                                         Common::Color{1.0f, 1.0f, 0.0f, 0.5f}, Common::Color{0.0f, 1.0f, 1.0f, 1.0f}};
        for (int i = 0; i < 240; ++i)
        {
            const float x = -3.5f + static_cast<float>(i % 16) * 0.4375f;
            const float y = -3.5f + static_cast<float>(i / 16) * 0.4375f;
            Sprite2D *sprite = AddSprite(scene, Vector3(x, y, 0.0f), palette[i % 6], (i / 7) % 3, Vector2(0.625f, 0.375f));
            switch (i % 5)
            {
            case 1: sprite->SetShape(Shape2D::Circle); break;
            case 2: sprite->SetShape(Shape2D::Triangle); sprite->SetFlipY(true); break;
            case 3: sprite->SetShape(Shape2D::RegularPolygon); sprite->SetSides(5); break;
            default: break;
            }
            if (i % 11 == 0)
            {
                sprite->SetBlendMode(SpriteBlend::Additive);
            }
            if (i % 13 < 4)
            {
                sprite->SetTexture(texture);
            }
            if (i % 17 == 0)
            {
                sprite->GetGameObject().GetPositionable()->SetRotation(Math::Quaternion::FromEulerAngles(0.0f, 0.0f, 30.0f));
            }
        }
    }
}

TEST(Sprite2DBatchingTest, AThousandAlikeSpritesAreOneDrawCallInsteadOfAThousand)
{
    const auto build = [](Scene &scene)
    {
        for (int i = 0; i < 1000; ++i)
        {
            // Different colours and positions batch: the colour rides in the vertex colours
            // On the 1/16-unit pixel grid, so no edge passes through a pixel centre
            const float x = -3.75f + static_cast<float>(i % 40) * 0.1875f;
            const float y = -3.75f + static_cast<float>(i / 40) * 0.25f;
            AddSprite(scene, Vector3(x, y, 0.0f), i % 2 == 0 ? Common::Color::Green : Common::Color::Red, 0, Vector2(0.125f, 0.125f));
        }
    };
    const Result batched = Draw(build, true);
    EXPECT_EQ(batched.counters.drawCalls, 1u);
    EXPECT_EQ(batched.stats.sprites, 1000u);
    EXPECT_EQ(batched.stats.drawCalls, 1u);
    EXPECT_EQ(batched.stats.batches, 1u);
    EXPECT_EQ(batched.stats.batchedSprites, 1000u);
    EXPECT_EQ(batched.counters.triangles, 2000u) << "two triangles a rect, all in the one draw";

    const Result alone = Draw(build, false);
    EXPECT_EQ(alone.counters.drawCalls, 1000u);
    EXPECT_EQ(alone.stats.drawCalls, 1000u);
    EXPECT_EQ(alone.stats.batches, 0u);
    EXPECT_EQ(DifferingPixels(batched, alone), 0);
    EXPECT_GT(Lit(batched), 1000);
}

TEST(Sprite2DBatchingTest, TheBatchedPictureIsTheUnbatchedOne)
{
    const auto texture = Checker();
    ASSERT_NE(texture, nullptr);
    const auto build = [&](Scene &scene) { MixedScene(scene, texture); };
    const Result batched = Draw(build, true);
    const Result alone = Draw(build, false);
    EXPECT_EQ(alone.counters.drawCalls, 240u);
    EXPECT_LT(batched.counters.drawCalls, alone.counters.drawCalls) << "some runs batch";
    EXPECT_GT(batched.stats.batches, 0u);
    EXPECT_EQ(batched.stats.sprites, 240u);
    EXPECT_EQ(batched.counters.drawCalls, batched.stats.drawCalls) << "every draw of the frame is a sprite's";
    // Rotated and round edges cross pixel centres at arbitrary places, where transforming on the CPU then on the GPU can round a
    // centre that lies on an edge to the other side; a handful of such pixels out of 16,384 is allowed, nothing more
    EXPECT_LE(DifferingPixels(batched, alone), 4);
    EXPECT_GT(Lit(batched), 2000);
}

TEST(Sprite2DBatchingTest, WhatBreaksABatch)
{
    const auto texture = Checker();
    ASSERT_NE(texture, nullptr);
    const auto countDraws = [](const std::function<void(Scene &)> &build) { return Draw(build, true).counters.drawCalls; };

    // Alike: one draw
    EXPECT_EQ(countDraws([](Scene &scene)
    {
        for (int i = 0; i < 4; ++i)
        {
            AddSprite(scene, Vector3(static_cast<float>(i), 0.0f, 0.0f), Common::Color::Red, i);
        }
    }), 1u) << "sorting orders alone do not break a run that stays adjacent";

    // Each blend mode change, texture change, saturation or flash change starts a new batch
    EXPECT_EQ(countDraws([](Scene &scene)
    {
        AddSprite(scene, Vector3(0, 0, 0), Common::Color::Red, 0);
        AddSprite(scene, Vector3(1, 0, 0), Common::Color::Red, 1)->SetBlendMode(SpriteBlend::Additive);
        AddSprite(scene, Vector3(2, 0, 0), Common::Color::Red, 2);
    }), 3u);
    EXPECT_EQ(countDraws([&](Scene &scene)
    {
        AddSprite(scene, Vector3(0, 0, 0), Common::Color::Red, 0);
        AddSprite(scene, Vector3(1, 0, 0), Common::Color::Red, 1)->SetTexture(texture);
        AddSprite(scene, Vector3(2, 0, 0), Common::Color::Red, 2)->SetTexture(texture);
    }), 2u);
    EXPECT_EQ(countDraws([](Scene &scene)
    {
        AddSprite(scene, Vector3(0, 0, 0), Common::Color::Red, 0);
        AddSprite(scene, Vector3(1, 0, 0), Common::Color::Red, 1)->SetSaturation(0.0f);
        AddSprite(scene, Vector3(2, 0, 0), Common::Color::Red, 2)->SetFlash(Common::Color{1, 1, 1, 0.5f});
    }), 3u);

    // Another renderable between them in the sorted order
    EXPECT_EQ(countDraws([](Scene &scene)
    {
        AddSprite(scene, Vector3(0, 0, 0), Common::Color::Red, -1);
        AddSprite(scene, Vector3(1, 0, 0), Common::Color::Red, -1);
        auto quad = GameObject::Create("Quad");
        quad->CreatePositionable();
        auto *renderer = quad->AddComponent<MeshRenderer>();
        renderer->SetMesh(Mesh::GetBuiltin(BuiltinMesh::Quad));
        auto glass = Material::Create(ShadingModel::Unlit);
        glass->SetAlphaMode(AlphaMode::Blend);
        renderer->SetMaterial(0, glass);
        scene.AddRootGameObject(quad);
        AddSprite(scene, Vector3(2, 0, 0), Common::Color::Red, 1);
        AddSprite(scene, Vector3(3, 0, 0), Common::Color::Red, 1);
    }), 3u) << "two sprites, the quad (sort key 0), two sprites";
}

TEST(Sprite2DBatchingTest, ASpriteWithAUserShaderDrawsAloneAndEndsTheRun)
{
    RenderSettings::SetUserShaderErrorStyle(UserShaderErrorStyle::Unlit);
    auto shader = Shader::FromSource("{ \"n2shader\": 1, \"params\": [] }\n#stage fragment\nvoid main() { n2_FragColor = n2_Color; }\n",
                                     "plain");
    ASSERT_NE(shader, nullptr);
    shader->SetCookedDirectory(std::filesystem::temp_directory_path() / "n2-no-cooked-data-here");
    const Result result = Draw([&](Scene &scene)
    {
        AddSprite(scene, Vector3(0, 0, 0), Common::Color::Red, 0);
        AddSprite(scene, Vector3(1, 0, 0), Common::Color::Red, 1);
        AddSprite(scene, Vector3(2, 0, 0), Common::Color::Red, 2)->SetShader(shader);
        AddSprite(scene, Vector3(3, 0, 0), Common::Color::Red, 3);
        AddSprite(scene, Vector3(-1, 0, 0), Common::Color::Red, 4);
    }, true);
    EXPECT_EQ(result.counters.drawCalls, 3u) << "a batch of two, the shader sprite, a batch of two";
    EXPECT_EQ(result.stats.sprites, 5u);
    EXPECT_EQ(result.stats.batches, 2u);
    EXPECT_EQ(result.stats.batchedSprites, 4u);
}

TEST(Sprite2DBatchingTest, ALoneSpriteDrawsItself)
{
    const Result result = Draw([](Scene &scene) { AddSprite(scene, Vector3(0, 0, 0), Common::Color::Red, 0); }, true);
    EXPECT_EQ(result.counters.drawCalls, 1u);
    EXPECT_EQ(result.stats.batches, 0u) << "a batch of one is the sprite's own draw";
    EXPECT_EQ(result.stats.sprites, 1u);
}

TEST(Sprite2DBatchingTest, FiveThousandSpritesDrawAndTheTimeIsPrinted)
{
    for (const bool batching : {false, true})
    {
        const auto start = std::chrono::steady_clock::now();
        const Result result = Draw([](Scene &scene)
        {
            for (int i = 0; i < 5000; ++i)
            {
                Sprite2D *sprite = AddSprite(scene, Vector3(-3.9f + static_cast<float>(i % 80) * 0.1f, -3.9f + static_cast<float>(i / 80) * 0.12f, 0.0f),
                                             i % 3 == 0 ? Common::Color::Green : Common::Color{0.2f, 0.6f, 1.0f, 0.8f}, i % 2,
                                             Vector2(0.06f, 0.06f));
                sprite->SetShape(Shape2D::Circle);
            }
        }, batching);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("[SPRITE2D-PERF] 5000 circles, batching %s: %u draw calls, scene built and drawn on the software renderer in %.1f ms\n",
                    batching ? "on" : "off", static_cast<unsigned>(result.counters.drawCalls), ms);
        if (batching)
        {
            EXPECT_LE(result.counters.drawCalls, 2u) << "two sorting orders, so at most two runs";
        }
    }
}

#ifdef N2ENGINE_SCRIPTING_ENABLED
TEST(Sprite2DBatchingTest, AScriptReadsTheFrameStats)
{
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    sol::state &lua = Scripting::LuaRuntime::Instance().GetState();
    const sol::protected_function_result result = lua.safe_script(R"(
        local stats = Graphics.GetFrameStats()
        for _, key in ipairs({'drawCalls', 'triangles', 'vertices', 'sprites', 'spriteDrawCalls', 'spriteBatches', 'batchedSprites'}) do
            assert(type(stats[key]) == 'number', key)
        end
        -- No window here, so no renderer: all zeros
        assert(stats.drawCalls == 0 and stats.sprites == 0)
    )", sol::script_pass_on_error);
    if (!result.valid())
    {
        const sol::error error = result;
        FAIL() << error.what();
    }
}
#endif
