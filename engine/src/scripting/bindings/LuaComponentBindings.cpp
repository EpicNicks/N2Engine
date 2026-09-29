#include "engine/scripting/bindings/LuaBindings.hpp"

#include <format>
#include <functional>
#include <map>
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
        struct ComponentAccess
        {
            std::function<sol::object(GameObject &, sol::this_state)> add;
            std::function<sol::object(const GameObject &, sol::this_state)> get;
        };

        template <typename T>
        ComponentAccess MakeAccess()
        {
            return {
                [](GameObject &go, sol::this_state state) { return sol::make_object(state, go.AddComponent<T>()); },
                [](const GameObject &go, sol::this_state state) -> sol::object
                {
                    if (T *component = go.GetComponent<T>())
                    {
                        return sol::make_object(state, component);
                    }
                    return sol::lua_nil;
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

        // The setters live on the PolygonRenderer<T> template base, which isn't a Lua type,
        // so bind through lambdas on the concrete renderer
        lua.new_usertype<Example::CubeRenderer>(
            "CubeRenderer",
            sol::no_constructor,
            sol::base_classes, sol::bases<Component>(),

            "SetColor", [](Example::CubeRenderer &r, const Common::Color &color) { r.SetColor(color); },
            "GetColor", [](const Example::CubeRenderer &r) { return r.GetColor(); },
            "SetSize", [](Example::CubeRenderer &r, const Math::Vector3 &size) { r.SetSize(size); },
            "GetSize", [](const Example::CubeRenderer &r) { return r.GetSize(); }
        );

        lua.new_usertype<Example::SphereRenderer>(
            "SphereRenderer",
            sol::no_constructor,
            sol::base_classes, sol::bases<Component>(),

            "SetColor", [](Example::SphereRenderer &r, const Common::Color &color) { r.SetColor(color); },
            "GetColor", [](const Example::SphereRenderer &r) { return r.GetColor(); },
            "SetRadius", &Example::SphereRenderer::SetRadius,
            "GetRadius", &Example::SphereRenderer::GetRadius,
            "SetSubdivision", &Example::SphereRenderer::SetSubdivision
        );

        lua.new_usertype<LuaComponent>(
            "LuaComponent",
            sol::no_constructor,
            sol::base_classes, sol::bases<Component>(),

            // Path to a script under the project's assets, e.g. "res://scripts/CameraController.lua"
            "SetScript", [](LuaComponent &c, const std::string &path) { c.SetScript(IO::ResourcePath(path)); },
            "GetScriptPath", [](const LuaComponent &c) { return c.GetScriptPath().ToString(); },
            "HasMissingScript", &LuaComponent::HasMissingScript
        );
    }
}
