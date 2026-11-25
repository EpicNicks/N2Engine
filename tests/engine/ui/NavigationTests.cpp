#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/Navigation.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/ScrollView.hpp"
#include "engine/ui/Slider.hpp"
#include "engine/ui/UINavigator.hpp"
#include "engine/ui/UISystem.hpp"

// Issue #445: keyboard and gamepad UI navigation: the selection and its messages, automatic and explicit moves, Tab,
// sliders, submit and cancel, held-key repeat, clicks, scrolling the selection into view, and what is saved

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;

namespace
{
    const Vector2i Viewport{800, 600};

    /// Counts the selection messages an object gets
    class SelectionProbe final : public Component
    {
    public:
        explicit SelectionProbe(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "SelectionProbe"; }
        static constexpr bool IsSingleton = false;
        void OnSelect() override { ++selects; }
        void OnDeselect() override { ++deselects; }
        void OnSubmit() override { ++submits; }
        void OnCancel() override { ++cancels; }
        int selects = 0;
        int deselects = 0;
        int submits = 0;
        int cancels = 0;
    };

    class NavigationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            scene = Scene::Create("Navigation");
            canvas = UISystem::CreateCanvas("Canvas");
            scene->AddRootGameObject(canvas);
        }

        /// A button with its middle at (x, y)
        GameObject::Ptr AddButton(const std::string &name, const float x, const float y,
                                  const GameObject::Ptr &parent = nullptr)
        {
            auto button = UISystem::CreateButton(name, name);
            auto *rectTransform = button->GetComponent<RectTransform>();
            rectTransform->SetAnchorMin(Vector2{0.0f, 0.0f});
            rectTransform->SetAnchorMax(Vector2{0.0f, 0.0f});
            rectTransform->SetAnchoredPosition(Vector2{x, y});
            rectTransform->SetSizeDelta(Vector2{80.0f, 30.0f});
            (parent ? parent : canvas)->AddChild(button, false);
            return button;
        }

        void Frame(const NavigationInput &input, const float deltaTime = 0.016f)
        {
            navigator.Process(*scene, Viewport, input, deltaTime);
        }

        /// One press and release of a direction
        void Press(bool NavigationInput::*key)
        {
            NavigationInput input;
            input.*key = true;
            Frame(input);
            Frame(NavigationInput{});
        }

        std::unique_ptr<Scene> scene;
        GameObject::Ptr canvas;
        UINavigator navigator;
    };
}

TEST_F(NavigationTest, NothingHappensWithoutASelection)
{
    const auto a = AddButton("A", 100.0f, 100.0f);
    auto *probe = a->AddComponent<SelectionProbe>();
    int clicks = 0;
    a->GetComponent<Button>()->AddOnClick([&clicks] { ++clicks; });
    NavigationInput input;
    input.down = true;
    input.next = true;
    input.submit = true;
    input.cancel = true;
    input.pointerPressed = a.get();
    Frame(input);
    EXPECT_EQ(navigator.GetSelected(), nullptr) << "a click selects nothing either";
    EXPECT_EQ(clicks, 0);
    EXPECT_FALSE(navigator.WasSubmitPressed());
    EXPECT_EQ(probe->selects, 0);
    EXPECT_FALSE(navigator.Move(*scene, Viewport, NavigationDirection::Down));
    EXPECT_FALSE(navigator.Step(*scene, Viewport, false));
}

