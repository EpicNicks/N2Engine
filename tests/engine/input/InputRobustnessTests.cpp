#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <nlohmann/json.hpp>

#include "engine/Window.hpp"
#include "engine/base/EventHandler.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/input/InputBindingFactory.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/input/InputValue.hpp"
#include "engine/input/Mouse.hpp"

using namespace N2Engine;
using namespace N2Engine::Input;
using Math::Vector2;
using json = nlohmann::json;

namespace
{
    // A binding with a fixed value, so actions can be driven without a window or gamepad
    class FakeBinding final : public InputBinding
    {
    public:
        explicit FakeBinding(InputValue value) : InputBinding(static_cast<GLFWwindow *>(nullptr)), value(value) {}
        InputValue getValue() override { return value; }
        [[nodiscard]] BindingType GetType() const override { return BindingType::KeyboardButton; }
        [[nodiscard]] json Serialize() const override { return json::object(); }
        InputValue value;
    };

    InputAction ActionWith(std::vector<InputValue> values)
    {
        InputAction action("Test");
        for (const auto &v : values)
        {
            action.AddBinding(std::make_unique<FakeBinding>(v));
        }
        return action;
    }
}

// ============================================================================
// EventHandler: subscribe/unsubscribe during dispatch
// ============================================================================

TEST(EventHandlerTest, HandlerCanUnsubscribeItself)
{
    Base::EventHandler<> event;
    int onceCalls = 0, otherCalls = 0;
    size_t onceId = 0;
    onceId = event += [&] { ++onceCalls; event -= onceId; }; // the "fire once" idiom
    event += [&] { ++otherCalls; };

    event();
    event();

    EXPECT_EQ(onceCalls, 1);
    EXPECT_EQ(otherCalls, 2) << "removing one subscriber mid-dispatch skipped the next";
    EXPECT_EQ(event.GetSubscriberCount(), 1u);
}

TEST(EventHandlerTest, HandlerCanSubscribeAnother)
{
    Base::EventHandler<> event;
    int addedCalls = 0;
    event += [&] { event += [&] { ++addedCalls; }; };

    event(); // adds one; it isn't called during this dispatch
    EXPECT_EQ(addedCalls, 0);

    event(); // calls it once, adds another
    EXPECT_EQ(addedCalls, 1);
}

TEST(EventHandlerTest, HandlerCanRemoveALaterOne)
{
    Base::EventHandler<int> event;
    int laterCalls = 0;
    size_t laterId = 0;
    event += [&](int) { event -= laterId; };
    laterId = event += [&](int) { ++laterCalls; };

    event(1);

    EXPECT_EQ(laterCalls, 0);
    EXPECT_EQ(event.GetSubscriberCount(), 1u);
}

// ============================================================================
// Combining binding values
// ============================================================================

TEST(InputActionValueTest, SmallStickDeflectionStaysSmall)
{
    InputAction action = ActionWith({Vector2(0.05f, 0.0f)});
    action.Update();

    // Used to become `true`, which reads as a full-strength (1, 0)
    EXPECT_NEAR(action.GetVector2Value().x, 0.05f, 1e-6f);
    EXPECT_NEAR(action.GetVector2Value().y, 0.0f, 1e-6f);
    EXPECT_TRUE(action.GetBoolValue());
}

TEST(InputActionValueTest, VectorBindingsAddUpWithinUnitCircle)
{
    InputAction action = ActionWith({Vector2(0.3f, 0.0f), Vector2(0.0f, 0.4f)});
    action.Update();
    EXPECT_NEAR(action.GetVector2Value().x, 0.3f, 1e-6f);
    EXPECT_NEAR(action.GetVector2Value().y, 0.4f, 1e-6f);

    InputAction saturated = ActionWith({Vector2(1.0f, 0.0f), Vector2(1.0f, 0.0f)});
    saturated.Update();
    EXPECT_NEAR(saturated.GetVector2Value().Length(), 1.0f, 1e-5f);
}

