// Lines, trails and 2D meshes (docs/meshes.html#lines, issue #445 item 5): the line mesh (miter, bevel, caps, loops, degenerate
// points), polygon triangulation, the checks on a script's mesh, trail point aging, saving, and drawing on a headless software
// renderer (readback), as Sprite2DTests.cpp draws.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/Camera.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/rendering/Geometry2D.hpp"
#include "engine/rendering/LineRenderer.hpp"
#include "engine/rendering/MeshRenderer2D.hpp"
#include "engine/rendering/TrailRenderer.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

#include <nlohmann/json.hpp>

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
    bool IsBlack(const Rgb &c) { return c.r < 30 && c.g < 30 && c.b < 30; }

    /// The scene `build` makes, drawn by an orthographic camera showing -2 to 2 on both axes: one world unit is 16 pixels, and the
    /// world origin is the pixel (32, 32)
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

            auto scene = Scene::Create("LineFrame");
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

    template <typename T>
    T *AddTo(Scene &scene, const std::string &name, const Vector3 &position = Vector3(0.0f, 0.0f, 0.0f))
    {
        auto go = GameObject::Create(name);
        go->CreatePositionable();
        go->GetPositionable()->SetPosition(position);
        auto *component = go->AddComponent<T>();
        scene.AddRootGameObject(go);
        return component;
    }

    float SignedArea(const Renderer::Common::MeshData &data, const std::size_t triangle)
    {
        const auto &a = data.vertices[data.indices[triangle * 3]].position;
        const auto &b = data.vertices[data.indices[triangle * 3 + 1]].position;
        const auto &c = data.vertices[data.indices[triangle * 3 + 2]].position;
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    }

    void ExpectValidCounterClockwise(const Renderer::Common::MeshData &data)
    {
        ASSERT_FALSE(data.indices.empty());
        ASSERT_EQ(data.indices.size() % 3, 0u);
        for (const std::uint32_t index : data.indices)
        {
            ASSERT_LT(index, data.vertices.size());
        }
        for (const auto &v : data.vertices)
        {
            EXPECT_TRUE(std::isfinite(v.position[0]) && std::isfinite(v.position[1]) && std::isfinite(v.position[2]));
        }
        for (std::size_t t = 0; t < data.indices.size() / 3; ++t)
        {
            EXPECT_GE(SignedArea(data, t), 0.0f) << "triangle " << t;
        }
    }

    std::vector<LinePoint> Points(const std::vector<Vector2> &xy, const float width = 0.2f)
    {
        std::vector<LinePoint> points;
        for (const Vector2 &p : xy)
        {
            LinePoint point;
            point.position = Vector3(p.x, p.y, 0.0f);
            point.width = width;
            points.push_back(point);
        }
        return points;
    }

    bool HasVertexAt(const Renderer::Common::MeshData &data, const float x, const float y)
    {
        for (const auto &v : data.vertices)
        {
            if (std::abs(v.position[0] - x) < 1e-4f && std::abs(v.position[1] - y) < 1e-4f)
            {
                return true;
            }
        }
        return false;
    }

    float TotalArea(const Renderer::Common::MeshData &data)
    {
        float area = 0.0f;
        for (std::size_t t = 0; t < data.indices.size() / 3; ++t)
        {
            area += 0.5f * SignedArea(data, t);
        }
        return area;
    }
}

// ===== The line mesh =====

TEST(LineMeshTest, ASegmentIsOneQuadOfItsWidth)
{
    const auto data = MakeLineMesh(Points({{0.0f, 0.0f}, {2.0f, 0.0f}}, 0.5f), LineShape{});
    ExpectValidCounterClockwise(data);
    EXPECT_EQ(data.indices.size(), 6u);
    EXPECT_NEAR(TotalArea(data), 2.0f * 0.5f, 1e-5f);
    EXPECT_TRUE(HasVertexAt(data, 0.0f, 0.25f));
    EXPECT_TRUE(HasVertexAt(data, 2.0f, -0.25f));
    for (const auto &v : data.vertices)
    {
        EXPECT_FLOAT_EQ(v.normal[2], 1.0f);
    }
}

