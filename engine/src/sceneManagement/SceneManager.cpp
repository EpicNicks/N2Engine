#include <format>
#include <string>

#include "engine/Logger.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/sceneManagement/Scene.hpp"

using namespace N2Engine;

namespace
{
    // The name stored scene data declares (what Scene::Deserialize would set), without building it
    std::string SceneNameOf(const nlohmann::json &sceneData)
    {
        if (sceneData.is_object())
        {
            if (const auto it = sceneData.find("name"); it != sceneData.end() && it->is_string())
            {
                return it->get<std::string>();
            }
        }
        return {};
    }
}

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
    // A repeated request for the scene already pending keeps the caller's instance from AddScene;
    // a request for another scene supersedes it (that instance is destroyed, its snapshot stays stored)
    std::unique_ptr<Scene> pendingScene;
    if (instance._sceneChange._updatingScene && instance._sceneChange._pendingSceneIndex == sceneIndex)
    {
        pendingScene = std::move(instance._sceneChange._pendingScene);
    }
    instance._sceneChange = SceneChange{
        ._updatingScene = true, ._pendingSceneIndex = sceneIndex, ._pendingScene = std::move(pendingScene)
    };
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

std::unique_ptr<Scene> SceneManager::GetScene(const std::string &sceneName)
{
    if (int sceneIndex = GetSceneIndex(sceneName); sceneIndex != -1)
    {
        // Returned by value: the old version returned .get() of this temporary, which dangled
        return Scene::FromJSON(GetInstance()._scenes[sceneIndex].data);
    }
    Logger::Error("SceneManager::GetScene - Scene name not found: " + sceneName);
    return nullptr;
}

int SceneManager::GetSceneIndex(const std::string &sceneName)
{
    for (int i = 0; i < GetInstance()._scenes.size(); i++)
    {
        // Compare the name kept beside the data; deserializing the scene (building every object and
        // component, with their physics/audio/script state) just to read its name was far too costly
        if (GetInstance()._scenes[i].name == sceneName)
        {
            return i;
        }
    }
    Logger::Error("Index of scene with name: " + sceneName + " not found: ");
    return -1;
}

int SceneManager::StoreScene(nlohmann::json data)
{
    SceneManager &instance = GetInstance();
    std::string name = SceneNameOf(data);
    if (!name.empty())
    {
        for (int i = 0; i < static_cast<int>(instance._scenes.size()); i++)
        {
            if (instance._scenes[i].name != name)
            {
                continue;
            }
            // Same name: replace it, since lookups by name could never reach a second entry
            instance._scenes[i].data = std::move(data);
            if (instance._sceneChange._pendingSceneIndex == i)
            {
                // A load already pending for this scene builds from the new data, not an older instance
                instance._sceneChange._pendingScene.reset();
            }
            return i;
        }
    }
    instance._scenes.push_back(StoredScene{.name = std::move(name), .data = std::move(data)});
    return static_cast<int>(instance._scenes.size()) - 1;
}

void SceneManager::AddScene(std::unique_ptr<Scene> &&scene, bool loadAdded)
{
    if (!scene)
    {
        Logger::Error("SceneManager::AddScene - scene is null");
        return;
    }

    // The snapshot is what later loads of this scene rebuild from
    const int index = StoreScene(scene->Serialize());
    if (loadAdded)
    {
        // Load the caller's object itself (it used to be dropped and rebuilt from the snapshot, leaving
        // every GameObject::Ptr the caller kept in an orphaned scene)
        LoadScene(index);
        GetInstance()._sceneChange._pendingScene = std::move(scene);
    }
}

void SceneManager::AddScene(nlohmann::json &j)
{
    // verify the scene is correct json
    if (Scene::FromJSON(j, true))
    {
        StoreScene(std::move(j));
    }
}

bool SceneManager::DeleteScene(const std::string &sceneName)
{
    const int index = GetSceneIndex(sceneName);
    if (index == -1)
    {
        return false;
    }

    SceneManager &instance = GetInstance();
    if (index == instance._curSceneIndex ||
        (instance._sceneChange._updatingScene && index == instance._sceneChange._pendingSceneIndex))
    {
        Logger::Error("SceneManager::DeleteScene - can't delete a loaded or loading scene: " + sceneName);
        return false;
    }

    // Erase the named scene (this used to erase the loaded scene's index instead)
    instance._scenes.erase(instance._scenes.begin() + index);

    // Indices after the erased one shift down by one
    if (instance._curSceneIndex > index)
    {
        --instance._curSceneIndex;
    }
    if (instance._sceneChange._updatingScene && instance._sceneChange._pendingSceneIndex > index)
    {
        --instance._sceneChange._pendingSceneIndex;
    }
    return true;
}

void SceneManager::UpdateScene(int sceneIndex, nlohmann::json &newSceneData)
{
    auto &instance = GetInstance();
    if (sceneIndex >= 0 && sceneIndex < static_cast<int>(instance._scenes.size()))
    {
        instance._scenes[sceneIndex] = StoredScene{.name = SceneNameOf(newSceneData), .data = newSceneData};
        if (instance._sceneChange._pendingSceneIndex == sceneIndex)
        {
            // A load already pending for this scene builds from the new data, not an older instance
            instance._sceneChange._pendingScene.reset();
        }
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

    SceneChange change = std::move(instance._sceneChange);
    instance._sceneChange = SceneChange{};
    const int nextIndex = change._pendingSceneIndex;

    // Build the next scene before tearing down the current one: if its data is malformed, the
    // current scene stays loaded (it used to be cleared first, leaving no scene at all).
    // A scene handed to AddScene(scene, true) is used as is; any other load rebuilds from the stored data.
    std::unique_ptr<Scene> nextScene = change._pendingScene
                                           ? std::move(change._pendingScene)
                                           : Scene::FromJSON(instance._scenes.at(nextIndex).data);
    if (!nextScene)
    {
        Logger::Error(std::format("Scene {} could not be loaded; keeping the current scene", nextIndex));
        return;
    }

    if (GetCurSceneIndex() != -1 && instance._loadedScene)
    {
        GetCurSceneRef().Clear();
    }

    instance._curSceneIndex = nextIndex;
    instance._loadedScene = std::move(nextScene);
}
