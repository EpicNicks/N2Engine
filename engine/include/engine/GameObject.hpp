#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <generator>
#include <optional>

#include <nlohmann/json.hpp>

#include "engine/base/Asset.hpp"
#include "engine/Component.hpp"
#include "engine/scheduling/CoroutineWait.hpp"
#include "engine/scheduling/Coroutine.hpp"

#include "engine/ComponentConcepts.hpp"

namespace N2Engine::Math
{
    class Vector3;
    class Quaternion;
}

namespace N2Engine
{
    class Transform;
    class Scene;
    class Positionable;
    class ReferenceResolver;

    /**
     * Container class for Components
     * Unlike Unity, may or may not have a transform/positionable
     */
    class GameObject final : public Base::Asset, public std::enable_shared_from_this<GameObject>
    {
        friend class Scene;

    public:
        using Ptr = std::shared_ptr<GameObject>;
        using WeakPtr = std::weak_ptr<GameObject>;

        /// The tag every GameObject starts with (as in Unity)
        static constexpr const char *DefaultTag = "Untagged";

    private:
        std::string _name;
        std::string _tag{DefaultTag};
        int _layer = 0; // Layers::Default
        bool _isActive = true;
        std::optional<Math::UUID> _prefabReference;
        bool _isMarkedForDestruction = false;
        // Set once destruction has finished (see IsTornDown); later than _isMarkedForDestruction, which is
        // set before OnDisable/OnDestroy run
        bool _isTornDown = false;
        mutable bool _activeInHierarchyCached = true;
        mutable bool _activeInHierarchyDirty = true;

        WeakPtr _parent;
        std::vector<Ptr> _children;
        std::unique_ptr<Positionable> _positionable;

        // Component system
        std::vector<std::unique_ptr<Component>> _components;
        std::unordered_map<std::type_index, Component*> _componentMap;

        // Transform is special - always present for positioned objects
        std::shared_ptr<Transform> _transform;

        Scene *_scene = nullptr;

        // Private methods
        void UpdateActiveInHierarchyCache() const;
        /// IsActiveInHierarchy without the destruction check: whether this object was active before
        /// it was marked for destruction (the destroy pass needs that to decide on OnDisable)
        [[nodiscard]] bool IsActiveInHierarchyIgnoringDestruction() const;
        /// Invalidates the cached IsActiveInHierarchy for this object and all descendants
        void MarkActiveInHierarchyDirty() const;
        /// Fires OnEnable/OnDisable on this subtree after its effective active state changed.
        /// Skips components that are disabled themselves, and children that are inactive themselves.
        void NotifyActiveChanged(bool nowActive) const;
        /// Call after a hierarchy change that may have flipped IsActiveInHierarchy (reparenting)
        void NotifyIfActiveChanged(bool wasActiveInHierarchy) const;
        void SetScene(Scene *scene);
        void Purge();
        /// Only unlinks the child from this object; the caller decides where it goes (a new parent, the
        /// scene's roots, or nowhere because it's being destroyed)
        void DetachChild(Ptr child, bool keepWorldPosition);
        /// Raw pointers to the current components, for running callbacks that may add or remove some
        [[nodiscard]] std::vector<Component *> SnapshotComponents() const;
        /// Whether the component is still one of this object's (a callback may have removed and freed it).
        /// A linear search, so snapshot loops are O(n^2) (fine for a handful of components). By address: a
        /// component added by a callback at a freed one's address would pass (a lifetime token would not).
        [[nodiscard]] bool OwnsComponent(const Component *component) const;

    public:
        // Construction
        static Ptr Create(const std::string &name = "GameObject");
        GameObject();
        explicit GameObject(std::string name);
        /// Detaches from its scene if it's freed while still in one, so the scene
        /// and its coroutine scheduler never keep pointers to a freed object
        ~GameObject() override;

        // Basic properties
        const std::string& GetName() const { return _name; }
        void SetName(const std::string &name) { _name = name; }

        // Tag: one free-form string per object, compared exactly (case-sensitive). Serialized.
        const std::string& GetTag() const { return _tag; }
        void SetTag(const std::string &tag) { _tag = tag; }
        bool CompareTag(const std::string &tag) const { return _tag == tag; }

