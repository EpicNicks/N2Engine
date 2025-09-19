#pragma once

#include <vector>
#include <string>

#include "engine/sceneManagement/Scene.hpp"

namespace N2Engine
{
    class SceneManager
    {
        friend class Application;

    private:
        int _curSceneIndex = -1;
        std::vector<Scene> _scenes;

        static SceneManager &GetInstance();

    public:
        static int GetCurSceneIndex();
        static Scene &GetCurScene();
        static void LoadScene(int sceneIndex);
        static void LoadScene(std::string &sceneName);

        Scene &operator[](int index);
        const Scene &operator[](int index) const;
    };
}