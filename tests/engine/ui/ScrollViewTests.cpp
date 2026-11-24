#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <text/TextLayout.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectMask.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/ScrollView.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

// Issue #445: RectMask clipping (the clip the layout works out, the clipped geometry of Image and UIText, the hit test
// through masks, a software-rendered readback) and the ScrollView (normalized position, wheel, drag, elastic spring back,
// inertia, clamping, scrollbars, OnValueChanged, a drag cancelling a button's press)

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;

namespace
{
    constexpr float Tolerance = 1e-3f;
    const Vector2i Viewport{800, 600};

    void ExpectRect(const Rect &actual, const Rect &expected, const char *what = "")
    {
        EXPECT_NEAR(actual.x, expected.x, Tolerance) << what << " x";
        EXPECT_NEAR(actual.y, expected.y, Tolerance) << what << " y";
        EXPECT_NEAR(actual.width, expected.width, Tolerance) << what << " width";
        EXPECT_NEAR(actual.height, expected.height, Tolerance) << what << " height";
    }

    /// A child element at a fixed rect inside its parent (bottom-left anchors and pivot), with an Image unless `image` is
    /// false
    GameObject::Ptr AddPanel(const GameObject::Ptr &parent, const std::string &name, const Rect &local, const bool image = true)
    {
        auto element = UISystem::CreateElement(name);
        auto *rectTransform = element->GetComponent<RectTransform>();
        rectTransform->SetAnchorMin(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchorMax(Vector2{0.0f, 0.0f});
        rectTransform->SetPivot(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchoredPosition(Vector2{local.x, local.y});
        rectTransform->SetSizeDelta(Vector2{local.width, local.height});
        if (image)
        {
            element->AddComponent<Image>();
        }
        parent->AddChild(element, false);
        return element;
    }

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
}

// ============================================================================
// Rect helpers
// ============================================================================

TEST(RectMaskTest, IntersectAndContainsRect)
{
    const Rect a{0.0f, 0.0f, 100.0f, 50.0f};
    ExpectRect(Rect::Intersect(a, Rect{50.0f, 25.0f, 100.0f, 100.0f}), Rect{50.0f, 25.0f, 50.0f, 25.0f});
    EXPECT_FALSE(Rect::Intersect(a, Rect{200.0f, 0.0f, 10.0f, 10.0f}).HasArea()) << "no overlap: no area";
    EXPECT_GE(Rect::Intersect(a, Rect{200.0f, 0.0f, 10.0f, 10.0f}).width, 0.0f) << "never a negative size";
    EXPECT_TRUE(a.ContainsRect(Rect{10.0f, 10.0f, 20.0f, 20.0f}));
    EXPECT_TRUE(a.ContainsRect(a)) << "edges included";
    EXPECT_FALSE(a.ContainsRect(Rect{90.0f, 10.0f, 20.0f, 20.0f}));
}

// ============================================================================
// The clip the layout works out
// ============================================================================

TEST(RectMaskTest, AMaskClipsItsDescendantsButNotItsOwnGraphic)
{
    const auto scene = Scene::Create("RectMask_Clip");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto masked = AddPanel(canvas, "Masked", Rect{100.0f, 100.0f, 100.0f, 100.0f});
    masked->AddComponent<RectMask>();
    const auto child = AddPanel(masked, "Child", Rect{50.0f, 50.0f, 100.0f, 100.0f});
    const auto grandchild = AddPanel(child, "Grandchild", Rect{0.0f, 0.0f, 10.0f, 10.0f});
    const auto outside = AddPanel(canvas, "Outside", Rect{0.0f, 0.0f, 10.0f, 10.0f});

    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Viewport);
    ASSERT_EQ(items.size(), 4u);
    EXPECT_FALSE(ItemOf(items, *masked)->clip.has_value()) << "the mask's own image is not clipped by it";
    ASSERT_TRUE(ItemOf(items, *child)->clip.has_value());
    ExpectRect(*ItemOf(items, *child)->clip, Rect{100.0f, 100.0f, 100.0f, 100.0f});
    ASSERT_TRUE(ItemOf(items, *grandchild)->clip.has_value()) << "the whole subtree";
    ExpectRect(*ItemOf(items, *grandchild)->clip, Rect{100.0f, 100.0f, 100.0f, 100.0f});
    EXPECT_FALSE(ItemOf(items, *outside)->clip.has_value());

    // A disabled mask clips nothing
    masked->GetComponent<RectMask>()->SetActive(false);
    const std::vector<UIDrawItem> unmasked = UISystem::CollectGraphics(*scene, Viewport);
    EXPECT_FALSE(ItemOf(unmasked, *child)->clip.has_value());
}

TEST(RectMaskTest, NestedMasksClipToTheOverlap)
{
    const auto scene = Scene::Create("RectMask_Nested");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto outer = AddPanel(canvas, "Outer", Rect{0.0f, 0.0f, 200.0f, 200.0f}, false);
    outer->AddComponent<RectMask>();
    const auto inner = AddPanel(outer, "Inner", Rect{100.0f, 100.0f, 200.0f, 200.0f}, false);
    inner->AddComponent<RectMask>();
    const auto leaf = AddPanel(inner, "Leaf", Rect{0.0f, 0.0f, 200.0f, 200.0f});

    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Viewport);
    ASSERT_EQ(items.size(), 1u);
    ASSERT_TRUE(items[0].clip.has_value());
    ExpectRect(*items[0].clip, Rect{100.0f, 100.0f, 100.0f, 100.0f});
    EXPECT_TRUE(items[0].IsVisible());
    static_cast<void>(leaf);
}