TEST(LineMeshTest, AMiterCornerMeetsAtAPointWithNoExtraTriangle)
{
    const auto data = MakeLineMesh(Points({{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}}), LineShape{});
    ExpectValidCounterClockwise(data);
    EXPECT_EQ(data.indices.size(), 12u);
    // A left turn: the outer corner is to the lower right, the inner one to the upper left, both half the width out on each axis
    EXPECT_TRUE(HasVertexAt(data, 1.1f, -0.1f));
    EXPECT_TRUE(HasVertexAt(data, 0.9f, 0.1f));
}

TEST(LineMeshTest, ABevelCutsTheOuterCornerWithOneTriangle)
{
    LineShape shape;
    shape.join = LineJoin::Bevel;
    const auto data = MakeLineMesh(Points({{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}}), shape);
    ExpectValidCounterClockwise(data);
    EXPECT_EQ(data.indices.size(), 15u);
    EXPECT_FALSE(HasVertexAt(data, 1.1f, -0.1f));
    EXPECT_TRUE(HasVertexAt(data, 1.0f, -0.1f)); // the first segment's corner
    EXPECT_TRUE(HasVertexAt(data, 1.1f, 0.0f));  // the second segment's
}

TEST(LineMeshTest, AMiterPastTheLimitIsBevelledAndStaysNearTheLine)
{
    LineShape shape;
    shape.miterLimit = 2.0f;
    // Nearly straight back: the miter would reach far past the corner
    const auto data = MakeLineMesh(Points({{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.1f}}), shape);
    ExpectValidCounterClockwise(data);
    EXPECT_EQ(data.indices.size(), 15u);
    for (const auto &v : data.vertices)
    {
        EXPECT_LE(v.position[0], 1.0f + 0.1f * 2.0f + 1e-4f);
    }
}

TEST(LineMeshTest, ASquareCapReachesHalfTheWidthPastTheEnds)
{
    LineShape shape;
    shape.cap = LineCap::Square;
    const auto data = MakeLineMesh(Points({{0.0f, 0.0f}, {1.0f, 0.0f}}), shape);
    EXPECT_TRUE(HasVertexAt(data, -0.1f, 0.1f));
    EXPECT_TRUE(HasVertexAt(data, 1.1f, -0.1f));
    EXPECT_NEAR(TotalArea(data), 1.2f * 0.2f, 1e-5f);
}

TEST(LineMeshTest, AClosedLoopHasASegmentBackToTheStart)
{
    LineShape shape;
    shape.loop = true;
    // The last point repeats the first: dropped
    const auto data = MakeLineMesh(Points({{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}}), shape);
    ExpectValidCounterClockwise(data);
    EXPECT_EQ(data.indices.size(), 4u * 6u);
    // Every corner is a miter: a ring from 0.1 outside the square to 0.1 inside
    EXPECT_TRUE(HasVertexAt(data, -0.1f, -0.1f));
    EXPECT_TRUE(HasVertexAt(data, 0.1f, 0.1f));
    EXPECT_NEAR(TotalArea(data), 1.2f * 1.2f - 0.8f * 0.8f, 1e-4f);
    // u runs once around: the closing segment ends at u = 1 on the first point
    float largestU = 0.0f;
    for (const auto &v : data.vertices)
    {
        largestU = std::max(largestU, v.texCoord[0]);
    }
    EXPECT_FLOAT_EQ(largestU, 1.0f);
}

