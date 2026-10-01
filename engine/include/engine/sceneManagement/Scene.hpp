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
#include "engine/rendering/Light.hpp"
#include "engine/base/Asset.hpp"

namespace N2Engine
{
    namespace Scheduling
    {
        class CoroutineScheduler;
    }

    class Component;
    class GameObject;

    class Scene : public Base::Asset
    {
        friend class SceneManager;
        friend class Application;
        friend class GameObject;

    private:
        std::vector<std::shared_ptr<GameObject>> _rootGameObjects;
        // Raw views of components owned by GameObjects. Every path that removes a component or
        // object from this scene goes through DetachComponent, so these never hold freed pointers.
        // mutable: OnAllActiveComponents (const) compacts entries detached while it was iterating
        mutable std::vector<Component*> _components;
        mutable int _componentIterationDepth = 0;
        std::vector<Component*> _attachQueue; // vector, not queue, so pending entries can be removed
        Component *_attachingComponent = nullptr; // the one in OnAttach; DetachComponent clears it
        std::vector<Rendering::Light*> _sceneLights;

        std::unique_ptr<Scheduling::CoroutineScheduler> _coroutineScheduler;

        std::queue<std::shared_ptr<GameObject>> _markedForDestructionQueue;
        // diagnostics
        mutable bool _hasWarnedNoLights = false;

    private:
        explicit Scene(std::string name);

    public:
        std::string sceneName;

    public:
        ~Scene() override;
        // Not movable: GameObjects and the coroutine scheduler hold pointers to their Scene, which a
        // move would leave pointing at the moved-from object (scenes live behind unique_ptr instead)
        Scene(Scene &&) = delete;
        Scene& operator=(Scene &&) = delete;

        Scene(const Scene &) = delete;
        Scene& operator=(const Scene &) = delete;

        static std::unique_ptr<Scene> Create(const std::string &name);

        void AddRootGameObject(std::shared_ptr<GameObject> gameObject);
        void AddRootGameObjects(const std::vector<std::shared_ptr<GameObject>> &gameObjects);
        void AddRootGameObjects(std::initializer_list<std::shared_ptr<GameObject>> gameObjects);

        bool RemoveRootGameObject(std::shared_ptr<GameObject> gameObject);

        bool DestroyGameObject(std::shared_ptr<GameObject> gameObject);

        [[nodiscard]] const std::vector<std::shared_ptr<GameObject>>& GetRootGameObjects() const
        {
            return _rootGameObjects;
        }

        [[nodiscard]] size_t GetRootGameObjectCount() const { return _rootGameObjects.size(); }

        void TraverseAll(std::function<void(std::shared_ptr<GameObject>)> callback) const;
        void TraverseAllActive(std::function<void(std::shared_ptr<GameObject>)> callback) const;
        bool TraverseUntil(std::function<bool(std::shared_ptr<GameObject>)> callback) const;

        [[nodiscard]] std::shared_ptr<GameObject> FindGameObject(const std::string &name) const;
        [[nodiscard]] std::vector<std::shared_ptr<GameObject>> FindGameObjectsByTag(const std::string &tag) const;
        std::shared_ptr<GameObject> FindGameObjectByUUID(Math::UUID uuid);

        [[nodiscard]] std::vector<std::shared_ptr<GameObject>> GetAllGameObjects() const;

        template <DerivedFromComponent T>
        T* FindObjectByType(bool includeInactive = true) const;
        template <DerivedFromComponent T>
        std::vector<T*> FindObjectsByType(bool includeInactive = true) const;

        [[nodiscard]] Renderer::Common::SceneLightingData CollectLighting() const;
        [[nodiscard]] Scheduling::CoroutineScheduler* GetCoroutineScheduler() const;

        void ProcessAttachQueue();
        void Update() const;
        void FixedUpdate() const;
        void LateUpdate() const;
        void AdvanceCoroutines() const;
        void ProcessDestroyed();
        void OnApplicationQuit() const;
        void Clear();

        [[nodiscard]] nlohmann::json Serialize() const override;
        void Deserialize(const nlohmann::json &j) override;

        static std::unique_ptr<Scene> FromJSON(const nlohmann::json &j, bool validate = false);

        std::string GetResourceType() const override;

    private:
        void Render(Renderer::Common::IRenderer *renderer);
        void RenderRecursive(std::shared_ptr<GameObject> gameObject, Renderer::Common::IRenderer *renderer);
        void TraverseGameObjectRecursive(std::shared_ptr<GameObject> gameObject,
                                         std::function<void(std::shared_ptr<GameObject>)> callback,
                                         bool onlyActive = false) const;
        bool TraverseGameObjectUntil(std::shared_ptr<GameObject> gameObject,
                                     std::function<bool(std::shared_ptr<GameObject>)> callback) const;
        void AddComponentToAttachQueue(Component *component);
        /// Forgets a component: drops it from _components, _sceneLights and the attach queue.
        /// Called before a component is freed or leaves this scene (RemoveComponent, SetScene, Clear).
        void DetachComponent(Component *component);

        void OnAllActiveComponents(const std::function<void(Component *)> &callback) const;

        void MarkHierarchyForDestruction(std::shared_ptr<GameObject> gameObject,
                                         std::vector<std::shared_ptr<GameObject>> &markedObjects);
        void CallOnDestroyForGameObject(std::shared_ptr<GameObject> gameObject);
        void PurgeMarkedGameObject(std::shared_ptr<GameObject> gameObject);
    };
}