TEST(RectMaskTest, TheHitTestRespectsTheClip)
{
    const auto scene = Scene::Create("RectMask_Hit");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto masked = AddPanel(canvas, "Masked", Rect{100.0f, 100.0f, 100.0f, 100.0f});
    masked->AddComponent<RectMask>();
    const auto child = AddPanel(masked, "Child", Rect{50.0f, 50.0f, 100.0f, 100.0f});

    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{175.0f, 175.0f}, Viewport), child.get()) << "inside the clip";
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{225.0f, 225.0f}, Viewport), nullptr)
        << "inside the child's rect but clipped away: nothing";
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{120.0f, 120.0f}, Viewport), masked.get()) << "the mask's own image";

    // Moved wholly out of the mask: never hit, never drawn
    child->GetComponent<RectTransform>()->SetAnchoredPosition(Vector2{300.0f, 0.0f});
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{450.0f, 150.0f}, Viewport), nullptr);
    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Viewport);
    EXPECT_FALSE(ItemOf(items, *child)->IsVisible());
}

// ============================================================================
// Clipped geometry
// ============================================================================

TEST(RectMaskTest, AnImageQuadIsCutWithItsTextureCoordinates)
{
    const Image::Quad quad{Rect{0.0f, 0.0f, 100.0f, 50.0f}};
    const std::optional<Image::Quad> cut = Image::ClipQuad(quad, Rect{25.0f, 10.0f, 100.0f, 100.0f});
    ASSERT_TRUE(cut.has_value());
    ExpectRect(cut->position, Rect{25.0f, 10.0f, 75.0f, 40.0f});
    ExpectRect(cut->uv, Rect{0.25f, 0.2f, 0.75f, 0.8f}, "uv");
    EXPECT_FALSE(Image::ClipQuad(quad, Rect{200.0f, 0.0f, 10.0f, 10.0f}).has_value());

    const auto scene = Scene::Create("RectMask_ImageQuads");
    const auto element = UISystem::CreateElement("Image");
    auto *image = element->AddComponent<Image>();
    EXPECT_TRUE(image->BuildQuads(quad.position, std::nullopt).empty()) << "no clip: the plain quad";
    EXPECT_TRUE(image->BuildQuads(quad.position, Rect{-10.0f, -10.0f, 200.0f, 200.0f}).empty()) << "inside the clip";
    const std::vector<Image::Quad> quads = image->BuildQuads(quad.position, Rect{25.0f, 10.0f, 100.0f, 100.0f});
    ASSERT_EQ(quads.size(), 1u);
    EXPECT_EQ(quads[0], *cut);
}