TEST(InputActionValueTest, AxisValueKeepsItsSign)
{
    InputAction action = ActionWith({0.2f, -0.6f});
    action.Update();
    EXPECT_FLOAT_EQ(action.GetFloatValue(), -0.6f);
}

TEST(InputActionValueTest, ButtonsAreBooleans)
{
    InputAction released = ActionWith({false, false});
    released.Update();
    EXPECT_FALSE(released.GetBoolValue());

    InputAction pressed = ActionWith({false, true});
    pressed.Update();
    EXPECT_TRUE(pressed.GetBoolValue());
    EXPECT_EQ(pressed.GetPhase(), ActionPhase::Started);
}

// ============================================================================
// Gamepad axes
// ============================================================================

TEST(AxisBindingTest, ReleasedTriggerReadsZero)
{
    // GLFW reports triggers as -1 at rest; that used to count as held
    EXPECT_FLOAT_EQ(AxisBinding::NormalizeAxis(GamepadAxis::LeftTrigger, -1.0f), 0.0f);
    EXPECT_FLOAT_EQ(AxisBinding::NormalizeAxis(GamepadAxis::RightTrigger, 1.0f), 1.0f);
    EXPECT_GT(AxisBinding::NormalizeAxis(GamepadAxis::RightTrigger, 0.0f), 0.0f); // half pulled
}