TEST(LineMeshTest, DegeneratePointsAreDroppedOrRefusedWithAReason)
{
    // Repeated points are dropped: the same mesh as two points
    const auto repeated = MakeLineMesh(Points({{0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}}), LineShape{});
    EXPECT_EQ(repeated.indices.size(), 6u);

    std::string error;
    EXPECT_TRUE(MakeLineMesh(Points({{1.0f, 1.0f}, {1.0f, 1.0f}}), LineShape{}, &error).vertices.empty());
    EXPECT_NE(error.find("two distinct points"), std::string::npos) << error;
    EXPECT_TRUE(MakeLineMesh(Points({}), LineShape{}, &error).vertices.empty());

    error.clear();
    EXPECT_TRUE(MakeLineMesh(Points({{0.0f, 0.0f}, {std::numeric_limits<float>::quiet_NaN(), 0.0f}}), LineShape{}, &error).vertices.empty());
    EXPECT_NE(error.find("not finite"), std::string::npos) << error;

    // Straight back on itself, with either join: finite, valid
    for (const LineJoin join : {LineJoin::Miter, LineJoin::Bevel})
    {
        LineShape shape;
        shape.join = join;
        ExpectValidCounterClockwise(MakeLineMesh(Points({{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}}), shape));
    }
    // Three points in a line: no corner
    EXPECT_EQ(MakeLineMesh(Points({{0.0f, 0.0f}, {1.0f, 0.0f}, {2.0f, 0.0f}}), LineShape{}).indices.size(), 12u);
    // A loop of two points is an open line
    LineShape loop;
    loop.loop = true;
    EXPECT_EQ(MakeLineMesh(Points({{0.0f, 0.0f}, {1.0f, 0.0f}}), loop).indices.size(), 6u);
}

TEST(LineMeshTest, WidthAndColourGoFromStartToEndByLength)
{
    const std::vector<Vector3> positions = {Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(4.0f, 0.0f, 0.0f)};
    const auto points = LinePointsAlongLength(positions, false, 1.0f, 0.0f, Common::Color(1.0f, 0.0f, 0.0f, 1.0f),
                                              Common::Color(0.0f, 0.0f, 1.0f, 0.0f));
    ASSERT_EQ(points.size(), 3u);
    EXPECT_FLOAT_EQ(points[0].width, 1.0f);
    EXPECT_FLOAT_EQ(points[1].width, 0.75f);
    EXPECT_FLOAT_EQ(points[2].width, 0.0f);
    EXPECT_FLOAT_EQ(points[1].color.r, 0.75f);
    EXPECT_FLOAT_EQ(points[1].color.b, 0.25f);
    EXPECT_FLOAT_EQ(points[2].color.a, 0.0f);
}

// ===== Polygons and script meshes =====

TEST(TriangulateTest, AConcavePolygonInEitherWindingIsCoveredExactly)
{
    // An L: area 3
    std::vector<Vector2> l = {{0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 2.0f}, {0.0f, 2.0f}};
    for (int pass = 0; pass < 2; ++pass)
    {
        std::string error;
        const std::vector<std::uint32_t> indices = TriangulatePolygon2D(l, &error);
        ASSERT_EQ(indices.size(), 4u * 3u) << error;
        float area = 0.0f;
        for (std::size_t t = 0; t < indices.size(); t += 3)
        {
            const Vector2 &a = l[indices[t]], &b = l[indices[t + 1]], &c = l[indices[t + 2]];
            const float twice = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            EXPECT_GT(twice, 0.0f);
            area += 0.5f * twice;
        }
        EXPECT_NEAR(area, 3.0f, 1e-5f);
        std::reverse(l.begin(), l.end()); // now clockwise
    }
}

TEST(TriangulateTest, PolygonsThatCannotBeFilledSayWhy)
{
    std::string error;
    EXPECT_TRUE(TriangulatePolygon2D(std::vector<Vector2>{{0.0f, 0.0f}, {1.0f, 0.0f}}, &error).empty());
    EXPECT_NE(error.find("3 to"), std::string::npos) << error;
    EXPECT_TRUE(TriangulatePolygon2D(std::vector<Vector2>{{0.0f, 0.0f}, {1.0f, 0.0f}, {2.0f, 0.0f}}, &error).empty());
    EXPECT_NE(error.find("no area"), std::string::npos) << error;
    // A bow tie crosses itself
    error.clear();
    const auto bowTie = TriangulatePolygon2D(std::vector<Vector2>{{0.0f, 0.0f}, {2.0f, 2.0f}, {2.0f, 0.0f}, {0.0f, 2.0f}}, &error);
    EXPECT_TRUE(bowTie.empty() || bowTie.size() % 3 == 0);
    // Points in a line along an edge are fine
    EXPECT_EQ(TriangulatePolygon2D(std::vector<Vector2>{{0.0f, 0.0f}, {1.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 2.0f}, {0.0f, 2.0f}}).size() % 3, 0u);
    EXPECT_FALSE(TriangulatePolygon2D(std::vector<Vector2>{{0.0f, 0.0f}, {1.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 2.0f}, {0.0f, 2.0f}}).empty());
}

