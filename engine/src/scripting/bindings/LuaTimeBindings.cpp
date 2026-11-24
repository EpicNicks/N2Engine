#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/Time.hpp"

#include <chrono>
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
        // One warning per session, not per process: SetSimulationMode resets it
        bool unscaledWarned = false;
        int unscaledWarnings = 0;

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
                if (!unscaledWarned)
                {
                    unscaledWarned = true;
                    ++unscaledWarnings;
                    Logger::Warn("Time.unscaledTime was read in OnFixedUpdate of a Deterministic session: it is the frame "
                                 "clock and differs between peers. Use Time.time (tick clock there) or Time.tick");
                }
            }
            return Time::GetUnscaledTime();
        }
    }

    SimCallbackScope::SimCallbackScope() noexcept { ++simDepth; }
    SimCallbackScope::~SimCallbackScope() { --simDepth; }
    bool SimCallbackScope::Active() noexcept { return simDepth > 0; }
    void ResetSimTimeWarnings() noexcept { unscaledWarned = false; }
    int SimTimeWarningCount() noexcept { return unscaledWarnings; }

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
                return UnscaledTimeForScripts();
            }),

            "fixedUnscaledDeltaTime", sol::property([](const LuaTime&) -> float {
                return Time::GetFixedUnscaledDeltaTime();
            }),

            // Wall-clock time: Unix time, UTC seconds since 1970 as a float (sub-second precision). For offline
            // progress and timestamps; not frame-aligned and not the same on two machines (never use it in
            // simulation code of a network session). The user can change the system clock, so it can go backwards.
            "unixTime", sol::property([](const LuaTime&) -> double {
                return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
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
