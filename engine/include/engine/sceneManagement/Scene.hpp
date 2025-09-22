#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <queue>

#include <renderer/common/Renderer.hpp>

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
        std::vector<std::shared_ptr<Component>> _components;
        std::queue<std::shared_ptr<Component>> _attachQueue;

        std::queue<std::shared_ptr<GameObject>> _markedForDestructionQueue;

    public:
        std::string sceneName;
        Scene(const std::string &name);

        void AddRootGameObject(std::shared_ptr<GameObject> gameObject);

        bool RemoveRootGameObject(std::shared_ptr<GameObject> gameObject);

        bool DestroyGameObject(std::shared_ptr<GameObject> gameObject);

        const std::vector<std::shared_ptr<GameObject>> &GetRootGameObjects() const { return _rootGameObjects; }
        size_t GetRootGameObjectCount() const { return _rootGameObjects.size(); }

        void TraverseAll(std::function<void(std::shared_ptr<GameObject>)> callback) const;
        void TraverseAllActive(std::function<void(std::shared_ptr<GameObject>)> callback) const;
        bool TraverseUntil(std::function<bool(std::shared_ptr<GameObject>)> callback) const;

        std::shared_ptr<GameObject> FindGameObject(const std::string &name) const;
        std::vector<std::shared_ptr<GameObject>> FindGameObjectsByTag(const std::string &tag) const;

        std::vector<std::shared_ptr<GameObject>> GetAllGameObjects() const;

        template <typename T>
        std::shared_ptr<T> FindObjectByType(bool includeInactive) const;
        template <typename T>
        std::vector<std::shared_ptr<T>> FindObjectsByType(bool includeInactive) const;

        void ProcessAttachQueue();
        void Update();
        void FixedUpdate();
        void LateUpdate();
        void AdvanceCoroutines();
        void ProcessDestroyed();
        void Clear();

    private:
        void Render(Renderer::Common::IRenderer *renderer);
        void RenderRecursive(std::shared_ptr<GameObject> gameObject, Renderer::Common::IRenderer *renderer);
        void TraverseGameObjectRecursive(std::shared_ptr<GameObject> gameObject, std::function<void(std::shared_ptr<GameObject>)> callback, bool onlyActive = false) const;
        bool TraverseGameObjectUntil(std::shared_ptr<GameObject> gameObject, std::function<bool(std::shared_ptr<GameObject>)> callback) const;
        void AddComponentToAttachQueue(std::shared_ptr<Component> component);

        void MarkHierarchyForDestruction(std::shared_ptr<GameObject> gameObject, std::vector<std::shared_ptr<GameObject>> &markedObjects);
        void CallOnDestroyForGameObject(std::shared_ptr<GameObject> gameObject);
        void PurgeMarkedGameObject(std::shared_ptr<GameObject> gameObject);
    };
}