TEST(AxisBindingTest, StickAxisHasDeadzoneAndKeepsRange)
{
    EXPECT_FLOAT_EQ(AxisBinding::NormalizeAxis(GamepadAxis::LeftX, 0.05f), 0.0f);  // drift
    EXPECT_FLOAT_EQ(AxisBinding::NormalizeAxis(GamepadAxis::LeftX, -0.05f), 0.0f);
    EXPECT_FLOAT_EQ(AxisBinding::NormalizeAxis(GamepadAxis::LeftX, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(AxisBinding::NormalizeAxis(GamepadAxis::LeftX, -1.0f), -1.0f);
}

// ============================================================================
// Mouse
// ============================================================================

TEST(MouseTest, ScrollIsVisibleForTheFrameAfterItArrives)
{
    Mouse mouse(nullptr); // no window: position stays zero, scroll still works

    mouse.AccumulateScroll(0.0f, 1.0f); // scroll callbacks arrive during glfwPollEvents
    mouse.AccumulateScroll(0.0f, 2.0f);
    mouse.Update();                     // then InputSystem::Update runs

    EXPECT_FLOAT_EQ(mouse.GetScrollDelta().y, 3.0f) << "Update zeroed the scroll it should publish";

    mouse.Update();
    EXPECT_FLOAT_EQ(mouse.GetScrollDelta().y, 0.0f);
}

// ============================================================================
// InputSystem (headless: an uninitialized Window has no GLFW window)
// ============================================================================

class InputSystemHeadlessTest : public ::testing::Test
{
protected:
    Window _window;
    InputSystem _input{_window};

    static json EmptyMap() { return {{"actions", json::object()}}; }
};

TEST_F(InputSystemHeadlessTest, GetActionMapDoesNotSwitchTheActiveMap)
{
    ASSERT_NE(_input.CreateActionMapFromJson("Gameplay", EmptyMap()), nullptr);
    ASSERT_NE(_input.CreateActionMapFromJson("Menu", EmptyMap()), nullptr);
    ASSERT_NE(_input.LoadActionMap("Gameplay"), nullptr);

    ASSERT_NE(_input.GetActionMap("Menu"), nullptr);

    ASSERT_NE(_input.GetCurActionMap(), nullptr);
    EXPECT_EQ(_input.GetCurActionMap()->name, "Gameplay");
}

TEST_F(InputSystemHeadlessTest, ReplacingActiveMapFromItsOwnCallbackIsSafe)
{
    _input.MakeActionMap("Live", [](ActionMap *map)
    {
        map->MakeInputAction("Poke", [](InputAction *action)
        {
            action->AddBinding(std::make_unique<FakeBinding>(true));
        });
    });
    _input.LoadActionMap("Live");

    bool replaced = false;
    (*_input.GetActionMap("Live"))["Poke"].GetOnStateChanged() += [&](InputAction &)
    {
        // Frees the map whose Update is running, unless it's retired until the update ends
        _input.CreateActionMapFromJson("Live", EmptyMap());
        replaced = true;
    };

    _input.Update();

    EXPECT_TRUE(replaced);
    EXPECT_EQ(_input.GetActionMap("Live")->Serialize()["actions"].size(), 0u);
    _input.Update(); // the replacement runs normally
}

TEST_F(InputSystemHeadlessTest, RemovingActionsFromACallbackIsSafe)
{
    _input.MakeActionMap("Edit", [](ActionMap *map)
    {
        map->MakeInputAction("First", [](InputAction *action) { action->AddBinding(std::make_unique<FakeBinding>(true)); });
        map->MakeInputAction("Second", [](InputAction *action) { action->AddBinding(std::make_unique<FakeBinding>(true)); });
    });
    _input.LoadActionMap("Edit");
    ActionMap *map = _input.GetActionMap("Edit");

    auto removeOthers = [map](InputAction &self)
    {
        map->RemoveInputAction(self.GetName() == "First" ? "Second" : "First");
    };
    (*map)["First"].GetOnStateChanged() += removeOthers;
    (*map)["Second"].GetOnStateChanged() += removeOthers;

    EXPECT_NO_FATAL_FAILURE(_input.Update());
    EXPECT_EQ(map->Serialize()["actions"].size(), 1u);
}

// ============================================================================
// Binding names
// ============================================================================

TEST(BindingValidationTest, UnknownNamesAreRejected)
{
    // Unknown names used to silently become the enum's first value (e.g. a mistyped key became Unknown)
    auto badKey = CreateBindingFromJson(nullptr, {{"type", "KeyboardButton"}, {"key", "Escpae"}});
    ASSERT_FALSE(badKey.has_value());
    EXPECT_EQ(badKey.error(), BindingParseError::InvalidValue);

    auto badType = CreateBindingFromJson(nullptr, {{"type", "Keyboard"}, {"key", "Escape"}});
    ASSERT_FALSE(badType.has_value());
    EXPECT_EQ(badType.error(), BindingParseError::InvalidType);

    auto badAxis = CreateBindingFromJson(nullptr, {{"type", "GamepadStick"}, {"xAxis", "LeftX"}, {"yAxis", "Sideways"}});
    ASSERT_FALSE(badAxis.has_value());
    EXPECT_EQ(badAxis.error(), BindingParseError::InvalidValue);

    auto badComposite = CreateBindingFromJson(nullptr, {
        {"type", "Vector2Composite"}, {"up", "W"}, {"down", "S"}, {"left", "A"}, {"right", "Dee"}});
    ASSERT_FALSE(badComposite.has_value());
    EXPECT_EQ(badComposite.error(), BindingParseError::InvalidValue);

    EXPECT_FALSE(BindingParseErrorToString(BindingParseError::InvalidValue).empty());
}

TEST(BindingValidationTest, KnownNamesStillWork)
{
    EXPECT_TRUE(CreateBindingFromJson(nullptr, {{"type", "KeyboardButton"}, {"key", "Unknown"}}).has_value())
        << "the first enum value is still a valid name";
    EXPECT_TRUE(CreateBindingFromJson(nullptr, {{"type", "GamepadAxis"}, {"axis", "LeftTrigger"}}).has_value());
    EXPECT_TRUE(CreateBindingFromJson(nullptr, {{"type", "MouseButton"}, {"button", "Left"}}).has_value());
}
