#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Vector2.hpp>

#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/common/Color.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/input/PointerDispatcher.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

// UI::Button driven by a PointerDispatcher with injected pointer input over a real UI hit test: the state
// machine, the tint on its target graphic, OnClick and its listeners, and serialization

using namespace N2Engine;
using namespace N2Engine::UI;
using Math::Vector2;

namespace
{
    constexpr float Tolerance = 1e-4f;
    const Vector2i Viewport{800, 600};

    void ExpectColor(const Common::Color &actual, const Common::Color &expected, const char *what = "")
    {
        EXPECT_NEAR(actual.r, expected.r, Tolerance) << what << " r";
        EXPECT_NEAR(actual.g, expected.g, Tolerance) << what << " g";
        EXPECT_NEAR(actual.b, expected.b, Tolerance) << what << " b";
        EXPECT_NEAR(actual.a, expected.a, Tolerance) << what << " a";
    }

    Common::Color Multiply(const Common::Color &a, const Common::Color &b)
    {
        return Common::Color{a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a};
    }

    /// Collects the messages logged while it lives
    class LogCapture
    {
    public:
        LogCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, Logger::LogLevel)
            {
                messages.emplace_back(message);
            };
        }
        ~LogCapture() { Logger::logEvent -= _id; }
        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        [[nodiscard]] bool Contains(const std::string &text) const
        {
            for (const std::string &message : messages)
            {
                if (message.find(text) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        std::vector<std::string> messages;

    private:
        size_t _id = 0;
    };
}

// A canvas with one button element covering canvas x 0..100, y 0..100, which is window x 0..100, y 500..600.
// Nothing is under the pointer outside it (the world provider hits nothing). Fades are off unless a test
// turns them on.
class ButtonTest : public ::testing::Test
{
protected:
    std::unique_ptr<Scene> _scene = Scene::Create("ButtonTest");
    Input::PointerDispatcher _dispatcher;
    Input::Mouse _mouse{nullptr};
    GameObject::Ptr _canvas;
    GameObject::Ptr _buttonObject;
    Image *_image = nullptr;
    Button *_button = nullptr;
    int _clicks = 0;

    const Common::Color _base{0.5f, 0.8f, 1.0f, 0.9f};
    const Vector2 _over{50.0f, 550.0f};
    const Vector2 _outside{400.0f, 100.0f};

    void SetUp() override
    {
        _canvas = UISystem::CreateCanvas("Canvas");
        _scene->AddRootGameObject(_canvas);

        _buttonObject = UISystem::CreateElement("Button");
        auto *rectTransform = _buttonObject->GetComponent<RectTransform>();
        rectTransform->SetAnchorMin(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchorMax(Vector2{0.0f, 0.0f});
        rectTransform->SetPivot(Vector2{0.0f, 0.0f});
        rectTransform->SetAnchoredPosition(Vector2{0.0f, 0.0f});
        rectTransform->SetSizeDelta(Vector2{100.0f, 100.0f});
        _image = _buttonObject->AddComponent<Image>();
        _image->SetColor(_base);
        _button = _buttonObject->AddComponent<Button>();
        _button->SetFadeDuration(0.0f);
        _canvas->AddChild(_buttonObject, false);

        _button->AddOnClick([this] { ++_clicks; });

        _dispatcher.SetUIHitProvider([this](const Vector2 &point)
        {
            return UISystem::HitTestWindowPoint(*_scene, point, Viewport);
        });
        _dispatcher.SetWorldHitProvider([](const Vector2 &) -> GameObject * { return nullptr; });
    }

    void Frame(const Vector2 &windowPoint, const bool held)
    {
        _mouse.InjectPointer(windowPoint, held ? Input::Mouse::ButtonBit(0) : 0u);
        _mouse.Update();
        _dispatcher.Process(Input::PointerState::FromMouse(_mouse));
    }

    void ExpectTint(const Common::Color &stateColor, const char *what)
    {
        ExpectColor(_image->GetDrawColor(), Multiply(_base, stateColor), what);
        ExpectColor(_image->GetColor(), _base, "the image's own colour never changes");
    }
};

TEST_F(ButtonTest, HoverHighlightsPressTintsAndReleaseOverItClicksOnce)
{
    EXPECT_EQ(_button->GetState(), Button::State::Normal);
    ExpectColor(_image->GetDrawColor(), _base, "normal (white) leaves the colour as it is");
    EXPECT_EQ(_button->GetTargetGraphic(), _image) << "the first graphic on the object by default";

    Frame(_over, false);
    EXPECT_EQ(_button->GetState(), Button::State::Highlighted);
    ExpectTint(Button::DefaultHighlightedColor, "hover");

    Frame(_over, true);
    EXPECT_EQ(_button->GetState(), Button::State::Pressed);
    ExpectTint(Button::DefaultPressedColor, "press");
    EXPECT_EQ(_clicks, 0);

    Frame(_over, true);
    EXPECT_EQ(_button->GetState(), Button::State::Pressed) << "held";

    Frame(_over, false);
    EXPECT_EQ(_clicks, 1);
    EXPECT_EQ(_button->GetState(), Button::State::Highlighted) << "released while still over it";
    ExpectTint(Button::DefaultHighlightedColor, "after release");

    Frame(_over, false);
    EXPECT_EQ(_clicks, 1) << "one click per press";
}

TEST_F(ButtonTest, PressAndHoverInTheSameFrameShowPressed)
{
    Frame(_over, true); // Down comes before Enter
    EXPECT_EQ(_button->GetState(), Button::State::Pressed);
    ExpectTint(Button::DefaultPressedColor, "press");
}

TEST_F(ButtonTest, ReleaseOutsideFiresNoClick)
{
    Frame(_over, false);
    Frame(_over, true);
    Frame(_outside, true);
    EXPECT_EQ(_button->GetState(), Button::State::Normal) << "dragged off: pressed but not over it";
    ExpectTint(Button::DefaultNormalColor, "dragged off");

    Frame(_outside, false);
    EXPECT_EQ(_clicks, 0);
    EXPECT_EQ(_button->GetState(), Button::State::Normal);
    ExpectColor(_image->GetDrawColor(), _base, "back to the image's own colour");
}

TEST_F(ButtonTest, DraggingOffAndBackOnBeforeReleaseClicksOnce)
{
    Frame(_over, true);
    Frame(_outside, true);
    Frame(_over, true);
    EXPECT_EQ(_button->GetState(), Button::State::Pressed) << "back over it, still held";
    Frame(_over, false);
    EXPECT_EQ(_clicks, 1);
}

TEST_F(ButtonTest, PressOutsideThenReleaseOverItIsNoClick)
{
    Frame(_outside, true);
    Frame(_over, true);
    EXPECT_EQ(_button->GetState(), Button::State::Highlighted) << "the press went down elsewhere";
    Frame(_over, false);
    EXPECT_EQ(_clicks, 0);
}

TEST_F(ButtonTest, NonInteractableShowsDisabledAndIgnoresClicks)
{
    _button->SetInteractable(false);
    EXPECT_EQ(_button->GetState(), Button::State::Disabled);
    ExpectTint(Button::DefaultDisabledColor, "disabled");

    Frame(_over, false);
    Frame(_over, true);
    EXPECT_EQ(_button->GetState(), Button::State::Disabled);
    ExpectTint(Button::DefaultDisabledColor, "still disabled while pressed");
    Frame(_over, false);
    EXPECT_EQ(_clicks, 0);
    EXPECT_FALSE(_button->Click());
    EXPECT_EQ(_clicks, 0);

    // Turned back on while hovered: it followed the pointer all along
    _button->SetInteractable(true);
    EXPECT_EQ(_button->GetState(), Button::State::Highlighted);
    ExpectTint(Button::DefaultHighlightedColor, "interactable again");
    Frame(_over, true);
    Frame(_over, false);
    EXPECT_EQ(_clicks, 1);
}

TEST_F(ButtonTest, DisabledComponentDoesNothing)
{
    _button->SetActive(false);
    Frame(_over, false);
    Frame(_over, true);
    EXPECT_EQ(_button->GetState(), Button::State::Normal);
    ExpectColor(_image->GetDrawColor(), _base, "no tint change");
    Frame(_over, false);
    EXPECT_EQ(_clicks, 0);
    EXPECT_FALSE(_button->Click());
    EXPECT_EQ(_clicks, 0);

    // Enabled again: the pointer must come onto it again to highlight it (as in Unity)
    _button->SetActive(true);
    EXPECT_EQ(_button->GetState(), Button::State::Normal);
    Frame(_outside, false);
    Frame(_over, false);
    EXPECT_EQ(_button->GetState(), Button::State::Highlighted);
    Frame(_over, true);
    Frame(_over, false);
    EXPECT_EQ(_clicks, 1);
}

TEST_F(ButtonTest, ADisabledButtonClearsItsTintAndChangesShowWhenEnabled)
{
    _button->SetFadeDuration(1.0f);
    _button->SetInteractable(false);
    _button->UpdateFade(1.0f);
    ExpectTint(Button::DefaultDisabledColor, "not interactable");

    // Disabled: no tint at all, as Unity clears a disabled Selectable's
    _button->SetActive(false);
    ExpectColor(_image->GetDrawColor(), _base, "disabled component, no tint");

    // A change while disabled doesn't start a fade that nothing would advance
    _button->SetInteractable(true);
    EXPECT_FALSE(_button->IsFading());
    ExpectColor(_image->GetDrawColor(), _base, "still untinted");

    _button->SetActive(true);
    EXPECT_FALSE(_button->IsFading());
    ExpectTint(Button::DefaultNormalColor, "enabled again");
}

TEST_F(ButtonTest, AButtonFreedWithoutOnDestroyUntintsItsGraphic)
{
    // The target lives on another object; the button's own object is never in a scene, so freeing it
    // runs no OnDestroy, only the destructor
    auto target = UISystem::CreateElement("Target");
    auto *image = target->AddComponent<Image>();
    auto loose = UISystem::CreateElement("Loose");
    auto *button = loose->AddComponent<Button>();
    button->SetFadeDuration(0.0f);
    button->SetTargetGraphic(image);
    button->SetInteractable(false);
    ASSERT_FALSE(image->GetTint() == Common::Color::White);

    loose.reset();
    ExpectColor(image->GetTint(), Common::Color::White, "untinted when freed");
}

TEST_F(ButtonTest, DisablingWhilePressedForgetsThePress)
{
    Frame(_over, true);
    ASSERT_EQ(_button->GetState(), Button::State::Pressed);

    _button->SetActive(false);
    EXPECT_EQ(_button->GetState(), Button::State::Normal);
    ExpectColor(_image->GetDrawColor(), _base, "the tint goes back to normal at once");

    // Enabled again before the release: the press it forgot doesn't make a click
    _button->SetActive(true);
    Frame(_over, false);
    EXPECT_EQ(_clicks, 0);
}

TEST_F(ButtonTest, DeactivatingTheObjectWhileHoveredForgetsTheHover)
{
    Frame(_over, false);
    ASSERT_EQ(_button->GetState(), Button::State::Highlighted);

    _buttonObject->SetActive(false); // OnDisable; the dispatcher will send it no Exit
    EXPECT_EQ(_button->GetState(), Button::State::Normal);
    ExpectColor(_image->GetDrawColor(), _base, "tint cleared");
    Frame(_over, false);

    _buttonObject->SetActive(true);
    EXPECT_EQ(_button->GetState(), Button::State::Normal);
    Frame(_over, false); // the dispatcher dropped it, so this is a new Enter
    EXPECT_EQ(_button->GetState(), Button::State::Highlighted);
}

TEST_F(ButtonTest, TintMultipliesTheBaseColourAndRestoresIt)
{
    _button->SetHighlightedColor(Common::Color{0.5f, 0.5f, 0.5f, 1.0f});
    _button->SetColorMultiplier(2.0f);
    Frame(_over, false);
    ExpectColor(_button->GetCurrentTint(), Common::Color{1.0f, 1.0f, 1.0f, 2.0f}, "colour times multiplier");
    ExpectColor(_image->GetDrawColor(), Multiply(_base, Common::Color{1.0f, 1.0f, 1.0f, 2.0f}), "drawn");

    // Changing the image's colour while tinted keeps the tint on top of the new colour
    const Common::Color newBase{0.2f, 0.4f, 0.6f, 1.0f};
    _image->SetColor(newBase);
    ExpectColor(_image->GetDrawColor(), Multiply(newBase, Common::Color{1.0f, 1.0f, 1.0f, 2.0f}), "new colour");

    _button->SetColorMultiplier(1.0f);
    Frame(_outside, false);
    ExpectColor(_image->GetDrawColor(), newBase, "normal: the image's own colour, unchanged");
    ExpectColor(_image->GetColor(), newBase, "never overwritten");
}

TEST_F(ButtonTest, FadeMovesTheTintOverUnscaledTime)
{
    _button->SetFadeDuration(0.2f);
    Frame(_over, false);
    EXPECT_TRUE(_button->IsFading());
    ExpectColor(_button->GetCurrentTint(), Button::DefaultNormalColor, "starts from the old state's tint");

    _button->UpdateFade(0.1f);
    const Common::Color halfway{
        (Button::DefaultNormalColor.r + Button::DefaultHighlightedColor.r) * 0.5f,
        (Button::DefaultNormalColor.g + Button::DefaultHighlightedColor.g) * 0.5f,
        (Button::DefaultNormalColor.b + Button::DefaultHighlightedColor.b) * 0.5f,
        1.0f};
    ExpectColor(_button->GetCurrentTint(), halfway, "halfway");
    ExpectColor(_image->GetDrawColor(), Multiply(_base, halfway), "applied while fading");

    _button->UpdateFade(0.15f);
    EXPECT_FALSE(_button->IsFading());
    ExpectColor(_button->GetCurrentTint(), Button::DefaultHighlightedColor, "arrived");

    // Disabling the component jumps straight to the resting tint
    Frame(_over, true);
    EXPECT_TRUE(_button->IsFading());
    _button->SetActive(false);
    EXPECT_FALSE(_button->IsFading());
    ExpectColor(_button->GetCurrentTint(), Button::DefaultNormalColor, "instant on disable");
}

TEST_F(ButtonTest, OnUpdateLeavesATintSetByHandAloneWhileIdle)
{
    _button->OnUpdate(); // the button has tinted its graphic (white, Normal)
    ASSERT_EQ(_button->GetState(), Button::State::Normal);
    ASSERT_FALSE(_button->IsFading());

    const Common::Color byHand{0.25f, 0.5f, 0.75f, 1.0f};
    _image->SetTint(byHand);
    _button->OnUpdate();
    _button->OnUpdate();
    ExpectColor(_image->GetTint(), byHand, "no fade step and no new target: the button writes nothing");

    // The next state change writes the button's tint again
    Frame(_over, false);
    ExpectTint(Button::DefaultHighlightedColor, "hover");
}

TEST_F(ButtonTest, OnUpdateTintsAGraphicThatReplacesTheDefaultTarget)
{
    _button->SetInteractable(false);
    ExpectTint(Button::DefaultDisabledColor, "own image");

    ASSERT_TRUE(_buttonObject->RemoveComponent(_image));
    _image = _buttonObject->AddComponent<Image>();
    _image->SetColor(_base);
    ExpectColor(_image->GetTint(), Common::Color::White, "not tinted until the button sees it");

    _button->OnUpdate();
    EXPECT_EQ(_button->GetTargetGraphic(), _image);
    ExpectTint(Button::DefaultDisabledColor, "the new graphic is the target, and is tinted");
}

TEST_F(ButtonTest, ExplicitTargetGraphicIsTintedAndTheOldOneIsReleased)
{
    auto other = UISystem::CreateElement("Other");
    auto *otherImage = other->AddComponent<Image>();
    _canvas->AddChild(other, false);

    _button->SetInteractable(false);
    ExpectTint(Button::DefaultDisabledColor, "own image first");

    _button->SetTargetGraphic(otherImage);
    EXPECT_EQ(_button->GetTargetGraphic(), otherImage);
    ExpectColor(otherImage->GetTint(), Button::DefaultDisabledColor, "the new target");
    ExpectColor(_image->GetTint(), Common::Color::White, "the old target gets a white tint back");

    _button->SetTargetGraphic(nullptr);
    EXPECT_EQ(_button->GetTargetGraphic(), _image) << "back to the default";
    ExpectColor(otherImage->GetTint(), Common::Color::White, "released");
    ExpectTint(Button::DefaultDisabledColor, "own image again");
}

TEST_F(ButtonTest, DestroyedTargetGraphicDoesNotCrash)
{
    auto other = UISystem::CreateElement("Other");
    auto *otherImage = other->AddComponent<Image>();
    _canvas->AddChild(other, false);
    _button->SetTargetGraphic(otherImage);

    ASSERT_TRUE(other->RemoveComponent(otherImage)); // freed now
    EXPECT_EQ(_button->GetTargetGraphic(), nullptr) << "an explicit target doesn't fall back";
    Frame(_over, false);
    Frame(_over, true);
    _button->UpdateFade(0.1f);
    Frame(_over, false);
    EXPECT_EQ(_clicks, 1) << "clicks still work without a graphic to tint";

    // The default target removed: nothing to tint, and nothing touched
    _button->SetTargetGraphic(nullptr);
    ASSERT_TRUE(_buttonObject->RemoveComponent(_image));
    _image = nullptr;
    EXPECT_EQ(_button->GetTargetGraphic(), nullptr);
    _button->OnMouseEnter();
    _button->OnMouseDown();
    _button->OnMouseUpAsButton();
    _button->OnMouseUp();
    _button->OnMouseExit();
    EXPECT_EQ(_clicks, 2);
}

TEST_F(ButtonTest, DestroyingTheButtonResetsTheTint)
{
    Frame(_over, false);
    ASSERT_FALSE(_image->GetTint() == Common::Color::White);
    ASSERT_TRUE(_buttonObject->RemoveComponent<Button>());
    _button = nullptr;
    ExpectColor(_image->GetTint(), Common::Color::White, "white again");
    Frame(_over, true);
    Frame(_over, false); // no Button left to receive it
    EXPECT_EQ(_clicks, 0);
}

TEST_F(ButtonTest, ListenersMayAddAndRemoveListenersDuringAClick)
{
    std::vector<std::string> calls;
    Button::ListenerId selfRemoving = 0;
    selfRemoving = _button->AddOnClick([&]
    {
        calls.emplace_back("self");
        _button->RemoveOnClick(selfRemoving);
    });
    Button::ListenerId victim = 0;
    _button->AddOnClick([&]
    {
        calls.emplace_back("remover");
        _button->RemoveOnClick(victim);
        _button->AddOnClick([&] { calls.emplace_back("added"); });
    });
    victim = _button->AddOnClick([&] { calls.emplace_back("victim"); });

    ASSERT_TRUE(_button->Click());
    EXPECT_EQ(calls, (std::vector<std::string>{"self", "remover"}))
        << "a listener removed during the click isn't called; one added is first called next time";

    calls.clear();
    ASSERT_TRUE(_button->Click());
    EXPECT_EQ(calls, (std::vector<std::string>{"remover", "added"}));
    EXPECT_EQ(_clicks, 2);

    _button->ClearOnClick();
    EXPECT_EQ(_button->GetOnClickListenerCount(), 0u);
    calls.clear();
    EXPECT_TRUE(_button->Click());
    EXPECT_TRUE(calls.empty());
}

TEST_F(ButtonTest, ClearingFromAListenerStopsTheRest)
{
    _button->ClearOnClick();
    int later = 0;
    _button->AddOnClick([this] { _button->ClearOnClick(); });
    _button->AddOnClick([&later] { ++later; });
    _button->Click();
    EXPECT_EQ(later, 0);
    EXPECT_EQ(_button->GetOnClickListenerCount(), 0u);
}

TEST_F(ButtonTest, AThrowingListenerIsLoggedAndTheOthersRun)
{
    LogCapture log;
    _button->AddOnClick([] { throw std::runtime_error("listener failure"); });
    int after = 0;
    _button->AddOnClick([&after] { ++after; });

    Frame(_over, true);
    Frame(_over, false);
    EXPECT_EQ(_clicks, 1);
    EXPECT_EQ(after, 1);
    EXPECT_TRUE(log.Contains("listener failure"));
}

TEST_F(ButtonTest, AListenerMayDestroyTheButton)
{
    _button->ClearOnClick();
    int later = 0;
    _button->AddOnClick([this] { _buttonObject->RemoveComponent<Button>(); });
    _button->AddOnClick([&later] { ++later; });

    Frame(_over, true);
    Frame(_over, false); // the button is freed during its own OnMouseUpAsButton
    EXPECT_EQ(_buttonObject->GetComponent<Button>(), nullptr);
    EXPECT_EQ(later, 0) << "listeners after the destroy aren't called";
    ExpectColor(_image->GetTint(), Common::Color::White, "the tint is released");
    Frame(_over, false);
}

TEST_F(ButtonTest, DestroyingTheButtonReleasesItsListeners)
{
    // Whatever a listener holds (here a shared_ptr; from Lua, a function and its upvalues) is let go when
    // the button is destroyed, not kept until something else frees the component
    auto held = std::make_shared<int>(0);
    _button->AddOnClick([held] { ++*held; });
    EXPECT_EQ(held.use_count(), 2);

    _button->OnDestroy(); // what RemoveComponent and Destroy run first
    EXPECT_EQ(held.use_count(), 1);
    EXPECT_EQ(_button->GetOnClickListenerCount(), 0u);
    ASSERT_TRUE(_buttonObject->RemoveComponent<Button>());
    _button = nullptr;
}

TEST_F(ButtonTest, SceneRoundTripKeepsTheSettingsButNotTheListeners)
{
    _button->SetInteractable(false);
    _button->SetNormalColor(Common::Color{0.9f, 0.8f, 0.7f, 1.0f});
    _button->SetHighlightedColor(Common::Color{0.1f, 0.2f, 0.3f, 1.0f});
    _button->SetPressedColor(Common::Color{0.4f, 0.5f, 0.6f, 1.0f});
    _button->SetDisabledColor(Common::Color{0.3f, 0.3f, 0.3f, 0.25f});
    _button->SetColorMultiplier(1.5f);
    _button->SetFadeDuration(0.3f);

    const nlohmann::json saved = _buttonObject->Serialize();
    const auto loaded = GameObject::Deserialize(saved);
    ASSERT_NE(loaded, nullptr);
    auto *button = loaded->GetComponent<Button>();
    ASSERT_NE(button, nullptr);
    EXPECT_FALSE(button->IsInteractable());
    ExpectColor(button->GetNormalColor(), Common::Color{0.9f, 0.8f, 0.7f, 1.0f}, "normal");
    ExpectColor(button->GetHighlightedColor(), Common::Color{0.1f, 0.2f, 0.3f, 1.0f}, "highlighted");
    ExpectColor(button->GetPressedColor(), Common::Color{0.4f, 0.5f, 0.6f, 1.0f}, "pressed");
    ExpectColor(button->GetDisabledColor(), Common::Color{0.3f, 0.3f, 0.3f, 0.25f}, "disabled");
    EXPECT_FLOAT_EQ(button->GetColorMultiplier(), 1.5f);
    EXPECT_FLOAT_EQ(button->GetFadeDuration(), 0.3f);
    EXPECT_EQ(button->GetOnClickListenerCount(), 0u) << "listeners are not serialized";
    EXPECT_EQ(button->GetTargetGraphic(), loaded->GetComponent<Image>());

    // Attached to a scene, the loaded settings show at once
    button->OnAttach();
    EXPECT_EQ(button->GetState(), Button::State::Disabled);
    ExpectColor(loaded->GetComponent<Image>()->GetTint(), Common::Color{0.45f, 0.45f, 0.45f, 0.375f},
                "disabled colour times the multiplier");

    EXPECT_TRUE(ComponentRegistry::Instance().IsRegistered("Button"));
}

TEST(ButtonHelperTest, CreateButtonMakesAnImageAButtonAndACentredLabel)
{
    const auto buttonObject = UISystem::CreateButton("Play", "Start");
    ASSERT_NE(buttonObject, nullptr);
    EXPECT_EQ(buttonObject->GetName(), "Play");
    auto *image = buttonObject->GetComponent<Image>();
    auto *button = buttonObject->GetComponent<Button>();
    ASSERT_NE(image, nullptr);
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(image->GetRaycastTarget());
    EXPECT_EQ(button->GetTargetGraphic(), image);

    ASSERT_EQ(buttonObject->GetChildCount(), 1u);
    const auto label = buttonObject->GetChild(0);
    auto *text = label->GetComponent<UIText>();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->GetText(), "Start");
    EXPECT_FALSE(text->GetRaycastTarget()) << "the label lets the pointer through to the button";
    EXPECT_EQ(text->GetHorizontalAlign(), Text::HorizontalAlign::Center);
    EXPECT_EQ(text->GetVerticalAlign(), Text::VerticalAlign::Middle);

    // The label covers the button's rect
    const auto canvas = UISystem::CreateCanvas();
    canvas->AddChild(buttonObject, false);
    const auto scene = Scene::Create("ButtonHelper");
    scene->AddRootGameObject(canvas);
    static_cast<void>(UISystem::CollectGraphics(*scene, Viewport));
    const Rect buttonRect = buttonObject->GetComponent<RectTransform>()->GetRect();
    const Rect labelRect = label->GetComponent<RectTransform>()->GetRect();
    EXPECT_NEAR(buttonRect.width, 160.0f, Tolerance);
    EXPECT_NEAR(buttonRect.height, 40.0f, Tolerance);
    EXPECT_NEAR(labelRect.x, buttonRect.x, Tolerance);
    EXPECT_NEAR(labelRect.y, buttonRect.y, Tolerance);
    EXPECT_NEAR(labelRect.width, buttonRect.width, Tolerance);
    EXPECT_NEAR(labelRect.height, buttonRect.height, Tolerance);
}