TEST(ScriptMeshTest, AValidDescriptionGivesItsVerticesColoursAndUvs)
{
    ScriptMeshDesc desc;
    desc.positions = {Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.5f)};
    desc.colors = {Common::Color::Red, Common::Color::Green, Common::Color::Blue};
    desc.uvs = {Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(0.0f, 1.0f)};
    desc.indices = {1, 2, 3};
    desc.indexBase = 1;
    const auto data = MakeScriptMeshData(desc);
    ASSERT_TRUE(data.has_value());
    ASSERT_EQ(data->vertices.size(), 3u);
    EXPECT_EQ(data->indices, (std::vector<std::uint32_t>{0, 1, 2}));
    EXPECT_FLOAT_EQ(data->vertices[2].position[2], 0.5f);
    EXPECT_FLOAT_EQ(data->vertices[1].color[1], 1.0f);
    EXPECT_FLOAT_EQ(data->vertices[2].texCoord[1], 1.0f);
    EXPECT_NE(Mesh::Create(*data), nullptr);
}

TEST(ScriptMeshTest, EveryBadDescriptionIsRefusedWithAReason)
{
    const auto refused = [](const ScriptMeshDesc &desc, const std::string &reason)
    {
        std::string error;
        EXPECT_FALSE(MakeScriptMeshData(desc, &error).has_value()) << reason;
        EXPECT_NE(error.find(reason), std::string::npos) << error;
    };
    ScriptMeshDesc good;
    good.positions = {Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f)};
    good.indices = {1, 2, 3};
    good.indexBase = 1;
    ASSERT_TRUE(MakeScriptMeshData(good).has_value());

    ScriptMeshDesc outside = good;
    outside.indices = {1, 2, 4};
    refused(outside, "outside the vertices 1 to 3");
    ScriptMeshDesc zero = good;
    zero.indices = {0, 1, 2};
    refused(zero, "outside the vertices");
    ScriptMeshDesc negative = good;
    negative.indices = {1, 2, -1};
    refused(negative, "outside the vertices");
    ScriptMeshDesc partial = good;
    partial.indices = {1, 2};
    refused(partial, "whole triangles");
    ScriptMeshDesc colours = good;
    colours.colors = {Common::Color::Red};
    refused(colours, "colours for 3 vertices");
    ScriptMeshDesc uvs = good;
    uvs.uvs = {Vector2(0.0f, 0.0f)};
    refused(uvs, "uvs for 3 vertices");
    ScriptMeshDesc none;
    none.indices = {1, 2, 3};
    refused(none, "vertices");
    ScriptMeshDesc tooMany = good;
    tooMany.positions.assign(kMaxScriptMeshVertices + 1, Vector3(0.0f, 0.0f, 0.0f));
    refused(tooMany, "vertices");
    ScriptMeshDesc notFinite = good;
    notFinite.positions[1].x = std::numeric_limits<float>::infinity();
    refused(notFinite, "not finite");
}

// ===== Trails =====

