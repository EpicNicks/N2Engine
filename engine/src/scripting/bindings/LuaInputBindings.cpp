#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/Window.hpp"
#include "engine/Application.hpp"
#include "engine/input/InputSystem.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindInput(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();

        // ===== ActionPhase Enum =====
        lua.new_enum("ActionPhase",
            "Waiting", Input::ActionPhase::Waiting,
            "Started", Input::ActionPhase::Started,
            "Performed", Input::ActionPhase::Performed,
            "Cancelled", Input::ActionPhase::Cancelled
        );

        // ===== InputValue =====
        lua.new_usertype<Input::InputValue>("InputValue",
            sol::no_constructor,

            "GetBool", &Input::InputValue::asBool,
            "GetFloat", &Input::InputValue::asFloat,
            "GetVector2", &Input::InputValue::asVector2
        );

        // ===== InputAction =====
        lua.new_usertype<Input::InputAction>("InputAction",
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
            "Subscribe", [](Input::InputAction& action, sol::function callback) -> size_t {
                return action.GetOnStateChanged() += [callback](Input::InputAction& act) {
                    auto result = callback(std::ref(act));
                    if (!result.valid()) {
                        sol::error err = result;
                        Logger::Error(std::format("InputAction callback error: {}", err.what()));
                    }
                };
            },

            "Unsubscribe", [](Input::InputAction& action, size_t id) {
                action.GetOnStateChanged() -= id;
            },

            // Access to the event handler directly
            "OnStateChanged", [](Input::InputAction& action) -> Base::EventHandler<Input::InputAction&>& {
                return action.GetOnStateChanged();
            }
        );

        // ===== ActionMap =====
        lua.new_usertype<Input::ActionMap>("ActionMap",
            sol::no_constructor,

            // Access actions
            "Get", [](Input::ActionMap& map, const std::string& actionName) -> Input::InputAction* {
                try {
                    return &map[actionName];
                } catch (...) {
                    return nullptr;
                }
            },

            // Operator[] support
            sol::meta_function::index, [](Input::ActionMap& map, const std::string& actionName) -> Input::InputAction* {
                try {
                    return &map[actionName];
                } catch (...) {
                    return nullptr;
                }
            },

            // State
            "disabled", &Input::ActionMap::disabled,
            "name", sol::readonly(&Input::ActionMap::name)
        );

        // ===== Input Global =====
        lua["Input"] = lua.create_table_with(
            "GetActionMap", [](const std::string& mapName) -> Input::ActionMap* {
                auto* app = &Application::GetInstance();
                if (!app) return nullptr;

                auto* inputSystem = app->GetWindow().GetInputSystem();
                if (!inputSystem) return nullptr;

                return inputSystem->GetActionMap(mapName);
            },

            "LoadActionMap", [](const std::string& mapName) -> Input::ActionMap* {
                auto* app = &Application::GetInstance();
                if (!app) return nullptr;

                auto* inputSystem = app->GetWindow().GetInputSystem();
                if (!inputSystem) return nullptr;

                return inputSystem->LoadActionMap(mapName);
            }
        );
    }
}
