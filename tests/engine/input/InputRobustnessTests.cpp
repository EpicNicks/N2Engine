#include <gtest/gtest.h>

#include <engine/input/InputDevice.hpp>

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
        explicit FakeBinding(InputValue value) : InputBinding(static_cast<N2Engine::Input::InputDevice *>(nullptr)), value(value) {}
        InputValue getValue() override { return value; }
        [[nodiscard]] BindingType GetType() const override { return BindingType::KeyboardButton; }
        [[nodiscard]] json Serialize() const override { return json::object(); }
        InputValue value;
    };

    // Heap-held, as everywhere else: actions can't be moved (script handles stay tied to one object)
    std::unique_ptr<InputAction> ActionWith(std::vector<InputValue> values)
    {
        auto action = std::make_unique<InputAction>("Test");
        for (const auto &v : values)
        {
            action->AddBinding(std::make_unique<FakeBinding>(v));
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
    const auto action = ActionWith({Vector2(0.05f, 0.0f)});
    action->Update();

    // Used to become `true`, which reads as a full-strength (1, 0)
    EXPECT_NEAR(action->GetVector2Value().x, 0.05f, 1e-6f);
    EXPECT_NEAR(action->GetVector2Value().y, 0.0f, 1e-6f);
    EXPECT_TRUE(action->GetBoolValue());
}

TEST(InputActionValueTest, VectorBindingsAddUpWithinUnitCircle)
{
    const auto action = ActionWith({Vector2(0.3f, 0.0f), Vector2(0.0f, 0.4f)});
    action->Update();
    EXPECT_NEAR(action->GetVector2Value().x, 0.3f, 1e-6f);
    EXPECT_NEAR(action->GetVector2Value().y, 0.4f, 1e-6f);

    const auto saturated = ActionWith({Vector2(1.0f, 0.0f), Vector2(1.0f, 0.0f)});
    saturated->Update();
    EXPECT_NEAR(saturated->GetVector2Value().Length(), 1.0f, 1e-5f);
}

TEST(InputActionValueTest, AxisValueKeepsItsSign)
{
    const auto action = ActionWith({0.2f, -0.6f});
    action->Update();
    EXPECT_FLOAT_EQ(action->GetFloatValue(), -0.6f);
}

TEST(InputActionValueTest, ButtonsAreBooleans)
{
    const auto released = ActionWith({false, false});
    released->Update();
    EXPECT_FALSE(released->GetBoolValue());

    const auto pressed = ActionWith({false, true});
    pressed->Update();
    EXPECT_TRUE(pressed->GetBoolValue());
    EXPECT_EQ(pressed->GetPhase(), ActionPhase::Started);
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

TEST_F(InputSystemHeadlessTest, ReloadingBindingsFromACallbackIsSafe)
{
    _input.MakeActionMap("Live", [](ActionMap *map)
    {
        map->MakeInputAction("Poke", [](InputAction *action)
        {
            action->AddBinding(std::make_unique<FakeBinding>(true));
        });
    });
    _input.LoadActionMap("Live");

    bool reloaded = false;
    (*_input.GetActionMap("Live"))["Poke"].GetOnStateChanged() += [&](InputAction &)
    {
        // Deserialize used to free every map immediately, including the one whose Update is running
        reloaded = _input.Deserialize({{"actionMaps", {{"Live", EmptyMap()}}}});
    };

    _input.Update();

    EXPECT_TRUE(reloaded);
    ASSERT_NE(_input.GetActionMap("Live"), nullptr);
    EXPECT_EQ(_input.GetActionMap("Live")->Serialize()["actions"].size(), 0u);
    _input.Update();
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

// ============================================================================
// Optional fields of the wrong type
// ============================================================================

TEST(BindingValidationTest, WrongTypedOptionalFieldIsAParseErrorNotAnException)
{
    // json::value used to throw nlohmann::json::type_error for these, out of every parser on the path
    const std::vector<json> bad = {
        {{"type", "GamepadStick"}, {"xAxis", "LeftX"}, {"yAxis", "LeftY"}, {"deadzone", "0.2"}},
        {{"type", "GamepadStick"}, {"xAxis", "LeftX"}, {"yAxis", "LeftY"}, {"invertY", 1}},
        {{"type", "GamepadStick"}, {"xAxis", "LeftX"}, {"yAxis", "LeftY"}, {"invertX", nullptr}},
        {{"type", "GamepadStick"}, {"xAxis", "LeftX"}, {"yAxis", "LeftY"}, {"gamepadId", "0"}},
        {{"type", "GamepadAxis"}, {"axis", "LeftX"}, {"gamepadId", true}},
        {{"type", "GamepadAxis"}, {"axis", "LeftX"}, {"gamepadId", 1.5}},
        {{"type", "GamepadButton"}, {"button", "South"}, {"gamepadId", -1}},
        {{"type", "GamepadButton"}, {"button", "South"}, {"gamepadId", 3000000000u}}, // above INT_MAX
    };
    for (const auto &binding : bad)
    {
        std::expected<std::unique_ptr<InputBinding>, BindingParseError> result;
        EXPECT_NO_THROW(result = CreateBindingFromJson(nullptr, binding)) << binding.dump();
        ASSERT_FALSE(result.has_value()) << binding.dump();
        EXPECT_EQ(result.error(), BindingParseError::InvalidOptionalField) << binding.dump();
    }
    EXPECT_FALSE(BindingParseErrorToString(BindingParseError::InvalidOptionalField).empty());

    // Right types (whole numbers for numbers included) still parse
    EXPECT_TRUE(CreateBindingFromJson(nullptr, {{"type", "GamepadStick"}, {"xAxis", "LeftX"}, {"yAxis", "LeftY"},
                                                {"gamepadId", 1}, {"deadzone", 0}, {"invertX", true}})
                    .has_value());
    EXPECT_TRUE(CreateBindingFromJson(nullptr, {{"type", "GamepadButton"}, {"button", "South"}, {"gamepadId", 2u}})
                    .has_value());
}

// Not ActionMapSerializationTest: that suite is a fixture (TEST_F) in InputSystemTests.cpp
TEST(ActionMapValidationTest, WrongTypedOptionalFieldsDoNotThrow)
{
    const json badBinding = {{"type", "GamepadStick"}, {"xAxis", "LeftX"}, {"yAxis", "LeftY"}, {"invertY", 1}};
    const json goodBinding = {{"type", "KeyboardButton"}, {"key", "W"}};
    const json map = {{"actions", {{"Move", {{"bindings", json::array({badBinding, goodBinding})}}}}}};

    // The bad binding is skipped like any other invalid one; the rest of the action survives
    std::expected<std::unique_ptr<ActionMap>, ActionMapParseError> parsed;
    EXPECT_NO_THROW(parsed = ActionMap::Deserialize(map, "Gameplay", nullptr));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed.value())["Move"].Serialize()["bindings"].size(), 1u);

    json badDisabled = map;
    badDisabled["disabled"] = "yes";
    EXPECT_NO_THROW(parsed = ActionMap::Deserialize(badDisabled, "Gameplay", nullptr));
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), ActionMapParseError::InvalidDisabledType);
}

