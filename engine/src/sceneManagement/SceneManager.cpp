#include <string>

#include "engine/Logger.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/sceneManagement/Scene.hpp"

using namespace N2Engine;

SceneManager& SceneManager::GetInstance()
{
    static SceneManager instance;
    return instance;
}

Scene& SceneManager::GetCurSceneRef()
{
    return *GetInstance()._loadedScene;
}

Scene* SceneManager::GetCurScene()
{
    return GetInstance()._loadedScene.get();
}

int SceneManager::GetCurSceneIndex()
{
    return GetInstance()._curSceneIndex;
}

void SceneManager::LoadScene(const int sceneIndex)
{
    SceneManager &instance = GetInstance();
    if (sceneIndex < 0 || sceneIndex >= instance._scenes.size())
    {
        Logger::Error("Scene index: " + std::to_string(sceneIndex) + " out of range");
        return;
    }
    instance._sceneChange = SceneChange{._updatingScene = true, ._pendingSceneIndex = sceneIndex};
}

void SceneManager::LoadScene(const std::string &sceneName)
{
    if (auto sceneIndex = GetSceneIndex(sceneName); sceneIndex != -1)
    {
        LoadScene(sceneIndex);
    }
    else
    {
        Logger::Error("Scene name not found: " + sceneName + " and could not be loaded.");
    }
}

Scene* SceneManager::GetScene(const std::string &sceneName)
{
    if (int sceneIndex = GetSceneIndex(sceneName); sceneIndex != -1)
    {
        return Scene::FromJSON(GetInstance()._scenes[sceneIndex]).get();
    }
    Logger::Error("SceneManager::GetScene - Scene name not found: " + sceneName);
    return nullptr;
}

int SceneManager::GetSceneIndex(const std::string &sceneName)
{
    for (int i = 0; i < GetInstance()._scenes.size(); i++)
    {
        if (auto sceneJson = GetInstance()._scenes[i]; !sceneJson.is_null())
        {
            if (auto scene = Scene::FromJSON(sceneJson); scene != nullptr && scene->sceneName == sceneName)
            {
                return i;
            }
        }
    }
    Logger::Error("Index of scene with name: " + sceneName + " not found: ");
    return -1;
}

void SceneManager::AddScene(std::unique_ptr<Scene> &&scene, bool loadAdded)
{
    GetInstance()._scenes.push_back(scene->Serialize());
    if (loadAdded)
    {
        const int newSceneIndex = static_cast<int>(GetInstance()._scenes.size()) - 1;
        LoadScene(newSceneIndex);
    }
}

void SceneManager::AddScene(nlohmann::json &j)
{
    // verify the scene is correct json
    if (Scene::FromJSON(j, true))
    {
        GetInstance()._scenes.push_back(std::move(j));
    }
}

bool SceneManager::DeleteScene(const std::string &sceneName)
{
    if (GetScene(sceneName))
    {
        SceneManager &instance = GetInstance();
        instance._scenes.erase(instance._scenes.begin() + instance._curSceneIndex);
        return true;
    }
    return false;
}

void SceneManager::UpdateScene(int sceneIndex, nlohmann::json &newSceneData)
{
    auto &instance = GetInstance();
    if (sceneIndex > 0 && sceneIndex < static_cast<int>(instance._scenes.size()))
    {
        instance._scenes[sceneIndex] = newSceneData;
    }
}

void SceneManager::UpdateScene(const std::string &sceneName, nlohmann::json &newSceneData)
{
    UpdateScene(GetSceneIndex(sceneName), newSceneData);
}

void SceneManager::ProcessAnyPendingSceneChange()
{
    SceneManager &instance = GetInstance();
    if (!instance._sceneChange._updatingScene)
    {
        return;
    }

    if (GetCurSceneIndex() != -1)
    {
        GetCurSceneRef().Clear();
    }

    instance._curSceneIndex = instance._sceneChange._pendingSceneIndex;
    instance._sceneChange = SceneChange{false, -1};

    instance._loadedScene = Scene::FromJSON(instance._scenes.at(instance._curSceneIndex));
}