TEST(TrailRendererTest, PointsAgeAndExpireAfterTheTime)
{
    auto go = GameObject::Create("Trail");
    auto *trail = go->AddComponent<TrailRenderer>();
    trail->SetTime(1.0f);
    trail->SetMinVertexDistance(0.1f);

    trail->Step(Vector3(0.0f, 0.0f, 0.0f), 0.0f);
    trail->Step(Vector3(1.0f, 0.0f, 0.0f), 0.4f);
    trail->Step(Vector3(2.0f, 0.0f, 0.0f), 0.4f);
    ASSERT_EQ(trail->GetPointCount(), 3u);
    EXPECT_FLOAT_EQ(trail->GetPoints()[0].age, 0.8f);
    EXPECT_FLOAT_EQ(trail->GetPoints()[2].age, 0.0f);

    trail->Step(Vector3(3.0f, 0.0f, 0.0f), 0.4f); // the first point is 1.2 s old: gone
    ASSERT_EQ(trail->GetPointCount(), 3u);
    EXPECT_FLOAT_EQ(trail->GetPoints()[0].position.x, 1.0f);

    // Closer than the minimum distance: no new point, but the head moves
    trail->Step(Vector3(3.05f, 0.0f, 0.0f), 0.1f);
    EXPECT_EQ(trail->GetPointCount(), 3u);
    const auto line = trail->GetLinePoints();
    ASSERT_EQ(line.size(), 4u);
    EXPECT_FLOAT_EQ(line.back().position.x, 3.05f);

    // Standing still for longer than the time: everything expires but the newest point, which the head sits on
    for (int i = 0; i < 20; ++i)
    {
        trail->Step(Vector3(3.05f, 0.0f, 0.0f), 0.1f);
    }
    EXPECT_LE(trail->GetPointCount(), 1u);
    EXPECT_EQ(trail->GetMesh(), nullptr);
}

TEST(TrailRendererTest, WidthAndColourFadeByAgeFromTheHead)
{
    auto go = GameObject::Create("Trail");
    auto *trail = go->AddComponent<TrailRenderer>();
    trail->SetTime(2.0f);
    trail->SetStartWidth(1.0f);
    trail->SetEndWidth(0.0f);
    trail->SetStartColor(Common::Color(1.0f, 1.0f, 1.0f, 1.0f));
    trail->SetEndColor(Common::Color(1.0f, 1.0f, 1.0f, 0.0f));
    trail->Step(Vector3(0.0f, 0.0f, 0.0f), 0.0f);
    trail->Step(Vector3(1.0f, 0.0f, 0.0f), 1.0f);
    const auto line = trail->GetLinePoints();
    ASSERT_EQ(line.size(), 3u); // two points and the head (on the newest)
    EXPECT_FLOAT_EQ(line[0].width, 0.5f);
    EXPECT_FLOAT_EQ(line[0].color.a, 0.5f);
    EXPECT_FLOAT_EQ(line[2].width, 1.0f);
    EXPECT_FLOAT_EQ(line[2].color.a, 1.0f);
    ASSERT_NE(trail->GetMesh(), nullptr);
}

TEST(TrailRendererTest, ClearForgetsEveryPointAndNotEmittingAddsNone)
{
    auto go = GameObject::Create("Trail");
    auto *trail = go->AddComponent<TrailRenderer>();
    trail->Step(Vector3(0.0f, 0.0f, 0.0f), 0.0f);
    trail->Step(Vector3(1.0f, 0.0f, 0.0f), 0.1f);
    ASSERT_EQ(trail->GetPointCount(), 2u);
    trail->Clear(); // a teleport
    EXPECT_EQ(trail->GetPointCount(), 0u);
    EXPECT_TRUE(trail->GetLinePoints().empty());
    trail->Step(Vector3(50.0f, 0.0f, 0.0f), 0.1f);
    ASSERT_EQ(trail->GetPointCount(), 1u);
    EXPECT_FLOAT_EQ(trail->GetPoints()[0].position.x, 50.0f);

    trail->SetEmitting(false);
    trail->Step(Vector3(60.0f, 0.0f, 0.0f), 0.1f);
    EXPECT_EQ(trail->GetPointCount(), 1u);
    EXPECT_EQ(trail->GetLinePoints().size(), 1u); // no head while not emitting

    // A time of zero keeps nothing
    trail->SetEmitting(true);
    trail->SetTime(0.0f);
    trail->Step(Vector3(70.0f, 0.0f, 0.0f), 0.1f);
    EXPECT_EQ(trail->GetPointCount(), 0u);
}

// ===== Drawing (software renderer readback) =====

