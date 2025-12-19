#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/input/InputBinding.hpp"
#include "engine/input/InputBindingFactory.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputTypes.hpp"

using namespace N2Engine;
using namespace N2Engine::Input;
using json = nlohmann::json;

// Tests use nullptr for window since we're only testing serialization logic
class InputBindingSerializationTest : public ::testing::Test
{
protected:
    GLFWwindow* window = nullptr;
};


// KeyboardButtonBinding Tests
TEST_F(InputBindingSerializationTest, KeyboardButtonBinding_Serialize)
{
    const KeyboardButtonBinding binding(window, Key::Space);

    json j = binding.Serialize();

    EXPECT_EQ(j["type"], "KeyboardButton");
    EXPECT_EQ(j["key"], "Space");
}

TEST_F(InputBindingSerializationTest, KeyboardButtonBinding_Deserialize)
{
    json j = {
        {"type", "KeyboardButton"},
        {"key", "Space"}
    };

    const auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetType(), BindingType::KeyboardButton);
}

TEST_F(InputBindingSerializationTest, KeyboardButtonBinding_RoundTrip)
{
    const KeyboardButtonBinding original(window, Key::A);

    json j = original.Serialize();
    auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetType(), BindingType::KeyboardButton);

    json roundTripped = result.value()->Serialize();
    EXPECT_EQ(j, roundTripped);
}

