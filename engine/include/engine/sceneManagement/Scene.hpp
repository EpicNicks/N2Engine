#pragma once

#include <string>
#include <memory>
#include <vector>
#include <initializer_list>
#include <functional>
#include <queue>
#include <nlohmann/json.hpp>

#include <renderer/common/Renderer.hpp>
#include "engine/ComponentConcepts.hpp"

namespace N2Engine
{
    class Component;
    class GameObject;

    class Scene
    {
        friend class SceneManager;
        friend class Application;
        friend class GameObject;

    private:
        std::vector<std::shared_ptr<GameObject>> _rootGameObjects;
        std::vector<Component*> _components;
        std::queue<Component*> _attachQueue;

        std::queue<std::shared_ptr<GameObject>> _markedForDestructionQueue;
        explicit Scene(std::string name);

    public:
        std::string sceneName;

        static std::unique_ptr<Scene> Create(const std::string &name);

        void AddRootGameObject(std::shared_ptr<GameObject> gameObject);
        void AddRootGameObjects(const std::vector<std::shared_ptr<GameObject>> &gameObjects);
        void AddRootGameObjects(std::initializer_list<std::shared_ptr<GameObject>> gameObjects);

        bool RemoveRootGameObject(std::shared_ptr<GameObject> gameObject);

        bool DestroyGameObject(std::shared_ptr<GameObject> gameObject);

        [[nodiscard]] const std::vector<std::shared_ptr<GameObject>> &GetRootGameObjects() const { return _rootGameObjects; }
        [[nodiscard]] size_t GetRootGameObjectCount() const { return _rootGameObjects.size(); }

        void TraverseAll(std::function<void(std::shared_ptr<GameObject>)> callback) const;
        void TraverseAllActive(std::function<void(std::shared_ptr<GameObject>)> callback) const;
        bool TraverseUntil(std::function<bool(std::shared_ptr<GameObject>)> callback) const;

        [[nodiscard]] std::shared_ptr<GameObject> FindGameObject(const std::string &name) const;
        [[nodiscard]] std::vector<std::shared_ptr<GameObject>> FindGameObjectsByTag(const std::string &tag) const;

        [[nodiscard]] std::vector<std::shared_ptr<GameObject>> GetAllGameObjects() const;

        template <DerivedFromComponent T>
        std::shared_ptr<T> FindObjectByType(bool includeInactive) const;
        template <DerivedFromComponent T>
        std::vector<std::shared_ptr<T>> FindObjectsByType(bool includeInactive) const;

        void ProcessAttachQueue();
        void Update() const;
        void FixedUpdate() const;
        void LateUpdate() const;
        void AdvanceCoroutines();
        void ProcessDestroyed();
        void OnApplicationQuit() const;
        void Clear();

        [[nodiscard]] nlohmann::json Serialize() const;
        static std::shared_ptr<Scene> Deserialize(const nlohmann::json &j);

    private:
        void Render(Renderer::Common::IRenderer *renderer);
        void RenderRecursive(std::shared_ptr<GameObject> gameObject, Renderer::Common::IRenderer *renderer);
        void TraverseGameObjectRecursive(std::shared_ptr<GameObject> gameObject, std::function<void(std::shared_ptr<GameObject>)> callback, bool onlyActive = false) const;
        bool TraverseGameObjectUntil(std::shared_ptr<GameObject> gameObject, std::function<bool(std::shared_ptr<GameObject>)> callback) const;
        void AddComponentToAttachQueue(Component* component);

        void OnAllActiveComponents(const std::function<void(Component*)>& callback) const;

        void MarkHierarchyForDestruction(std::shared_ptr<GameObject> gameObject, std::vector<std::shared_ptr<GameObject>> &markedObjects);
        void CallOnDestroyForGameObject(std::shared_ptr<GameObject> gameObject);
        void PurgeMarkedGameObject(std::shared_ptr<GameObject> gameObject);
    };
}