TEST_F(InputSystemHeadlessTest, WrongTypedOptionalFieldsDoNotThrow)
{
    const json badBinding = {{"type", "GamepadAxis"}, {"axis", "LeftX"}, {"gamepadId", "first"}};
    const json map = {{"actions", {{"Fire", {{"bindings", json::array({badBinding})}}}}}};

    ActionMap *created = nullptr;
    EXPECT_NO_THROW(created = _input.CreateActionMapFromJson("Gameplay", map)); // also Lua's Input.CreateActionMap
    ASSERT_NE(created, nullptr);
    EXPECT_EQ((*created)["Fire"].Serialize()["bindings"].size(), 0u);

    json badDisabled = map;
    badDisabled["disabled"] = 0;
    EXPECT_NO_THROW(created = _input.CreateActionMapFromJson("Menu", badDisabled));
    EXPECT_EQ(created, nullptr);

    // A whole-system load skips the bad map and keeps the good one
    bool loaded = false;
    EXPECT_NO_THROW(loaded = _input.Deserialize({{"actionMaps", {{"Gameplay", map}, {"Menu", badDisabled}}}}));
    EXPECT_TRUE(loaded);
    EXPECT_NE(_input.GetActionMap("Gameplay"), nullptr);
    EXPECT_EQ(_input.GetActionMap("Menu"), nullptr);
}

// ============================================================================
// Gamepad button composites, axis halves and press points, chords
// ============================================================================

