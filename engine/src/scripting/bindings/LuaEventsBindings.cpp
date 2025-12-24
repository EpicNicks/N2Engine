#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
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
            [](Base::EventHandler<> &handler, sol::function callback) -> size_t
            {
                return handler += [callback]()
                {
                    auto result = callback();
                    if (!result.valid())
                    {
                        sol::error err = result;
                        Logger::Error(
                            std::format("Event callback error: {}", err.what()));
                    }
                };
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
            [](Base::EventHandler<int, int> &handler,
               sol::function callback) -> size_t
            {
                return handler += [callback](int width, int height)
                {
                    auto result = callback(width, height);
                    if (!result.valid())
                    {
                        sol::error err = result;
                        Logger::Error(
                            std::format(
                                "WindowResizeEvent callback error: {}",
                                err.what()));
                    }
                };
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
            [](Base::EventHandler<GameObject&> &handler,
               sol::function callback) -> size_t
            {
                return handler += [callback](GameObject &go)
                {
                    auto result = callback(std::ref(go));
                    if (!result.valid())
                    {
                        sol::error err = result;
                        Logger::Error(
                            std::format(
                                "GameObjectEvent callback error: {}",
                                err.what()));
                    }
                };
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

            "Subscribe", [](Base::EventHandler<Input::InputAction&> &handler, sol::function callback) -> size_t
            {
                return handler += [callback](Input::InputAction &action)
                {
                    auto result = callback(std::ref(action));
                    if (!result.valid())
                    {
                        sol::error err = result;
                        Logger::Error(std::format("InputActionEvent callback error: {}", err.what()));
                    }
                };
            },

            "Unsubscribe", [](Base::EventHandler<Input::InputAction&> &handler, size_t id)
            {
                handler -= id;
            }
        );
    }
}
