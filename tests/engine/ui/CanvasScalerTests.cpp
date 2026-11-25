#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <math/Vector2.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/CanvasScaler.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Rect.hpp"
#include "engine/ui/RectMask.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/Slider.hpp"
#include "engine/ui/UISystem.hpp"

// Issue #445: the CanvasScaler (reference resolution): the scale each mode gives, what the layout, the hit test, masks,
// controls and the software renderer make of it, and that it is saved

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;

namespace
{
    constexpr float Tolerance = 1e-3f;

    GameObject::Ptr ScaledCanvas(const Scene::Ptr &scene, const Vector2 &reference)
    {
        const auto canvas = UISystem::CreateCanvas("Canvas");
        scene->AddRootGameObject(canvas);
        canvas->AddComponent<CanvasScaler>()->SetReferenceResolution(reference);
        return canvas;
    }

    /// A child element pinned to its parent's bottom-left, with an Image unless `image` is false
    GameObject::Ptr AddPanel(const GameObject::Ptr &parent, const Rect &local, const bool image = true)
    {
        auto element = UISystem::CreateElement("Panel");
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
}

TEST(CanvasScalerTest, EachModeGivesItsScale)
{
    const auto object = UISystem::CreateCanvas("Canvas");
    auto *scaler = object->AddComponent<CanvasScaler>();
    scaler->SetReferenceResolution(Vector2{800.0f, 600.0f});
    const Vector2i wide{1600, 600}; // width ratio 2, height ratio 1

    scaler->SetMatchWidthOrHeight(0.0f);
    EXPECT_FLOAT_EQ(scaler->ComputeScale(wide), 2.0f);
    scaler->SetMatchWidthOrHeight(1.0f);
    EXPECT_FLOAT_EQ(scaler->ComputeScale(wide), 1.0f);
    scaler->SetMatchWidthOrHeight(0.5f);
    EXPECT_NEAR(scaler->ComputeScale(wide), std::sqrt(2.0f), Tolerance) << "a geometric mix, as Unity's";
    scaler->SetMatchWidthOrHeight(7.0f);
    EXPECT_FLOAT_EQ(scaler->ComputeScale(wide), 1.0f) << "clamped";

    scaler->SetScreenMatchMode(ScreenMatchMode::Expand);
    EXPECT_FLOAT_EQ(scaler->ComputeScale(wide), 1.0f);
    scaler->SetScreenMatchMode(ScreenMatchMode::Shrink);
    EXPECT_FLOAT_EQ(scaler->ComputeScale(wide), 2.0f);

    scaler->SetUIScaleMode(CanvasScaleMode::ConstantPixelSize);
    scaler->SetScaleFactor(1.5f);
    EXPECT_FLOAT_EQ(scaler->ComputeScale(wide), 1.5f);
}

TEST(CanvasScalerTest, BadValuesScaleNothing)
{
    const auto object = UISystem::CreateCanvas("Canvas");
    auto *scaler = object->AddComponent<CanvasScaler>();
    const Vector2i viewport{1600, 900};
    for (const Vector2 &reference : {Vector2{0.0f, 600.0f}, Vector2{800.0f, -1.0f},
                                     Vector2{std::numeric_limits<float>::quiet_NaN(), 600.0f}})
    {
        scaler->SetReferenceResolution(reference);
        EXPECT_FLOAT_EQ(scaler->ComputeScale(viewport), 1.0f);
    }
    scaler->SetUIScaleMode(CanvasScaleMode::ConstantPixelSize);
    for (const float factor : {0.0f, -2.0f, std::numeric_limits<float>::infinity()})
    {
        scaler->SetScaleFactor(factor);
        EXPECT_FLOAT_EQ(scaler->ComputeScale(viewport), 1.0f);
    }
    scaler->SetScaleFactor(2.0f);
    EXPECT_FLOAT_EQ(scaler->ComputeScale(Vector2i{0, 900}), 1.0f) << "an empty viewport";
}

TEST(CanvasScalerTest, TheLayoutIsInCanvasUnitsAndEverythingIsDrawnScaled)
{
    const auto scene = Scene::Create("Scaler_Layout");
    const auto canvas = ScaledCanvas(scene, Vector2{400.0f, 300.0f});
    const auto panel = AddPanel(canvas, Rect{10.0f, 20.0f, 100.0f, 50.0f});

    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Vector2i{800, 600});
    EXPECT_FLOAT_EQ(canvas->GetComponent<CanvasScaler>()->GetAppliedScale(), 2.0f);
    const RectTransform &rectTransform = *panel->GetComponent<RectTransform>();
    EXPECT_FLOAT_EQ(rectTransform.GetRect().x, 10.0f) << "canvas units";
    EXPECT_FLOAT_EQ(rectTransform.GetRect().width, 100.0f);
    EXPECT_NEAR(rectTransform.GetCanvasCorners()[2].x, 220.0f, Tolerance) << "window units";
    EXPECT_NEAR(rectTransform.GetCanvasCorners()[2].y, 140.0f, Tolerance);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_TRUE(items[0].Hits(Vector2{215.0f, 135.0f}));
    EXPECT_FALSE(items[0].Hits(Vector2{225.0f, 100.0f}));
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{215.0f, 135.0f}, Vector2i{800, 600}), panel.get());
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{15.0f, 25.0f}, Vector2i{800, 600}), nullptr)
        << "where it would be unscaled";
}

