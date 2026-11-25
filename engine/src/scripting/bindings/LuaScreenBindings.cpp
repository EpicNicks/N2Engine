#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>

#include <math/Vector2.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/Renderer.hpp>

#include "engine/Application.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/PostProcessStack.hpp"
#include "engine/rendering/RenderTexture.hpp"
#include "engine/rendering/SpriteBatcher.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"

// The Screen table: the screen tint of the game's scene pass (Application::SetScreenTint, effects design stage E2), for the
// time-stop effect (a grey circle that grows and shrinks) and full-screen grey (a death or pause screen). The UI is drawn
// over it in its own colours.
//
// The Graphics table: what the renderer the game runs on can do (effects design, decision E). Graphics.CanBlend() is false on
// the software renderer (the web's fallback), which cannot blend, so a game can skip soft glow sprites there.
//
// Issue #445 (docs/post-processing.html): the PostProcessStack component (Set{...}, Get()), RenderTexture.new(width, height) and
// Graphics.CanPostProcess().
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

        [[noreturn]] void FailPost(const std::string &message)
        {
            throw std::runtime_error("PostProcessStack:Set: " + message);
        }

        float PostNumber(const sol::object &value, const std::string &key)
        {
            if (value.get_type() != sol::type::number)
            {
                FailPost(std::format("{} must be a number", key));
            }
            const double number = value.as<double>();
            if (!std::isfinite(number) || std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max()))
            {
                FailPost(std::format("{} must be a finite number", key));
            }
            return static_cast<float>(number);
        }

        /// PostProcessStack:Set{bloom = true, bloomThreshold = 0.7, ...}: only the fields given change; each is type-checked and
        /// an unknown one is an error (a failed call changes nothing). Out-of-range numbers are kept and clamped when a frame
        /// uses them, as the inspector's are
        void SetPostProcess(Rendering::PostProcessStack &stack, const sol::object &argument)
        {
            if (argument.get_type() != sol::type::table)
            {
                FailPost("takes a table: stack:Set{bloom = true, bloomIntensity = 1.5}");
            }
            Renderer::Common::PostProcessSettings settings = stack.GetSettings();
            for (const auto &pair : argument.as<sol::table>())
            {
                if (pair.first.get_type() != sol::type::string)
                {
                    FailPost("the keys must be field names");
                }
                const std::string key = pair.first.as<std::string>();
                if (key == "bloom")
                {
                    if (pair.second.get_type() != sol::type::boolean)
                    {
                        FailPost("bloom must be a boolean");
                    }
                    settings.bloom = pair.second.as<bool>();
                }
                else if (key == "bloomThreshold")
                {
                    settings.bloomThreshold = PostNumber(pair.second, key);
                }
                else if (key == "bloomIntensity")
                {
                    settings.bloomIntensity = PostNumber(pair.second, key);
                }
                else if (key == "bloomIterations")
                {
                    const float iterations = PostNumber(pair.second, key);
                    if (iterations != std::floor(iterations))
                    {
                        FailPost("bloomIterations must be a whole number");
                    }
                    settings.bloomIterations = static_cast<int>(std::clamp(iterations, -1000.0f, 1000.0f));
                }
                else if (key == "bloomTint")
                {
                    if (!pair.second.is<Common::Color>())
                    {
                        FailPost("bloomTint must be a Color");
                    }
                    const Common::Color tint = pair.second.as<Common::Color>();
                    settings.bloomTint[0] = tint.r;
                    settings.bloomTint[1] = tint.g;
                    settings.bloomTint[2] = tint.b;
                }
                else if (key == "invert")
                {
                    settings.invert = PostNumber(pair.second, key);
                }
                else if (key == "chromaticAberration")
                {
                    settings.chromaticAberration = PostNumber(pair.second, key);
                }
                else if (key == "vignette")
                {
                    settings.vignette = PostNumber(pair.second, key);
                }
                else if (key == "blur")
                {
                    if (pair.second.get_type() != sol::type::boolean)
                    {
                        FailPost("blur must be a boolean");
                    }
                    settings.blur = pair.second.as<bool>();
                }
                else if (key == "blurStrength")
                {
                    settings.blurStrength = PostNumber(pair.second, key);
                }
                else if (key == "blurIterations")
                {
                    const float iterations = PostNumber(pair.second, key);
                    if (iterations != std::floor(iterations))
                    {
                        FailPost("blurIterations must be a whole number");
                    }
                    settings.blurIterations = static_cast<int>(std::clamp(iterations, -1000.0f, 1000.0f));
                }
                else
                {
                    FailPost(std::format("unknown field '{}' (the fields are bloom, bloomThreshold, bloomIntensity, "
                                         "bloomIterations, bloomTint, invert, chromaticAberration, vignette, blur, blurStrength, "
                                         "blurIterations)",
                                         key));
                }
            }
            stack.SetSettings(settings);
        }

        sol::table GetPostProcess(const Rendering::PostProcessStack &stack, const sol::this_state state)
        {
            sol::table table = sol::state_view(state).create_table();
            table["bloom"] = stack.bloom;
            table["bloomThreshold"] = stack.bloomThreshold;
            table["bloomIntensity"] = stack.bloomIntensity;
            table["bloomIterations"] = stack.bloomIterations;
            table["bloomTint"] = Common::Color(stack.bloomTint);
            table["invert"] = stack.invert;
            table["chromaticAberration"] = stack.chromaticAberration;
            table["vignette"] = stack.vignette;
            table["blur"] = stack.blur;
            table["blurStrength"] = stack.blurStrength;
            table["blurIterations"] = stack.blurIterations;
            return table;
        }

        /// A whole number from 1 to MaxRenderTextureSize, else a Lua error naming it
        std::uint32_t TextureSide(const double value, const char *name)
        {
            if (!std::isfinite(value) || value != std::floor(value) || value < 1.0 ||
                value > static_cast<double>(Renderer::Common::MaxRenderTextureSize))
            {
                throw std::runtime_error(std::format("RenderTexture.new: {} must be a whole number from 1 to {}", name,
                                                     Renderer::Common::MaxRenderTextureSize));
            }
            return static_cast<std::uint32_t>(value);
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
        using PostProcessRef = ComponentRef<Rendering::PostProcessStack>;
        BindComponentType<Rendering::PostProcessStack>(
            lua, "PostProcessStack",
            "Set", [](const PostProcessRef &ref, const sol::object &argument) { SetPostProcess(*ref.Pin(), argument); },
            "Get", [](const PostProcessRef &ref, const sol::this_state state) { return GetPostProcess(*ref.Pin(), state); }
        );

        lua.new_usertype<Rendering::RenderTexture>(
            "RenderTexture", sol::no_constructor,
            "new", [](const double width, const double height) {
                std::shared_ptr<Rendering::RenderTexture> texture =
                    Rendering::RenderTexture::Create(TextureSide(width, "width"), TextureSide(height, "height"));
                if (!texture)
                {
                    throw std::runtime_error("RenderTexture.new: could not make the texture");
                }
                return texture;
            },
            "GetWidth", [](const Rendering::RenderTexture &texture) { return texture.GetWidth(); },
            "GetHeight", [](const Rendering::RenderTexture &texture) { return texture.GetHeight(); },
            "GetCaptureCount", [](const Rendering::RenderTexture &texture) { return texture.GetCaptureCount(); }
        );

        lua["Graphics"] = lua.create_table_with(
            // Whether the renderer runs PostProcessStack effects and render-texture captures (all four backends do)
            "CanPostProcess", []() {
                const Renderer::Common::IRenderer *renderer = Application::GetInstance().GetWindow().GetRenderer();
                return renderer != nullptr && renderer->SupportsPostProcess();
            },
            // True without a renderer (a headless run draws nothing, so nothing is lost either way)
            "CanBlend", []() {
                const Renderer::Common::IRenderer *renderer = Application::GetInstance().GetWindow().GetRenderer();
                return renderer == nullptr || renderer->SupportsBlending();
            },
            // What the latest frame drew (issue #445): the renderer's counters and the sprite batching's. Zeros without a renderer
            "GetFrameStats", [](const sol::this_state state)
            {
                sol::state_view lua(state);
                const Renderer::Common::IRenderer *renderer = Application::GetInstance().GetWindow().GetRenderer();
                const Renderer::Common::RenderCounters counters =
                    renderer != nullptr ? renderer->GetLastFrameCounters() : Renderer::Common::RenderCounters{};
                const Rendering::SpriteBatchStats sprites = Rendering::SpriteBatcher::GetFrameStats(renderer);
                sol::table stats = lua.create_table();
                stats["drawCalls"] = counters.drawCalls;
                stats["triangles"] = counters.triangles;
                stats["vertices"] = counters.vertices;
                stats["sprites"] = sprites.sprites;
                stats["spriteDrawCalls"] = sprites.drawCalls;
                stats["spriteBatches"] = sprites.batches;
                stats["batchedSprites"] = sprites.batchedSprites;
                return stats;
            }
        );
    }
}
