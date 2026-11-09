// Sprite2D (docs/sprite2d.html): the shape meshes, the draw order, flips, outlines and the software renderer's cut-out, drawn as
// Application::Render draws a scene on a headless software renderer, plus serialization and Lua.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/rendering/ShapeMesh2D.hpp"
#include "engine/rendering/Sprite2D.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/scripting/LuaRuntime.hpp"

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
        /// (x, y) counted from the bottom-left pixel
        [[nodiscard]] Rgb At(const int x, const int y) const
        {
            const std::size_t i = (static_cast<std::size_t>(y) * Size + static_cast<std::size_t>(x)) * 4;
            return {rgba[i], rgba[i + 1], rgba[i + 2]};
        }
    };

    bool IsRed(const Rgb &c) { return c.r > 200 && c.g < 60 && c.b < 60; }
    bool IsGreen(const Rgb &c) { return c.g > 200 && c.r < 60 && c.b < 60; }
    bool IsBlue(const Rgb &c) { return c.b > 200 && c.r < 60 && c.g < 60; }
    bool IsBlack(const Rgb &c) { return c.r < 30 && c.g < 30 && c.b < 30; }

    /// The scene `build` makes, drawn by an orthographic camera that shows -2 to 2 on both axes: one world unit is 16 pixels, and
    /// the world origin is the pixel (32, 32)
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

            auto scene = Scene::Create("Sprite2DFrame");
            build(*scene);

            renderer.BeginFrame();
            renderer.SetViewProjection(camera.GetViewMatrix().Data(), camera.GetProjectionMatrix().Data());
            renderer.UpdateSceneLighting(scene->CollectLighting(), camera.GetPosition());
            scene->Render(&renderer, camera);
            renderer.EndFrame();
            renderer.Present();
            renderer.ReadFramebuffer(frame.rgba.data(), Size, Size);
            scene.reset(); // before the renderer shuts down
        }
        renderer.Shutdown();
        return frame;
    }

    Sprite2D *AddSprite(Scene &scene, const std::string &name, const Vector3 &position, const Common::Color &color, const int order = 0)
    {
        auto go = GameObject::Create(name);
        go->CreatePositionable();
        go->GetPositionable()->SetPosition(position);
        auto *sprite = go->AddComponent<Sprite2D>();
        sprite->SetColor(color);
        sprite->SetSortingOrder(order);
        scene.AddRootGameObject(go);
        return sprite;
    }

    float SignedArea(const Renderer::Common::MeshData &data, const std::size_t triangle)
    {
        const auto &a = data.vertices[data.indices[triangle * 3]].position;
        const auto &b = data.vertices[data.indices[triangle * 3 + 1]].position;
        const auto &c = data.vertices[data.indices[triangle * 3 + 2]].position;
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    }

    void ExpectCounterClockwise(const Renderer::Common::MeshData &data)
    {
        ASSERT_FALSE(data.indices.empty());
        for (std::size_t t = 0; t < data.indices.size() / 3; ++t)
        {
            EXPECT_GT(SignedArea(data, t), 0.0f) << "triangle " << t;
        }
    }
}

// ===== Meshes =====

TEST(ShapeMesh2DTest, EachShapeIsAFanFacingTheCameraWithinItsSize)
{
    for (const auto [shape, corners] : {std::pair{Shape2D::Rect, 4}, {Shape2D::Triangle, 3}, {Shape2D::Circle, kCircleSegments2D}})
    {
        ShapeDesc2D desc;
        desc.shape = shape;
        desc.size = Vector2(2.0f, 1.0f);
        const auto data = MakeShapeMesh2D(desc);
        EXPECT_EQ(data.vertices.size(), static_cast<std::size_t>(corners + 1));
        EXPECT_EQ(data.indices.size(), static_cast<std::size_t>(corners * 3));
        ExpectCounterClockwise(data);
        for (const auto &v : data.vertices)
        {
            EXPECT_LE(std::abs(v.position[0]), 1.0f + 1e-5f);
            EXPECT_LE(std::abs(v.position[1]), 0.5f + 1e-5f);
            EXPECT_FLOAT_EQ(v.position[2], 0.0f);
            EXPECT_FLOAT_EQ(v.normal[2], 1.0f);
        }
    }
}

TEST(ShapeMesh2DTest, ARegularPolygonHasACornerAtTheTop)
{
    ShapeDesc2D desc;
    desc.shape = Shape2D::RegularPolygon;
    desc.sides = 5;
    const auto points = ShapeOutline2D(desc);
    ASSERT_EQ(points.size(), 5u);
    EXPECT_NEAR(points[0].x, 0.0f, 1e-5f);
    EXPECT_NEAR(points[0].y, 0.5f, 1e-5f);
}

