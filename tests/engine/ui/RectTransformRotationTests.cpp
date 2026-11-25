#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/QuadClip.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectMask.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/ScrollView.hpp"
#include "engine/ui/Slider.hpp"
#include "engine/ui/Transform2D.hpp"
#include "engine/ui/UIGraphic.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

// Issue #445: RectTransform rotation and scale: the transform maths, what the layout resolves, the hit test, masks that
// are turned against what they clip, the cut meshes, a software-rendered readback, and the Slider and ScrollView read
// along their own axes

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;

namespace
{
    constexpr float Tolerance = 1e-3f;
    const Vector2i Viewport{800, 600};

    void ExpectPoint(const Vector2 &actual, const float x, const float y, const char *what = "")
    {
        EXPECT_NEAR(actual.x, x, Tolerance) << what << " x";
        EXPECT_NEAR(actual.y, y, Tolerance) << what << " y";
    }

    void ExpectRect(const Rect &actual, const Rect &expected, const char *what = "")
    {
        EXPECT_NEAR(actual.x, expected.x, Tolerance) << what << " x";
        EXPECT_NEAR(actual.y, expected.y, Tolerance) << what << " y";
        EXPECT_NEAR(actual.width, expected.width, Tolerance) << what << " width";
        EXPECT_NEAR(actual.height, expected.height, Tolerance) << what << " height";
    }

    /// A child element at a fixed rect inside its parent (bottom-left anchors, the pivot at the rect's centre so a turn
    /// stays about the middle), with an Image unless `image` is false
    GameObject::Ptr AddPanel(const GameObject::Ptr &parent, const std::string &name, const Rect &local, const bool image = true)
    {
        auto element = UISystem::CreateElement(name);
        auto *rectTransform = element->GetComponent<RectTransform>();
        rectTransform->SetAnchorMin(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchorMax(Vector2{0.0f, 0.0f});
        rectTransform->SetPivot(Vector2{0.5f, 0.5f});
        rectTransform->SetAnchoredPosition(Vector2{local.x + local.width * 0.5f, local.y + local.height * 0.5f});
        rectTransform->SetSizeDelta(Vector2{local.width, local.height});
        if (image)
        {
            element->AddComponent<Image>();
        }
        parent->AddChild(element, false);
        return element;
    }

    RectTransform &TransformOf(const GameObject::Ptr &object) { return *object->GetComponent<RectTransform>(); }

    const UIDrawItem *ItemOf(const std::vector<UIDrawItem> &items, const GameObject &gameObject)
    {
        for (const UIDrawItem &item : items)
        {
            if (&item.graphic->GetGameObject() == &gameObject)
            {
                return &item;
            }
        }
        return nullptr;
    }

    /// Records what UISystem hands a graphic when it draws it
    class ProbeGraphic final : public UIGraphic
    {
    public:
        explicit ProbeGraphic(GameObject &gameObject) : UIGraphic(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "ProbeGraphic"; }
        void RenderUI(Renderer::Common::IRenderer *, const Rect &rect, const Renderer::Common::RenderState &,
                      const Matrix4 &canvasToWorld) override
        {
            ++draws;
            drawnRect = rect;
            drawnClip = GetRenderClip();
            drawnPolygon = GetRenderClipPolygon();
            model = canvasToWorld;
        }
        static constexpr bool IsSingleton = false;

        int draws = 0;
        Rect drawnRect{};
        std::optional<Rect> drawnClip;
        ConvexPolygon drawnPolygon;
        Matrix4 model = Matrix4::identity();
    };

    /// Renders the scene's UI with the software renderer and returns the red channel, rows top down
    std::vector<std::uint8_t> RenderRed(Scene &scene, const int width, const int height)
    {
        Renderer::Software::SoftwareRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, static_cast<uint32_t>(width), static_cast<uint32_t>(height)));
        renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
        renderer.BeginFrame();
        UISystem::Render(scene, &renderer, Vector2i{width, height});
        renderer.EndFrame();
        renderer.Present();
        renderer.ReadFramebuffer(rgba.data(), width, height);
        std::vector<std::uint8_t> red(static_cast<std::size_t>(width) * height);
        for (std::size_t i = 0; i < red.size(); ++i)
        {
            red[i] = rgba[i * 4];
        }
        renderer.Shutdown();
        return red;
    }
}

