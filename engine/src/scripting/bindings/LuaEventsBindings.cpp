#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/ScriptCallback.hpp"
#include "engine/base/EventHandler.hpp"
#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"

namespace N2Engine::Input
{
    class InputAction;
}

namespace N2Engine::Scripting::Bindings
{
    void BindEvents(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        lua.new_usertype<Base::EventHandler<>>(
            "Event",
            sol::no_constructor,

            "Subscribe",
            [](Base::EventHandler<> &handler, sol::protected_function callback) -> size_t
            {
                return handler += MakeScriptCallback<>(std::move(callback), "Event");
            },

            "Unsubscribe", [](Base::EventHandler<> &handler, size_t id)
            {
                handler -= id;
            },

            "Invoke", [](Base::EventHandler<> &handler)
            {
                handler();
            },

            "GetSubscriberCount", &Base::EventHandler<>::GetSubscriberCount
        );

        // ===== EventHandler<int, int> - Window resize events =====
        lua.new_usertype<Base::EventHandler<int, int>>(
            "WindowResizeEvent",
            sol::no_constructor,

            "Subscribe",
            [](Base::EventHandler<int, int> &handler, sol::protected_function callback) -> size_t
            {
                return handler += MakeScriptCallback<int, int>(std::move(callback), "WindowResizeEvent");
            },

            "Unsubscribe",
            [](Base::EventHandler<int, int> &handler, size_t id)
            {
                handler -= id;
            }
        );

        // ===== EventHandler<GameObject&> - GameObject events =====
        lua.new_usertype<Base::EventHandler<GameObject&>>(
            "GameObjectEvent",
            sol::no_constructor,

            "Subscribe",
            [](Base::EventHandler<GameObject&> &handler, sol::protected_function callback) -> size_t
            {
                return handler += MakeScriptCallback<GameObject&>(std::move(callback), "GameObjectEvent");
            },

            "Unsubscribe",
            [](Base::EventHandler<GameObject&> &handler, size_t id)
            {
                handler -= id;
            }
        );

        // ===== EventHandler<InputAction&> - Input action events =====
        lua.new_usertype<Base::EventHandler<Input::InputAction&>>(
            "InputActionEvent",
            sol::no_constructor,

            "Subscribe", [](Base::EventHandler<Input::InputAction&> &handler, sol::protected_function callback) -> size_t
            {
                return handler += MakeScriptCallback<Input::InputAction&>(std::move(callback), "InputActionEvent");
            },

            "Unsubscribe", [](Base::EventHandler<Input::InputAction&> &handler, size_t id)
            {
                handler -= id;
            }
        );
    }
}
