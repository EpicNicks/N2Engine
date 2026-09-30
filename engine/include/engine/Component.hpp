#pragma once

#include <memory>

#include <nlohmann/json.hpp>
#include "engine/base/Asset.hpp"
#include "engine/physics/PhysicsTypes.hpp"

namespace N2Engine
{
    class GameObject;
    class Scene;
    class ReferenceResolver;

    /**
     * Base class for all components
     * Components are attached to GameObjects and provide functionality
     */
    class Component : public Base::Asset
    {
        friend class GameObject;
        friend class Scene;

    protected:
        GameObject &_gameObject;
        bool _isMarkedForDestruction = false;
        bool _isActive = true;

        explicit Component(GameObject &gameObject);

    private:
        // Only this component owns it, so it expires when the component is freed. Script handles keep a
        // weak_ptr to it and report the component as destroyed instead of dangling.
        std::shared_ptr<const bool> _lifetime = std::make_shared<bool>(true);

        /// The one teardown sequence for every destroy path (Destroy, RemoveComponent, scene switch):
        /// OnDisable if the component was enabled in an active hierarchy, then OnDestroy, exactly once.
        void RunDestroyCallbacks(bool objectWasActiveInHierarchy);

    public:
        [[nodiscard]] GameObject& GetGameObject() const;

        // Serialization interface
        [[nodiscard]] nlohmann::json Serialize() const override;
        void Deserialize(const nlohmann::json &j) override;
        virtual void Deserialize(const nlohmann::json &j, ReferenceResolver *resolver);
        [[nodiscard]] virtual std::string GetTypeName() const = 0;

        std::string GetResourceType() const override { return "Component"; }

        // Lifecycle methods
        virtual void OnAttach() {}

        virtual void OnUpdate() {}

        virtual void OnFixedUpdate() {}

        virtual void OnLateUpdate() {}

        virtual void OnDestroy() {}

        virtual void OnEnable() {}

        virtual void OnDisable() {}

        virtual void OnApplicationQuit() {}

        // physics collision events
        virtual void OnCollisionEnter(const Physics::Collision &collision) {}

        virtual void OnCollisionStay(const Physics::Collision &collision) {}

        virtual void OnCollisionExit(const Physics::Collision &collision) {}

        virtual void OnTriggerEnter(Physics::Trigger trigger) {}

        virtual void OnTriggerStay(Physics::Trigger trigger) {}

        virtual void OnTriggerExit(Physics::Trigger trigger) {}

        [[nodiscard]] bool IsDestroyed() const;
        /// Expires when this component is freed (for references that must not dangle, e.g. from Lua)
        [[nodiscard]] std::weak_ptr<const bool> GetLifetimeToken() const { return _lifetime; }
        [[nodiscard]] bool IsActive() const;
        void SetActive(bool active);

        static constexpr bool IsSingleton = false;
    };
}