TEST(CanvasScalerTest, AnchorsFollowTheCanvasSize)
{
    const auto scene = Scene::Create("Scaler_Anchors");
    const auto canvas = ScaledCanvas(scene, Vector2{800.0f, 600.0f}); // match the width
    const auto stretched = AddPanel(canvas, Rect{});
    stretched->GetComponent<RectTransform>()->StretchToParent();
    static_cast<void>(UISystem::CollectGraphics(*scene, Vector2i{1600, 900}));
    const Rect &rect = stretched->GetComponent<RectTransform>()->GetRect();
    EXPECT_FLOAT_EQ(rect.width, 800.0f);
    EXPECT_FLOAT_EQ(rect.height, 450.0f);
}

TEST(CanvasScalerTest, ADisabledScalerAndAWorldCanvasAreUnscaled)
{
    const auto scene = Scene::Create("Scaler_Off");
    const auto canvas = ScaledCanvas(scene, Vector2{400.0f, 300.0f});
    const auto panel = AddPanel(canvas, Rect{0.0f, 0.0f, 10.0f, 10.0f});
    canvas->GetComponent<CanvasScaler>()->SetActive(false);
    std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Vector2i{800, 600});
    ASSERT_EQ(items.size(), 1u);
    EXPECT_TRUE(items[0].transform.IsIdentity());

    const auto world = UISystem::CreateCanvas("World", CanvasRenderMode::WorldSpace);
    scene->AddRootGameObject(world);
    world->AddComponent<CanvasScaler>()->SetReferenceResolution(Vector2{1.0f, 1.0f});
    const auto worldPanel = AddPanel(world, Rect{0.0f, 0.0f, 10.0f, 10.0f});
    items = UISystem::CollectWorldCanvasGraphics(*world->GetComponent<Canvas>());
    ASSERT_EQ(items.size(), 1u);
    EXPECT_TRUE(items[0].transform.IsIdentity());
    static_cast<void>(panel);
    static_cast<void>(worldPanel);
}