// ============================================================================
// Transform2D
// ============================================================================

TEST(Transform2DTest, AroundAPivotKeepsThePivotAndTurnsCounterClockwise)
{
    EXPECT_TRUE(Transform2D::AroundPivot(Vector2{5.0f, 5.0f}, 0.0f, Vector2{1.0f, 1.0f}).IsIdentity());
    EXPECT_TRUE(Transform2D::AroundPivot(Vector2{5.0f, 5.0f}, 360.0f, Vector2{1.0f, 1.0f}).IsIdentity());

    const Transform2D quarter = Transform2D::AroundPivot(Vector2{10.0f, 20.0f}, 90.0f, Vector2{1.0f, 1.0f});
    EXPECT_EQ(quarter.a, 0.0f) << "a quarter turn is exact";
    EXPECT_EQ(quarter.b, 1.0f);
    ExpectPoint(quarter.Apply(Vector2{10.0f, 20.0f}), 10.0f, 20.0f, "pivot");
    ExpectPoint(quarter.Apply(Vector2{11.0f, 20.0f}), 10.0f, 21.0f, "+x goes to +y");
    EXPECT_EQ(Transform2D::AroundPivot(Vector2{0.0f, 0.0f}, -90.0f, Vector2{1.0f, 1.0f}).b, -1.0f);
    EXPECT_EQ(Transform2D::AroundPivot(Vector2{0.0f, 0.0f}, 450.0f, Vector2{1.0f, 1.0f}).b, 1.0f);

    // Scale first, then the turn
    const Transform2D both = Transform2D::AroundPivot(Vector2{0.0f, 0.0f}, 90.0f, Vector2{2.0f, 3.0f});
    ExpectPoint(both.Apply(Vector2{1.0f, 0.0f}), 0.0f, 2.0f);
    ExpectPoint(both.Apply(Vector2{0.0f, 1.0f}), -3.0f, 0.0f);

    // Non-finite values count as none
    EXPECT_TRUE(Transform2D::AroundPivot(Vector2{0.0f, 0.0f}, std::nanf(""), Vector2{std::nanf(""), 1.0f}).IsIdentity());
}

TEST(Transform2DTest, ComposesAndInverts)
{
    const Transform2D outer = Transform2D::AroundPivot(Vector2{3.0f, -2.0f}, 33.0f, Vector2{1.5f, 0.5f});
    const Transform2D inner = Transform2D::AroundPivot(Vector2{-7.0f, 4.0f}, -71.0f, Vector2{-2.0f, 1.0f});
    const Vector2 point{12.0f, 8.0f};
    const Vector2 expected = outer.Apply(inner.Apply(point));
    ExpectPoint((outer * inner).Apply(point), expected.x, expected.y, "inner first");
    const std::optional<Transform2D> inverse = (outer * inner).Inverse();
    ASSERT_TRUE(inverse.has_value());
    ExpectPoint(inverse->Apply(expected), point.x, point.y, "round trip");
    EXPECT_FALSE(Transform2D::AroundPivot(Vector2{0.0f, 0.0f}, 20.0f, Vector2{0.0f, 1.0f}).Inverse().has_value());
}