TEST_F(NavigationTest, SelectingSendsTheMessagesAndTintsTheButton)
{
    const auto a = AddButton("A", 100.0f, 100.0f);
    const auto b = AddButton("B", 100.0f, 200.0f);
    auto *probeA = a->AddComponent<SelectionProbe>();
    auto *probeB = b->AddComponent<SelectionProbe>();
    auto *button = a->GetComponent<Button>();
    button->SetFadeDuration(0.0f);

    navigator.SetSelected(a.get());
    EXPECT_EQ(navigator.GetSelected(), a.get());
    EXPECT_EQ(probeA->selects, 1);
    EXPECT_EQ(button->GetState(), Button::State::Selected);
    EXPECT_EQ(button->GetCurrentTint(), button->GetSelectedColor());
    navigator.SetSelected(a.get());
    EXPECT_EQ(probeA->selects, 1) << "already selected";

    // The pointer over a selected button leaves it Selected; pressed wins
    button->OnMouseEnter();
    EXPECT_EQ(button->GetState(), Button::State::Selected);
    button->OnMouseDown();
    EXPECT_EQ(button->GetState(), Button::State::Pressed);
    button->OnMouseUp();
    button->OnMouseExit();

    navigator.SetSelected(b.get());
    EXPECT_EQ(probeA->deselects, 1);
    EXPECT_EQ(probeB->selects, 1);
    EXPECT_EQ(button->GetState(), Button::State::Normal);
    EXPECT_EQ(button->GetCurrentTint(), button->GetNormalColor());

    navigator.SetSelected(nullptr);
    EXPECT_EQ(probeB->deselects, 1);
    EXPECT_EQ(navigator.GetSelected(), nullptr);
}

TEST_F(NavigationTest, ADeactivatedOrDestroyedSelectionIsDropped)
{
    const auto a = AddButton("A", 100.0f, 100.0f);
    navigator.SetSelected(a.get());
    a->SetActive(false);
    EXPECT_EQ(navigator.GetSelected(), nullptr);
    navigator.SetSelected(a.get());
    EXPECT_EQ(navigator.GetSelected(), nullptr) << "an inactive object cannot be selected";
    a->SetActive(true);
    EXPECT_EQ(a->GetComponent<Button>()->GetState(), Button::State::Normal) << "no longer shown as selected";
}

TEST_F(NavigationTest, TheArrowsGoToTheNearestElementInThatDirection)
{
    // A 2 x 2 grid and a stray one far to the right
    const auto bottomLeft = AddButton("BL", 100.0f, 100.0f);
    const auto bottomRight = AddButton("BR", 250.0f, 100.0f);
    const auto topLeft = AddButton("TL", 100.0f, 200.0f);
    const auto topRight = AddButton("TR", 250.0f, 200.0f);
    const auto far = AddButton("Far", 700.0f, 200.0f);

    navigator.SetSelected(bottomLeft.get());
    Press(&NavigationInput::right);
    EXPECT_EQ(navigator.GetSelected(), bottomRight.get());
    Press(&NavigationInput::up);
    EXPECT_EQ(navigator.GetSelected(), topRight.get());
    Press(&NavigationInput::left);
    EXPECT_EQ(navigator.GetSelected(), topLeft.get());
    Press(&NavigationInput::down);
    EXPECT_EQ(navigator.GetSelected(), bottomLeft.get());
    Press(&NavigationInput::down);
    EXPECT_EQ(navigator.GetSelected(), bottomLeft.get()) << "nothing below";
    navigator.SetSelected(topRight.get());
    Press(&NavigationInput::right);
    EXPECT_EQ(navigator.GetSelected(), far.get());
}

TEST_F(NavigationTest, ElementsThatCannotBeSelectedAreSkipped)
{
    const auto a = AddButton("A", 100.0f, 300.0f);
    const auto disabled = AddButton("Disabled", 100.0f, 250.0f);
    const auto hidden = AddButton("Hidden", 100.0f, 200.0f);
    const auto kept = AddButton("Kept off", 100.0f, 150.0f);
    const auto last = AddButton("Last", 100.0f, 100.0f);
    disabled->GetComponent<Button>()->SetInteractable(false);
    hidden->SetActive(false);
    kept->AddComponent<Navigation>()->SetMode(NavigationMode::None);

    navigator.SetSelected(a.get());
    Press(&NavigationInput::down);
    EXPECT_EQ(navigator.GetSelected(), last.get());
    EXPECT_EQ(UINavigator::CollectSelectables(*scene, Viewport),
              (std::vector<GameObject *>{a.get(), last.get()}));

    // A script can still select it, and the arrows don't leave it
    navigator.SetSelected(kept.get());
    Press(&NavigationInput::down);
    EXPECT_EQ(navigator.GetSelected(), kept.get());
}