TEST(CanvasScalerTest, MasksAndControlsWorkInAScaledCanvas)
{
    const auto scene = Scene::Create("Scaler_Controls");
    const auto canvas = ScaledCanvas(scene, Vector2{400.0f, 300.0f});
    const auto mask = AddPanel(canvas, Rect{0.0f, 0.0f, 50.0f, 50.0f}, false);
    mask->AddComponent<RectMask>();
    const auto child = AddPanel(mask, Rect{0.0f, 0.0f, 100.0f, 100.0f});
    const auto slider = UISystem::CreateSlider("Slider"); // 160 x 20 in the middle of the 400 x 300 canvas
    canvas->AddChild(slider, false);

    const std::vector<UIDrawItem> items = UISystem::CollectGraphics(*scene, Vector2i{800, 600});
    ASSERT_FALSE(items.empty());
    ASSERT_TRUE(items[0].clip.has_value());
    EXPECT_NEAR(items[0].clip->width, 100.0f, Tolerance) << "the mask in window units";
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{90.0f, 90.0f}, Vector2i{800, 600}), child.get());
    EXPECT_EQ(UISystem::HitTest(*scene, Vector2{110.0f, 90.0f}, Vector2i{800, 600}), nullptr);

    // The bar spans window x 240..560: three quarters along is 480
    slider->GetComponent<Slider>()->SetValueFromPointer(Vector2{480.0f, 300.0f});
    EXPECT_NEAR(slider->GetComponent<Slider>()->GetValue(), 0.75f, Tolerance);
}

TEST(CanvasScalerTest, TheSoftwareRendererDrawsScaled)
{
    constexpr int size = 40;
    const auto scene = Scene::Create("Scaler_Software");
    const auto canvas = ScaledCanvas(scene, Vector2{20.0f, 20.0f});
    const auto mask = AddPanel(canvas, Rect{0.0f, 0.0f, 10.0f, 10.0f}, false);
    mask->AddComponent<RectMask>();
    AddPanel(mask, Rect{0.0f, 0.0f, 20.0f, 20.0f}); // cut to the lower left quarter of the screen

    Renderer::Software::SoftwareRenderer renderer;
    ASSERT_TRUE(renderer.Initialize(nullptr, size, size));
    renderer.Clear(0.0f, 0.0f, 0.0f, 1.0f);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(size) * size * 4);
    renderer.BeginFrame();
    UISystem::Render(*scene, &renderer, Vector2i{size, size});
    renderer.EndFrame();
    renderer.Present();
    renderer.ReadFramebuffer(rgba.data(), size, size);
    renderer.Shutdown();

    int lit = 0;
    for (std::size_t i = 0; i < rgba.size(); i += 4)
    {
        lit += rgba[i] > 128 ? 1 : 0;
    }
    EXPECT_NEAR(lit, size * size / 4, size) << "a quarter of the screen: 10 of 20 units, doubled";
}

TEST(CanvasScalerTest, ItIsSaved)
{
    const auto object = UISystem::CreateCanvas("Canvas");
    auto *scaler = object->AddComponent<CanvasScaler>();
    scaler->SetUIScaleMode(CanvasScaleMode::ConstantPixelSize);
    scaler->SetScaleFactor(2.5f);
    scaler->SetReferenceResolution(Vector2{1920.0f, 1080.0f});
    scaler->SetScreenMatchMode(ScreenMatchMode::Shrink);
    scaler->SetMatchWidthOrHeight(0.25f);
    const nlohmann::json saved = scaler->Serialize();

    const auto other = UISystem::CreateCanvas("Other");
    auto *loaded = other->AddComponent<CanvasScaler>();
    loaded->Deserialize(saved);
    EXPECT_EQ(loaded->GetUIScaleMode(), CanvasScaleMode::ConstantPixelSize);
    EXPECT_FLOAT_EQ(loaded->GetScaleFactor(), 2.5f);
    EXPECT_FLOAT_EQ(loaded->GetReferenceResolution().x, 1920.0f);
    EXPECT_EQ(loaded->GetScreenMatchMode(), ScreenMatchMode::Shrink);
    EXPECT_FLOAT_EQ(loaded->GetMatchWidthOrHeight(), 0.25f);
}
