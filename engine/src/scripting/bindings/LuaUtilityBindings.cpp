#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/common/Color.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindUtility(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
        // ===== Color =====
        lua.new_usertype<Common::Color>("Color",
            sol::call_constructor,
            sol::constructors<
                Common::Color(),
                Common::Color(float, float, float, float)
            >(),
            
            sol::base_classes, sol::bases<Math::Vector4>(),

            // reference parameters from Vector4
            "r", &Common::Color::w,
            "g", &Common::Color::x,
            "b", &Common::Color::y,
            "a", &Common::Color::z,
            
            // Static colors
            "White", sol::property([]() { return Common::Color::White; }),
            "Black", sol::property([]() { return Common::Color::Black; }),
            "Red", sol::property([]() { return Common::Color::Red; }),
            "Green", sol::property([]() { return Common::Color::Green; }),
            "Blue", sol::property([]() { return Common::Color::Blue; }),
            "Cyan", sol::property([]() { return Common::Color::Cyan; }),
            "Yellow", sol::property([]() { return Common::Color::Yellow; }),
            "Magenta", sol::property([]() { return Common::Color::Magenta; }),
            "Transparent", sol::property([]() { return Common::Color::Transparent; }),
            
            "FromHex", &Common::Color::FromHex,
            "ToHex", &Common::Color::ToHex
        );
    }
}