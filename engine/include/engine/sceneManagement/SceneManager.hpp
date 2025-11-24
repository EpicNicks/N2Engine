#pragma once

#include <vector>
#include <string>
#include <memory>

namespace N2Engine
{
    class Scene;

    class SceneManager
    {
        friend class Application;

    private:
        struct SceneChange
        {
            bool _updatingScene = false;
            int _pendingSceneIndex = -1;
        };

        SceneChange _sceneChange;

        int _curSceneIndex = -1;
        std::vector<std::unique_ptr<Scene>> _scenes;

        static SceneManager &GetInstance();

        static void ProcessAnyPendingSceneChange();

    public:
        static int GetCurSceneIndex();
        static Scene &GetCurSceneRef();
        static void LoadScene(int sceneIndex);
        static void LoadScene(const std::string &sceneName);
        static void AddScene(std::unique_ptr<Scene> &&scene);

        Scene &operator[](int index);
        const Scene &operator[](int index) const;
    };
}