TEST(LineRendererTest, AlwaysTheTransparentQueueWithItsSortingOrder)
{
    auto go = GameObject::Create("Line");
    auto *line = go->AddComponent<LineRenderer>();
    line->SetSortingOrder(7);
    EXPECT_EQ(line->GetRenderQueue().queue, RenderQueue::Transparent);
    EXPECT_EQ(line->GetRenderQueue().sortKey, 7);
}

TEST(LineRendererTest, ALineDrawsItsWidthInItsColour)
{
    const Frame frame = Draw([](Scene &scene) {
        auto *line = AddTo<LineRenderer>(scene, "Line");
        const std::vector<Vector3> points = {Vector3(-1.5f, 0.0f, 0.0f), Vector3(1.5f, 0.0f, 0.0f)};
        ASSERT_TRUE(line->SetPositions(points));
        line->SetWidth(0.5f); // 8 pixels
        line->SetColor(Common::Color::Red);
    });
    EXPECT_TRUE(IsRed(frame.At(32, 32)));
    EXPECT_TRUE(IsRed(frame.At(12, 34)));
    EXPECT_TRUE(IsBlack(frame.At(32, 40))) << "half a unit above the line";
    EXPECT_TRUE(IsBlack(frame.At(4, 32))) << "past the end";
}

TEST(LineRendererTest, LocalPointsMoveWithTheObjectAndWorldPointsDoNot)
{
    for (const bool world : {false, true})
    {
        const Frame frame = Draw([world](Scene &scene) {
            auto *line = AddTo<LineRenderer>(scene, "Line", Vector3(0.0f, 1.0f, 0.0f));
            const std::vector<Vector3> points = {Vector3(-1.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f)};
            line->SetPositions(points);
            line->SetWidth(0.25f);
            line->SetColor(Common::Color::Green);
            line->SetUseWorldSpace(world);
        });
        EXPECT_EQ(IsGreen(frame.At(32, 48)), !world) << "world " << world;
        EXPECT_EQ(IsGreen(frame.At(32, 32)), world) << "world " << world;
    }
}

TEST(LineRendererTest, AClosedSquareLeavesItsMiddleEmpty)
{
    const Frame frame = Draw([](Scene &scene) {
        auto *line = AddTo<LineRenderer>(scene, "Square");
        const std::vector<Vector3> points = {Vector3(-1.0f, -1.0f, 0.0f), Vector3(1.0f, -1.0f, 0.0f), Vector3(1.0f, 1.0f, 0.0f),
                                             Vector3(-1.0f, 1.0f, 0.0f)};
        line->SetPositions(points);
        line->SetLoop(true);
        line->SetWidth(0.25f);
        line->SetColor(Common::Color::Red);
        line->SetBlendMode(Blend2D::Additive);
    });
    EXPECT_TRUE(IsBlack(frame.At(32, 32)));
    EXPECT_TRUE(IsRed(frame.At(32, 16))) << "bottom edge";
    EXPECT_TRUE(IsRed(frame.At(16, 32))) << "the closing edge, left";
    EXPECT_TRUE(IsRed(frame.At(16, 16))) << "a mitred corner";
}

TEST(LineRendererTest, TheSoftwareRendererCutsOutWhatIsUnderHalfFaded)
{
    const Frame frame = Draw([](Scene &scene) {
        auto *line = AddTo<LineRenderer>(scene, "Fade");
        const std::vector<Vector3> points = {Vector3(-1.5f, 0.0f, 0.0f), Vector3(1.5f, 0.0f, 0.0f)};
        line->SetPositions(points);
        line->SetWidth(0.5f);
        line->SetStartColor(Common::Color(1.0f, 0.0f, 0.0f, 1.0f));
        line->SetEndColor(Common::Color(1.0f, 0.0f, 0.0f, 0.0f));
    });
    EXPECT_TRUE(IsRed(frame.At(16, 32))) << "five sixths opaque";
    EXPECT_TRUE(IsBlack(frame.At(48, 32))) << "one sixth opaque";
}