TEST(Transform2DTest, ConvexPolygonsIntersectAndContain)
{
    const ConvexPolygon square = TransformedCorners(Rect{0.0f, 0.0f, 10.0f, 10.0f}, Transform2D{});
    const ConvexPolygon moved = TransformedCorners(Rect{5.0f, 5.0f, 10.0f, 10.0f}, Transform2D{});
    const ConvexPolygon overlap = IntersectConvex(square, moved);
    EXPECT_NEAR(PolygonArea(overlap), 25.0f, Tolerance);
    const std::optional<Rect> aligned = AsAxisAlignedRect(overlap);
    ASSERT_TRUE(aligned.has_value());
    ExpectRect(*aligned, Rect{5.0f, 5.0f, 5.0f, 5.0f});
    EXPECT_TRUE(IntersectConvex(square, TransformedCorners(Rect{20.0f, 0.0f, 5.0f, 5.0f}, Transform2D{})).empty());
    EXPECT_TRUE(IntersectConvex(square, TransformedCorners(Rect{10.0f, 0.0f, 5.0f, 5.0f}, Transform2D{})).empty())
        << "touching edges are no overlap";

    // A mirrored rect's corners are still counter-clockwise
    const ConvexPolygon mirrored = TransformedCorners(
        Rect{0.0f, 0.0f, 10.0f, 10.0f}, Transform2D::AroundPivot(Vector2{0.0f, 0.0f}, 0.0f, Vector2{-1.0f, 1.0f}));
    EXPECT_GT(PolygonArea(mirrored), 0.0f);
    EXPECT_TRUE(ConvexContains(mirrored, Vector2{-5.0f, 5.0f}));
    EXPECT_FALSE(ConvexContains(mirrored, Vector2{5.0f, 5.0f}));

    const ConvexPolygon diamond = TransformedCorners(
        Rect{-10.0f, -10.0f, 20.0f, 20.0f}, Transform2D::AroundPivot(Vector2{0.0f, 0.0f}, 45.0f, Vector2{1.0f, 1.0f}));
    EXPECT_FALSE(AsAxisAlignedRect(diamond).has_value());
    EXPECT_TRUE(ConvexContains(diamond, Vector2{0.0f, 13.0f}));
    EXPECT_FALSE(ConvexContains(diamond, Vector2{9.0f, 9.0f}));
}

// ============================================================================
// Layout
// ============================================================================

TEST(RectTransformRotationTest, TheRectStaysAndTheCornersTurn)
{
    const auto scene = Scene::Create("RectRotation_Layout");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto panel = AddPanel(canvas, "Panel", Rect{100.0f, 100.0f, 100.0f, 40.0f});
    const auto child = AddPanel(panel, "Child", Rect{0.0f, 0.0f, 10.0f, 10.0f});
    TransformOf(panel).SetRotation(90.0f);

    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Viewport);
    ExpectRect(TransformOf(panel).GetRect(), Rect{100.0f, 100.0f, 100.0f, 40.0f}, "the layout is not turned");
    ExpectRect(TransformOf(child).GetRect(), Rect{100.0f, 100.0f, 10.0f, 10.0f}, "nor the child's");

    // About the centre (150, 120): the bottom-left corner goes to the bottom-right of the turned shape
    const auto corners = TransformOf(panel).GetCanvasCorners();
    ExpectPoint(corners[0], 170.0f, 70.0f, "bottom-left");
    ExpectPoint(corners[1], 130.0f, 70.0f, "top-left");
    ExpectPoint(corners[2], 130.0f, 170.0f, "top-right");
    ExpectPoint(corners[3], 170.0f, 170.0f, "bottom-right");

    // The child turns with it
    ExpectPoint(TransformOf(child).LocalToCanvas(Vector2{100.0f, 100.0f}), 170.0f, 70.0f, "child");
    const UIDrawItem *item = ItemOf(items, *child);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->transform, TransformOf(panel).GetLocalToCanvas());
    const std::optional<Vector2> back = TransformOf(child).CanvasToLocal(Vector2{170.0f, 70.0f});
    ASSERT_TRUE(back.has_value());
    ExpectPoint(*back, 100.0f, 100.0f);

    // Turning it back is exactly the unturned layout again
    TransformOf(panel).SetRotation(0.0f);
    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));
    EXPECT_TRUE(TransformOf(child).GetLocalToCanvas().IsIdentity());
}

TEST(RectTransformRotationTest, ScaleAndRotationComposeDownTheHierarchy)
{
    const auto scene = Scene::Create("RectRotation_Compose");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto parent = AddPanel(canvas, "Parent", Rect{100.0f, 100.0f, 100.0f, 100.0f});
    const auto child = AddPanel(parent, "Child", Rect{60.0f, 40.0f, 20.0f, 20.0f}); // centre (170, 150)
    TransformOf(parent).SetScale(Vector2{2.0f, 2.0f});
    TransformOf(child).SetRotation(45.0f);

    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));
    // The child's pivot: 20 right of the parent's centre (150, 150), doubled
    ExpectPoint(TransformOf(child).LocalToCanvas(Vector2{170.0f, 150.0f}), 190.0f, 150.0f);
    // Its right edge's middle, turned by 45 degrees (10 units out) and doubled
    const float diagonal = 20.0f * std::sqrt(0.5f);
    ExpectPoint(TransformOf(child).LocalToCanvas(Vector2{180.0f, 150.0f}), 190.0f + diagonal, 150.0f + diagonal);
}

