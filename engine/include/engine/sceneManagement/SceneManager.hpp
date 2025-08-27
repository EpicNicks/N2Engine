#pragma once

#include <vector>
#include "Scene.hpp"

class SceneManager
{
private:
    int _curSceneIndex;
    std::vector<Scene> _scenes;

    static SceneManager &GetInstance();

public:
    static int GetCurSceneIndex();
    static Scene &GetCurScene();
    static void LoadScene(int sceneIndex);
    static void LoadScene(std::string &sceneName);
};