        // Layer: an index 0..31 (see Layers), 0 (Default) unless set. Serialized. Its colliders' shapes move
        // to the new layer straight away; children keep their own layer (as in Unity).
        int GetLayer() const { return _layer; }
        /// Out-of-range values are clamped to 0..31 with a warning
        void SetLayer(int layer);
        /// Sets the layer on this object and all its descendants
        void SetLayerRecursive(int layer);

        // Active state management
        bool IsActive() const { return _isActive; }
        bool IsActiveInHierarchy() const;
        void SetActive(bool active);
        void SetActiveRecursive(bool active);

        // Hierarchy management
        Ptr GetParent() const { return _parent.lock(); }
        void SetParent(Ptr parent, bool keepWorldPosition = true);
        void AddChild(Ptr child, bool keepWorldPosition = true);
        /// Detaches the child, which becomes a root of its scene (the same as child->SetParent(nullptr)).
        /// A child that's being destroyed is only unlinked: it leaves the scene with its destroyed hierarchy.
        /// Note: this edits the scene's root list, so it isn't supported inside a TraverseAll/TraverseUntil
        /// callback (nor is AddChild of a root).
        void RemoveChild(Ptr child, bool keepWorldPosition = true);

        const std::vector<Ptr>& GetChildren() const { return _children; }
        size_t GetChildCount() const { return _children.size(); }
        Ptr GetChild(size_t index) const;
        Ptr FindChild(const std::string &name) const;
        Ptr FindChildRecursive(const std::string &name) const;
        std::vector<Ptr> GetChildrenRecursive() const;

        // Hierarchy utility methods
        bool IsChildOf(const Ptr &potentialParent) const;
        bool IsParentOf(const Ptr &potentialChild);
        Ptr GetRoot();
        size_t GetHierarchyDepth() const;
        std::string GetHierarchyPath() const;

        // Transform/Positionable management
        Positionable* GetPositionable() const;
        void CreatePositionable();
        bool HasPositionable() const;

        // Component system - Template declarations
        template <DerivedFromComponent T>
        T* AddComponent();

        template <DerivedFromComponent T>
        T* GetComponent() const;

        template <DerivedFromComponent T>
        std::vector<T*> GetComponents() const;

        template <DerivedFromComponent T>
        bool HasComponent() const;

        template <DerivedFromComponent T>
        bool RemoveComponent();

        // Component system - Non-template methods
        Component* GetComponent(const std::type_index &type) const;
        /// Removes the component GetComponent finds for the type (the first of it)
        bool RemoveComponent(const std::type_index &type);
        /// Removes this specific component (e.g. the second of two of a type)
        bool RemoveComponent(Component *component);
        void RemoveAllComponents();
        size_t GetComponentCount() const;
        const std::vector<std::unique_ptr<Component>>& GetAllComponents() const { return _components; }

        // Scene management
        Scene* GetScene() const { return _scene; }

        void Destroy();
        bool IsDestroyed() const;
        /// True once destruction has finished: its components' OnDisable/OnDestroy have run, or Destroy was
        /// called outside a scene. IsDestroyed is already true during those callbacks and this isn't, so
        /// references held elsewhere (e.g. by Lua scripts) keep working until the teardown is over.
        bool IsTornDown() const { return _isTornDown; }

        Scheduling::Coroutine* StartCoroutine(std::generator<Scheduling::ICoroutineWait> &&coroutine);
        bool StopCoroutine(Scheduling::Coroutine *coroutine);
        void StopAllCoroutines();

        // Serialization
        nlohmann::json Serialize() const override;
        static Ptr Deserialize(const nlohmann::json &j, ReferenceResolver *resolver = nullptr);

        std::string GetResourceType() const override { return "GameObject"; }

        // Static utility methods
        /// Scene::FindGameObject on the given scene (nullptr for a null scene)
        static Ptr FindGameObjectByName(const std::string &name, Scene *scene);
        /// Scene::FindGameObjectWithTag on the given scene (nullptr for a null scene)
        static Ptr FindGameObjectWithTag(const std::string &tag, Scene *scene);
        /// Scene::FindGameObjectsWithTag on the given scene (empty for a null scene)
        static std::vector<Ptr> FindGameObjectsWithTag(const std::string &tag, Scene *scene);
        /// The same as FindGameObjectsWithTag (the older name)
        static std::vector<Ptr> FindGameObjectsByTag(const std::string &tag, Scene *scene);
    };
}
