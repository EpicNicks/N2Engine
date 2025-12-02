#pragma once

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
        GameObject& _gameObject;
        bool _isMarkedForDestruction = false;
        bool _isActive = true;

        explicit Component(GameObject& gameObject);

    public:
        [[nodiscard]] GameObject& GetGameObject() const;

        // Serialization interface
        [[nodiscard]] virtual nlohmann::json Serialize() const;
        virtual void Deserialize(const nlohmann::json& j);
        virtual void Deserialize(const nlohmann::json& j, ReferenceResolver* resolver);
        [[nodiscard]] virtual std::string GetTypeName() const = 0;

        // Lifecycle methods
        virtual void OnAttach()
        {
        }

        virtual void OnUpdate()
        {
        }

        virtual void OnFixedUpdate()
        {
        }

        virtual void OnLateUpdate()
        {
        }

        virtual void OnDestroy()
        {
        }

        virtual void OnEnable()
        {
        }

        virtual void OnDisable()
        {
        }

        virtual void OnApplicationQuit()
        {
        }

        // physics collision events
        virtual void OnCollisionEnter(const Physics::Collision& collision)
        {
        }

        virtual void OnCollisionStay(const Physics::Collision& collision)
        {
        }

        virtual void OnCollisionExit(const Physics::Collision& collision)
        {
        }

        virtual void OnTriggerEnter(Physics::Trigger trigger)
        {
        }

        virtual void OnTriggerStay(Physics::Trigger trigger)
        {
        }

        virtual void OnTriggerExit(Physics::Trigger trigger)
        {
        }

        [[nodiscard]] bool IsDestroyed() const;
        [[nodiscard]] bool IsActive() const;
        void SetActive(bool active);

        static constexpr bool IsSingleton = false;
    };
}