TEST(ShapeMesh2DTest, APolygonInEitherWindingIsMadeCounterClockwise)
{
    ShapeDesc2D desc;
    desc.shape = Shape2D::Polygon;
    desc.vertices = {{-0.5f, -0.5f}, {-0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, -0.5f}}; // clockwise
    ExpectCounterClockwise(MakeShapeMesh2D(desc));
}

TEST(ShapeMesh2DTest, ShapesThatCannotBeDrawnMakeNothingAndSayWhy)
{
    const auto fails = [](const ShapeDesc2D &desc, const std::string &reason)
    {
        std::string error;
        EXPECT_TRUE(MakeShapeMesh2D(desc, &error).vertices.empty());
        EXPECT_NE(error.find(reason), std::string::npos) << error;
        EXPECT_EQ(GetShapeMesh2D(desc), nullptr);
    };
    ShapeDesc2D zero;
    zero.size = Vector2(0.0f, 1.0f);
    fails(zero, "size");
    ShapeDesc2D sides;
    sides.shape = Shape2D::RegularPolygon;
    sides.sides = 2;
    fails(sides, "3 to 64 sides");
    ShapeDesc2D concave;
    concave.shape = Shape2D::Polygon;
    concave.vertices = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.0f, -0.2f}, {0.0f, 0.5f}};
    fails(concave, "not convex");
    ShapeDesc2D few;
    few.shape = Shape2D::Polygon;
    few.vertices = {{0.0f, 0.0f}, {1.0f, 0.0f}};
    fails(few, "3 to 64 points");
}

TEST(ShapeMesh2DTest, ThePivotMovesTheShapeAroundTheOrigin)
{
    ShapeDesc2D desc;
    desc.size = Vector2(2.0f, 4.0f);
    desc.pivot = Vector2(0.0f, 0.0f);
    const auto points = ShapeOutline2D(desc);
    ASSERT_EQ(points.size(), 4u);
    EXPECT_FLOAT_EQ(points[0].x, 0.0f);
    EXPECT_FLOAT_EQ(points[0].y, 0.0f);
    EXPECT_FLOAT_EQ(points[2].x, 2.0f);
    EXPECT_FLOAT_EQ(points[2].y, 4.0f);
}

TEST(ShapeMesh2DTest, TheRegionMapsTheSizeOntoPartOfTheTexture)
{
    ShapeDesc2D desc;
    desc.regionOffset = Vector2(0.5f, 0.25f);
    desc.regionSize = Vector2(0.5f, 0.25f);
    const auto data = MakeShapeMesh2D(desc);
    ASSERT_EQ(data.vertices.size(), 5u);
    // vertex 1 is the lower-left corner, vertex 3 the upper-right
    EXPECT_FLOAT_EQ(data.vertices[1].texCoord[0], 0.5f);
    EXPECT_FLOAT_EQ(data.vertices[1].texCoord[1], 0.25f);
    EXPECT_FLOAT_EQ(data.vertices[3].texCoord[0], 1.0f);
    EXPECT_FLOAT_EQ(data.vertices[3].texCoord[1], 0.5f);
}

TEST(ShapeMesh2DTest, AnOutlineIsARingOfTheGivenWidthAndATooWideOneIsFilled)
{
    ShapeDesc2D desc;
    desc.size = Vector2(2.0f, 1.0f);
    desc.outline = 0.1f;
    const auto ring = MakeShapeMesh2D(desc);
    ASSERT_EQ(ring.vertices.size(), 8u);
    EXPECT_EQ(ring.indices.size(), 24u);
    ExpectCounterClockwise(ring);
    // The inner corner of the lower-left one: 0.1 in from both sides, whatever the aspect
    EXPECT_NEAR(ring.vertices[1].position[0], -0.9f, 1e-5f);
    EXPECT_NEAR(ring.vertices[1].position[1], -0.4f, 1e-5f);

    desc.outline = 0.6f; // more than half the height
    EXPECT_EQ(MakeShapeMesh2D(desc).vertices.size(), 5u);
}

TEST(ShapeMesh2DTest, OneDescriptionGivesOneSharedMesh)
{
    ShapeDesc2D desc;
    desc.shape = Shape2D::Circle;
    desc.size = Vector2(0.25f, 0.25f);
    const auto first = GetShapeMesh2D(desc);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(GetShapeMesh2D(desc), first);
    desc.outline = 0.05f;
    EXPECT_NE(GetShapeMesh2D(desc), first);
}

// ===== Drawing =====

TEST(Sprite2DTest, AlwaysTheTransparentQueueWithItsSortingOrder)
{
    auto go = GameObject::Create("Sprite");
    auto *sprite = go->AddComponent<Sprite2D>();
    sprite->SetSortingOrder(7);
    EXPECT_EQ(sprite->GetRenderQueue().queue, RenderQueue::Transparent);
    EXPECT_EQ(sprite->GetRenderQueue().sortKey, 7);
}