TEST_F(InputBindingSerializationTest, KeyboardButtonBinding_MissingKey)
{
    json j = {
        {"type", "KeyboardButton"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingKey);
}

// GamepadButtonBinding Tests
TEST_F(InputBindingSerializationTest, GamepadButtonBinding_Serialize)
{
    GamepadButtonBinding binding(window, GamepadButton::South, 1);

    json j = binding.Serialize();

    EXPECT_EQ(j["type"], "GamepadButton");
    EXPECT_EQ(j["button"], "South");
    EXPECT_EQ(j["gamepadId"], 1);
}

TEST_F(InputBindingSerializationTest, GamepadButtonBinding_Deserialize)
{
    json j = {
        {"type", "GamepadButton"},
        {"button", "North"},
        {"gamepadId", 2}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetType(), BindingType::GamepadButton);
}

TEST_F(InputBindingSerializationTest, GamepadButtonBinding_DefaultGamepadId)
{
    json j = {
        {"type", "GamepadButton"},
        {"button", "South"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());

    json roundTripped = result.value()->Serialize();
    EXPECT_EQ(roundTripped["gamepadId"], 0);
}

TEST_F(InputBindingSerializationTest, GamepadButtonBinding_MissingButton)
{
    json j = {
        {"type", "GamepadButton"},
        {"gamepadId", 0}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingButton);
}

// AxisBinding Tests
TEST_F(InputBindingSerializationTest, AxisBinding_Serialize)
{
    AxisBinding binding(window, GamepadAxis::RightTrigger, 0);

    json j = binding.Serialize();

    EXPECT_EQ(j["type"], "GamepadAxis");
    EXPECT_EQ(j["axis"], "RightTrigger");
    EXPECT_EQ(j["gamepadId"], 0);
}

TEST_F(InputBindingSerializationTest, AxisBinding_Deserialize)
{
    json j = {
        {"type", "GamepadAxis"},
        {"axis", "LeftTrigger"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetType(), BindingType::GamepadAxis);
}

TEST_F(InputBindingSerializationTest, AxisBinding_MissingAxis)
{
    json j = {
        {"type", "GamepadAxis"},
        {"gamepadId", 0}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingAxis);
}

// GamepadStickBinding Tests
TEST_F(InputBindingSerializationTest, GamepadStickBinding_Serialize)
{
    GamepadStickBinding binding(window, GamepadAxis::LeftX, GamepadAxis::LeftY, 0, 0.2f, true, false);

    json j = binding.Serialize();

    EXPECT_EQ(j["type"], "GamepadStick");
    EXPECT_EQ(j["xAxis"], "LeftX");
    EXPECT_EQ(j["yAxis"], "LeftY");
    EXPECT_EQ(j["gamepadId"], 0);
    EXPECT_FLOAT_EQ(j["deadzone"], 0.2f);
    EXPECT_EQ(j["invertX"], true);
    EXPECT_EQ(j["invertY"], false);
}

TEST_F(InputBindingSerializationTest, GamepadStickBinding_Deserialize)
{
    json j = {
        {"type", "GamepadStick"},
        {"xAxis", "RightX"},
        {"yAxis", "RightY"},
        {"deadzone", 0.1f},
        {"invertX", false},
        {"invertY", true}
    };

    const auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetType(), BindingType::GamepadStick);
}

TEST_F(InputBindingSerializationTest, GamepadStickBinding_DefaultValues)
{
    json j = {
        {"type", "GamepadStick"},
        {"xAxis", "LeftX"},
        {"yAxis", "LeftY"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());

    json roundTripped = result.value()->Serialize();
    EXPECT_EQ(roundTripped["gamepadId"], 0);
    EXPECT_FLOAT_EQ(roundTripped["deadzone"], 0.15f);
    EXPECT_EQ(roundTripped["invertX"], false);
    EXPECT_EQ(roundTripped["invertY"], false);
}

TEST_F(InputBindingSerializationTest, GamepadStickBinding_MissingXAxis)
{
    json j = {
        {"type", "GamepadStick"},
        {"yAxis", "LeftY"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingAxis);
}

TEST_F(InputBindingSerializationTest, GamepadStickBinding_MissingYAxis)
{
    json j = {
        {"type", "GamepadStick"},
        {"xAxis", "LeftX"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingAxis);
}

// Vector2CompositeBinding Tests
TEST_F(InputBindingSerializationTest, Vector2CompositeBinding_Serialize)
{
    const Vector2CompositeBinding binding(window, Key::W, Key::S, Key::A, Key::D);

    json j = binding.Serialize();

    EXPECT_EQ(j["type"], "Vector2Composite");
    EXPECT_EQ(j["up"], "W");
    EXPECT_EQ(j["down"], "S");
    EXPECT_EQ(j["left"], "A");
    EXPECT_EQ(j["right"], "D");
}

TEST_F(InputBindingSerializationTest, Vector2CompositeBinding_Deserialize)
{
    json j = {
        {"type", "Vector2Composite"},
        {"up", "Up"},
        {"down", "Down"},
        {"left", "Left"},
        {"right", "Right"}
    };

    const auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetType(), BindingType::Vector2Composite);
}

TEST_F(InputBindingSerializationTest, Vector2CompositeBinding_RoundTrip)
{
    Vector2CompositeBinding original(window, Key::I, Key::K, Key::J, Key::L);

    json j = original.Serialize();
    auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());

    json roundTripped = result.value()->Serialize();
    EXPECT_EQ(j, roundTripped);
}

TEST_F(InputBindingSerializationTest, Vector2CompositeBinding_MissingUp)
{
    json j = {
        {"type", "Vector2Composite"},
        {"down", "S"},
        {"left", "A"},
        {"right", "D"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingCompositeKeys);
}

TEST_F(InputBindingSerializationTest, Vector2CompositeBinding_MissingMultipleKeys)
{
    json j = {
        {"type", "Vector2Composite"},
        {"up", "W"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingCompositeKeys);
}

// MouseButtonBinding Tests
TEST_F(InputBindingSerializationTest, MouseButtonBinding_Serialize)
{
    MouseButtonBinding binding(window, MouseButton::Left);

    json j = binding.Serialize();

    EXPECT_EQ(j["type"], "MouseButton");
    EXPECT_EQ(j["button"], "Left");
}

TEST_F(InputBindingSerializationTest, MouseButtonBinding_Deserialize)
{
    json j = {
        {"type", "MouseButton"},
        {"button", "Right"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetType(), BindingType::MouseButton);
}

TEST_F(InputBindingSerializationTest, MouseButtonBinding_MissingButton)
{
    json j = {
        {"type", "MouseButton"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingButton);
}

// General Error Cases
TEST_F(InputBindingSerializationTest, MissingType)
{
    json j = {
        {"key", "Space"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingType);
}

TEST_F(InputBindingSerializationTest, InvalidTypeNotString)
{
    json j = {
        {"type", 123},
        {"key", "Space"}
    };

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingType);
}

TEST_F(InputBindingSerializationTest, EmptyJson)
{
    json j = json::object();

    auto result = CreateBindingFromJson(window, j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), BindingParseError::MissingType);
}

// BindingParseErrorToString Tests
TEST(BindingParseErrorTest, AllErrorsHaveStrings)
{
    EXPECT_FALSE(BindingParseErrorToString(BindingParseError::MissingType).empty());
    EXPECT_FALSE(BindingParseErrorToString(BindingParseError::InvalidType).empty());
    EXPECT_FALSE(BindingParseErrorToString(BindingParseError::MissingKey).empty());
    EXPECT_FALSE(BindingParseErrorToString(BindingParseError::MissingButton).empty());
    EXPECT_FALSE(BindingParseErrorToString(BindingParseError::MissingAxis).empty());
    EXPECT_FALSE(BindingParseErrorToString(BindingParseError::MissingCompositeKeys).empty());
}

// InputAction Serialization Tests
class InputActionSerializationTest : public ::testing::Test
{
protected:
    GLFWwindow* window = nullptr;
};

TEST_F(InputActionSerializationTest, EmptyAction_Serialize)
{
    InputAction action("TestAction");

    json j = action.Serialize();

    EXPECT_TRUE(j.contains("bindings"));
    EXPECT_TRUE(j["bindings"].is_array());
    EXPECT_TRUE(j["bindings"].empty());
}

TEST_F(InputActionSerializationTest, ActionWithSingleBinding_Serialize)
{
    InputAction action("Jump");
    action.AddBinding(std::make_unique<KeyboardButtonBinding>(window, Key::Space));

    json j = action.Serialize();

    ASSERT_EQ(j["bindings"].size(), 1);
    EXPECT_EQ(j["bindings"][0]["type"], "KeyboardButton");
    EXPECT_EQ(j["bindings"][0]["key"], "Space");
}

TEST_F(InputActionSerializationTest, ActionWithMultipleBindings_Serialize)
{
    InputAction action("Fire");
    action.AddBinding(std::make_unique<MouseButtonBinding>(window, MouseButton::Left));
    action.AddBinding(std::make_unique<GamepadButtonBinding>(window, GamepadButton::South, 0));

    json j = action.Serialize();

    ASSERT_EQ(j["bindings"].size(), 2);
    EXPECT_EQ(j["bindings"][0]["type"], "MouseButton");
    EXPECT_EQ(j["bindings"][1]["type"], "GamepadButton");
}

// ActionMap Serialization Tests
class ActionMapSerializationTest : public ::testing::Test
{
protected:
    GLFWwindow* window = nullptr;
};

TEST_F(ActionMapSerializationTest, EmptyMap_Serialize)
{
    const ActionMap map("Gameplay");

    json j = map.Serialize();

    EXPECT_EQ(j["disabled"], false);
    EXPECT_TRUE(j["actions"].is_object());
    EXPECT_TRUE(j["actions"].empty());
}

TEST_F(ActionMapSerializationTest, MapWithActions_Serialize)
{
    ActionMap map("Gameplay");

    auto jumpAction = std::make_unique<InputAction>("Jump");
    jumpAction->AddBinding(std::make_unique<KeyboardButtonBinding>(window, Key::Space));
    map.AddInputAction(std::move(jumpAction));

    auto moveAction = std::make_unique<InputAction>("Move");
    moveAction->AddBinding(std::make_unique<Vector2CompositeBinding>(window, Key::W, Key::S, Key::A, Key::D));
    map.AddInputAction(std::move(moveAction));

    json j = map.Serialize();

    EXPECT_EQ(j["disabled"], false);
    ASSERT_TRUE(j["actions"].contains("Jump"));
    ASSERT_TRUE(j["actions"].contains("Move"));
    EXPECT_EQ(j["actions"]["Jump"]["bindings"][0]["key"], "Space");
    EXPECT_EQ(j["actions"]["Move"]["bindings"][0]["type"], "Vector2Composite");
}

TEST_F(ActionMapSerializationTest, DisabledMap_Serialize)
{
    ActionMap map("Paused");
    map.disabled = true;

    json j = map.Serialize();

    EXPECT_EQ(j["disabled"], true);
}

// Full Config Serialization Integration Tests
class FullConfigSerializationTest : public ::testing::Test
{
protected:
    GLFWwindow* window = nullptr;

    static json CreateValidConfig()
    {
        return {
            {"actionMaps", {
                {"Gameplay", {
                    {"disabled", false},
                    {"actions", {
                        {"Move", {
                            {"bindings", {
                                {
                                    {"type", "Vector2Composite"},
                                    {"up", "W"},
                                    {"down", "S"},
                                    {"left", "A"},
                                    {"right", "D"}
                                },
                                {
                                    {"type", "GamepadStick"},
                                    {"xAxis", "LeftX"},
                                    {"yAxis", "LeftY"}
                                }
                            }}
                        }},
                        {"Jump", {
                            {"bindings", {
                                {
                                    {"type", "KeyboardButton"},
                                    {"key", "Space"}
                                },
                                {
                                    {"type", "GamepadButton"},
                                    {"button", "South"}
                                }
                            }}
                        }},
                        {"Fire", {
                            {"bindings", {
                                {
                                    {"type", "MouseButton"},
                                    {"button", "Left"}
                                },
                                {
                                    {"type", "GamepadAxis"},
                                    {"axis", "RightTrigger"}
                                }
                            }}
                        }}
                    }}
                }},
                {"UI", {
                    {"disabled", false},
                    {"actions", {
                        {"Confirm", {
                            {"bindings", {
                                {
                                    {"type", "KeyboardButton"},
                                    {"key", "Enter"}
                                }
                            }}
                        }}
                    }}
                }}
            }}
        };
    }
};

TEST_F(FullConfigSerializationTest, ValidConfig_Structure)
{
    json config = CreateValidConfig();

    EXPECT_TRUE(config.contains("actionMaps"));
    EXPECT_TRUE(config["actionMaps"].contains("Gameplay"));
    EXPECT_TRUE(config["actionMaps"].contains("UI"));
    EXPECT_EQ(config["actionMaps"]["Gameplay"]["actions"].size(), 3);
    EXPECT_EQ(config["actionMaps"]["UI"]["actions"].size(), 1);
}

TEST_F(FullConfigSerializationTest, ValidConfig_BindingCounts)
{
    json config = CreateValidConfig();

    EXPECT_EQ(config["actionMaps"]["Gameplay"]["actions"]["Move"]["bindings"].size(), 2);
    EXPECT_EQ(config["actionMaps"]["Gameplay"]["actions"]["Jump"]["bindings"].size(), 2);
    EXPECT_EQ(config["actionMaps"]["Gameplay"]["actions"]["Fire"]["bindings"].size(), 2);
    EXPECT_EQ(config["actionMaps"]["UI"]["actions"]["Confirm"]["bindings"].size(), 1);
}

// Deserialization Edge Cases
TEST_F(InputBindingSerializationTest, AllKeyboardKeys_RoundTrip)
{
    std::vector<Key> keysToTest = {
        Key::A, Key::Z, Key::Key0, Key::Key9,
        Key::F1, Key::F12, Key::Space, Key::Enter,
        Key::LeftShift, Key::RightControl, Key::Escape
    };

    for (const Key key : keysToTest)
    {
        KeyboardButtonBinding original(window, key);
        json j = original.Serialize();
        auto result = CreateBindingFromJson(window, j);

        ASSERT_TRUE(result.has_value()) << "Failed for key: " << j["key"];

        json roundTripped = result.value()->Serialize();
        EXPECT_EQ(j, roundTripped) << "Round trip failed for key: " << j["key"];
    }
}

TEST_F(InputBindingSerializationTest, AllGamepadButtons_RoundTrip)
{
    const std::vector<GamepadButton> buttonsToTest = {
        GamepadButton::South, GamepadButton::East,
        GamepadButton::West, GamepadButton::North,
        GamepadButton::LeftBumper, GamepadButton::RightBumper,
        GamepadButton::Start, GamepadButton::Back,
        GamepadButton::DpadUp, GamepadButton::DpadDown
    };

    for (const GamepadButton button : buttonsToTest)
    {
        GamepadButtonBinding original(window, button, 0);
        json j = original.Serialize();
        auto result = CreateBindingFromJson(window, j);

        ASSERT_TRUE(result.has_value()) << "Failed for button: " << j["button"];

        json roundTripped = result.value()->Serialize();
        EXPECT_EQ(j, roundTripped) << "Round trip failed for button: " << j["button"];
    }
}

TEST_F(InputBindingSerializationTest, AllGamepadAxes_RoundTrip)
{
    std::vector<GamepadAxis> axesToTest = {
        GamepadAxis::LeftX, GamepadAxis::LeftY,
        GamepadAxis::RightX, GamepadAxis::RightY,
        GamepadAxis::LeftTrigger, GamepadAxis::RightTrigger
    };

    for (const GamepadAxis axis : axesToTest)
    {
        AxisBinding original(window, axis, 0);
        json j = original.Serialize();
        auto result = CreateBindingFromJson(window, j);

        ASSERT_TRUE(result.has_value()) << "Failed for axis: " << j["axis"];

        json roundTripped = result.value()->Serialize();
        EXPECT_EQ(j, roundTripped) << "Round trip failed for axis: " << j["axis"];
    }
}

TEST_F(InputBindingSerializationTest, AllMouseButtons_RoundTrip)
{
    std::vector<MouseButton> buttonsToTest = {
        MouseButton::Left, MouseButton::Right, MouseButton::Middle,
        MouseButton::Button4, MouseButton::Button5
    };

    for (MouseButton button : buttonsToTest)
    {
        MouseButtonBinding original(window, button);
        json j = original.Serialize();
        auto result = CreateBindingFromJson(window, j);

        ASSERT_TRUE(result.has_value()) << "Failed for button: " << j["button"];

        json roundTripped = result.value()->Serialize();
        EXPECT_EQ(j, roundTripped) << "Round trip failed for button: " << j["button"];
    }
}