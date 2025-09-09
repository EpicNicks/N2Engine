#pragma once

#include <vector>
#include "engine/sceneManagement/Scene.hpp"

namespace N2Engine
{
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
}