TEST(Sprite2DTest, AFilledShapeDrawsInItsColour)
{
    const Frame frame = Draw([](Scene &scene) { AddSprite(scene, "Red", Vector3(0.0f, 0.0f, 0.0f), Common::Color::Red); });
    EXPECT_TRUE(IsRed(frame.At(32, 32)));
    EXPECT_TRUE(IsBlack(frame.At(5, 5))) << "outside the 1 x 1 square";
}

TEST(Sprite2DTest, TheHigherSortingOrderIsOnTopWhateverTheHierarchyOrder)
{
    const auto overlap = [](const int blueOrder, const int redOrder)
    {
        return Draw([&](Scene &scene)
        {
            AddSprite(scene, "Blue", Vector3(0.0f, 0.0f, 0.0f), Common::Color::Blue, blueOrder)->SetSize(Vector2(2.0f, 2.0f));
            AddSprite(scene, "Red", Vector3(0.0f, 0.0f, 0.0f), Common::Color::Red, redOrder)->SetShape(Shape2D::Circle);
        });
    };
    EXPECT_TRUE(IsRed(overlap(0, 1).At(32, 32)));
    EXPECT_TRUE(IsBlue(overlap(1, 0).At(32, 32)));
    EXPECT_TRUE(IsBlue(overlap(1, 0).At(40, 40))) << "outside the circle, inside the square";
}

TEST(Sprite2DTest, FlipYTurnsATriangleUpsideDown)
{
    const auto triangle = [](const bool flip)
    {
        return Draw([&](Scene &scene)
        {
            auto *sprite = AddSprite(scene, "Tri", Vector3(0.0f, 0.0f, 0.0f), Common::Color::Green);
            sprite->SetShape(Shape2D::Triangle);
            sprite->SetSize(Vector2(2.0f, 2.0f));
            sprite->SetFlipY(flip);
        });
    };
    // Near the bottom corners: inside an upright triangle's base, outside an upside-down one
    EXPECT_TRUE(IsGreen(triangle(false).At(18, 18)));
    EXPECT_TRUE(IsBlack(triangle(true).At(18, 18)));
    EXPECT_TRUE(IsGreen(triangle(true).At(18, 46)));
}

TEST(Sprite2DTest, AnOutlineLeavesTheMiddleEmpty)
{
    const Frame frame = Draw([](Scene &scene)
    {
        auto *sprite = AddSprite(scene, "Frame", Vector3(0.0f, 0.0f, 0.0f), Common::Color::Green);
        sprite->SetSize(Vector2(3.0f, 3.0f));
        sprite->SetOutline(0.25f);
    });
    EXPECT_TRUE(IsBlack(frame.At(32, 32)));
    EXPECT_TRUE(IsGreen(frame.At(32, 9))) << "on the bottom border";
}

TEST(Sprite2DTest, ATexturesTransparentPixelsAreCutOutOnTheSoftwareRenderer)
{
    // Left half transparent, right half opaque green
    const std::vector<std::uint8_t> pixels = {255, 0, 0, 0, 0, 255, 0, 255};
    Renderer::Common::TextureOptions options;
    options.filter = Renderer::Common::TextureFilter::Nearest;
    options.mipmaps = false;
    const auto texture = Texture::Create(2, 1, pixels, options);
    ASSERT_NE(texture, nullptr);

    const Frame frame = Draw([&](Scene &scene)
    {
        AddSprite(scene, "Back", Vector3(0.0f, 0.0f, 0.0f), Common::Color::Blue, -1)->SetSize(Vector2(3.0f, 3.0f));
        auto *sprite = AddSprite(scene, "Textured", Vector3(0.0f, 0.0f, 0.0f), Common::Color::White);
        sprite->SetSize(Vector2(2.0f, 2.0f));
        sprite->SetTexture(texture);
    });
    EXPECT_TRUE(IsBlue(frame.At(20, 32))) << "the transparent half shows what is behind it";
    EXPECT_TRUE(IsGreen(frame.At(44, 32)));
}

TEST(Sprite2DTest, ASpriteThatCannotBeDrawnDrawsNothing)
{
    const Frame frame = Draw([](Scene &scene)
    {
        auto *sprite = AddSprite(scene, "Bad", Vector3(0.0f, 0.0f, 0.0f), Common::Color::Red);
        sprite->SetShape(Shape2D::RegularPolygon);
        sprite->SetSides(1);
    });
    EXPECT_TRUE(IsBlack(frame.At(32, 32)));
}

