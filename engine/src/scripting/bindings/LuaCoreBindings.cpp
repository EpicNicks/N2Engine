#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <vector>

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        // GameObjects reach Lua only as GameObjectRef handles, never as pointers a script could keep
        sol::optional<GameObjectRef> RefOrNil(const GameObject::Ptr &gameObject)
        {
            if (!gameObject)
            {
                return sol::nullopt;
            }
            return GameObjectRef(gameObject);
        }

        // A plain Lua table (the vector itself would be pushed as a view into the scene's own list)
        sol::as_table_t<std::vector<GameObjectRef>> RefList(const std::vector<GameObject::Ptr> &gameObjects)
        {
            std::vector<GameObjectRef> refs;
            refs.reserve(gameObjects.size());
            for (const auto &gameObject : gameObjects)
            {
                refs.emplace_back(gameObject);
            }
            return sol::as_table(std::move(refs));
        }

        GameObjectRef &RequireRef(GameObjectRef *ref, const char *function)
        {
            if (!ref)
            {
                throw std::runtime_error(std::format("{}: expected a GameObject, got nil", function));
            }
            return *ref;
        }
    }

    void BindCore(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        lua.new_usertype<PositionableRef>(
            "Positionable",
            sol::no_constructor,
            sol::meta_function::equal_to, &SameRef<PositionableRef>,

            "IsValid", [](const PositionableRef &p) { return p.IsValid(); },
            "GetPosition", Forward<PositionableRef, &Positionable::GetPosition>(),
            "SetPosition", Forward<PositionableRef, &Positionable::SetPosition>(),
            "GetRotation", Forward<PositionableRef, &Positionable::GetRotation>(),
            "SetRotation", Forward<PositionableRef, &Positionable::SetRotation>(),
            "GetScale", Forward<PositionableRef, &Positionable::GetScale>(),
            "SetScale", Forward<PositionableRef, &Positionable::SetScale>(),
            "GetForward", Forward<PositionableRef, &Positionable::GetForward>(),
            "GetRight", Forward<PositionableRef, &Positionable::GetRight>(),
            "GetUp", Forward<PositionableRef, &Positionable::GetUp>()
        );

        lua.new_usertype<GameObjectRef>(
            "GameObject",
            // The script owns an object it creates, so it survives until a scene or parent holds it
            sol::call_constructor, sol::factories(
                []() { return GameObjectRef::Owning(GameObject::Create()); },
                [](const std::string &name) { return GameObjectRef::Owning(GameObject::Create(name)); }
            ),
            "Create", sol::overload(
                []() { return GameObjectRef::Owning(GameObject::Create()); },
                [](const std::string &name) { return GameObjectRef::Owning(GameObject::Create(name)); }
            ),
            sol::meta_function::equal_to, &SameRef<GameObjectRef>,

            "IsValid", [](const GameObjectRef &go) { return go.IsValid(); },
            "GetName", Forward<GameObjectRef, &GameObject::GetName>(),
            "SetName", Forward<GameObjectRef, &GameObject::SetName>(),
            "IsActive", Forward<GameObjectRef, &GameObject::IsActive>(),
            "SetActive", Forward<GameObjectRef, &GameObject::SetActive>(),
            "CreatePositionable", Forward<GameObjectRef, &GameObject::CreatePositionable>(),
            "GetPositionable", [](const GameObjectRef &go) -> sol::optional<PositionableRef>
            {
                const GameObject::Ptr gameObject = go.Pin();
                if (!gameObject->HasPositionable())
                {
                    return sol::nullopt;
                }
                return PositionableRef(*gameObject);
            },
            "HasPositionable", Forward<GameObjectRef, &GameObject::HasPositionable>(),
            // Handle arguments are pointers so nil arrives as null (a clean error) rather than failing sol's check
            "AddChild", [](const GameObjectRef &go, GameObjectRef *child)
            {
                const GameObject::Ptr parent = go.Pin();
                const GameObject::Ptr added = RequireRef(child, "GameObject:AddChild").Pin();
                parent->AddChild(added);
                if (added->GetParent() == parent)
                {
                    child->ReleaseOwnership(); // the parent owns it now
                }
            },
            "RemoveChild", [](const GameObjectRef &go, GameObjectRef *child)
            {
                const GameObject::Ptr parent = go.Pin();
                const GameObject::Ptr detached = RequireRef(child, "GameObject:RemoveChild").Pin();
                const bool wasChild = detached->GetParent() == parent;
                parent->RemoveChild(detached);
                // If RemoveChild made it a root of its scene, the scene owns it: an owning handle too would
                // keep it alive past Destroy or a scene switch
                const Scene *scene = detached->GetScene();
                const bool nowSceneRoot = scene && std::ranges::find(scene->GetRootGameObjects(), detached) !=
                                                   scene->GetRootGameObjects().end();
                if (wasChild && detached->GetParent() != parent && !nowSceneRoot)
                {
                    // The parent may have been its only owner; the handle the script passed keeps it alive
                    child->TakeOwnership();
                }
            },
            "GetParent", [](const GameObjectRef &go) { return RefOrNil(go.Pin()->GetParent()); },
            "FindChild", [](const GameObjectRef &go, const std::string &name)
            {
                return RefOrNil(go.Pin()->FindChild(name));
            },
            "FindChildRecursive", [](const GameObjectRef &go, const std::string &name)
            {
                return RefOrNil(go.Pin()->FindChildRecursive(name));
            },
            "Destroy", [](const GameObjectRef &go)
            {
                // Destroying an already destroyed object is a no-op, not an error
                if (const GameObject::Ptr gameObject = go.Lock(); gameObject && !gameObject->IsDestroyed())
                {
                    gameObject->Destroy();
                }
            },
            "AddComponent", [](const GameObjectRef &go, const std::string &typeName, sol::this_state state)
            {
                return AddComponentByName(*go.Pin(), typeName, state);
            },
            "GetComponent", [](const GameObjectRef &go, const std::string &typeName, sol::this_state state)
            {
                return GetComponentByName(*go.Pin(), typeName, state);
            }
        );

        // A handle too: the scene is freed on a scene switch, and a script may keep it past that
        SceneRef::s_luaName = "Scene";
        lua.new_usertype<SceneRef>(
            "Scene",
            sol::no_constructor,
            sol::meta_function::equal_to, &SameRef<SceneRef>,

            "IsValid", [](const SceneRef &scene) { return scene.IsValid(); },
            "sceneName", sol::property(
                [](const SceneRef &scene) { return scene.Pin()->sceneName; },
                [](const SceneRef &scene, const std::string &name) { scene.Pin()->sceneName = name; }
            ),

            "FindGameObject", [](const SceneRef &scene, const std::string &name)
            {
                return RefOrNil(scene.Pin()->FindGameObject(name));
            },
            "FindGameObjectsByTag", [](const SceneRef &scene, const std::string &tag)
            {
                return RefList(scene.Pin()->FindGameObjectsByTag(tag));
            },
            "GetAllGameObjects", [](const SceneRef &scene) { return RefList(scene.Pin()->GetAllGameObjects()); },
            "GetRootGameObjects", [](const SceneRef &scene) { return RefList(scene.Pin()->GetRootGameObjects()); },
            "AddRootGameObject", [](const SceneRef &sceneRef, GameObjectRef *gameObject)
            {
                Scene *scene = sceneRef.Pin();
                const GameObject::Ptr root = RequireRef(gameObject, "Scene:AddRootGameObject").Pin();
                scene->AddRootGameObject(root);
                if (std::ranges::find(scene->GetRootGameObjects(), root) != scene->GetRootGameObjects().end())
                {
                    gameObject->ReleaseOwnership(); // the scene owns it now
                }
            },
            "RemoveRootGameObject", [](const SceneRef &sceneRef, GameObjectRef *gameObject)
            {
                Scene *scene = sceneRef.Pin();
                const GameObject::Ptr root = gameObject ? gameObject->Lock() : nullptr;
                if (!root || !scene->RemoveRootGameObject(root))
                {
                    return false;
                }
                // The scene may have been its only owner; the handle the script passed keeps it alive
                gameObject->TakeOwnership();
                return true;
            },
            "DestroyGameObject", [](const SceneRef &sceneRef, const GameObjectRef *gameObject)
            {
                Scene *scene = sceneRef.Pin();
                // False for nil, or an object that's already destroyed or gone
                const GameObject::Ptr target = gameObject ? gameObject->Lock() : nullptr;
                return target && !target->IsDestroyed() && scene->DestroyGameObject(target);
            }
        );

        // ===== SceneManager (global) =====
        lua["SceneManager"] = lua.create_table_with(
            "GetCurrentScene", []() -> sol::optional<SceneRef>
            {
                Scene *scene = SceneManager::GetCurScene();
                if (!scene)
                {
                    return sol::nullopt; // no scene is loaded
                }
                return SceneRef(*scene);
            },
            "GetCurrentSceneIndex", &SceneManager::GetCurSceneIndex,
            "LoadScene", sol::overload(
                static_cast<void(*)(int)>(&SceneManager::LoadScene),
                static_cast<void(*)(const std::string &)>(&SceneManager::LoadScene)
            )
        );
    }
}
