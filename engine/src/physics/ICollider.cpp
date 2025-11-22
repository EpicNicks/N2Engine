#include "engine/physics/ICollider.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Logger.hpp"

namespace N2Engine::Physics
{
    ICollider::ICollider(GameObject &gameObject)
        : Component(gameObject)
    {
    }


    void ICollider::OnAttach()
    {
        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        // If we were already initialized, clean up first
        if (_handle.IsValid())
        {
            // Destroy old body if we owned it
            if (_ownsBody)
            {
                backend->DestroyBody(_handle);
            }
            _handle = Physics::INVALID_PHYSICS_HANDLE;
            _ownsBody = false;
        }

        // Check if GameObject has a Rigidbody

        if (const auto *rb = _gameObject.GetComponent<Rigidbody>(); rb && !rb->IsDestroyed() && rb->GetHandle().
            IsValid())
        {
            // Use Rigidbody's existing body
            _handle = rb->GetHandle();
            _ownsBody = false;

            Logger::Info(std::format("Collider attached to Rigidbody on GameObject: {}", _gameObject.GetName()));
        }
        else
        {
            // No Rigidbody - create our own static body
            const Positionable *positionable = _gameObject.GetPositionable();
            if (!positionable)
            {
                _gameObject.CreatePositionable();
                positionable = _gameObject.GetPositionable();
            }

            _handle = backend->CreateStaticBody(
                positionable->GetPosition(),
                positionable->GetRotation(),
                nullptr // No Rigidbody, but Collider can receive callbacks
            );

            _ownsBody = true;

            Logger::Info(std::format("Collider created static body for GameObject: {}", _gameObject.GetName()));
        }

        // Attach the collider shape to the body
        if (_handle.IsValid())
        {
            AttachShape(backend);

            // Apply trigger setting
            if (_isTrigger)
            {
                backend->SetIsTrigger(_handle, true);
            }

            // Register this collider with the backend
            backend->RegisterCollider(_handle, this);
        }
    }

    void ICollider::OnDestroy()
    {
        if (!_handle.IsValid())
        {
            return;
        }

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->UnregisterCollider(_handle, this);

            // Only destroy body if we created it (not owned by Rigidbody)
            if (_ownsBody)
            {
                backend->DestroyBody(_handle);
            }
        }

        _handle = Physics::INVALID_PHYSICS_HANDLE;
        _ownsBody = false;
    }

    void ICollider::SetIsTrigger(bool isTrigger)
    {
        _isTrigger = isTrigger;

        if (!_handle.IsValid())
            return;

        if (auto *backend = Application::GetInstance().Get3DPhysicsBackend())
        {
            backend->SetIsTrigger(_handle, isTrigger);
        }
    }

    void ICollider::SetMaterial(const PhysicsMaterial &material)
    {
        _material = material;
        // TODO: Update existing shapes if already created
    }

    void ICollider::OnTransformChanged() const
    {
        if (_ownsBody)
        {
            // This is a static collider
            static int moveCount = 0;
            if (++moveCount % 10 == 0)
            {
                Logger::Warn("Static collider being moved frequently. Consider using a Kinematic Rigidbody instead!");
            }

            auto *backend = Application::GetInstance().Get3DPhysicsBackend();
            if (!backend)
            {
                return;
            }
            auto positionable = GetGameObject().GetPositionable();
            if (!positionable)
            {
                GetGameObject().CreatePositionable();
                positionable = GetGameObject().GetPositionable();
            }
            // Update PhysX static body (expensive)
            backend->SetStaticBodyTransform(_handle, positionable->GetPosition(), positionable->GetRotation());
        }
    }
}
