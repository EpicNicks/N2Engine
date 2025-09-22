#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Logger.hpp"
#include <string>

using namespace N2Engine;

SceneManager &SceneManager::GetInstance()
{
    static SceneManager instance;
    return instance;
}

Scene &SceneManager::GetCurSceneRef()
{
    return GetInstance()[GetInstance().GetCurSceneIndex()];
}

int SceneManager::GetCurSceneIndex()
{
    return GetInstance()._curSceneIndex;
}

void SceneManager::LoadScene(int sceneIndex)
{
    SceneManager &instance = GetInstance();
    if (sceneIndex < 0 || sceneIndex >= instance._scenes.size())
    {
        Logger::Error("Scene index: " + std::to_string(sceneIndex) + " out of range");
        return;
    }
    instance._sceneChange = SceneChange{true, sceneIndex};
}

void SceneManager::LoadScene(std::string &sceneName)
{
    for (int i = 0; i < GetInstance()._scenes.size(); i++)
    {
        if (GetInstance()._scenes[i]->sceneName == sceneName)
        {
            LoadScene(i);
            return;
        }
    }
    Logger::Error("Scene name not found: " + sceneName);
}

void SceneManager::AddScene(std::unique_ptr<Scene> &&scene)
{
    GetInstance()._scenes.push_back(std::move(scene));
}

void SceneManager::ProcessAnyPendingSceneChange()
{
    SceneManager &instance = GetInstance();
    if (!instance._sceneChange._updatingScene)
    {
        return;
    }
    instance._curSceneIndex = instance._sceneChange._pendingSceneIndex;
    instance._sceneChange = SceneChange{false, -1};

    // TODO populate the scene pointer (the underlying value should be able to be loaded and unloaded)
}

Scene &SceneManager::operator[](int index)
{
    return *(_scenes[index]);
}

const Scene &SceneManager::operator[](int index) const
{
    return *(_scenes[index]);
}