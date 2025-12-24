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
            "White", sol::var(Common::Color::White),
            "Black", sol::var(Common::Color::Black),
            "Red", sol::var(Common::Color::Red),
            "Green", sol::var(Common::Color::Green),
            "Blue", sol::var(Common::Color::Blue),
            "Cyan", sol::var(Common::Color::Cyan),
            "Yellow", sol::var(Common::Color::Yellow),
            "Magenta", sol::var(Common::Color::Magenta),
            "Transparent", sol::var(Common::Color::Transparent),
            
            "FromHex", &Common::Color::FromHex,
            "ToHex", &Common::Color::ToHex
        );
    }
}