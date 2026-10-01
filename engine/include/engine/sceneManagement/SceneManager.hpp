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
            // The caller's own Scene from AddScene(scene, true): loaded as-is rather than a copy rebuilt
            // from the stored data, so objects the caller kept belong to the live scene
            std::unique_ptr<Scene> _pendingScene;
        };

        // A registered scene: its data (what loading it rebuilds from) and its name, kept alongside so
        // looking a scene up by name never builds it
        struct StoredScene
        {
            std::string name;
            nlohmann::json data;
        };

        SceneChange _sceneChange;

        int _curSceneIndex = -1;
        // store raw data so data doesn't persist
        std::vector<StoredScene> _scenes;

        // actual loaded scene, can have dynamic data not included in the original json
        std::unique_ptr<Scene> _loadedScene;

        static SceneManager& GetInstance();
        /// Index of the stored scene with this name, or -1 (without logging)
        static int FindSceneIndex(const std::string &sceneName);
        /// Stores data under the scene name it contains, replacing a stored scene with that name. Returns its index.
        static int StoreScene(nlohmann::json data);
        /// Tears down the loaded scene and drops a pending one (stored data is kept). Called by
        /// Application::Shutdown while physics/audio are still up, not left to static destruction.
        static void UnloadScenes();

    public:
        /// Static destruction at process exit (no Application::Shutdown, e.g. tests): the scenes still held
        /// are freed without teardown callbacks, since the systems those callbacks use may be gone already
        ~SceneManager();

        static int GetCurSceneIndex();
        static Scene* GetCurScene();
        static Scene& GetCurSceneRef();

        static void LoadScene(int sceneIndex);
        static void LoadScene(const std::string &sceneName);

        static int GetSceneIndex(const std::string &sceneName);
        /// Whether a scene with this name is stored (GetSceneIndex, without logging a miss)
        static bool HasScene(const std::string &sceneName);
        /// A new Scene built from the stored data (the caller owns it); not the loaded scene. nullptr if not found.
        static std::unique_ptr<Scene> GetScene(const std::string &sceneName);

        /// Stores a snapshot of the scene, replacing (with a warning) any stored scene with the same name.
        /// Always takes the scene. loadAdded: this very Scene object becomes the loaded scene at the next
        /// ProcessAnyPendingSceneChange (unless another load is requested first); loading the scene again
        /// later rebuilds it from the snapshot. Otherwise only the snapshot is kept and the object is destroyed.
        static void AddScene(std::unique_ptr<Scene> scene, bool loadAdded = false);
        /// Stores scene data if it builds, replacing (with a warning) any stored scene with the same name.
        static void AddScene(nlohmann::json &j);
        /// Replaces stored data. A loaded scene keeps running as it is until it is loaded again.
        /// Refused if the data renames the scene to the name of another stored scene.
        static void UpdateScene(const std::string &sceneName, nlohmann::json &newSceneData);
        static void UpdateScene(int sceneIndex, nlohmann::json &newSceneData);
        /// Removes a stored scene by name. Refuses to delete the loaded scene or one pending load.
        static bool DeleteScene(const std::string &sceneName);

        static void ProcessAnyPendingSceneChange();
    };
}
