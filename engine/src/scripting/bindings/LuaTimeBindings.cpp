#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Time.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindTime(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
        // ===== Time Global =====
        lua["Time"] = lua.create_table_with(
            // Scaled time (affected by timeScale)
            "deltaTime", sol::property([]() -> float {
                return Time::GetDeltaTime();
            }),
            
            "time", sol::property([]() -> float {
                return Time::GetTime();
            }),
            
            "fixedDeltaTime", sol::property([]() -> float {
                return Time::GetFixedDeltaTime();
            }),
            
            // Unscaled time (not affected by timeScale)
            "unscaledDeltaTime", sol::property([]() -> float {
                return Time::GetUnscaledDeltaTime();
            }),
            
            "unscaledTime", sol::property([]() -> float {
                return Time::GetUnscaledTime();
            }),
            
            "fixedUnscaledDeltaTime", sol::property([]() -> float {
                return Time::GetFixedUnscaledDeltaTime();
            }),
            
            // Time scale control
            "timeScale", sol::property(
                []() -> float { 
                    return Time::GetTimeScale(); 
                },
                [](float scale) { 
                    Time::SetTimeScale(scale); 
                }
            )
        );
    }
}