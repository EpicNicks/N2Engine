#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Logger.hpp"
#include <string>

using namespace N2Engine;

SceneManager &SceneManager::GetInstance()
{
    static SceneManager instance;
    return instance;
}

Scene &SceneManager::GetCurScene()
{
    return GetInstance()[GetInstance().GetCurSceneIndex()];
}

int SceneManager::GetCurSceneIndex()
{
    return GetInstance()._curSceneIndex;
}

void SceneManager::LoadScene(int sceneIndex)
{
    if (sceneIndex < 0 || sceneIndex >= GetInstance()._scenes.size())
    {
        Logger::Error("Scene index: " + std::to_string(sceneIndex) + " out of range");
        return;
    }
    GetInstance()._curSceneIndex = sceneIndex;
    // handle loading
}

void SceneManager::LoadScene(std::string &sceneName)
{
    for (int i = 0; i < GetInstance()._scenes.size(); i++)
    {
        if (GetInstance()._scenes[i].sceneName == sceneName)
        {
            LoadScene(i);
            return;
        }
    }
    Logger::Error("Scene name not found: " + sceneName);
}

Scene &SceneManager::operator[](int index)
{
    return _scenes[index];
}

const Scene &SceneManager::operator[](int index) const
{
    return _scenes[index];
}