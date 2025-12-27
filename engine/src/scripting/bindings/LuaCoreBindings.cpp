#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindCore(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        lua.new_usertype<Positionable>(
            "Positionable",
            "GetPosition", &Positionable::GetPosition,
            "SetPosition", &Positionable::SetPosition,
            "GetRotation", &Positionable::GetRotation,
            "SetRotation", &Positionable::SetRotation,
            "GetScale", &Positionable::GetScale,
            "SetScale", &Positionable::SetScale,
            "GetForward", &Positionable::GetForward,
            "GetRight", &Positionable::GetRight,
            "GetUp", &Positionable::GetUp
        );

        lua.new_usertype<GameObject>(
            "GameObject",
            sol::call_constructor,
            sol::constructors<
                GameObject(),
                GameObject(std::string)
            >(),

            "GetName", &GameObject::GetName,
            "SetName", &GameObject::SetName,
            "IsActive", &GameObject::IsActive,
            "SetActive", &GameObject::SetActive,
            "CreatePositionable", &GameObject::CreatePositionable,
            "GetPositionable", &GameObject::GetPositionable,
            "HasPositionable", &GameObject::HasPositionable,
            "AddChild", [](GameObject &go, std::shared_ptr<GameObject> child)
            {
                go.AddChild(child);
            },
            "RemoveChild", [](GameObject &go, std::shared_ptr<GameObject> child)
            {
                go.RemoveChild(child);
            },
            "GetParent", &GameObject::GetParent,
            "FindChild", &GameObject::FindChild,
            "FindChildRecursive", &GameObject::FindChildRecursive,
            "Destroy", &GameObject::Destroy
        );

        lua.new_usertype<Component>(
            "Component",
            "GetGameObject", &Component::GetGameObject,
            "IsActive", &Component::IsActive,
            "SetActive", &Component::SetActive,
            "IsDestroyed", &Component::IsDestroyed
        );

        lua.new_usertype<Scene>(
            "Scene",
            sol::no_constructor,

            "sceneName", &Scene::sceneName,

            "FindGameObject", &Scene::FindGameObject,
            "FindGameObjectsByTag", &Scene::FindGameObjectsByTag,
            "GetAllGameObjects", &Scene::GetAllGameObjects,
            "GetRootGameObjects", &Scene::GetRootGameObjects,
            "AddRootGameObject", &Scene::AddRootGameObject,
            "RemoveRootGameObject", &Scene::RemoveRootGameObject,
            "DestroyGameObject", &Scene::DestroyGameObject
        );

        // ===== SceneManager (global) =====
        lua["SceneManager"] = lua.create_table_with(
            "GetCurrentScene", []() -> Scene&
            {
                return SceneManager::GetCurSceneRef();
            },
            "GetCurrentSceneIndex", &SceneManager::GetCurSceneIndex,
            "LoadScene", sol::overload(
                static_cast<void(*)(int)>(&SceneManager::LoadScene),
                static_cast<void(*)(const std::string &)>(&SceneManager::LoadScene)
            )
        );
    }
}
