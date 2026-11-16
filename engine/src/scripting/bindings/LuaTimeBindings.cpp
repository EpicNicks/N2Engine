#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/Time.hpp"

#include <cstdint>
#include <format>

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        // sol::property only works on usertypes: on a plain table, Time.deltaTime would be
        // the property object rather than a number. Time is a single instance of this type.
        struct LuaTime {};

        thread_local int simDepth = 0;

        // Inside a simulation callback of a network mode the frame clock is a per-peer value
        bool UseTickClock()
        {
            return simDepth > 0 && Application::GetInstance().GetSimulationClock().IsNetworked();
        }

        float SimTime()
        {
            return static_cast<float>(static_cast<double>(Time::GetTick()) * Time::GetFixedTimestep());
        }

        float UnscaledTimeForScripts()
        {
            if (UseTickClock() &&
                Application::GetInstance().GetSimulationMode() == SimulationMode::Deterministic)
            {
                static bool warned = false;
                if (!warned)
                {
                    warned = true;
                    Logger::Warn("Time.unscaledTime was read in OnFixedUpdate of a Deterministic session: it is the frame "
                                 "clock and differs between peers. Use Time.time (tick clock there) or Time.tick");
                }
            }
            return UnscaledTimeForScripts();
        }
    }

    SimCallbackScope::SimCallbackScope() noexcept { ++simDepth; }
    SimCallbackScope::~SimCallbackScope() { --simDepth; }
    bool SimCallbackScope::Active() noexcept { return simDepth > 0; }

    void BindTime(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();

        // ===== Time Global =====
        lua.new_usertype<LuaTime>("LuaTime",
            sol::no_constructor,

            // Scaled time (affected by timeScale)
            "deltaTime", sol::property([](const LuaTime&) -> float {
                return UseTickClock() ? Time::GetFixedDeltaTime() : Time::GetDeltaTime();
            }),

            "time", sol::property([](const LuaTime&) -> float {
                return UseTickClock() ? SimTime() : Time::GetTime();
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