TEST(TrailRendererTest, ATrailDrawsWhereTheObjectHasBeen)
{
    const Frame frame = Draw([](Scene &scene) {
        auto *trail = AddTo<TrailRenderer>(scene, "Trail");
        trail->SetTime(10.0f);
        trail->SetWidth(0.5f);
        trail->SetColor(Common::Color::Green);
        trail->Step(Vector3(-1.5f, -1.0f, 0.0f), 0.0f);
        trail->Step(Vector3(0.0f, 0.0f, 0.0f), 0.1f);
        trail->Step(Vector3(1.5f, 0.0f, 0.0f), 0.1f);
    });
    EXPECT_TRUE(IsGreen(frame.At(40, 32)));
    EXPECT_TRUE(IsGreen(frame.At(20, 24)));
    EXPECT_TRUE(IsBlack(frame.At(32, 50)));
}

TEST(MeshRenderer2DTest, AConcaveMeshDrawsWithItsNotchEmpty)
{
    const Frame frame = Draw([](Scene &scene) {
        // An L from -1.5 to 1.5: the upper-right quarter is the notch
        const std::vector<Vector2> outline = {{-1.5f, -1.5f}, {1.5f, -1.5f}, {1.5f, 0.0f}, {0.0f, 0.0f}, {0.0f, 1.5f}, {-1.5f, 1.5f}};
        ScriptMeshDesc desc;
        for (const Vector2 &p : outline)
        {
            desc.positions.emplace_back(p.x, p.y, 0.0f);
        }
        const auto indices = TriangulatePolygon2D(outline);
        desc.indices.assign(indices.begin(), indices.end());
        const auto data = MakeScriptMeshData(desc);
        ASSERT_TRUE(data.has_value());
        auto *renderer = AddTo<MeshRenderer2D>(scene, "L");
        renderer->SetMesh(Mesh::Create(*data));
        renderer->SetColor(Common::Color::Red);
    });
    EXPECT_TRUE(IsRed(frame.At(16, 16)));
    EXPECT_TRUE(IsRed(frame.At(16, 48)));
    EXPECT_TRUE(IsRed(frame.At(48, 16)));
    EXPECT_TRUE(IsBlack(frame.At(48, 48))) << "the notch";
}

TEST(MeshRenderer2DTest, VertexColoursAreDrawn)
{
    const Frame frame = Draw([](Scene &scene) {
        ScriptMeshDesc desc;
        desc.positions = {Vector3(-1.0f, -1.0f, 0.0f), Vector3(1.0f, -1.0f, 0.0f), Vector3(1.0f, 1.0f, 0.0f), Vector3(-1.0f, 1.0f, 0.0f)};
        desc.colors.assign(4, Common::Color::Green);
        desc.indices = {0, 1, 2, 0, 2, 3};
        const auto data = MakeScriptMeshData(desc);
        ASSERT_TRUE(data.has_value());
        AddTo<MeshRenderer2D>(scene, "Square")->SetMesh(Mesh::Create(*data));
    });
    EXPECT_TRUE(IsGreen(frame.At(32, 32)));
    EXPECT_TRUE(IsBlack(frame.At(4, 4)));
}

// ===== Saving =====