TEST(RectMaskTest, TextGlyphsAreDroppedOrCut)
{
    Text::TextLayout layout;
    Text::GlyphQuad inside;
    inside.position = Text::Rect{0.0f, 0.0f, 10.0f, 10.0f};
    inside.uv = Text::Rect{0.0f, 0.0f, 0.1f, 0.1f};
    Text::GlyphQuad across = inside;
    across.position = Text::Rect{15.0f, 0.0f, 25.0f, 10.0f};
    across.uv = Text::Rect{0.2f, 0.4f, 0.4f, 0.6f}; // y-down: uv.minY is the top edge
    Text::GlyphQuad outside = inside;
    outside.position = Text::Rect{40.0f, 0.0f, 50.0f, 10.0f};
    layout.quads = {inside, across, outside};

    std::vector<Common::Color> colors{Common::Color::Red, Common::Color::Green, Common::Color::Blue};
    // The clip keeps x <= 20 and y >= 5 (the bottom half is cut)
    const Text::TextLayout clipped = UIText::ClipLayout(layout, Text::Rect{-100.0f, 5.0f, 20.0f, 100.0f}, &colors);
    ASSERT_EQ(clipped.quads.size(), 2u);
    ASSERT_EQ(colors.size(), 2u) << "the colours follow the quads";
    EXPECT_EQ(colors[0], Common::Color::Red);
    EXPECT_EQ(colors[1], Common::Color::Green);

    const Text::GlyphQuad &first = clipped.quads[0];
    EXPECT_FLOAT_EQ(first.position.minY, 5.0f);
    EXPECT_FLOAT_EQ(first.uv.minY, 0.0f) << "the top stays";
    EXPECT_NEAR(first.uv.maxY, 0.05f, 1e-6f) << "the bottom half of the glyph's uv is cut";

    const Text::GlyphQuad &second = clipped.quads[1];
    EXPECT_FLOAT_EQ(second.position.minX, 15.0f);
    EXPECT_FLOAT_EQ(second.position.maxX, 20.0f);
    EXPECT_NEAR(second.uv.minX, 0.2f, 1e-6f);
    EXPECT_NEAR(second.uv.maxX, 0.3f, 1e-6f) << "half the width, half the uv";
    EXPECT_NEAR(second.uv.minY, 0.4f, 1e-6f);
    EXPECT_NEAR(second.uv.maxY, 0.5f, 1e-6f);
}

TEST(RectMaskTest, TheSoftwareRendererDrawsOnlyInsideTheMask)
{
    constexpr int width = 40;
    constexpr int height = 40;
    Renderer::Software::SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, static_cast<uint32_t>(width), static_cast<uint32_t>(height)));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    {
        auto scene = Scene::Create("RectMask_Software");
        const auto canvas = UISystem::CreateCanvas("Canvas");
        scene->AddRootGameObject(canvas);
        // The left half masks a white image covering the whole screen
        const auto masked = AddPanel(canvas, "Masked", Rect{0.0f, 0.0f, 20.0f, 40.0f}, false);
        masked->AddComponent<RectMask>();
        const auto white = AddPanel(masked, "White", Rect{-10.0f, -10.0f, 60.0f, 60.0f});
        white->GetComponent<Image>()->SetColor(Common::Color::White);

        renderer.BeginFrame();
        UISystem::Render(*scene, &renderer, Vector2i{width, height});
        renderer.EndFrame();
        renderer.Present();
        renderer.ReadFramebuffer(rgba.data(), width, height);
        scene.reset();
    }
    renderer.Shutdown();

    auto red = [&rgba](const int x, const int y) { return rgba[(static_cast<std::size_t>(y) * width + x) * 4]; };
    int wrong = 0;
    for (int y = 1; y < height - 1; ++y)
    {
        for (int x = 1; x < width - 1; ++x)
        {
            if (x == 19 || x == 20)
            {
                continue; // the clip's edge
            }
            const bool inside = x < 20;
            wrong += (red(x, y) > 128) == inside ? 0 : 1;
        }
    }
    EXPECT_EQ(wrong, 0) << "white inside the mask, the clear colour outside";
}

