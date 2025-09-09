#include "engine/sceneManagement/SceneManager.hpp"

using namespace N2Engine;

SceneManager &SceneManager::GetInstance()
{
    static SceneManager instance;
    return instance;
}

Scene &SceneManager::GetCurScene()
{
    return GetInstance()._scenes[GetInstance().GetCurSceneIndex()];
}

int SceneManager::GetCurSceneIndex()
{
    return GetInstance()._curSceneIndex;
}

void SceneManager::LoadScene(int sceneIndex)
{
    if (sceneIndex < 0 || sceneIndex >= GetInstance()._scenes.size())
    {
        // log error maybe
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
    // log error maybe
}