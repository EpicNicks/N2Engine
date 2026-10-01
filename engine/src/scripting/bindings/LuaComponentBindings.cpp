#include "engine/scripting/bindings/LuaBindings.hpp"

#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>

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
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"

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

        // Every type here needs a Lua usertype (see BindPhysics, BindAudio and BindComponents)
        const std::map<std::string, ComponentAccess> &ComponentTable()
        {
            static const std::map<std::string, ComponentAccess> table{
                {"Rigidbody", MakeAccess<Physics::Rigidbody>()},
                {"BoxCollider", MakeAccess<Physics::BoxCollider>()},
                {"SphereCollider", MakeAccess<Physics::SphereCollider>()},
                {"CapsuleCollider", MakeAccess<Physics::CapsuleCollider>()},
                {"CubeRenderer", MakeAccess<Example::CubeRenderer>()},
                {"SphereRenderer", MakeAccess<Example::SphereRenderer>()},
                {"AudioSource", MakeAccess<Audio::AudioSource>()},
                {"AudioListener", MakeAccess<Audio::AudioListener>()},
                {"LuaComponent", MakeAccess<LuaComponent>()},
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