TEST_F(NavigationTest, ExplicitNavigationFollowsItsNeighbours)
{
    const auto a = AddButton("A", 100.0f, 300.0f);
    const auto b = AddButton("B", 100.0f, 200.0f);
    const auto c = AddButton("C", 500.0f, 500.0f);
    auto *navigation = a->AddComponent<Navigation>();
    navigation->SetMode(NavigationMode::Explicit);
    navigation->SetNeighbor(NavigationDirection::Down, c->GetComponent<RectTransform>());

    navigator.SetSelected(a.get());
    Press(&NavigationInput::down);
    EXPECT_EQ(navigator.GetSelected(), c.get()) << "not the nearer B";
    navigator.SetSelected(a.get());
    Press(&NavigationInput::up);
    EXPECT_EQ(navigator.GetSelected(), a.get()) << "none set";
    c->SetActive(false);
    Press(&NavigationInput::down);
    EXPECT_EQ(navigator.GetSelected(), a.get()) << "an inactive neighbour";
    static_cast<void>(b);
}

TEST_F(NavigationTest, TabStepsInDrawOrderAndWraps)
{
    const auto a = AddButton("A", 400.0f, 100.0f);
    const auto b = AddButton("B", 100.0f, 500.0f);
    const auto c = AddButton("C", 200.0f, 200.0f);
    navigator.SetSelected(a.get());
    Press(&NavigationInput::next);
    EXPECT_EQ(navigator.GetSelected(), b.get());
    Press(&NavigationInput::next);
    EXPECT_EQ(navigator.GetSelected(), c.get());
    Press(&NavigationInput::next);
    EXPECT_EQ(navigator.GetSelected(), a.get()) << "wraps";
    Press(&NavigationInput::previous);
    EXPECT_EQ(navigator.GetSelected(), c.get());
}

TEST_F(NavigationTest, AHeldDirectionRepeatsAfterADelay)
{
    std::vector<GameObject::Ptr> column;
    for (int i = 0; i < 6; ++i)
    {
        column.push_back(AddButton("B" + std::to_string(i), 100.0f, 500.0f - 60.0f * static_cast<float>(i)));
    }
    navigator.SetSelected(column[0].get());
    NavigationInput down;
    down.down = true;
    Frame(down, 0.016f);
    EXPECT_EQ(navigator.GetSelected(), column[1].get()) << "at once";
    Frame(down, 0.3f);
    EXPECT_EQ(navigator.GetSelected(), column[1].get()) << "within the delay";
    Frame(down, 0.3f);
    EXPECT_EQ(navigator.GetSelected(), column[2].get()) << "the delay passed";
    Frame(down, 0.05f);
    EXPECT_EQ(navigator.GetSelected(), column[2].get());
    Frame(down, 0.06f);
    EXPECT_EQ(navigator.GetSelected(), column[3].get()) << "the interval passed";
    Frame(down, 5.0f);
    EXPECT_EQ(navigator.GetSelected(), column[4].get()) << "once a frame at most";
    Frame(NavigationInput{});
    Frame(down);
    EXPECT_EQ(navigator.GetSelected(), column[5].get()) << "pressed again";
}

TEST_F(NavigationTest, AKeyAlreadyHeldWhenSomethingIsSelectedWaits)
{
    const auto a = AddButton("A", 100.0f, 300.0f);
    const auto b = AddButton("B", 100.0f, 200.0f);
    NavigationInput down;
    down.down = true;
    Frame(down);
    navigator.SetSelected(a.get());
    Frame(down);
    EXPECT_EQ(navigator.GetSelected(), a.get());
    Frame(down, 1.0f);
    EXPECT_EQ(navigator.GetSelected(), b.get());
}