// ============================================================================
// ScrollView
// ============================================================================

class ScrollViewTest : public ::testing::Test
{
protected:
    std::unique_ptr<Scene> _scene = Scene::Create("ScrollView_Test");
    GameObject::Ptr _canvas;
    GameObject::Ptr _view;
    ScrollView *_scroll = nullptr;
    RectTransform *_content = nullptr;

    void SetUp() override
    {
        _canvas = UISystem::CreateCanvas("Canvas");
        _scene->AddRootGameObject(_canvas);
        // 300 x 200 at the screen's centre: x 250..550, y 200..400; content 300 x 400 pinned to the top
        _view = UISystem::CreateScrollView("View");
        _canvas->AddChild(_view, false);
        _scroll = _view->GetComponent<ScrollView>();
        _content = _scroll->GetContent();
        LayOut();
    }

    void LayOut() const { static_cast<void>(UISystem::CollectGraphics(*_scene, Viewport)); }
};

TEST_F(ScrollViewTest, TheHelperBuildsAMaskedViewWithContentAndAScrollbar)
{
    ASSERT_NE(_scroll, nullptr);
    ASSERT_NE(_content, nullptr);
    EXPECT_NE(_view->GetComponent<RectMask>(), nullptr);
    EXPECT_EQ(_content->GetGameObject().GetName(), "Content");
    EXPECT_TRUE(_scroll->GetVertical());
    EXPECT_FALSE(_scroll->GetHorizontal());
    ASSERT_NE(_scroll->GetVerticalScrollbar(), nullptr);
    EXPECT_EQ(_scroll->GetHorizontalScrollbar(), nullptr);
    ExpectRect(_scroll->GetViewRect(), Rect{250.0f, 200.0f, 300.0f, 200.0f}, "view");
    ExpectRect(_scroll->GetContentRect(), Rect{250.0f, 0.0f, 300.0f, 400.0f}, "content");
    EXPECT_NEAR(_scroll->GetNormalizedPosition().y, 1.0f, Tolerance) << "starts at the top";
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("ScrollView"));
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("RectMask"));
}

TEST_F(ScrollViewTest, TheNormalizedPositionMovesTheContent)
{
    _scroll->SetNormalizedPosition(Vector2{0.0f, 0.0f});
    EXPECT_NEAR(_content->GetAnchoredPosition().y, 200.0f, Tolerance);
    EXPECT_NEAR(_scroll->GetNormalizedPosition().y, 0.0f, Tolerance) << "read back before the next layout";
    LayOut();
    EXPECT_NEAR(_scroll->GetNormalizedPosition().y, 0.0f, Tolerance) << "and after it";

    _scroll->SetVerticalNormalizedPosition(0.25f);
    EXPECT_NEAR(_content->GetAnchoredPosition().y, 150.0f, Tolerance);
    _scroll->SetVerticalNormalizedPosition(3.0f);
    EXPECT_NEAR(_scroll->GetNormalizedPosition().y, 1.0f, Tolerance) << "clamped unless Unrestricted";
    EXPECT_NEAR(_scroll->GetNormalizedPosition().x, 0.0f, Tolerance) << "content no wider than the view";
}

TEST_F(ScrollViewTest, TheWheelScrollsBySensitivityAndClampedStopsAtTheEdge)
{
    _scroll->Scroll(Vector2{0.0f, -1.0f}); // a notch down: further down the content
    EXPECT_NEAR(_content->GetAnchoredPosition().y, ScrollView::DefaultScrollSensitivity, Tolerance);
    EXPECT_NEAR(_scroll->GetNormalizedPosition().y, 1.0f - 30.0f / 200.0f, Tolerance);

    _scroll->SetMovementType(ScrollMovementType::Clamped);
    _scroll->SetVerticalNormalizedPosition(1.0f);
    _scroll->Scroll(Vector2{0.0f, 1.0f}); // up, already at the top
    EXPECT_NEAR(_scroll->GetNormalizedPosition().y, 1.0f, Tolerance) << "clamped";
    _scroll->Scroll(Vector2{0.0f, -100.0f});
    EXPECT_NEAR(_scroll->GetNormalizedPosition().y, 0.0f, Tolerance) << "clamped at the bottom";
}

