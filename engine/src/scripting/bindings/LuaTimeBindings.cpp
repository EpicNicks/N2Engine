#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Time.hpp"

#include <cstdint>

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        // sol::property only works on usertypes: on a plain table, Time.deltaTime would be
        // the property object rather than a number. Time is a single instance of this type.
        struct LuaTime {};
    }

    void BindTime(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();

        // ===== Time Global =====
        lua.new_usertype<LuaTime>("LuaTime",
            sol::no_constructor,

            // Scaled time (affected by timeScale)
            "deltaTime", sol::property([](const LuaTime&) -> float {
                return Time::GetDeltaTime();
            }),

            "time", sol::property([](const LuaTime&) -> float {
                return Time::GetTime();
            }),

            "fixedDeltaTime", sol::property([](const LuaTime&) -> float {
                return Time::GetFixedDeltaTime();
            }),

            // Unscaled time (not affected by timeScale)
            "unscaledDeltaTime", sol::property([](const LuaTime&) -> float {
                return Time::GetUnscaledDeltaTime();
            }),

            "unscaledTime", sol::property([](const LuaTime&) -> float {
                return Time::GetUnscaledTime();
            }),

            "fixedUnscaledDeltaTime", sol::property([](const LuaTime&) -> float {
                return Time::GetFixedUnscaledDeltaTime();
            }),

            // Fixed ticks begun (read-only integer)
            "tick", sol::property([](const LuaTime&) -> std::int64_t {
                return static_cast<std::int64_t>(Time::GetTick());
            }),

            // Time scale control
            "timeScale", sol::property(
                [](const LuaTime&) -> float {
                    return Time::GetTimeScale();
                },
                [](LuaTime&, float scale) {
                    Time::SetTimeScale(scale);
                }
            )
        );

        lua["Time"] = LuaTime{};
    }
}