TEST_F(NavigationTest, SubmitClicksAndCancelTells)
{
    const auto a = AddButton("A", 100.0f, 300.0f);
    auto *probe = a->AddComponent<SelectionProbe>();
    int clicks = 0;
    a->GetComponent<Button>()->AddOnClick([&clicks] { ++clicks; });
    navigator.SetSelected(a.get());
    NavigationInput input;
    input.submit = true;
    Frame(input);
    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(probe->submits, 1);
    EXPECT_TRUE(navigator.WasSubmitPressed());
    EXPECT_FALSE(navigator.WasCancelPressed());
    input = NavigationInput{};
    input.cancel = true;
    Frame(input);
    EXPECT_EQ(probe->cancels, 1);
    EXPECT_TRUE(navigator.WasCancelPressed());
    EXPECT_FALSE(navigator.WasSubmitPressed()) << "only the frame it was pressed";
    EXPECT_EQ(navigator.GetSelected(), a.get()) << "cancel does not deselect";

    navigator.SetEnabled(false);
    input = NavigationInput{};
    input.submit = true;
    Frame(input);
    EXPECT_EQ(clicks, 1) << "input off";
    navigator.Submit();
    EXPECT_EQ(clicks, 2) << "a script still can";
}

TEST_F(NavigationTest, AClickMovesTheSelectionToWhatWasClicked)
{
    const auto a = AddButton("A", 100.0f, 300.0f);
    const auto b = AddButton("B", 100.0f, 200.0f);
    navigator.SetSelected(a.get());
    NavigationInput input;
    input.pointerPressed = b->FindChild("Label").get(); // on something inside the button
    Frame(input);
    EXPECT_EQ(navigator.GetSelected(), b.get());
    input.pointerPressed = canvas.get();
    Frame(input);
    EXPECT_EQ(navigator.GetSelected(), b.get()) << "a click elsewhere keeps the selection";
}

TEST_F(NavigationTest, ASelectedSliderTakesTheArrowsAlongItsBar)
{
    const auto sliderObject = UISystem::CreateSlider("Volume"); // in the middle of the canvas
    canvas->AddChild(sliderObject, false);
    const auto below = AddButton("Below", 400.0f, 100.0f);
    auto *slider = sliderObject->GetComponent<Slider>();
    slider->SetMaxValue(10.0f);
    slider->SetValue(5.0f);
    navigator.SetSelected(sliderObject.get());

    const auto *handle = slider->GetHandleRect()->GetGameObject().GetComponent<Image>();
    EXPECT_EQ(handle->GetTint(), slider->GetSelectedColor()) << "the handle shows the selection";

    Press(&NavigationInput::right);
    EXPECT_FLOAT_EQ(slider->GetValue(), 6.0f);
    Press(&NavigationInput::left);
    Press(&NavigationInput::left);
    EXPECT_FLOAT_EQ(slider->GetValue(), 4.0f);
    slider->SetDirection(SliderDirection::RightToLeft);
    Press(&NavigationInput::right);
    EXPECT_FLOAT_EQ(slider->GetValue(), 3.0f) << "reversed";
    slider->SetDirection(SliderDirection::LeftToRight);
    slider->SetWholeNumbers(true);
    slider->SetMaxValue(100.0f);
    Press(&NavigationInput::right);
    EXPECT_FLOAT_EQ(slider->GetValue(), 4.0f) << "by one with whole numbers";

    Press(&NavigationInput::down); // across the bar: moves on
    EXPECT_EQ(navigator.GetSelected(), below.get());
    EXPECT_EQ(handle->GetTint(), Common::Color::White);
}

