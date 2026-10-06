#include "engine/scripting/bindings/LuaBindings.hpp"

#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "engine/GameObjectScene.hpp"
#include "engine/audio/AudioListener.hpp"
#include "engine/audio/AudioSource.hpp"
#include "engine/common/Color.hpp"
#include "engine/example/renderers/CubeRenderer.hpp"
#include "engine/example/renderers/SphereRenderer.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/TextRenderer.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/text/Font.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UIText.hpp"

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        // Components reach Lua only as ComponentRef<T> handles, never as raw pointers a script could keep
        struct ComponentAccess
        {
            std::function<sol::object(GameObject &, sol::this_state)> add;
            std::function<sol::object(const GameObject &, sol::this_state)> get;
            std::function<sol::object(Component &, lua_State *)> asLua; // null object if not this type
            std::function<std::optional<ComponentRefBase>(const sol::object &)> asRef; // nullopt if not this type
        };

        template <typename T>
        ComponentAccess MakeAccess()
        {
            return {
                [](GameObject &go, sol::this_state state)
                {
                    return sol::make_object(state, ComponentRef<T>(*go.AddComponent<T>()));
                },
                [](const GameObject &go, sol::this_state state) -> sol::object
                {
                    if (T *component = go.GetComponent<T>())
                    {
                        return sol::make_object(state, ComponentRef<T>(*component));
                    }
                    return sol::lua_nil;
                },
                [](Component &component, lua_State *state) -> sol::object
                {
                    if (auto *typed = dynamic_cast<T *>(&component))
                    {
                        return sol::make_object(state, ComponentRef<T>(*typed));
                    }
                    return sol::lua_nil;
                },
                [](const sol::object &object) -> std::optional<ComponentRefBase>
                {
                    if (object.is<ComponentRef<T>>())
                    {
                        return ComponentRefBase(object.as<ComponentRef<T>>());
                    }
                    return std::nullopt;
                },
            };
        }

        // Every type here needs a Lua usertype (see BindPhysics, BindAudio, BindComponents and BindUI)
        const std::map<std::string, ComponentAccess> &ComponentTable()
        {
            static const std::map<std::string, ComponentAccess> table{
                {"Rigidbody", MakeAccess<Physics::Rigidbody>()},
                {"BoxCollider", MakeAccess<Physics::BoxCollider>()},
                {"SphereCollider", MakeAccess<Physics::SphereCollider>()},
                {"CapsuleCollider", MakeAccess<Physics::CapsuleCollider>()},
                {"CubeRenderer", MakeAccess<Example::CubeRenderer>()},
                {"SphereRenderer", MakeAccess<Example::SphereRenderer>()},
                {"TextRenderer", MakeAccess<Rendering::TextRenderer>()},
                {"AudioSource", MakeAccess<Audio::AudioSource>()},
                {"AudioListener", MakeAccess<Audio::AudioListener>()},
                {"LuaComponent", MakeAccess<LuaComponent>()},
                {"RectTransform", MakeAccess<UI::RectTransform>()},
                {"Canvas", MakeAccess<UI::Canvas>()},
                {"Image", MakeAccess<UI::Image>()},
                {"UIText", MakeAccess<UI::UIText>()},
                {"Button", MakeAccess<UI::Button>()},
            };
            return table;
        }

        const ComponentAccess &FindAccess(const std::string &typeName)
        {
            const auto &table = ComponentTable();
            if (const auto it = table.find(typeName); it != table.end())
            {
                return it->second;
            }

            std::string known;
            for (const auto &name : table | std::views::keys)
            {
                known += known.empty() ? name : ", " + name;
            }
            // sol turns this into a Lua error at the call site
            throw std::runtime_error(std::format("Unknown component type '{}'. Known types: {}", typeName, known));
        }
    }

    // Text alignments travel as their scene-file names ("Left", "Middle", ...)
    Text::HorizontalAlign ParseHorizontalAlign(const std::string &name)
    {
        if (name == "Left") return Text::HorizontalAlign::Left;
        if (name == "Center") return Text::HorizontalAlign::Center;
        if (name == "Right") return Text::HorizontalAlign::Right;
        throw std::runtime_error(
            std::format("Unknown horizontal alignment '{}'. Expected Left, Center or Right", name));
    }

    Text::VerticalAlign ParseVerticalAlign(const std::string &name)
    {
        if (name == "Top") return Text::VerticalAlign::Top;
        if (name == "Middle") return Text::VerticalAlign::Middle;
        if (name == "Bottom") return Text::VerticalAlign::Bottom;
        if (name == "Baseline") return Text::VerticalAlign::Baseline;
        throw std::runtime_error(
            std::format("Unknown vertical alignment '{}'. Expected Top, Middle, Bottom or Baseline", name));
    }

    std::string AlignName(const Text::HorizontalAlign align)
    {
        switch (align)
        {
        case Text::HorizontalAlign::Center: return "Center";
        case Text::HorizontalAlign::Right: return "Right";
        case Text::HorizontalAlign::Left:
        default: return "Left";
        }
    }

    std::string AlignName(const Text::VerticalAlign align)
    {
        switch (align)
        {
        case Text::VerticalAlign::Middle: return "Middle";
        case Text::VerticalAlign::Bottom: return "Bottom";
        case Text::VerticalAlign::Baseline: return "Baseline";
        case Text::VerticalAlign::Top:
        default: return "Top";
        }
    }

    std::shared_ptr<Text::Font> LoadFontOrThrow(const std::string &path, const std::string_view caller)
    {
        auto font = IO::Resources::Instance().Load<Text::Font>(std::filesystem::path(path));
        if (!font || !font->IsLoaded())
        {
            throw std::runtime_error(std::format("{}: can't load font '{}'", caller, path));
        }
        return font;
    }

    sol::object AddComponentByName(GameObject &gameObject, const std::string &typeName, sol::this_state state)
    {
        return FindAccess(typeName).add(gameObject, state);
    }

    sol::object GetComponentByName(const GameObject &gameObject, const std::string &typeName, sol::this_state state)
    {
        return FindAccess(typeName).get(gameObject, state);
    }

    sol::object ComponentToLua(Component &component, lua_State *state)
    {
        for (const auto &access : ComponentTable() | std::views::values)
        {
            if (sol::object typed = access.asLua(component, state); typed.valid())
            {
                return typed;
            }
        }
        return sol::make_object(state, ComponentRef<Component>(component));
    }

    bool SameComponent(const sol::object &a, const sol::object &b)
    {
        const auto asRef = [](const sol::object &object) -> std::optional<ComponentRefBase>
        {
            for (const auto &access : ComponentTable() | std::views::values)
            {
                if (auto ref = access.asRef(object))
                {
                    return ref;
                }
            }
            if (object.is<ComponentRef<Component>>())
            {
                return ComponentRefBase(object.as<ComponentRef<Component>>());
            }
            return std::nullopt;
        };

        const auto refA = asRef(a);
        const auto refB = asRef(b);
        return refA && refB && refA->RefersTo(*refB);
    }

    std::vector<std::string> GetScriptableComponentNames()
    {
        std::vector<std::string> names;
        for (const auto &name : ComponentTable() | std::views::keys)
        {
            names.push_back(name);
        }
        return names;
    }

    void BindComponents(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        // Returned by ComponentToLua for components with no Lua type of their own
        BindComponentType<Component>(lua, "Component");

        using CubeRendererRef = ComponentRef<Example::CubeRenderer>;
        BindComponentType<Example::CubeRenderer>(
            lua, "CubeRenderer",
            "SetColor", Forward<CubeRendererRef, &Example::CubeRenderer::SetColor>(),
            "GetColor", Forward<CubeRendererRef, &Example::CubeRenderer::GetColor>(),
            "SetSize", Forward<CubeRendererRef, &Example::CubeRenderer::SetSize>(),
            "GetSize", Forward<CubeRendererRef, &Example::CubeRenderer::GetSize>()
        );

        using SphereRendererRef = ComponentRef<Example::SphereRenderer>;
        BindComponentType<Example::SphereRenderer>(
            lua, "SphereRenderer",
            "SetColor", Forward<SphereRendererRef, &Example::SphereRenderer::SetColor>(),
            "GetColor", Forward<SphereRendererRef, &Example::SphereRenderer::GetColor>(),
            "SetRadius", Forward<SphereRendererRef, &Example::SphereRenderer::SetRadius>(),
            "GetRadius", Forward<SphereRendererRef, &Example::SphereRenderer::GetRadius>(),
            "SetSubdivision", Forward<SphereRendererRef, &Example::SphereRenderer::SetSubdivision>()
        );

        using TextRendererRef = ComponentRef<Rendering::TextRenderer>;
        BindComponentType<Rendering::TextRenderer>(
            lua, "TextRenderer",
            "SetText", Forward<TextRendererRef, &Rendering::TextRenderer::SetText>(),
            "GetText", Forward<TextRendererRef, &Rendering::TextRenderer::GetText>(),
            "SetColor", Forward<TextRendererRef, &Rendering::TextRenderer::SetColor>(),
            "GetColor", Forward<TextRendererRef, &Rendering::TextRenderer::GetColor>(),
            "SetFontSize", Forward<TextRendererRef, &Rendering::TextRenderer::SetFontSize>(),
            "GetFontSize", Forward<TextRendererRef, &Rendering::TextRenderer::GetFontSize>(),
            "SetMaxWidth", Forward<TextRendererRef, &Rendering::TextRenderer::SetMaxWidth>(),
            "GetMaxWidth", Forward<TextRendererRef, &Rendering::TextRenderer::GetMaxWidth>(),
            "SetLineSpacing", Forward<TextRendererRef, &Rendering::TextRenderer::SetLineSpacing>(),
            "GetLineSpacing", Forward<TextRendererRef, &Rendering::TextRenderer::GetLineSpacing>(),
            "SetLetterSpacing", Forward<TextRendererRef, &Rendering::TextRenderer::SetLetterSpacing>(),
            "GetLetterSpacing", Forward<TextRendererRef, &Rendering::TextRenderer::GetLetterSpacing>(),
            // Both names are checked before either is applied, so a bad one changes nothing
            "SetAlignment", [](const TextRendererRef &c, const std::string &horizontal, const std::string &vertical)
            {
                const Text::HorizontalAlign h = ParseHorizontalAlign(horizontal);
                const Text::VerticalAlign v = ParseVerticalAlign(vertical);
                const auto renderer = c.Pin();
                renderer->SetHorizontalAlign(h);
                renderer->SetVerticalAlign(v);
            },
            "GetAlignment", [](const TextRendererRef &c)
            {
                const auto renderer = c.Pin();
                return std::make_tuple(AlignName(renderer->GetHorizontalAlign()),
                                       AlignName(renderer->GetVerticalAlign()));
            },
            // A font file, e.g. "res://fonts/Title.ttf"; nil goes back to the default font. A path that
            // doesn't load raises an error and keeps the current font.
            "SetFont", [](const TextRendererRef &c, sol::optional<std::string> path)
            {
                const auto renderer = c.Pin();
                if (!path)
                {
                    renderer->SetFont(nullptr);
                    return;
                }
                renderer->SetFont(LoadFontOrThrow(*path, "TextRenderer:SetFont"));
            },
            // The laid-out block in local units: minX, minY, maxX, maxY (all 0 for empty text)
            "GetBounds", [](const TextRendererRef &c)
            {
                const auto renderer = c.Pin();
                const Text::Rect &bounds = renderer->GetLayout().bounds;
                return std::make_tuple(bounds.minX, bounds.minY, bounds.maxX, bounds.maxY);
            }
        );

        using LuaComponentRef = ComponentRef<LuaComponent>;
        BindComponentType<LuaComponent>(
            lua, "LuaComponent",
            // Path to a script under the project's assets, e.g. "res://scripts/CameraController.lua"
            "SetScript", [](const LuaComponentRef &c, const std::string &path)
            {
                c.Pin()->SetScript(IO::ResourcePath(path));
            },
            "GetScriptPath", [](const LuaComponentRef &c) { return c.Pin()->GetScriptPath().ToString(); },
            "HasMissingScript", Forward<LuaComponentRef, &LuaComponent::HasMissingScript>()
        );
    }
}
