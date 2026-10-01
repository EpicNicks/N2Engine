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

#include <stdexcept>

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        // Maps and actions reach Lua as handles: a map can be replaced or reloaded and an action replaced or
        // removed, while a script may keep either for longer than that
        sol::optional<ActionMapRef> MapRefOrNil(Input::ActionMap *map)
        {
            if (!map)
            {
                return sol::nullopt;
            }
            return ActionMapRef(*map);
        }

        sol::optional<InputActionRef> FindAction(const ActionMapRef &map, const std::string &actionName)
        {
            Input::ActionMap *actionMap = map.Pin();
            try
            {
                return InputActionRef((*actionMap)[actionName]);
            }
            catch (const std::out_of_range &)
            {
                return sol::nullopt; // no action by that name
            }
        }
    }

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

        // A plain pointer: the Mouse lives as long as the window's input system, i.e. until shutdown
        lua.new_usertype<Input::Mouse>(
            "Mouse",
            sol::no_constructor,
            "GetScrollDelta", &Input::Mouse::GetScrollDelta,
            "GetPosition", &Input::Mouse::GetPosition,
            "GetPositionDelta", &Input::Mouse::GetPositionDelta,
            // Buttons 0..7 (0 left, 1 right, 2 middle), as sampled at the start of the frame
            "GetButton", [](const Input::Mouse &mouse, const int button) { return mouse.GetButton(button); },
            "GetButtonDown", [](const Input::Mouse &mouse, const int button) { return mouse.GetButtonDown(button); },
            "GetButtonUp", [](const Input::Mouse &mouse, const int button) { return mouse.GetButtonUp(button); }
        );

        lua["Mouse"] = lua.create_table_with(
            "Get", []() -> Input::Mouse*
            {
                return Input::Mouse::Get();
            }
        );

        // ===== InputAction =====
        InputActionRef::s_luaName = "InputAction";
        lua.new_usertype<InputActionRef>(
            "InputAction",
            sol::no_constructor,
            sol::meta_function::equal_to, &SameRef<InputActionRef>,

            // False once the action is freed (replaced, removed, or its map replaced)
            "IsValid", [](const InputActionRef &action) { return action.IsValid(); },

            // Phase queries
            "GetPhase", Forward<InputActionRef, &Input::InputAction::GetPhase>(),
            "WasStarted", Forward<InputActionRef, &Input::InputAction::WasStarted>(),
            "WasPerformed", Forward<InputActionRef, &Input::InputAction::WasPerformed>(),
            "WasCancelled", Forward<InputActionRef, &Input::InputAction::WasCancelled>(),
            "IsActive", Forward<InputActionRef, &Input::InputAction::IsActive>(),

            // Value queries
            "GetVector2Value", Forward<InputActionRef, &Input::InputAction::GetVector2Value>(),
            "GetBoolValue", Forward<InputActionRef, &Input::InputAction::GetBoolValue>(),
            "GetFloatValue", Forward<InputActionRef, &Input::InputAction::GetFloatValue>(),

            // State
            "GetDisabled", Forward<InputActionRef, &Input::InputAction::GetDisabled>(),
            "SetDisabled", Forward<InputActionRef, &Input::InputAction::SetDisabled>(),
            "GetName", Forward<InputActionRef, &Input::InputAction::GetName>(),

            // Event subscription (returns subscription ID for unsubscribe)
            // A subscription made by a component script stops firing when that component is destroyed. The
            // event lives on the action, so its subscriptions go when the action is freed.
            "Subscribe", [](const InputActionRef &action, sol::protected_function callback) -> size_t
            {
                return action.Pin()->GetOnStateChanged() += MakeScriptCallback<Input::InputAction&>(
                    std::move(callback), "InputAction");
            },

            "Unsubscribe", [](const InputActionRef &action, size_t id)
            {
                action.Pin()->GetOnStateChanged() -= id;
            },

            // The action itself, which has the event's Subscribe/Unsubscribe (this used to return a reference
            // to the event, which dangled once the action was freed)
            "OnStateChanged", [](const InputActionRef &action)
            {
                static_cast<void>(action.Pin()); // an error once the action is gone, like any other method
                return action;
            }
        );

        // ===== ActionMap =====
        ActionMapRef::s_luaName = "ActionMap";
        lua.new_usertype<ActionMapRef>(
            "ActionMap",
            sol::no_constructor,
            sol::meta_function::equal_to, &SameRef<ActionMapRef>,

            // False once the map is freed (replaced, e.g. by Input.CreateActionMap with its name)
            "IsValid", [](const ActionMapRef &map) { return map.IsValid(); },

            // Access actions (nil for an unknown name)
            "Get", &FindAction,

            // Operator[] support
            sol::meta_function::index, &FindAction,

            // State
            "disabled", sol::property(
                [](const ActionMapRef &map) { return map.Pin()->disabled; },
                [](const ActionMapRef &map, const bool disabled) { map.Pin()->disabled = disabled; }
            ),
            "name", sol::property([](const ActionMapRef &map) { return map.Pin()->name; })
        );

        // ===== Input Global =====
        lua["Input"] = lua.create_table_with(
            "GetActionMap", [](const std::string &mapName) -> sol::optional<ActionMapRef>
            {
                auto *app = &Application::GetInstance();
                if (!app) return sol::nullopt;

                auto *inputSystem = app->GetWindow().GetInputSystem();
                if (!inputSystem) return sol::nullopt;

                return MapRefOrNil(inputSystem->GetActionMap(mapName));
            },

            "LoadActionMap", [](const std::string &mapName) -> sol::optional<ActionMapRef>
            {
                auto *app = &Application::GetInstance();
                if (!app) return sol::nullopt;

                auto *inputSystem = app->GetWindow().GetInputSystem();
                if (!inputSystem) return sol::nullopt;

                return MapRefOrNil(inputSystem->LoadActionMap(mapName));
            },

            // Input.CreateActionMap("Main Controls", {
            //     ["Move"] = { { type = "Vector2Composite", up = "W", down = "S", left = "A", right = "D" } },
            //     ["Quit"] = { { type = "KeyboardButton", key = "Escape" } },
            // })
            // Binding fields and enum names match the action map JSON format. Returns nil without a window.
            // Handles to a map it replaces become invalid.
            "CreateActionMap", [](const std::string &mapName, const sol::table &actions) -> sol::optional<ActionMapRef>
            {
                auto *inputSystem = Application::GetInstance().GetWindow().GetInputSystem();
                if (!inputSystem)
                {
                    Logger::Warn(std::format("Input.CreateActionMap('{}'): no input system (no window)", mapName));
                    return sol::nullopt;
                }

                return MapRefOrNil(inputSystem->CreateActionMapFromJson(mapName, ActionMapJsonFromLua(actions)));
            },

            // Unity's Input.mousePosition/GetMouseButton*: window coordinates (top-left origin, y down) and
            // buttons 0..7; (0, 0) and false without a mouse (no window)
            "GetMousePosition", []() -> Math::Vector2
            {
                const auto *mouse = Input::Mouse::Get();
                return mouse ? mouse->GetPosition() : Math::Vector2(0.0f, 0.0f);
            },
            "GetMouseButton", [](const int button)
            {
                const auto *mouse = Input::Mouse::Get();
                return mouse != nullptr && mouse->GetButton(button);
            },
            "GetMouseButtonDown", [](const int button)
            {
                const auto *mouse = Input::Mouse::Get();
                return mouse != nullptr && mouse->GetButtonDown(button);
            },
            "GetMouseButtonUp", [](const int button)
            {
                const auto *mouse = Input::Mouse::Get();
                return mouse != nullptr && mouse->GetButtonUp(button);
            },
            // Whether the pointer is over a UI element (PointerDispatcher::IsPointerOverUI)
            "IsPointerOverUI", []()
            {
                return Application::GetInstance().GetPointerDispatcher().IsPointerOverUI();
            }
        );
    }
}
