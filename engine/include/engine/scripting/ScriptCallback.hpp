#pragma once

#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <sol/sol.hpp>

#include "engine/Logger.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

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

        // And InputActions (an action's event passes the action): it can be replaced or removed later
        InputActionRef ToLuaArg(Input::InputAction &action);
    }

    /// Wraps a Lua function for storage in a C++ event. If a component script registered it, the
    /// callback is skipped once that component is destroyed, instead of running with freed
    /// `self.component`/`self.gameObject`. Errors are logged, never thrown into the event dispatch.
    /// Each call is an entry into Lua with a budget of its own (LuaBudgetScope). A callback that runs out of
    /// instructions or memory is logged once and then switched off, so it can't burn the budget on every event.
    template <typename... Args>
    std::function<void(Args...)> MakeScriptCallback(sol::protected_function callback, std::string eventName)
    {
        // Shared, so the event's copies of this std::function (EventHandler copies its callbacks for each dispatch)
        // copy a pointer: copying a sol::protected_function makes two registry references, allocations outside any
        // protected call that could fail at the memory cap before the try below is reached
        return [callback = std::make_shared<sol::protected_function>(std::move(callback)),
                owner = ScriptLifetimeScope::Current(),
                eventName = std::move(eventName), faulted = std::make_shared<bool>(false)](Args... args)
        {
            if ((owner && !*owner) || *faulted)
            {
                return; // the script that subscribed belongs to a destroyed component, or the callback was switched off
            }

            const LuaBudgetScope budget;
            const std::uint64_t panicsBefore = LuaRuntime::Instance().PanicCount();
            std::string message;
            try
            {
                const sol::protected_function_result result = (*callback)(Detail::ToLuaArg(args)...);
                if (result.valid())
                {
                    return;
                }
                // The budget error from the runtime's state when it is spent (the scope above is still open), a
                // memory error from the call's status, not from an error object that can be displaced, and the text
                // read without converting (ErrorText)
                message = LuaRuntime::Instance().LimitAwareError(LuaRuntime::ErrorText(result),
                                                                 result.status() == sol::call_status::memory);
            }
            catch (const std::exception &e)
            {
                // sol pushes the arguments (a handle is a userdata) before its protected call: at the memory cap that
                // allocation fails down the panic path, and sol throws. It must not leave into the event dispatch.
                LuaRuntime::Instance().RecoverAfterPanic(panicsBefore);
                message = LuaRuntime::Instance().LimitAwareError(e.what());
            }
            catch (...)
            {
                LuaRuntime::Instance().RecoverAfterPanic(panicsBefore);
                message = LuaRuntime::Instance().LimitAwareError("unknown error");
            }

            if (LuaRuntime::IsLimitError(message))
            {
                *faulted = true;
                Logger::Error(std::format("{} callback aborted: {}. It is switched off.", eventName, message));
                return;
            }
            Logger::Error(std::format("{} callback error: {}", eventName, message));
        };
    }
}