TEST(RectTransformRotationTest, TheCanvasObjectsOwnRotationIsNotUsed)
{
    const auto scene = Scene::Create("RectRotation_Canvas");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    auto *rectTransform = canvas->AddComponent<RectTransform>();
    rectTransform->SetRotation(45.0f);
    rectTransform->SetScale(Vector2{3.0f, 3.0f});
    const auto panel = AddPanel(canvas, "Panel", Rect{0.0f, 0.0f, 10.0f, 10.0f});
    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Viewport);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_TRUE(items[0].transform.IsIdentity());
    EXPECT_TRUE(rectTransform->GetLocalToCanvas().IsIdentity());
    static_cast<void>(panel);
}

TEST(RectTransformRotationTest, RotationAndScaleAreSavedAndOldDataKeepsTheDefaults)
{
    const auto element = UISystem::CreateElement("Saved");
    TransformOf(element).SetRotation(30.0f);
    TransformOf(element).SetScale(Vector2{2.0f, -1.0f});
    const nlohmann::json saved = TransformOf(element).Serialize();

    const auto loaded = UISystem::CreateElement("Loaded");
    TransformOf(loaded).Deserialize(saved);
    EXPECT_FLOAT_EQ(TransformOf(loaded).GetRotation(), 30.0f);
    ExpectPoint(TransformOf(loaded).GetScale(), 2.0f, -1.0f);

    // A scene saved before rotation and scale existed
    nlohmann::json old = saved;
    for (const char *key : {"rotation", "scale"})
    {
        old.erase(key);
        if (old.contains("members"))
        {
            old["members"].erase(key);
        }
    }
    const auto fromOld = UISystem::CreateElement("Old");
    TransformOf(fromOld).Deserialize(old);
    EXPECT_FLOAT_EQ(TransformOf(fromOld).GetRotation(), 0.0f);
    ExpectPoint(TransformOf(fromOld).GetScale(), 1.0f, 1.0f);
}

// ============================================================================
// Hit testing
// ============================================================================

TEST(RectTransformRotationTest, TheHitTestFollowsTheTurnedShape)
{
    const auto scene = Scene::Create("RectRotation_Hit");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto square = AddPanel(canvas, "Square", Rect{100.0f, 100.0f, 100.0f, 100.0f}); // centre (150, 150)
    TransformOf(square).SetRotation(45.0f);

    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{150.0f, 215.0f}, Viewport), square.get()) << "a tip of the diamond";
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{105.0f, 105.0f}, Viewport), nullptr)
        << "a corner of the unturned rect";

    TransformOf(square).SetRotation(0.0f);
    TransformOf(square).SetScale(Vector2{2.0f, 1.0f});
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{245.0f, 150.0f}, Viewport), square.get()) << "scaled wider";
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{150.0f, 205.0f}, Viewport), nullptr) << "not taller";

    TransformOf(square).SetScale(Vector2{0.0f, 1.0f});
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{150.0f, 150.0f}, Viewport), nullptr) << "flattened: never hit";
    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Viewport);
    EXPECT_FALSE(ItemOf(items, *square)->IsVisible());
}

TEST(RectTransformRotationTest, AMirroringScaleMovesTheChildrenAcross)
{
    const auto scene = Scene::Create("RectRotation_Mirror");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto parent = AddPanel(canvas, "Parent", Rect{100.0f, 100.0f, 100.0f, 100.0f}, false);
    const auto left = AddPanel(parent, "Left", Rect{0.0f, 0.0f, 20.0f, 100.0f});
    TransformOf(parent).SetScale(Vector2{-1.0f, 1.0f});
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{190.0f, 150.0f}, Viewport), left.get());
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{110.0f, 150.0f}, Viewport), nullptr);
}

// ============================================================================
// Masks
// ============================================================================

