#pragma once

#include <vector>
#include <string>
#include <memory>

#include "nlohmann/json.hpp"

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
        // store raw data so data doesn't persist
        std::vector<nlohmann::json> _scenes;

        // actual loaded scene, can have dynamic data not included in the original json
        std::unique_ptr<Scene> _loadedScene;

        static SceneManager& GetInstance();

    public:
        static int GetCurSceneIndex();
        static Scene* GetCurScene();
        static Scene& GetCurSceneRef();

        static void LoadScene(int sceneIndex);
        static void LoadScene(const std::string &sceneName);

        static int GetSceneIndex(const std::string &sceneName);
        /// A new Scene built from the stored data (the caller owns it); not the loaded scene. nullptr if not found.
        static std::unique_ptr<Scene> GetScene(const std::string &sceneName);

        static void AddScene(std::unique_ptr<Scene> &&scene, bool loadAdded = false);
        static void AddScene(nlohmann::json &j);
        static void UpdateScene(const std::string &sceneName, nlohmann::json &newSceneData);
        static void UpdateScene(int sceneIndex, nlohmann::json &newSceneData);
        /// Removes a stored scene by name. Refuses to delete the loaded scene or one pending load.
        static bool DeleteScene(const std::string &sceneName);

        static void ProcessAnyPendingSceneChange();
    };
}
