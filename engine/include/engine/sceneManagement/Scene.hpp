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

        // Opened for editing (SetEditMode)
        bool _editMode = false;

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

        /// Where CollectLighting finds the scene's lights
        enum class LightingSource
        {
            /// The lights that attached to the scene (OnAttach ran): the game's. A scene that was only loaded
            /// has none, so it is lit by the default light.
            Attached,
            /// Every active light in the hierarchy (a Light component on an object active in the hierarchy),
            /// attached or not, in traversal order, as Render finds renderables. Edit mode (the editor's
            /// viewport) uses it: components of a scene opened for editing never attach.
            Hierarchy,
        };

        /// The lighting Application::Render gives the renderer: the first lights of each type that fit
        /// (SceneLightingData::MAX_*), or one default directional light when the scene has no active light.
        /// In edit mode (IsEditMode) the lights come from the hierarchy, otherwise from the attached ones.
        [[nodiscard]] Renderer::Common::SceneLightingData CollectLighting() const;
        /// CollectLighting with the lights taken from `source`, whatever the mode
        [[nodiscard]] Renderer::Common::SceneLightingData CollectLighting(LightingSource source) const;

        /**
         * Marks the scene as opened for editing, not to run (the editor host does, for every scene it loads). Its
         * components never attach (nothing calls ProcessAttachQueue), so in edit mode:
         * - CollectLighting finds the lights in the hierarchy (LightingSource::Hierarchy), so the scene's lights
         *   light the editor's viewport;
         * - changing a GameObject's active state (SetActive, or reparenting it under an inactive object) runs no
         *   OnEnable/OnDisable on its components: they were never enabled, and a script's OnEnable must not run
         *   because someone toggled a checkbox.
         * Off by default; a scene in the game's loop never has it.
         */
        void SetEditMode(const bool editMode) { _editMode = editMode; }
        [[nodiscard]] bool IsEditMode() const { return _editMode; }

        /**
         * Draws the scene's renderables through `renderer` (Application::Render calls it each frame, after
         * setting the camera's view/projection and the lighting on the renderer).
         *
         * One traversal collects the active renderables (active components on objects active in the
         * hierarchy, depth first, parents before children, in component order). Then:
         * 1. Opaque renderables draw in that traversal order, with RenderState::Opaque() (not blended).
         * 2. Transparent renderables draw after all of them, stable-sorted by sortKey (ascending), then by
         *    camera-space depth of their world position, back to front, with RenderState::Transparent().
         *    A renderable whose object has no Positionable sorts at depth 0 (at the camera).
         * Each renderable's queue (GetRenderQueue, then DrawsInQueue for each queue) is read once, before
         * anything draws. A renderable that draws in both queues is called once in each (RenderInQueue with
         * that queue): in hierarchy order among the opaque ones, and sorted among the transparent ones.
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
        static void CollectLightsRecursive(const std::shared_ptr<GameObject> &gameObject,
                                           std::vector<Rendering::Light *> &out);
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