TEST(RectTransformRotationTest, ATurnedMaskClipsToItsShape)
{
    const auto scene = Scene::Create("RectRotation_Mask");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto mask = AddPanel(canvas, "Mask", Rect{100.0f, 100.0f, 100.0f, 100.0f}, false);
    mask->AddComponent<RectMask>();
    TransformOf(mask).SetRotation(45.0f);
    // The child turns back, so it covers a large square along the axes
    const auto child = AddPanel(mask, "Child", Rect{-50.0f, -50.0f, 200.0f, 200.0f});
    TransformOf(child).SetRotation(-45.0f);
    auto *probe = child->AddComponent<ProbeGraphic>();

    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Viewport);
    const UIDrawItem *item = ItemOf(items, *child);
    ASSERT_NE(item, nullptr);
    ASSERT_EQ(item->clipPolygon.size(), 4u) << "a turned mask is a polygon";
    ASSERT_TRUE(item->clip.has_value());
    const float reach = 100.0f * std::sqrt(0.5f);
    ExpectRect(*item->clip, Rect{150.0f - reach, 150.0f - reach, 2.0f * reach, 2.0f * reach}, "its bounds");
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{150.0f, 215.0f}, Viewport), child.get()) << "inside the diamond";
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{205.0f, 205.0f}, Viewport), nullptr) << "inside the box, outside it";

    // The graphic is handed the clip in its own space: here its space is the canvas's again, so the same diamond
    Renderer::Software::SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, 64, 64));
    renderer.BeginFrame();
    UISystem::Render(*scene, &renderer, Viewport);
    renderer.EndFrame();
    renderer.Shutdown();
    ASSERT_EQ(probe->draws, 1);
    ASSERT_EQ(probe->drawnPolygon.size(), 4u);
    EXPECT_NEAR(PolygonArea(probe->drawnPolygon), 10000.0f, 1.0f);
    EXPECT_TRUE(probe->GetRenderClipPolygon().empty()) << "cleared after the draw";
}

TEST(RectTransformRotationTest, AChildTurnedWithItsMaskGetsARectClip)
{
    const auto scene = Scene::Create("RectRotation_SharedTurn");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto mask = AddPanel(canvas, "Mask", Rect{100.0f, 100.0f, 100.0f, 100.0f}, false);
    mask->AddComponent<RectMask>();
    TransformOf(mask).SetRotation(30.0f);
    const auto child = AddPanel(mask, "Child", Rect{50.0f, 0.0f, 100.0f, 100.0f}, false);
    auto *probe = child->AddComponent<ProbeGraphic>();

    Renderer::Software::SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, 64, 64));
    renderer.BeginFrame();
    UISystem::Render(*scene, &renderer, Viewport);
    renderer.EndFrame();
    renderer.Shutdown();

    ASSERT_EQ(probe->draws, 1);
    EXPECT_TRUE(probe->drawnPolygon.empty()) << "no turn between them: still a rect, in the child's space";
    ASSERT_TRUE(probe->drawnClip.has_value());
    EXPECT_NEAR(probe->drawnClip->x, 100.0f, 0.01f);
    EXPECT_NEAR(probe->drawnClip->width, 100.0f, 0.01f);
    // The model matrix carries the turn
    EXPECT_NEAR(probe->model(1, 0), 0.5f, Tolerance) << "sin 30";
}

TEST(RectTransformRotationTest, AQuarterTurnedMaskStaysARect)
{
    const auto scene = Scene::Create("RectRotation_Quarter");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto mask = AddPanel(canvas, "Mask", Rect{100.0f, 100.0f, 100.0f, 40.0f}, false);
    mask->AddComponent<RectMask>();
    TransformOf(mask).SetRotation(90.0f);
    const auto child = AddPanel(mask, "Child", Rect{0.0f, 0.0f, 100.0f, 40.0f});
    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Viewport);
    const UIDrawItem *item = ItemOf(items, *child);
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->clipPolygon.empty());
    ASSERT_TRUE(item->clip.has_value());
    ExpectRect(*item->clip, Rect{130.0f, 70.0f, 40.0f, 100.0f});
}