TEST_F(ScrollViewTest, AnElasticViewOvershootsAndSpringsBack)
{
    _scroll->Scroll(Vector2{0.0f, 1.0f}); // up, past the top
    EXPECT_GT(_scroll->GetNormalizedPosition().y, 1.0f) << "elastic: past the edge";
    for (int frame = 0; frame < 120; ++frame)
    {
        _scroll->Step(1.0f / 60.0f);
    }
    EXPECT_NEAR(_scroll->GetNormalizedPosition().y, 1.0f, Tolerance) << "sprung back";
    EXPECT_EQ(_scroll->GetVelocity().y, 0.0f);
}

TEST_F(ScrollViewTest, DraggingFollowsThePointerWithRubberBandingPastAnEdge)
{
    _scroll->BeginDrag(Vector2{400.0f, 300.0f});
    EXPECT_TRUE(_scroll->IsDragging());
    _scroll->Drag(Vector2{400.0f, 350.0f});
    EXPECT_NEAR(_content->GetAnchoredPosition().y, 50.0f, Tolerance) << "follows the pointer inside the content";

    // 100 past the top: the content follows less (Unity's rubber band)
    _scroll->Drag(Vector2{400.0f, 200.0f});
    const float expected = -ScrollView::RubberDelta(100.0f, 200.0f);
    EXPECT_NEAR(_content->GetAnchoredPosition().y, expected, Tolerance);
    EXPECT_GT(_content->GetAnchoredPosition().y, -100.0f);
    EXPECT_LT(_content->GetAnchoredPosition().y, 0.0f);

    _scroll->EndDrag();
    EXPECT_FALSE(_scroll->IsDragging());
    for (int frame = 0; frame < 120; ++frame)
    {
        _scroll->Step(1.0f / 60.0f);
    }
    EXPECT_NEAR(_scroll->GetNormalizedPosition().y, 1.0f, Tolerance);
}

TEST_F(ScrollViewTest, InertiaCoastsAndSlowsDown)
{
    _scroll->SetVerticalNormalizedPosition(0.5f);
    const float start = _content->GetAnchoredPosition().y;
    _scroll->SetVelocity(Vector2{0.0f, 100.0f});
    _scroll->Step(0.1f);
    const float speed = _scroll->GetVelocity().y;
    EXPECT_LT(speed, 100.0f);
    EXPECT_GT(speed, 0.0f);
    EXPECT_NEAR(_content->GetAnchoredPosition().y, start + speed * 0.1f, Tolerance);

    _scroll->SetInertia(false);
    _scroll->SetVelocity(Vector2{0.0f, 100.0f});
    const float before = _content->GetAnchoredPosition().y;
    _scroll->Step(0.1f);
    EXPECT_EQ(_content->GetAnchoredPosition().y, before) << "no inertia: no coasting";
    EXPECT_EQ(_scroll->GetVelocity().y, 0.0f);
}

TEST_F(ScrollViewTest, TheScrollbarHandleShowsTheVisibleShare)
{
    _scroll->SetVerticalNormalizedPosition(0.5f);
    const RectTransform *handle = _scroll->GetVerticalScrollbar();
    ASSERT_NE(handle, nullptr);
    // Half the content is in view: the handle is half the track, a quarter up
    EXPECT_NEAR(handle->GetAnchorMin().y, 0.25f, Tolerance);
    EXPECT_NEAR(handle->GetAnchorMax().y, 0.75f, Tolerance);
    _scroll->SetVerticalNormalizedPosition(1.0f);
    EXPECT_NEAR(handle->GetAnchorMax().y, 1.0f, Tolerance);
}

