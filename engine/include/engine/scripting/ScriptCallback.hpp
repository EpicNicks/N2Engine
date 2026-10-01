#pragma once

#include <format>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <sol/sol.hpp>

#include "engine/Logger.hpp"
#include "engine/scripting/LuaHandles.hpp"

namespace N2Engine::Scripting
{
    /// Marks which script's code is running. LuaComponent opens one around each call into its script,
    /// so callbacks that script registers (e.g. InputAction:Subscribe in OnAttach) can be tied to it.
    class ScriptLifetimeScope
    {
    public:
        /// @param lifetime true while the owning component exists; set to false when it's destroyed
        explicit ScriptLifetimeScope(std::shared_ptr<const bool> lifetime);
        ~ScriptLifetimeScope();

        ScriptLifetimeScope(const ScriptLifetimeScope &) = delete;
        ScriptLifetimeScope &operator=(const ScriptLifetimeScope &) = delete;

        /// The running script's lifetime flag, or null when no component script is running
        /// (e.g. a scene setup script)
        static std::shared_ptr<const bool> Current();

    private:
        std::shared_ptr<const bool> _previous;
    };

    namespace Detail
    {
        // Class-type arguments go to Lua by reference, so scripts see the live object, not a copy
        template <typename T>
        decltype(auto) ToLuaArg(T &value)
        {
            if constexpr (std::is_class_v<T>)
            {
                return std::ref(value);
            }
            else
            {
                return value;
            }
        }

        // Except GameObjects: a handle, since the script may keep it after the event
        inline GameObjectRef ToLuaArg(GameObject &gameObject)
        {
            return GameObjectRef(gameObject);
        }
    }

    /// Wraps a Lua function for storage in a C++ event. If a component script registered it, the
    /// callback is skipped once that component is destroyed, instead of running with freed
    /// `self.component`/`self.gameObject`. Errors are logged, never thrown into the event dispatch.
    template <typename... Args>
    std::function<void(Args...)> MakeScriptCallback(sol::protected_function callback, std::string eventName)
    {
        return [callback = std::move(callback), owner = ScriptLifetimeScope::Current(),
                eventName = std::move(eventName)](Args... args)
        {
            if (owner && !*owner)
            {
                return; // the script that subscribed belongs to a destroyed component
            }

            const sol::protected_function_result result = callback(Detail::ToLuaArg(args)...);
            if (!result.valid())
            {
                const sol::error err = result;
                Logger::Error(std::format("{} callback error: {}", eventName, err.what()));
            }
        };
    }
}