TEST(RectTransformRotationTest, NestedTurnedMasksThatMissShowNothing)
{
    const auto scene = Scene::Create("RectRotation_Miss");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto outer = AddPanel(canvas, "Outer", Rect{0.0f, 0.0f, 100.0f, 100.0f}, false);
    outer->AddComponent<RectMask>();
    TransformOf(outer).SetRotation(10.0f);
    const auto inner = AddPanel(outer, "Inner", Rect{300.0f, 300.0f, 100.0f, 100.0f}, false);
    inner->AddComponent<RectMask>();
    const auto leaf = AddPanel(inner, "Leaf", Rect{0.0f, 0.0f, 100.0f, 100.0f});
    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Viewport);
    const UIDrawItem *item = ItemOf(items, *leaf);
    ASSERT_NE(item, nullptr);
    EXPECT_FALSE(item->IsVisible());
    EXPECT_FALSE(item->Hits(Vector2{50.0f, 50.0f}));
}

// ============================================================================
// Cut meshes and drawing
// ============================================================================

TEST(RectTransformRotationTest, ACutQuadKeepsItsTextureWhereItWas)
{
    Renderer::Common::MeshData quad;
    const auto vertex = [](const float x, const float y)
    {
        return Renderer::Common::Vertex{{x, y, 0.0f}, {0.0f, 0.0f, 1.0f}, {x / 10.0f, y / 10.0f}, {1.0f, 1.0f, 1.0f, 1.0f}};
    };
    quad.vertices = {vertex(0.0f, 0.0f), vertex(10.0f, 0.0f), vertex(10.0f, 10.0f), vertex(0.0f, 10.0f)};
    quad.indices = {0, 1, 2, 0, 2, 3};

    // A triangle over the lower left half
    const ConvexPolygon clip{Vector2{0.0f, 0.0f}, Vector2{10.0f, 0.0f}, Vector2{0.0f, 10.0f}};
    const Renderer::Common::MeshData cut = ClipQuadMesh(quad, clip);
    ASSERT_GE(cut.vertices.size(), 3u);
    ASSERT_EQ(cut.indices.size() % 3, 0u);
    float area = 0.0f;
    for (std::size_t i = 0; i < cut.indices.size(); i += 3)
    {
        const auto &a = cut.vertices[cut.indices[i]];
        const auto &b = cut.vertices[cut.indices[i + 1]];
        const auto &c = cut.vertices[cut.indices[i + 2]];
        area += 0.5f * ((b.position[0] - a.position[0]) * (c.position[1] - a.position[1]) -
                        (b.position[1] - a.position[1]) * (c.position[0] - a.position[0]));
    }
    EXPECT_NEAR(area, 50.0f, Tolerance) << "half the quad, counter-clockwise";
    for (const auto &v : cut.vertices)
    {
        EXPECT_LE(v.position[0] + v.position[1], 10.0f + Tolerance);
        EXPECT_NEAR(v.texCoord[0], v.position[0] / 10.0f, Tolerance);
        EXPECT_NEAR(v.texCoord[1], v.position[1] / 10.0f, Tolerance);
    }

    EXPECT_TRUE(ClipQuadMesh(quad, TransformedCorners(Rect{20.0f, 20.0f, 5.0f, 5.0f}, Transform2D{})).vertices.empty());
    EXPECT_TRUE(ClipQuadMesh(quad, ConvexPolygon{}).vertices.empty());
}

TEST(RectTransformRotationTest, TheSoftwareRendererDrawsTheTurnedImage)
{
    constexpr int size = 40;
    const auto scene = Scene::Create("RectRotation_Software");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto square = AddPanel(canvas, "Square", Rect{10.0f, 10.0f, 20.0f, 20.0f}); // centre (20, 20)
    TransformOf(square).SetRotation(45.0f);

    const std::vector<std::uint8_t> red = RenderRed(*scene, size, size);
    // The diamond |x - 20| + |y - 20| <= 14.14, symmetric, so the row order does not matter
    int wrong = 0;
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const float distance = std::abs(static_cast<float>(x) + 0.5f - 20.0f) +
                                   std::abs(static_cast<float>(y) + 0.5f - 20.0f);
            if (std::abs(distance - 14.142f) < 1.5f)
            {
                continue; // the edge
            }
            wrong += (red[static_cast<std::size_t>(y) * size + x] > 128) == (distance < 14.142f) ? 0 : 1;
        }
    }
    EXPECT_EQ(wrong, 0);
}

