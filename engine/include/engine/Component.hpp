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

        Component(GameObject &gameObject);

    public:
        GameObject &GetGameObject();

        // Serialization interface
        virtual nlohmann::json Serialize() const;
        virtual void Deserialize(const nlohmann::json &j);
        virtual void Deserialize(const nlohmann::json &j, ReferenceResolver *resolver);
        virtual std::string GetTypeName() const = 0;

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
        virtual void OnCollisionEnter(Physics::Collision collision) {}
        virtual void OnCollisionStay(Physics::Collision collision) {}
        virtual void OnCollisionExit(Physics::Collision collision) {}

        virtual void OnTriggerEnter(Physics::Trigger trigger) {}
        virtual void OnTriggerStay(Physics::Trigger trigger) {}
        virtual void OnTriggerExit(Physics::Trigger trigger) {}

        bool IsDestroyed();
        bool GetIsActive();

        static constexpr bool IsSingleton = false;
    };
}