TEST(GamepadButtonCompositeTest, CombinesLikeTheKeyboardComposite)
{
    EXPECT_EQ(GamepadButtonCompositeBinding::Combine(false, false, false, false), Vector2(0.0f, 0.0f));
    EXPECT_EQ(GamepadButtonCompositeBinding::Combine(true, false, false, false), Vector2(0.0f, 1.0f));
    EXPECT_EQ(GamepadButtonCompositeBinding::Combine(false, true, false, true), Vector2(1.0f, -1.0f));
    // Opposite buttons cancel
    EXPECT_EQ(GamepadButtonCompositeBinding::Combine(true, true, true, true), Vector2(0.0f, 0.0f));
}

TEST(GamepadButtonCompositeTest, ParsesAndRoundTrips)
{
    const json source = {{"type", "GamepadButtonComposite"}, {"up", "DpadUp"}, {"down", "DpadDown"},
                         {"left", "DpadLeft"}, {"right", "DpadRight"}};
    auto parsed = CreateBindingFromJson(nullptr, source);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed)->GetType(), BindingType::GamepadButtonComposite);
    const json written = (*parsed)->Serialize();
    EXPECT_EQ(written["up"], "DpadUp");
    EXPECT_EQ(written["gamepadId"], 0);
    EXPECT_TRUE(CreateBindingFromJson(nullptr, written).has_value());

    // Headless (no GLFW): a zero vector, so the action stays a Vector2 action
    EXPECT_TRUE((*parsed)->getValue().Is<Vector2>());

    auto missing = CreateBindingFromJson(nullptr, {{"type", "GamepadButtonComposite"}, {"up", "North"}});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error(), BindingParseError::MissingCompositeKeys);

    auto keyName = CreateBindingFromJson(nullptr, {{"type", "GamepadButtonComposite"}, {"up", "W"},
                                                   {"down", "S"}, {"left", "A"}, {"right", "D"}});
    ASSERT_FALSE(keyName.has_value());
    EXPECT_EQ(keyName.error(), BindingParseError::InvalidValue);
}

TEST(AxisBindingTest, DirectionKeepsOneHalf)
{
    const auto value = [](const float normalized, const AxisDirection direction)
    {
        return AxisBinding::Shape(normalized, direction, 0.0f).asFloat();
    };
    EXPECT_FLOAT_EQ(value(-0.7f, AxisDirection::Both), -0.7f);
    EXPECT_FLOAT_EQ(value(-0.7f, AxisDirection::Negative), 0.7f);
    EXPECT_FLOAT_EQ(value(-0.7f, AxisDirection::Positive), 0.0f);
    EXPECT_FLOAT_EQ(value(0.4f, AxisDirection::Positive), 0.4f);
    EXPECT_FLOAT_EQ(value(0.4f, AxisDirection::Negative), 0.0f);
}

TEST(AxisBindingTest, PressPointMakesAButton)
{
    const InputValue below = AxisBinding::Shape(0.49f, AxisDirection::Both, 0.5f);
    ASSERT_TRUE(below.Is<bool>());
    EXPECT_FALSE(below.asBool());
    EXPECT_TRUE(AxisBinding::Shape(0.5f, AxisDirection::Both, 0.5f).asBool());
    // A stick pushed left, as a button: only the negative half presses it
    EXPECT_TRUE(AxisBinding::Shape(-0.8f, AxisDirection::Negative, 0.5f).asBool());
    EXPECT_FALSE(AxisBinding::Shape(0.8f, AxisDirection::Negative, 0.5f).asBool());
}

TEST(AxisBindingTest, DirectionAndPressPointParseAndRoundTrip)
{
    auto parsed = CreateBindingFromJson(nullptr, {{"type", "GamepadAxis"}, {"axis", "LeftX"},
                                                  {"direction", "Negative"}, {"pressPoint", 0.5}});
    ASSERT_TRUE(parsed.has_value());
    const json written = (*parsed)->Serialize();
    EXPECT_EQ(written["direction"], "Negative");
    EXPECT_FLOAT_EQ(written["pressPoint"].get<float>(), 0.5f);
    // Headless: still a button value
    EXPECT_TRUE((*parsed)->getValue().Is<bool>());

    // Defaults are not written, so older maps serialize as before
    auto plain = CreateBindingFromJson(nullptr, {{"type", "GamepadAxis"}, {"axis", "LeftX"}});
    ASSERT_TRUE(plain.has_value());
    EXPECT_FALSE((*plain)->Serialize().contains("direction"));
    EXPECT_FALSE((*plain)->Serialize().contains("pressPoint"));

    const std::vector<json> bad = {
        {{"type", "GamepadAxis"}, {"axis", "LeftX"}, {"direction", "Left"}},
        {{"type", "GamepadAxis"}, {"axis", "LeftX"}, {"direction", 1}},
        {{"type", "GamepadAxis"}, {"axis", "LeftX"}, {"pressPoint", "0.5"}},
        {{"type", "GamepadAxis"}, {"axis", "LeftX"}, {"pressPoint", 1.5}},
        {{"type", "GamepadAxis"}, {"axis", "LeftX"}, {"pressPoint", -0.1}},
    };
    for (const auto &binding : bad)
    {
        auto result = CreateBindingFromJson(nullptr, binding);
        ASSERT_FALSE(result.has_value()) << binding.dump();
        EXPECT_EQ(result.error(), BindingParseError::InvalidOptionalField) << binding.dump();
    }
}

