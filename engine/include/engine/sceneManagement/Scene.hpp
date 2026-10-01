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
#include "engine/base/LifetimeToken.hpp"

namespace N2Engine
{
    namespace Scheduling
    {
        class CoroutineScheduler;
    }

    class Camera;
    class Component;
    class GameObject;
    class IRenderable;

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
        std::vector<Component*> _attaching; // components in OnAttach, innermost last; DetachComponent nulls them
        std::vector<Rendering::Light*> _sceneLights;

        std::unique_ptr<Scheduling::CoroutineScheduler> _coroutineScheduler;

        std::queue<std::shared_ptr<GameObject>> _markedForDestructionQueue;
        // diagnostics
        mutable bool _hasWarnedNoLights = false;

        // Expires when this scene is freed (e.g. on a scene switch); script handles to the scene check it
        Base::LifetimeToken _lifetime;

        // Set by SceneManager's own destructor (static destruction at process exit): the scenes it still
        // holds are then freed without the drop teardown, as the systems OnDestroy uses may already be gone
        bool _skipDropTeardown = false;

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
        /// The first GameObject (in traversal order) whose tag is exactly this, or nullptr
        [[nodiscard]] std::shared_ptr<GameObject> FindGameObjectWithTag(const std::string &tag) const;
        /// Every GameObject in the scene (active or not) whose tag is exactly this, in traversal order
        [[nodiscard]] std::vector<std::shared_ptr<GameObject>> FindGameObjectsWithTag(const std::string &tag) const;
        /// The same as FindGameObjectsWithTag (the older name)
        [[nodiscard]] std::vector<std::shared_ptr<GameObject>> FindGameObjectsByTag(const std::string &tag) const;
        std::shared_ptr<GameObject> FindGameObjectByUUID(Math::UUID uuid);

        [[nodiscard]] std::vector<std::shared_ptr<GameObject>> GetAllGameObjects() const;

        template <DerivedFromComponent T>
        T* FindObjectByType(bool includeInactive = true) const;
        template <DerivedFromComponent T>
        std::vector<T*> FindObjectsByType(bool includeInactive = true) const;

        [[nodiscard]] Renderer::Common::SceneLightingData CollectLighting() const;

        /**
         * Draws the scene's renderables through `renderer` (Application::Render calls it each frame, after
         * setting the camera's view/projection and the lighting on the renderer).
         *
         * One traversal collects the active renderables (active components on objects active in the
         * hierarchy, depth first, parents before children, in component order). Then:
         * 1. Opaque renderables draw in that traversal order, with RenderState::Opaque().
         * 2. Transparent renderables draw after all of them, stable-sorted by sortKey (ascending), then by
         *    camera-space depth of their world position, back to front, with RenderState::Transparent().
         *    A renderable whose object has no Positionable sorts at depth 0 (at the camera).
         * Each renderable's queue is read once, before anything draws.
         */
        void Render(Renderer::Common::IRenderer *renderer, const Camera &camera);
        [[nodiscard]] Scheduling::CoroutineScheduler* GetCoroutineScheduler() const;
        /// Expires when this scene is freed (for references that must not dangle, e.g. from Lua)
        [[nodiscard]] std::weak_ptr<const bool> GetLifetimeToken() const { return _lifetime.Get(); }

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
        static void CollectRenderablesRecursive(const std::shared_ptr<GameObject> &gameObject,
                                                std::vector<IRenderable *> &out);
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

        /// Clear(); attachedOnly: only components that attached to this scene (are in _components) get
        /// OnDisable/OnDestroy, the rest are released without callbacks (a scene dropped unloaded)
        void ClearImpl(bool attachedOnly);
        /// Whether any component has attached to this scene (got OnAttach here) and is still registered
        [[nodiscard]] bool HasAttachedComponents() const;

        void MarkHierarchyForDestruction(std::shared_ptr<GameObject> gameObject,
                                         std::vector<std::shared_ptr<GameObject>> &markedObjects);
        void CallOnDestroyForGameObject(std::shared_ptr<GameObject> gameObject, bool attachedOnly = false);
        void PurgeMarkedGameObject(std::shared_ptr<GameObject> gameObject);
    };
}
