#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>

#include <math/Vector2.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/Renderer.hpp>

#include "engine/Application.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"

// The Screen table: the screen tint of the game's scene pass (Application::SetScreenTint, effects design stage E2), for the
// time-stop effect (a grey circle that grows and shrinks) and full-screen grey (a death or pause screen). The UI is drawn
// over it in its own colours.
//
// The Graphics table: what the renderer the game runs on can do (effects design, decision E). Graphics.CanBlend() is false on
// the software renderer (the web's fallback), which cannot blend, so a game can skip soft glow sprites there.
namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        using Renderer::Common::ScreenTint;

        [[noreturn]] void Fail(const std::string &message)
        {
            throw std::runtime_error("Screen.SetTint: " + message);
        }

        /// A number field that is finite as a float, else a Lua error naming it (a finite double beyond the float range,
        /// such as 1e39, would become infinity and then no tint at all)
        float FiniteNumber(const sol::object &value, const std::string &key)
        {
            if (value.get_type() != sol::type::number)
            {
                Fail(std::format("{} must be a number", key));
            }
            const double number = value.as<double>();
            if (!std::isfinite(number) || std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max()))
            {
                Fail(std::format("{} must be a finite number", key));
            }
            return static_cast<float>(number);
        }

        /// Screen.SetTint{saturation = s, centre = Vector2(x, y), radius = r, inside = b}. saturation is required (0 to 1:
        /// 0 is grey, 1 no tint); the others are optional and start from ScreenTint's defaults on every call (centre
        /// (0.5, 0.5), a radius that covers the screen, inside true), never from the tint set before. Every field is
        /// type-checked and an unknown one is an error (a typo must not pass); a failed call changes nothing.
        void SetTint(const sol::object &argument)
        {
            if (argument.get_type() != sol::type::table)
            {
                Fail("takes a table: Screen.SetTint{saturation = 0.2, centre = Vector2(0.5, 0.5), radius = 1, inside = true}");
            }
            ScreenTint tint;
            bool hasSaturation = false;
            for (const auto &pair : argument.as<sol::table>())
            {
                if (pair.first.get_type() != sol::type::string)
                {
                    Fail("the keys must be field names (saturation, centre, radius, inside)");
                }
                const std::string key = pair.first.as<std::string>();
                if (key == "saturation")
                {
                    const float saturation = FiniteNumber(pair.second, key);
                    if (saturation < 0.0f || saturation > 1.0f)
                    {
                        Fail("saturation must be from 0 (grey) to 1 (no tint)");
                    }
                    tint.saturation = saturation;
                    hasSaturation = true;
                }
                else if (key == "centre")
                {
                    if (!pair.second.is<Math::Vector2>())
                    {
                        Fail("centre must be a Vector2 (0 to 1 of the screen, from the bottom left)");
                    }
                    const Math::Vector2 centre = pair.second.as<Math::Vector2>();
                    if (!std::isfinite(centre.x) || !std::isfinite(centre.y))
                    {
                        Fail("centre must be a finite Vector2");
                    }
                    tint.centreX = centre.x;
                    tint.centreY = centre.y;
                }
                else if (key == "radius")
                {
                    const float radius = FiniteNumber(pair.second, key);
                    if (radius < 0.0f)
                    {
                        Fail("radius must be 0 or more (in half screen heights)");
                    }
                    tint.radius = radius;
                }
                else if (key == "inside")
                {
                    if (pair.second.get_type() != sol::type::boolean)
                    {
                        Fail("inside must be a boolean");
                    }
                    tint.inside = pair.second.as<bool>();
                }
                else
                {
                    Fail(std::format("unknown field '{}' (the fields are saturation, centre, radius, inside)", key));
                }
            }
            if (!hasSaturation)
            {
                Fail("saturation is required (0 is grey, 1 no tint)");
            }
            Application::GetInstance().SetScreenTint(tint);
        }

        /// Screen.GetTint(): the tint as it is now, {saturation, centre, radius, inside}; saturation 1 when there is none
        sol::table GetTint(const sol::this_state state)
        {
            const ScreenTint &tint = Application::GetInstance().GetScreenTint();
            sol::table table = sol::state_view(state).create_table();
            table["saturation"] = tint.saturation;
            table["centre"] = Math::Vector2(tint.centreX, tint.centreY);
            table["radius"] = tint.radius;
            table["inside"] = tint.inside;
            return table;
        }
    }

    void BindScreen(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();
        // Lambdas, not function pointers: sol turns a C++ exception thrown inside a lambda into a Lua error
        lua["Screen"] = lua.create_table_with(
            "SetTint", [](const sol::object &argument) { SetTint(argument); },
            "ClearTint", [](const sol::variadic_args arguments) {
                if (arguments.size() != 0)
                {
                    throw std::runtime_error("Screen.ClearTint: takes no arguments");
                }
                Application::GetInstance().ClearScreenTint();
            },
            "GetTint", [](const sol::this_state state) { return GetTint(state); }
        );
        lua["Graphics"] = lua.create_table_with(
            // True without a renderer (a headless run draws nothing, so nothing is lost either way)
            "CanBlend", []() {
                const Renderer::Common::IRenderer *renderer = Application::GetInstance().GetWindow().GetRenderer();
                return renderer == nullptr || renderer->SupportsBlending();
            }
        );
    }
}
