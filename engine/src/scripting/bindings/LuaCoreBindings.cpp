#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/GameObjectScene.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindCore(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
        // Existing bindings (GameObject, Component, Positionable)...

        lua.new_usertype<Scene>("Scene",
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
            "GetCurrentScene", []() -> Scene& {
                return SceneManager::GetCurSceneRef();
            },
            "GetCurrentSceneIndex", &SceneManager::GetCurSceneIndex,
            "LoadScene", sol::overload(
                static_cast<void(*)(int)>(&SceneManager::LoadScene),
                static_cast<void(*)(const std::string&)>(&SceneManager::LoadScene)
            )
        );
    }
}