TEST(LineRendererTest, EveryFieldSurvivesASceneLoad)
{
    const auto go = GameObject::Create("Lines");
    go->CreatePositionable();
    auto *line = go->AddComponent<LineRenderer>();
    const std::vector<Vector3> points = {Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 2.0f, 3.0f)};
    line->SetPositions(points);
    line->SetStartWidth(0.25f);
    line->SetEndWidth(0.75f);
    line->SetStartColor(Common::Color::Red);
    line->SetEndColor(Common::Color::Blue);
    line->SetLoop(true);
    line->SetUseWorldSpace(true);
    line->SetJoin(LineJoin::Bevel);
    line->SetMiterLimit(3.0f);
    line->SetCap(LineCap::Square);
    line->SetBlendMode(Blend2D::Additive);
    line->SetSortingOrder(-2);
    auto *trail = go->AddComponent<TrailRenderer>();
    trail->SetTime(0.5f);
    trail->SetMinVertexDistance(0.25f);
    trail->SetEmitting(false);
    trail->SetEndWidth(0.125f);
    trail->SetJoin(LineJoin::Bevel);
    auto *mesh2D = go->AddComponent<MeshRenderer2D>();
    mesh2D->SetColor(Common::Color::Green);
    mesh2D->SetSortingOrder(5);

    const auto scene = Scene::Create("LinesRoundTrip");
    scene->AddRootGameObject(go);
    const auto loaded = Scene::FromJSON(scene->Serialize());
    ASSERT_NE(loaded, nullptr);
    const auto back = loaded->FindGameObject("Lines");
    ASSERT_NE(back, nullptr);
    const auto *lineCopy = back->GetComponent<LineRenderer>();
    ASSERT_NE(lineCopy, nullptr);
    ASSERT_EQ(lineCopy->GetPositionCount(), 2u);
    EXPECT_FLOAT_EQ(lineCopy->GetPositions()[1].z, 3.0f);
    EXPECT_FLOAT_EQ(lineCopy->GetStartWidth(), 0.25f);
    EXPECT_FLOAT_EQ(lineCopy->GetEndWidth(), 0.75f);
    EXPECT_GT(lineCopy->GetStartColor().r, 0.9f);
    EXPECT_GT(lineCopy->GetEndColor().b, 0.9f);
    EXPECT_TRUE(lineCopy->GetLoop());
    EXPECT_TRUE(lineCopy->GetUseWorldSpace());
    EXPECT_EQ(lineCopy->GetJoin(), LineJoin::Bevel);
    EXPECT_FLOAT_EQ(lineCopy->GetMiterLimit(), 3.0f);
    EXPECT_EQ(lineCopy->GetCap(), LineCap::Square);
    EXPECT_EQ(lineCopy->GetBlendMode(), Blend2D::Additive);
    EXPECT_EQ(lineCopy->GetSortingOrder(), -2);
    const auto *trailCopy = back->GetComponent<TrailRenderer>();
    ASSERT_NE(trailCopy, nullptr);
    EXPECT_FLOAT_EQ(trailCopy->GetTime(), 0.5f);
    EXPECT_FLOAT_EQ(trailCopy->GetMinVertexDistance(), 0.25f);
    EXPECT_FALSE(trailCopy->IsEmitting());
    EXPECT_FLOAT_EQ(trailCopy->GetEndWidth(), 0.125f);
    EXPECT_EQ(trailCopy->GetJoin(), LineJoin::Bevel);
    const auto *meshCopy = back->GetComponent<MeshRenderer2D>();
    ASSERT_NE(meshCopy, nullptr);
    EXPECT_GT(meshCopy->GetColor().g, 0.9f);
    EXPECT_EQ(meshCopy->GetSortingOrder(), 5);
}

TEST(LineRendererTest, TheInspectorSeesEveryFieldAndTheEnumOptions)
{
    auto &registry = ComponentRegistry::Instance();
    for (const char *name : {"LineRenderer", "TrailRenderer", "MeshRenderer2D"})
    {
        ASSERT_TRUE(registry.IsRegistered(name)) << name;
        const auto schema = registry.Describe(name);
        ASSERT_TRUE(schema.has_value()) << name;
        std::vector<std::string> saved;
        for (const auto &[key, value] : schema->defaults.items())
        {
            if (key != "isActive")
            {
                saved.push_back(key);
            }
        }
        std::vector<std::string> described;
        for (const FieldInfo &field : schema->fields)
        {
            described.push_back(field.name);
        }
        std::ranges::sort(saved);
        std::ranges::sort(described);
        EXPECT_EQ(described, saved) << name;
    }
    const auto line = registry.Describe("LineRenderer").value();
    bool sawJoin = false;
    for (const FieldInfo &field : line.fields)
    {
        if (field.name == "_join")
        {
            sawJoin = true;
            EXPECT_EQ(field.kind, FieldKind::Enum);
            EXPECT_EQ(field.enumOptions, (std::vector<std::string>{"Miter", "Bevel"}));
        }
    }
    EXPECT_TRUE(sawJoin);
}
