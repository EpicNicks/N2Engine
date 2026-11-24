#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector4.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/Slider.hpp"
#include "engine/ui/UISystem.hpp"

// Issue #445: the Slider (value, range, whole numbers, direction, fill and handle anchors, the pointer, OnValueChanged) and
// 9-slicing of Image (the nine quads, the shrink of borders, fillCenter)

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;

namespace
{
    constexpr float Tolerance = 1e-4f;
    const Vector2i Viewport{800, 600};

    void ExpectRect(const Rect &actual, const Rect &expected, const char *what = "")
    {
        EXPECT_NEAR(actual.x, expected.x, Tolerance) << what << " x";
        EXPECT_NEAR(actual.y, expected.y, Tolerance) << what << " y";
        EXPECT_NEAR(actual.width, expected.width, Tolerance) << what << " width";
        EXPECT_NEAR(actual.height, expected.height, Tolerance) << what << " height";
    }
}

class SliderTest : public ::testing::Test
{
protected:
    std::unique_ptr<Scene> _scene = Scene::Create("Slider_Test");
    GameObject::Ptr _canvas;
    GameObject::Ptr _object;
    Slider *_slider = nullptr;

    void SetUp() override
    {
        _canvas = UISystem::CreateCanvas("Canvas");
        _scene->AddRootGameObject(_canvas);
        _object = UISystem::CreateSlider("Volume");
        _canvas->AddChild(_object, false);
        _slider = _object->GetComponent<Slider>();
        LayOut();
    }

    void LayOut() const { static_cast<void>(UISystem::CollectGraphics(*_scene, Viewport)); }
};

TEST_F(SliderTest, TheHelperBuildsABarWithAFillAndAHandle)
{
    ASSERT_NE(_slider, nullptr);
    ASSERT_NE(_slider->GetFillRect(), nullptr);
    ASSERT_NE(_slider->GetHandleRect(), nullptr);
    EXPECT_EQ(_slider->GetFillRect()->GetGameObject().GetName(), "Fill");
    EXPECT_EQ(_slider->GetHandleRect()->GetGameObject().GetName(), "Handle");
    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("Slider"));
    EXPECT_FLOAT_EQ(_slider->GetValue(), 0.0f);
    EXPECT_FLOAT_EQ(_slider->GetMaxValue(), 1.0f);
}