TEST(RectTransformRotationTest, TheSoftwareRendererCutsAnImageToATurnedMask)
{
    constexpr int size = 40;
    const auto scene = Scene::Create("RectRotation_SoftwareMask");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto mask = AddPanel(canvas, "Mask", Rect{10.0f, 10.0f, 20.0f, 20.0f}, false);
    mask->AddComponent<RectMask>();
    TransformOf(mask).SetRotation(45.0f);
    // Covers the whole screen, turned back to the axes
    const auto white = AddPanel(mask, "White", Rect{-20.0f, -20.0f, 60.0f, 60.0f});
    TransformOf(white).SetRotation(-45.0f);

    const std::vector<std::uint8_t> red = RenderRed(*scene, size, size);
    int wrong = 0;
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const float distance = std::abs(static_cast<float>(x) + 0.5f - 20.0f) +
                                   std::abs(static_cast<float>(y) + 0.5f - 20.0f);
            if (std::abs(distance - 14.142f) < 1.5f)
            {
                continue;
            }
            wrong += (red[static_cast<std::size_t>(y) * size + x] > 128) == (distance < 14.142f) ? 0 : 1;
        }
    }
    EXPECT_EQ(wrong, 0) << "white inside the diamond only";
}

TEST(RectTransformRotationTest, TextUnderATurnedMaskDrawsOnlyInsideIt)
{
    constexpr int size = 80;
    const auto scene = Scene::Create("RectRotation_TextMask");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto mask = AddPanel(canvas, "Mask", Rect{20.0f, 20.0f, 40.0f, 40.0f}, false);
    mask->AddComponent<RectMask>();
    TransformOf(mask).SetRotation(45.0f);
    const auto label = AddPanel(mask, "Label", Rect{-20.0f, -20.0f, 80.0f, 80.0f}, false);
    TransformOf(label).SetRotation(-45.0f);
    auto *text = label->AddComponent<UIText>();
    text->SetText("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
    text->SetFontSize(18.0f);

    const std::vector<std::uint8_t> red = RenderRed(*scene, size, size);
    int inside = 0;
    int outside = 0;
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const float distance = std::abs(static_cast<float>(x) + 0.5f - 40.0f) +
                                   std::abs(static_cast<float>(y) + 0.5f - 40.0f);
            if (red[static_cast<std::size_t>(y) * size + x] > 128)
            {
                (distance < 28.3f + 1.5f ? inside : outside) += 1;
            }
        }
    }
    EXPECT_GT(inside, 40) << "glyphs are drawn inside the diamond";
    EXPECT_EQ(outside, 0) << "and none outside it";
}

// ============================================================================
// Controls read along their own axes
// ============================================================================

TEST(RectTransformRotationTest, ATurnedSliderIsReadAlongItsBar)
{
    const auto scene = Scene::Create("RectRotation_Slider");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto sliderObject = UISystem::CreateSlider("Slider"); // 160 x 20 in the middle: centre (400, 300)
    canvas->AddChild(sliderObject, false);
    TransformOf(sliderObject).SetRotation(90.0f); // left to right now runs bottom to top
    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));

    auto *slider = sliderObject->GetComponent<Slider>();
    slider->SetValueFromPointer(Vector2{400.0f, 300.0f + 40.0f});
    EXPECT_NEAR(slider->GetValue(), 0.75f, Tolerance);
    slider->SetValueFromPointer(Vector2{405.0f, 300.0f - 80.0f});
    EXPECT_NEAR(slider->GetValue(), 0.0f, Tolerance);
}

TEST(RectTransformRotationTest, ATurnedScrollViewDragsAlongItsOwnAxis)
{
    const auto scene = Scene::Create("RectRotation_Scroll");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto viewObject = UISystem::CreateScrollView("View"); // 300 x 200, vertical, content 400 high
    canvas->AddChild(viewObject, false);
    TransformOf(viewObject).SetRotation(90.0f); // its up is now the canvas's left
    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));

    auto *view = viewObject->GetComponent<ScrollView>();
    view->SetMovementType(ScrollMovementType::Clamped);
    const float start = view->GetContent()->GetAnchoredPosition().y;
    view->BeginDrag(Vector2{400.0f, 300.0f});
    view->Drag(Vector2{350.0f, 300.0f}); // 50 to the left: 50 up the view
    view->EndDrag();
    EXPECT_NEAR(view->GetContent()->GetAnchoredPosition().y, start + 50.0f, Tolerance);
}