TEST(Sprite2DTest, TwoThousandSpritesDrawAndTheTimeIsPrinted)
{
    const auto start = std::chrono::steady_clock::now();
    const Frame frame = Draw([](Scene &scene)
    {
        for (int i = 0; i < 2000; ++i)
        {
            auto *sprite = AddSprite(scene, "Bullet", Vector3(-1.9f + static_cast<float>(i % 40) * 0.1f, -1.9f + static_cast<float>(i / 40) * 0.075f, 0.0f),
                                     Common::Color::Green, i % 3);
            sprite->SetShape(Shape2D::Circle);
            sprite->SetSize(Vector2(0.05f, 0.05f));
        }
    });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("[SPRITE2D-PERF] 2000 circles, scene built and drawn on the software renderer at %dx%d in %.1f ms\n", Size, Size, ms);
    (void)frame;
}

// ===== Saving and Lua =====

TEST(Sprite2DTest, EveryFieldSurvivesASceneLoad)
{
    const auto go = GameObject::Create("Sprite");
    go->CreatePositionable();
    auto *sprite = go->AddComponent<Sprite2D>();
    sprite->SetShape(Shape2D::Polygon);
    sprite->SetSize(Vector2(2.0f, 3.0f));
    sprite->SetSides(9);
    sprite->SetVertices({{0.0f, 0.0f}, {0.5f, 0.0f}, {0.0f, 0.5f}});
    sprite->SetOutline(0.125f);
    sprite->SetColor(Common::Color::Red);
    sprite->SetRegion(Vector2(0.25f, 0.5f), Vector2(0.25f, 0.5f));
    sprite->SetFlipX(true);
    sprite->SetFlipY(true);
    sprite->SetPivot(Vector2(0.0f, 1.0f));
    sprite->SetSortingOrder(-4);

    const auto scene = Scene::Create("Sprite2DRoundTrip");
    scene->AddRootGameObject(go);
    const auto loaded = Scene::FromJSON(scene->Serialize());
    ASSERT_NE(loaded, nullptr);
    const auto back = loaded->FindGameObject("Sprite");
    ASSERT_NE(back, nullptr);
    const auto *copy = back->GetComponent<Sprite2D>();
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->GetShape(), Shape2D::Polygon);
    EXPECT_FLOAT_EQ(copy->GetSize().y, 3.0f);
    EXPECT_EQ(copy->GetSides(), 9);
    ASSERT_EQ(copy->GetVertices().size(), 3u);
    EXPECT_FLOAT_EQ(copy->GetVertices()[1].x, 0.5f);
    EXPECT_FLOAT_EQ(copy->GetOutline(), 0.125f);
    EXPECT_GT(copy->GetColor().r, 0.9f);
    EXPECT_LT(copy->GetColor().g, 0.1f);
    EXPECT_FLOAT_EQ(copy->GetRegionOffset().y, 0.5f);
    EXPECT_FLOAT_EQ(copy->GetRegionSize().x, 0.25f);
    EXPECT_TRUE(copy->GetFlipX());
    EXPECT_TRUE(copy->GetFlipY());
    EXPECT_FLOAT_EQ(copy->GetPivot().y, 1.0f);
    EXPECT_EQ(copy->GetSortingOrder(), -4);
}

TEST(Sprite2DTest, AScriptCanSetEveryField)
{
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    sol::state &lua = Scripting::LuaRuntime::Instance().GetState();
    const sol::protected_function_result result = lua.safe_script(R"(
        sprite_go = GameObject.Create('LuaSprite')
        local s = sprite_go:AddComponent('Sprite2D')
        s:SetShape(Sprite2DShape.RegularPolygon)
        assert(s:GetShape() == Sprite2DShape.RegularPolygon)
        s:SetSides(8)
        assert(s:GetSides() == 8)
        s:SetSize(Vector2(2, 1))
        assert(s:GetSize().x == 2)
        s:SetVertices({Vector2(0, 0), Vector2(1, 0), Vector2(0, 1)})
        assert(#s:GetVertices() == 3)
        s:SetOutline(0.5)
        assert(s:GetOutline() == 0.5)
        s:SetColor(Color.Red)
        s:SetRegion(Vector2(0.5, 0), Vector2(0.5, 1))
        assert(s:GetRegionOffset().x == 0.5)
        assert(s:GetRegionSize().y == 1)
        s:SetFlipX(true)
        assert(s:GetFlipX())
        s:SetFlipY(true)
        assert(s:GetFlipY())
        s:SetPivot(Vector2(0, 0))
        assert(s:GetPivot().x == 0)
        s:SetSortingOrder(3)
        assert(s:GetSortingOrder() == 3)
        s:SetTexture(nil)
        assert(not s:HasTexture())
        assert(sprite_go:GetComponent('Sprite2D') == s)
        local ok = pcall(function() s:SetVertices({Vector2(0, 0), 'corner'}) end)
        assert(not ok)
        sprite_go = nil
    )", sol::script_pass_on_error);
    if (!result.valid())
    {
        const sol::error error = result;
        FAIL() << error.what();
    }
}