TEST_F(NavigationTest, MovingInsideAScrollViewScrollsToTheSelection)
{
    const auto view = UISystem::CreateScrollView("List"); // 300 x 200, content 400 high
    canvas->AddChild(view, false);
    auto *scrollView = view->GetComponent<ScrollView>();
    const GameObject::Ptr content = view->FindChild("Content");
    std::vector<GameObject::Ptr> items;
    for (int i = 0; i < 8; ++i)
    {
        auto item = UISystem::CreateButton("Item" + std::to_string(i), "Item");
        auto *rectTransform = item->GetComponent<RectTransform>();
        rectTransform->SetAnchorMin(Vector2{0.0f, 1.0f});
        rectTransform->SetAnchorMax(Vector2{0.0f, 1.0f});
        rectTransform->SetPivot(Vector2{0.0f, 1.0f});
        rectTransform->SetAnchoredPosition(Vector2{10.0f, -50.0f * static_cast<float>(i)});
        rectTransform->SetSizeDelta(Vector2{200.0f, 40.0f});
        content->AddChild(item, false);
        items.push_back(item);
    }
    navigator.SetSelected(items[0].get());
    for (int i = 1; i < 8; ++i)
    {
        Press(&NavigationInput::down);
        ASSERT_EQ(navigator.GetSelected(), items[static_cast<std::size_t>(i)].get()) << i;
        static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));
        const Rect &item = items[static_cast<std::size_t>(i)]->GetComponent<RectTransform>()->GetRect();
        const Rect window = scrollView->GetViewRect();
        EXPECT_GE(item.YMin(), window.YMin() - 0.01f) << i;
        EXPECT_LE(item.YMax(), window.YMax() + 0.01f) << i;
    }
    EXPECT_LT(scrollView->GetNormalizedPosition().y, 0.5f) << "scrolled down";
    for (int i = 6; i >= 0; --i)
    {
        Press(&NavigationInput::up);
    }
    EXPECT_EQ(navigator.GetSelected(), items[0].get());
    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));
    EXPECT_NEAR(scrollView->GetNormalizedPosition().y, 1.0f, 0.01f) << "back at the top";
}

TEST_F(NavigationTest, AWorldCanvasKeepsTheSelectionOnItself)
{
    const auto overlay = AddButton("Overlay", 100.0f, 100.0f);
    const auto world = UISystem::CreateCanvas("World", CanvasRenderMode::WorldSpace);
    scene->AddRootGameObject(world);
    const auto first = AddButton("First", 20.0f, 20.0f, world);
    const auto second = AddButton("Second", 20.0f, 80.0f, world);
    navigator.SetSelected(first.get());
    Press(&NavigationInput::next);
    EXPECT_EQ(navigator.GetSelected(), second.get());
    Press(&NavigationInput::next);
    EXPECT_EQ(navigator.GetSelected(), first.get());
    EXPECT_EQ(UINavigator::CollectSelectables(*scene, Viewport), (std::vector<GameObject *>{overlay.get()}));
}

TEST_F(NavigationTest, TheSettingsAreSaved)
{
    const auto a = AddButton("A", 100.0f, 300.0f);
    const auto b = AddButton("B", 100.0f, 200.0f);
    auto *navigation = a->AddComponent<Navigation>();
    navigation->SetMode(NavigationMode::Explicit);
    navigation->SetNeighbor(NavigationDirection::Up, b->GetComponent<RectTransform>());
    const nlohmann::json saved = navigation->Serialize();
    EXPECT_TRUE(saved.dump().find("Explicit") != std::string::npos) << saved.dump();
    a->GetComponent<Button>()->SetSelectedColor(Common::Color{1.0f, 0.0f, 0.0f, 1.0f});
    const nlohmann::json button = a->GetComponent<Button>()->Serialize();
    b->GetComponent<Button>()->Deserialize(button);
    EXPECT_EQ(b->GetComponent<Button>()->GetSelectedColor(), (Common::Color{1.0f, 0.0f, 0.0f, 1.0f}));
}
