#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/LayoutGroup.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/ScrollView.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

// Issue #445: horizontal, vertical and grid layout groups (padding, spacing, alignment, controlled and expanded sizes,
// cell sizes and column counts), LayoutElement, and ContentSizeFitter growing a scroll view's content with its children,
// all applied in the UI layout before drawing and hit testing

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;

namespace
{
    constexpr float Tolerance = 1e-3f;
    const Vector2i Viewport{800, 600};

    void ExpectRect(const Rect &actual, const Rect &expected, const std::string &what = "")
    {
        EXPECT_NEAR(actual.x, expected.x, Tolerance) << what << " x";
        EXPECT_NEAR(actual.y, expected.y, Tolerance) << what << " y";
        EXPECT_NEAR(actual.width, expected.width, Tolerance) << what << " width";
        EXPECT_NEAR(actual.height, expected.height, Tolerance) << what << " height";
    }

    /// An element at the canvas's bottom-left, `width` x `height`
    GameObject::Ptr AddBox(const GameObject::Ptr &parent, const std::string &name, const float width, const float height)
    {
        auto element = UISystem::CreateElement(name);
        auto *rectTransform = element->GetComponent<RectTransform>();
        rectTransform->SetAnchorMin(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchorMax(Vector2{0.0f, 0.0f});
        rectTransform->SetPivot(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchoredPosition(Vector2{0.0f, 0.0f});
        rectTransform->SetSizeDelta(Vector2{width, height});
        element->AddComponent<Image>();
        parent->AddChild(element, false);
        return element;
    }

    Rect RectOf(const GameObject::Ptr &gameObject) { return gameObject->GetComponent<RectTransform>()->GetRect(); }

    class LayoutGroupTest : public ::testing::Test
    {
    protected:
        std::unique_ptr<Scene> _scene = Scene::Create("LayoutGroup_Test");
        GameObject::Ptr _canvas;
        GameObject::Ptr _group;

        void SetUp() override
        {
            _canvas = UISystem::CreateCanvas("Canvas");
            _scene->AddRootGameObject(_canvas);
            _group = AddBox(_canvas, "Group", 200.0f, 200.0f);
        }

        void LayOut() const { static_cast<void>(UISystem::CollectGraphics(*_scene, Viewport)); }
    };
}

TEST(LayoutMathTest, AlignmentFractions)
{
    EXPECT_FLOAT_EQ(AlignmentFraction(ChildAlignment::UpperLeft).x, 0.0f);
    EXPECT_FLOAT_EQ(AlignmentFraction(ChildAlignment::UpperLeft).y, 0.0f);
    EXPECT_FLOAT_EQ(AlignmentFraction(ChildAlignment::MiddleCenter).x, 0.5f);
    EXPECT_FLOAT_EQ(AlignmentFraction(ChildAlignment::MiddleCenter).y, 0.5f);
    EXPECT_FLOAT_EQ(AlignmentFraction(ChildAlignment::LowerRight).x, 1.0f);
    EXPECT_FLOAT_EQ(AlignmentFraction(ChildAlignment::LowerRight).y, 1.0f);
}

TEST_F(LayoutGroupTest, AVerticalGroupStacksItsChildrenFromTheTopWithPaddingAndSpacing)
{
    auto *group = _group->AddComponent<VerticalLayoutGroup>();
    group->SetPadding(LayoutPadding{10.0f, 10.0f, 10.0f, 10.0f});
    group->SetSpacing(5.0f);
    const auto a = AddBox(_group, "A", 100.0f, 20.0f);
    const auto b = AddBox(_group, "B", 50.0f, 30.0f);
    const auto c = AddBox(_group, "C", 80.0f, 10.0f);
    LayOut();
    ExpectRect(RectOf(a), Rect{10.0f, 170.0f, 100.0f, 20.0f}, "A");
    ExpectRect(RectOf(b), Rect{10.0f, 135.0f, 50.0f, 30.0f}, "B");
    ExpectRect(RectOf(c), Rect{10.0f, 120.0f, 80.0f, 10.0f}, "C");

    // Preferred size: the widest child and the stacked heights, plus the padding
    const Vector2 preferred = group->GetPreferredSize(RectOf(_group));
    EXPECT_NEAR(preferred.x, 120.0f, Tolerance);
    EXPECT_NEAR(preferred.y, 20.0f + 30.0f + 10.0f + 2 * 5.0f + 20.0f, Tolerance);

    // Lower right: the block at the bottom, each child at the right edge
    group->SetChildAlignment(ChildAlignment::LowerRight);
    LayOut();
    ExpectRect(RectOf(c), Rect{110.0f, 10.0f, 80.0f, 10.0f}, "C lower right");
    ExpectRect(RectOf(a), Rect{90.0f, 10.0f + 10.0f + 5.0f + 30.0f + 5.0f, 100.0f, 20.0f}, "A lower right");
}

TEST_F(LayoutGroupTest, AHorizontalGroupControlsAndExpandsChildSizes)
{
    auto *group = _group->AddComponent<HorizontalLayoutGroup>();
    group->SetPadding(LayoutPadding{10.0f, 10.0f, 10.0f, 10.0f});
    group->SetControlChildWidth(true);
    group->SetControlChildHeight(true);
    group->SetChildForceExpandWidth(true);
    group->SetChildForceExpandHeight(true);
    const auto a = AddBox(_group, "A", 100.0f, 20.0f);
    const auto b = AddBox(_group, "B", 50.0f, 30.0f);
    LayOut();
    // 150 of 180 used: 15 more each; both as high as the padded rect
    ExpectRect(RectOf(a), Rect{10.0f, 10.0f, 115.0f, 180.0f}, "A");
    ExpectRect(RectOf(b), Rect{125.0f, 10.0f, 65.0f, 180.0f}, "B");

    // Without expanding, centred: the row in the middle, each child centred vertically
    group->SetChildForceExpandWidth(false);
    group->SetChildForceExpandHeight(false);
    group->SetChildAlignment(ChildAlignment::MiddleCenter);
    // The children's own sizes are what the last layout set; give them back their sizes through LayoutElement
    a->AddComponent<LayoutElement>()->SetPreferredWidth(100.0f);
    a->GetComponent<LayoutElement>()->SetPreferredHeight(20.0f);
    b->AddComponent<LayoutElement>()->SetPreferredWidth(50.0f);
    b->GetComponent<LayoutElement>()->SetPreferredHeight(30.0f);
    LayOut();
    ExpectRect(RectOf(a), Rect{25.0f, 90.0f, 100.0f, 20.0f}, "A centred");
    ExpectRect(RectOf(b), Rect{125.0f, 85.0f, 50.0f, 30.0f}, "B centred");
}

TEST_F(LayoutGroupTest, IgnoredAndInactiveChildrenAreLeftOut)
{
    auto *group = _group->AddComponent<HorizontalLayoutGroup>();
    group->SetSpacing(10.0f);
    const auto a = AddBox(_group, "A", 20.0f, 20.0f);
    const auto ignored = AddBox(_group, "Ignored", 20.0f, 20.0f);
    ignored->AddComponent<LayoutElement>()->SetIgnoreLayout(true);
    const auto inactive = AddBox(_group, "Inactive", 20.0f, 20.0f);
    inactive->SetActive(false);
    const auto b = AddBox(_group, "B", 20.0f, 20.0f);
    LayOut();
    EXPECT_NEAR(RectOf(a).x, 0.0f, Tolerance);
    EXPECT_NEAR(RectOf(b).x, 30.0f, Tolerance) << "right after A: the other two take no room";
    ExpectRect(RectOf(ignored), Rect{0.0f, 0.0f, 20.0f, 20.0f}, "keeps its own place");
    EXPECT_EQ(group->LayoutChildren().size(), 2u);
}

TEST_F(LayoutGroupTest, AGridFillsRowsOfCells)
{
    auto *grid = _group->AddComponent<GridLayoutGroup>();
    grid->SetCellSize(Vector2{50.0f, 50.0f});
    grid->SetSpacing(Vector2{10.0f, 10.0f});
    std::vector<GameObject::Ptr> cells;
    for (int i = 0; i < 5; ++i)
    {
        cells.push_back(AddBox(_group, "Cell" + std::to_string(i), 5.0f, 5.0f));
    }
    LayOut();
    // Flexible: floor((200 + 10) / 60) = 3 columns, 2 rows
    EXPECT_EQ(grid->ColumnCount(5, 200.0f), 3);
    ExpectRect(RectOf(cells[0]), Rect{0.0f, 150.0f, 50.0f, 50.0f}, "cell 0");
    ExpectRect(RectOf(cells[2]), Rect{120.0f, 150.0f, 50.0f, 50.0f}, "cell 2");
    ExpectRect(RectOf(cells[3]), Rect{0.0f, 90.0f, 50.0f, 50.0f}, "cell 3");
    ExpectRect(RectOf(cells[4]), Rect{60.0f, 90.0f, 50.0f, 50.0f}, "cell 4");
    const Vector2 preferred = grid->GetPreferredSize(RectOf(_group));
    EXPECT_NEAR(preferred.x, 170.0f, Tolerance);
    EXPECT_NEAR(preferred.y, 110.0f, Tolerance);

    grid->SetConstraint(GridConstraint::FixedColumnCount);
    grid->SetConstraintCount(2);
    LayOut();
    ExpectRect(RectOf(cells[4]), Rect{0.0f, 30.0f, 50.0f, 50.0f}, "the third row");
    grid->SetConstraint(GridConstraint::FixedRowCount);
    grid->SetConstraintCount(1);
    EXPECT_EQ(grid->ColumnCount(5, 200.0f), 5) << "one row: every child in it";
}

TEST_F(LayoutGroupTest, AControlledTextChildGetsItsTextsHeight)
{
    auto *group = _group->AddComponent<VerticalLayoutGroup>();
    group->SetControlChildHeight(true);
    const auto label = UISystem::CreateText("Label", "Hello");
    _group->AddChild(label, false);
    LayOut();
    const Rect rect = RectOf(label);
    EXPECT_GT(rect.height, 10.0f) << "about a line of 24 px text";
    EXPECT_LT(rect.height, 60.0f);
    EXPECT_NEAR(rect.YMax(), 200.0f, Tolerance) << "at the top";
}

TEST(ContentSizeFitterTest, AScrollViewsContentGrowsWithItsChildren)
{
    const auto scene = Scene::Create("ContentSizeFitter_Scroll");
    const auto canvas = UISystem::CreateCanvas("Canvas");
    scene->AddRootGameObject(canvas);
    const auto view = UISystem::CreateScrollView("View");
    canvas->AddChild(view, false);
    auto *scroll = view->GetComponent<ScrollView>();
    RectTransform *content = scroll->GetContent();
    ASSERT_NE(content, nullptr);
    GameObject &contentObject = content->GetGameObject();
    auto *column = contentObject.AddComponent<VerticalLayoutGroup>();
    column->SetControlChildWidth(true);
    column->SetChildForceExpandWidth(true);
    auto *fitter = contentObject.AddComponent<ContentSizeFitter>();
    EXPECT_EQ(fitter->GetVerticalFit(), FitMode::PreferredSize) << "the default";
    EXPECT_EQ(fitter->GetHorizontalFit(), FitMode::Unconstrained);

    std::vector<GameObject::Ptr> items;
    for (int i = 0; i < 10; ++i)
    {
        auto item = UISystem::CreateElement("Item" + std::to_string(i));
        item->GetComponent<RectTransform>()->SetSizeDelta(Vector2{100.0f, 50.0f});
        item->AddComponent<Image>();
        contentObject.AddChild(item, false);
        items.push_back(item);
    }
    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));

    // 10 x 50 high, pinned at the view's top (y 400), as wide as the view (expanded)
    ExpectRect(content->GetRect(), Rect{250.0f, -100.0f, 300.0f, 500.0f}, "content");
    ExpectRect(items[9]->GetComponent<RectTransform>()->GetRect(), Rect{250.0f, -100.0f, 300.0f, 50.0f}, "the last item");

    // Scrolled to the bottom, the last item is in view and hit there
    scroll->SetVerticalNormalizedPosition(0.0f);
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{400.0f, 225.0f}, Viewport), items[9].get());

    // One more item: the content grows on the next layout
    auto extra = UISystem::CreateElement("Extra");
    extra->GetComponent<RectTransform>()->SetSizeDelta(Vector2{100.0f, 50.0f});
    contentObject.AddChild(extra, false);
    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));
    EXPECT_NEAR(content->GetRect().height, 550.0f, Tolerance);
}

TEST(LayoutRegistryTest, TheComponentsAreRegistered)
{
    for (const char *type : {"HorizontalLayoutGroup", "VerticalLayoutGroup", "GridLayoutGroup", "LayoutElement", "ContentSizeFitter"})
    {
        EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered(type)) << type;
    }
}
