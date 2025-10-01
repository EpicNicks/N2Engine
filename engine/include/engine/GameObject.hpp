#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <type_traits>
#include <generator>
#include <nlohmann/json.hpp>

#include "engine/base/Asset.hpp"
#include "engine/Component.hpp"
#include "engine/scheduling/CoroutineWait.hpp"
#include "engine/scheduling/Coroutine.hpp"

// Forward declarations
namespace Math
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
    class GameObject : public Base::Asset, public std::enable_shared_from_this<GameObject>
    {
        friend class Scene;

    public:
        using Ptr = std::shared_ptr<GameObject>;
        using WeakPtr = std::weak_ptr<GameObject>;

    private:
        std::string _name;
        bool _isActive = true;
        bool _isMarkedForDestruction = false;
        mutable bool _activeInHierarchyCached = true;
        mutable bool _activeInHierarchyDirty = true;

        WeakPtr _parent;
        std::vector<Ptr> _children;
        std::shared_ptr<Positionable> _positionable = nullptr;

        // Component system
        std::vector<std::shared_ptr<Component>> _components;
        std::unordered_map<std::type_index, std::shared_ptr<Component>> _componentMap;

        // Transform is special - always present for positioned objects
        std::shared_ptr<Transform> _transform;

        Scene *_scene = nullptr;

        // Private methods
        void UpdateActiveInHierarchyCache() const;
        void NotifyActiveChanged();
        void SetScene(Scene *scene);
        void Purge();

    public:
        // Construction
        static Ptr Create(const std::string &name = "GameObject");
        explicit GameObject(const std::string &name);

        // Basic properties
        const std::string &GetName() const { return _name; }
        void SetName(const std::string &name) { _name = name; }

        // Active state management
        bool IsActive() const { return _isActive; }
        bool IsActiveInHierarchy() const;
        void SetActive(bool active);
        void SetActiveRecursive(bool active);

        // Hierarchy management
        Ptr GetParent() const { return _parent.lock(); }
        void SetParent(Ptr parent, bool keepWorldPosition = true);
        void AddChild(Ptr child, bool keepWorldPosition = true);
        void RemoveChild(Ptr child, bool keepWorldPosition = true);

        const std::vector<Ptr> &GetChildren() const { return _children; }
        size_t GetChildCount() const { return _children.size(); }
        Ptr GetChild(size_t index) const;
        Ptr FindChild(const std::string &name) const;
        Ptr FindChildRecursive(const std::string &name) const;
        std::vector<Ptr> GetChildrenRecursive() const;

        // Hierarchy utility methods
        bool IsChildOf(Ptr potentialParent);
        bool IsParentOf(Ptr potentialChild);
        Ptr GetRoot();
        size_t GetHierarchyDepth() const;
        std::string GetHierarchyPath() const;

        // Transform/Positionable management
        std::shared_ptr<Positionable> GetPositionable() const;
        void CreatePositionable();
        bool HasPositionable() const;

        // Component system - Template declarations
        template <typename T>
        std::shared_ptr<T> AddComponent();

        template <typename T>
        std::shared_ptr<T> GetComponent() const;

        template <typename T>
        std::vector<std::shared_ptr<T>> GetComponents() const;

        template <typename T>
        bool HasComponent() const;

        template <typename T>
        bool RemoveComponent();

        // Component system - Non-template methods
        std::shared_ptr<Component> GetComponent(const std::type_index &type) const;
        bool RemoveComponent(const std::type_index &type);
        void RemoveAllComponents();
        size_t GetComponentCount() const;
        const std::vector<std::shared_ptr<Component>> &GetAllComponents() const { return _components; }

        // Scene management
        Scene *GetScene() const { return _scene; }

        void Destroy();
        bool IsDestroyed() const;

        Scheduling::Coroutine *StartCoroutine(std::generator<Scheduling::ICoroutineWait> &&coroutine);
        bool StopCoroutine(Scheduling::Coroutine *coroutine);
        void StopAllCoroutines();

        // Serialization
        nlohmann::json Serialize() const;
        static Ptr Deserialize(const nlohmann::json &j, ReferenceResolver *resolver = nullptr);

        // Static utility methods
        static Ptr FindGameObjectByName(const std::string &name, Scene *scene);
        static std::vector<Ptr> FindGameObjectsByTag(const std::string &tag, Scene *scene);
    };
}