TEST_F(SliderTest, TheValueIsClampedAndTheRangeClampsIt)
{
    _slider->SetMinValue(10.0f);
    _slider->SetMaxValue(20.0f);
    EXPECT_FLOAT_EQ(_slider->GetValue(), 10.0f) << "the old 0 moved into the new range";
    _slider->SetValue(99.0f);
    EXPECT_FLOAT_EQ(_slider->GetValue(), 20.0f);
    _slider->SetValue(-5.0f);
    EXPECT_FLOAT_EQ(_slider->GetValue(), 10.0f);
    _slider->SetValue(15.0f);
    EXPECT_FLOAT_EQ(_slider->GetNormalizedValue(), 0.5f);
    _slider->SetMaxValue(12.0f);
    EXPECT_FLOAT_EQ(_slider->GetValue(), 12.0f) << "a smaller range clamps the value";
    _slider->SetValue(std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(_slider->GetValue(), 10.0f) << "NaN counts as the minimum";
}

TEST_F(SliderTest, WholeNumbersRoundTheValue)
{
    _slider->SetMinValue(0.0f);
    _slider->SetMaxValue(10.0f);
    _slider->SetWholeNumbers(true);
    _slider->SetValue(3.6f);
    EXPECT_FLOAT_EQ(_slider->GetValue(), 4.0f);
    _slider->SetNormalizedValue(0.26f);
    EXPECT_FLOAT_EQ(_slider->GetValue(), 3.0f);
}

TEST_F(SliderTest, TheFillAndHandleFollowTheValue)
{
    _slider->SetValue(0.25f);
    EXPECT_NEAR(_slider->GetFillRect()->GetAnchorMax().x, 0.25f, Tolerance);
    EXPECT_NEAR(_slider->GetFillRect()->GetAnchorMin().x, 0.0f, Tolerance);
    EXPECT_NEAR(_slider->GetHandleRect()->GetAnchorMin().x, 0.25f, Tolerance);
    EXPECT_NEAR(_slider->GetHandleRect()->GetAnchorMax().x, 0.25f, Tolerance);

    _slider->SetDirection(SliderDirection::RightToLeft);
    EXPECT_NEAR(_slider->GetFillRect()->GetAnchorMin().x, 0.75f, Tolerance);
    EXPECT_NEAR(_slider->GetFillRect()->GetAnchorMax().x, 1.0f, Tolerance);
    EXPECT_NEAR(_slider->GetHandleRect()->GetAnchorMin().x, 0.75f, Tolerance);

    _slider->SetDirection(SliderDirection::BottomToTop);
    EXPECT_NEAR(_slider->GetFillRect()->GetAnchorMax().y, 0.25f, Tolerance);
    EXPECT_NEAR(_slider->GetFillRect()->GetAnchorMax().x, 1.0f, Tolerance);

    _slider->SetDirection(SliderDirection::TopToBottom);
    EXPECT_NEAR(_slider->GetFillRect()->GetAnchorMin().y, 0.75f, Tolerance);
}

TEST_F(SliderTest, ListenersRunOnChangeNotOnASilentSet)
{
    std::vector<float> seen;
    const Slider::ListenerId id = _slider->AddOnValueChanged([&seen](const float value) { seen.push_back(value); });
    EXPECT_EQ(_slider->GetOnValueChangedListenerCount(), 1u);
    _slider->SetValue(0.5f);
    _slider->SetValue(0.5f); // unchanged
    _slider->SetValueWithoutNotify(0.75f);
    _slider->SetValue(1.0f);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_FLOAT_EQ(seen[0], 0.5f);
    EXPECT_FLOAT_EQ(seen[1], 1.0f);
    _slider->RemoveOnValueChanged(id);
    _slider->SetValue(0.0f);
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_EQ(_slider->GetOnValueChangedListenerCount(), 0u);
}

TEST_F(SliderTest, APointerSetsTheValueAcrossTheBar)
{
    // 160 x 20 at the centre of an 800 x 600 canvas: x 320..480, y 290..310
    ExpectRect(_object->GetComponent<RectTransform>()->GetRect(), Rect{320.0f, 290.0f, 160.0f, 20.0f}, "bar");
    _slider->SetValueFromPointer(Vector2{400.0f, 300.0f});
    EXPECT_NEAR(_slider->GetValue(), 0.5f, Tolerance);
    _slider->SetValueFromPointer(Vector2{1000.0f, 300.0f});
    EXPECT_NEAR(_slider->GetValue(), 1.0f, Tolerance) << "past the end: clamped";
    _slider->SetDirection(SliderDirection::RightToLeft);
    _slider->SetValueFromPointer(Vector2{360.0f, 300.0f});
    EXPECT_NEAR(_slider->GetValue(), 0.75f, Tolerance);

    _slider->SetInteractable(false);
    _slider->SetValueFromPointer(Vector2{480.0f, 300.0f});
    EXPECT_NEAR(_slider->GetValue(), 0.75f, Tolerance) << "not interactable: the pointer is ignored";
    _slider->SetValue(0.1f);
    EXPECT_NEAR(_slider->GetValue(), 0.1f, Tolerance) << "but scripts still set it";
}

TEST_F(SliderTest, TheFieldsRoundTripThroughTheRegistry)
{
    _slider->SetMinValue(-2.0f);
    _slider->SetMaxValue(8.0f);
    _slider->SetValue(3.0f);
    _slider->SetWholeNumbers(true);
    _slider->SetDirection(SliderDirection::TopToBottom);
    const nlohmann::json saved = _object->Serialize();
    const auto loaded = GameObject::Deserialize(saved);
    ASSERT_NE(loaded, nullptr);
    auto *copy = loaded->GetComponent<Slider>();
    ASSERT_NE(copy, nullptr);
    EXPECT_FLOAT_EQ(copy->GetMinValue(), -2.0f);
    EXPECT_FLOAT_EQ(copy->GetMaxValue(), 8.0f);
    EXPECT_FLOAT_EQ(copy->GetValue(), 3.0f);
    EXPECT_TRUE(copy->GetWholeNumbers());
    EXPECT_EQ(copy->GetDirection(), SliderDirection::TopToBottom);
}

// ============================================================================
// 9-slicing
// ============================================================================

TEST(ImageSliceTest, NoBorderDrawsTheOrdinaryQuad)
{
    const auto element = UISystem::CreateElement("Image");
    auto *image = element->AddComponent<Image>();
    EXPECT_FALSE(image->IsSliced());
    EXPECT_TRUE(image->BuildQuads(Rect{0.0f, 0.0f, 100.0f, 100.0f}, std::nullopt, Vector2{32.0f, 32.0f}).empty());
}

TEST(ImageSliceTest, TheCornersKeepTheirSizeAndTheEdgesStretch)
{
    const auto element = UISystem::CreateElement("Image");
    auto *image = element->AddComponent<Image>();
    image->SetSliceBorder(8.0f, 4.0f, 8.0f, 4.0f);
    EXPECT_TRUE(image->IsSliced());
    const std::vector<Image::Quad> quads = image->SliceQuads(Rect{0.0f, 0.0f, 100.0f, 50.0f}, Vector2{32.0f, 16.0f});
    ASSERT_EQ(quads.size(), 9u);
    // Bottom-left corner: 8 x 4 canvas units showing the texture's lower-left 8 x 4 pixels (uv 0.25 x 0.25)
    ExpectRect(quads[0].position, Rect{0.0f, 0.0f, 8.0f, 4.0f}, "bottom-left");
    ExpectRect(quads[0].uv, Rect{0.0f, 0.0f, 0.25f, 0.25f}, "bottom-left uv");
    // Bottom edge stretches along x
    ExpectRect(quads[1].position, Rect{8.0f, 0.0f, 84.0f, 4.0f}, "bottom edge");
    // Centre takes the rest in both directions
    ExpectRect(quads[4].position, Rect{8.0f, 4.0f, 84.0f, 42.0f}, "centre");
    ExpectRect(quads[4].uv, Rect{0.25f, 0.25f, 0.5f, 0.5f}, "centre uv");
    // Top-right corner
    ExpectRect(quads[8].position, Rect{92.0f, 46.0f, 8.0f, 4.0f}, "top-right");
    ExpectRect(quads[8].uv, Rect{0.75f, 0.75f, 0.25f, 0.25f}, "top-right uv");
}

TEST(ImageSliceTest, TheSliceScaleResizesTheBordersAndNoCentreLeavesAFrame)
{
    const auto element = UISystem::CreateElement("Image");
    auto *image = element->AddComponent<Image>();
    image->SetSliceBorder(8.0f, 8.0f, 8.0f, 8.0f);
    image->SetSliceScale(2.0f);
    image->SetFillCenter(false);
    const std::vector<Image::Quad> quads = image->SliceQuads(Rect{0.0f, 0.0f, 100.0f, 100.0f}, Vector2{32.0f, 32.0f});
    ASSERT_EQ(quads.size(), 8u) << "no centre";
    ExpectRect(quads[0].position, Rect{0.0f, 0.0f, 16.0f, 16.0f}, "corner at scale 2");
}

TEST(ImageSliceTest, BordersShrinkInProportionWhenTheRectIsTooSmall)
{
    const auto element = UISystem::CreateElement("Image");
    auto *image = element->AddComponent<Image>();
    image->SetSliceBorder(10.0f, 0.0f, 30.0f, 0.0f);
    const std::vector<Image::Quad> quads = image->SliceQuads(Rect{0.0f, 0.0f, 20.0f, 20.0f}, Vector2{40.0f, 40.0f});
    // 10 + 30 = 40 > 20: both halve, to 5 and 15, leaving no middle column
    ASSERT_EQ(quads.size(), 2u);
    ExpectRect(quads[0].position, Rect{0.0f, 0.0f, 5.0f, 20.0f}, "left");
    ExpectRect(quads[1].position, Rect{5.0f, 0.0f, 15.0f, 20.0f}, "right");
}

TEST(ImageSliceTest, WithoutATextureSizeThereIsNothingToSlice)
{
    const auto element = UISystem::CreateElement("Image");
    auto *image = element->AddComponent<Image>();
    image->SetSliceBorder(4.0f, 4.0f, 4.0f, 4.0f);
    EXPECT_TRUE(image->SliceQuads(Rect{0.0f, 0.0f, 10.0f, 10.0f}, Vector2{0.0f, 0.0f}).empty());
}