TEST_F(ScrollViewTest, OnValueChangedReportsTheNewPosition)
{
    int calls = 0;
    Vector2 last{-1.0f, -1.0f};
    const ScrollView::ListenerId id = _scroll->AddOnValueChanged([&](const Vector2 position)
    {
        ++calls;
        last = position;
    });
    EXPECT_EQ(_scroll->GetOnValueChangedListenerCount(), 1u);
    _scroll->SetVerticalNormalizedPosition(0.5f);
    EXPECT_EQ(calls, 1);
    EXPECT_NEAR(last.y, 0.5f, Tolerance);
    _scroll->SetVerticalNormalizedPosition(0.5f);
    EXPECT_EQ(calls, 1) << "no move, no call";
    _scroll->RemoveOnValueChanged(id);
    _scroll->SetVerticalNormalizedPosition(0.0f);
    EXPECT_EQ(calls, 1);
}

TEST_F(ScrollViewTest, ContentScrolledOutOfTheViewIsNotHit)
{
    // A button at the top of the content, in view at first
    const auto button = UISystem::CreateButton("Item", "Item");
    auto *rectTransform = button->GetComponent<RectTransform>();
    rectTransform->SetAnchorMin(Vector2{0.0f, 1.0f});
    rectTransform->SetAnchorMax(Vector2{0.0f, 1.0f});
    rectTransform->SetPivot(Vector2{0.0f, 1.0f});
    rectTransform->SetAnchoredPosition(Vector2{0.0f, 0.0f});
    rectTransform->SetSizeDelta(Vector2{100.0f, 40.0f});
    _content->GetGameObject().AddChild(button, false);
    EXPECT_EQ(UISystem::HitTest(*_scene, Vector2{300.0f, 380.0f}, Viewport), button.get());

    // Scrolled 100 down: the button is above the view, clipped away; the point now hits the view's background
    _scroll->SetVerticalNormalizedPosition(0.5f);
    EXPECT_EQ(UISystem::HitTest(*_scene, Vector2{300.0f, 380.0f}, Viewport), _view.get());
    EXPECT_EQ(UISystem::HitTest(*_scene, Vector2{300.0f, 480.0f}, Viewport), nullptr)
        << "where the button is now, above the view";
}

TEST_F(ScrollViewTest, CancelPressMeansNoClick)
{
    const auto buttonObject = UISystem::CreateButton("Item", "Item");
    _content->GetGameObject().AddChild(buttonObject, false);
    auto *button = buttonObject->GetComponent<Button>();
    int clicks = 0;
    button->AddOnClick([&clicks] { ++clicks; });

    button->OnMouseDown();
    button->OnMouseUpAsButton();
    button->OnMouseUp();
    EXPECT_EQ(clicks, 1) << "a press and release over it clicks";

    button->OnMouseDown();
    button->CancelPress(); // what a ScrollView does when the press becomes a drag
    button->OnMouseUpAsButton();
    button->OnMouseUp();
    EXPECT_EQ(clicks, 1) << "the cancelled press does not";
}

TEST(ScrollViewMathTest, RubberDeltaAndSmoothDamp)
{
    EXPECT_FLOAT_EQ(ScrollView::RubberDelta(0.0f, 200.0f), 0.0f);
    EXPECT_LT(ScrollView::RubberDelta(1000.0f, 200.0f), 200.0f) << "never the whole view";
    EXPECT_FLOAT_EQ(ScrollView::RubberDelta(-100.0f, 200.0f), -ScrollView::RubberDelta(100.0f, 200.0f));
    EXPECT_FLOAT_EQ(ScrollView::RubberDelta(50.0f, 0.0f), 0.0f);

    float velocity = 0.0f;
    float position = 0.0f;
    for (int i = 0; i < 200; ++i)
    {
        position = ScrollView::SmoothDamp(position, 10.0f, velocity, 0.1f, 1.0f / 60.0f);
        EXPECT_LE(position, 10.0f) << "never past the target";
    }
    EXPECT_NEAR(position, 10.0f, 1e-3f);
}
