#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"

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
            "AddChild", [](const GameObjectRef &go, const GameObjectRef &child)
            {
                go.Pin()->AddChild(child.Pin());
            },
            "RemoveChild", [](const GameObjectRef &go, GameObjectRef &child)
            {
                const GameObject::Ptr detached = child.Pin();
                // The parent may have been its only owner; the script's handle keeps it alive instead
                child.TakeOwnership();
                go.Pin()->RemoveChild(detached);
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

        lua.new_usertype<Scene>(
            "Scene",
            sol::no_constructor,

            "sceneName", &Scene::sceneName,

            "FindGameObject", [](const Scene &scene, const std::string &name)
            {
                return RefOrNil(scene.FindGameObject(name));
            },
            "FindGameObjectsByTag", [](const Scene &scene, const std::string &tag)
            {
                return RefList(scene.FindGameObjectsByTag(tag));
            },
            "GetAllGameObjects", [](const Scene &scene) { return RefList(scene.GetAllGameObjects()); },
            "GetRootGameObjects", [](const Scene &scene) { return RefList(scene.GetRootGameObjects()); },
            "AddRootGameObject", [](Scene &scene, const GameObjectRef &gameObject)
            {
                scene.AddRootGameObject(gameObject.Pin());
            },
            "RemoveRootGameObject", [](Scene &scene, GameObjectRef &gameObject)
            {
                const GameObject::Ptr root = gameObject.Lock();
                if (!root)
                {
                    return false;
                }
                // The scene may have been its only owner; the script's handle keeps it alive instead
                gameObject.TakeOwnership();
                return scene.RemoveRootGameObject(root);
            },
            "DestroyGameObject", [](Scene &scene, const GameObjectRef &gameObject)
            {
                return scene.DestroyGameObject(gameObject.Lock()); // false once it's gone
            }
        );

        // ===== SceneManager (global) =====
        lua["SceneManager"] = lua.create_table_with(
            "GetCurrentScene", []() -> Scene*
            {
                return SceneManager::GetCurScene(); // nil when no scene is loaded
            },
            "GetCurrentSceneIndex", &SceneManager::GetCurSceneIndex,
            "LoadScene", sol::overload(
                static_cast<void(*)(int)>(&SceneManager::LoadScene),
                static_cast<void(*)(const std::string &)>(&SceneManager::LoadScene)
            )
        );
    }
}
