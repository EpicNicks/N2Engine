#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/Window.hpp"
#include "engine/Application.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/input/Mouse.hpp"
#include "engine/Logger.hpp"
#include "engine/scripting/LuaJson.hpp"
#include "engine/scripting/ScriptCallback.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindInput(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        // ===== ActionPhase Enum =====
        lua.new_enum(
            "ActionPhase",
            "Waiting", Input::ActionPhase::Waiting,
            "Started", Input::ActionPhase::Started,
            "Performed", Input::ActionPhase::Performed,
            "Cancelled", Input::ActionPhase::Cancelled
        );

        // ===== InputValue =====
        lua.new_usertype<Input::InputValue>(
            "InputValue",
            sol::no_constructor,

            "GetBool", &Input::InputValue::asBool,
            "GetFloat", &Input::InputValue::asFloat,
            "GetVector2", &Input::InputValue::asVector2
        );

        lua.new_usertype<Input::Mouse>(
            "Mouse",
            sol::no_constructor,
            "GetScrollDelta", &Input::Mouse::GetScrollDelta,
            "GetPosition", &Input::Mouse::GetPosition,
            "GetPositionDelta", &Input::Mouse::GetPositionDelta
        );

        lua["Mouse"] = lua.create_table_with(
            "Get", []() -> Input::Mouse*
            {
                return Input::Mouse::Get();
            }
        );

        // ===== InputAction =====
        lua.new_usertype<Input::InputAction>(
            "InputAction",
            sol::no_constructor,

            // Phase queries
            "GetPhase", &Input::InputAction::GetPhase,
            "WasStarted", &Input::InputAction::WasStarted,
            "WasPerformed", &Input::InputAction::WasPerformed,
            "WasCancelled", &Input::InputAction::WasCancelled,
            "IsActive", &Input::InputAction::IsActive,

            // Value queries
            "GetVector2Value", &Input::InputAction::GetVector2Value,
            "GetBoolValue", &Input::InputAction::GetBoolValue,
            "GetFloatValue", &Input::InputAction::GetFloatValue,

            // State
            "GetDisabled", &Input::InputAction::GetDisabled,
            "SetDisabled", &Input::InputAction::SetDisabled,
            "GetName", &Input::InputAction::GetName,

            // Event subscription (returns subscription ID for unsubscribe)
            // A subscription made by a component script stops firing when that component is destroyed
            "Subscribe", [](Input::InputAction &action, sol::protected_function callback) -> size_t
            {
                return action.GetOnStateChanged() += MakeScriptCallback<Input::InputAction&>(
                    std::move(callback), "InputAction");
            },

            "Unsubscribe", [](Input::InputAction &action, size_t id)
            {
                action.GetOnStateChanged() -= id;
            },

            // Access to the event handler directly
            "OnStateChanged", [](Input::InputAction &action) -> Base::EventHandler<Input::InputAction&>&
            {
                return action.GetOnStateChanged();
            }
        );

        // ===== ActionMap =====
        lua.new_usertype<Input::ActionMap>(
            "ActionMap",
            sol::no_constructor,

            // Access actions
            "Get", [](Input::ActionMap &map, const std::string &actionName) -> Input::InputAction*
            {
                try
                {
                    return &map[actionName];
                }
                catch (...)
                {
                    return nullptr;
                }
            },

            // Operator[] support
            sol::meta_function::index, [](Input::ActionMap &map, const std::string &actionName) -> Input::InputAction*
            {
                try
                {
                    return &map[actionName];
                }
                catch (...)
                {
                    return nullptr;
                }
            },

            // State
            "disabled", &Input::ActionMap::disabled,
            "name", sol::readonly(&Input::ActionMap::name)
        );

        // ===== Input Global =====
        lua["Input"] = lua.create_table_with(
            "GetActionMap", [](const std::string &mapName) -> Input::ActionMap*
            {
                auto *app = &Application::GetInstance();
                if (!app) return nullptr;

                auto *inputSystem = app->GetWindow().GetInputSystem();
                if (!inputSystem) return nullptr;

                return inputSystem->GetActionMap(mapName);
            },

            "LoadActionMap", [](const std::string &mapName) -> Input::ActionMap*
            {
                auto *app = &Application::GetInstance();
                if (!app) return nullptr;

                auto *inputSystem = app->GetWindow().GetInputSystem();
                if (!inputSystem) return nullptr;

                return inputSystem->LoadActionMap(mapName);
            },

            // Input.CreateActionMap("Main Controls", {
            //     ["Move"] = { { type = "Vector2Composite", up = "W", down = "S", left = "A", right = "D" } },
            //     ["Quit"] = { { type = "KeyboardButton", key = "Escape" } },
            // })
            // Binding fields and enum names match the action map JSON format. Returns nil without a window.
            "CreateActionMap", [](const std::string &mapName, const sol::table &actions) -> Input::ActionMap*
            {
                auto *inputSystem = Application::GetInstance().GetWindow().GetInputSystem();
                if (!inputSystem)
                {
                    Logger::Warn(std::format("Input.CreateActionMap('{}'): no input system (no window)", mapName));
                    return nullptr;
                }

                return inputSystem->CreateActionMapFromJson(mapName, ActionMapJsonFromLua(actions));
            }
        );
    }
}