TEST(ChordBindingTest, CountsOnlyWhileTheModifierIsHeld)
{
    auto modifier = std::make_unique<FakeBinding>(false);
    auto main = std::make_unique<FakeBinding>(true);
    FakeBinding *modifierRaw = modifier.get();
    FakeBinding *mainRaw = main.get();
    ChordBinding chord(nullptr, std::move(modifier), std::move(main));

    EXPECT_FALSE(chord.getValue().asBool());
    modifierRaw->value = true;
    EXPECT_TRUE(chord.getValue().asBool());
    mainRaw->value = false;
    EXPECT_FALSE(chord.getValue().asBool());

    // The released value keeps the main binding's type
    mainRaw->value = Vector2(0.5f, 0.5f);
    modifierRaw->value = false;
    const InputValue released = chord.getValue();
    ASSERT_TRUE(released.Is<Vector2>());
    EXPECT_EQ(released.asVector2(), Vector2(0.0f, 0.0f));
    mainRaw->value = 0.75f;
    ASSERT_TRUE(chord.getValue().Is<float>());
    EXPECT_FLOAT_EQ(chord.getValue().asFloat(), 0.0f);
}

TEST(ChordBindingTest, ParsesNestedBindingsAndRoundTrips)
{
    const json source = {{"type", "Chord"},
                         {"modifier", {{"type", "GamepadButton"}, {"button", "LeftThumb"}}},
                         {"binding", {{"type", "GamepadButton"}, {"button", "RightThumb"}}}};
    auto parsed = CreateBindingFromJson(nullptr, source);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed)->GetType(), BindingType::Chord);
    const json written = (*parsed)->Serialize();
    EXPECT_EQ(written["modifier"]["button"], "LeftThumb");
    EXPECT_EQ(written["binding"]["button"], "RightThumb");
    EXPECT_TRUE(CreateBindingFromJson(nullptr, written).has_value());

    auto missing = CreateBindingFromJson(nullptr, {{"type", "Chord"}, {"modifier", {{"type", "KeyboardButton"},
                                                                                    {"key", "LeftShift"}}}});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error(), BindingParseError::MissingChordBinding);

    // A bad inner binding reports its own error
    auto badInner = CreateBindingFromJson(nullptr, {{"type", "Chord"},
                                                    {"modifier", {{"type", "KeyboardButton"}, {"key", "Shfit"}}},
                                                    {"binding", {{"type", "KeyboardButton"}, {"key", "R"}}}});
    ASSERT_FALSE(badInner.has_value());
    EXPECT_EQ(badInner.error(), BindingParseError::InvalidValue);
}

TEST(ChordBindingTest, NestingIsBounded)
{
    json binding = {{"type", "KeyboardButton"}, {"key", "R"}};
    for (int i = 0; i < MaxChordDepth; ++i)
    {
        binding = {{"type", "Chord"}, {"modifier", {{"type", "KeyboardButton"}, {"key", "LeftShift"}}},
                   {"binding", binding}};
    }
    EXPECT_TRUE(CreateBindingFromJson(nullptr, binding).has_value());

    binding = {{"type", "Chord"}, {"modifier", {{"type", "KeyboardButton"}, {"key", "LeftShift"}}},
               {"binding", binding}};
    auto tooDeep = CreateBindingFromJson(nullptr, binding);
    ASSERT_FALSE(tooDeep.has_value());
    EXPECT_EQ(tooDeep.error(), BindingParseError::ChordTooDeep);
    EXPECT_FALSE(BindingParseErrorToString(BindingParseError::ChordTooDeep).empty());
}
