#include "engine/physics/ICollider.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Logger.hpp"

#include <format>

namespace N2Engine::Physics
{
    ICollider::ICollider(GameObject& gameObject)
        : Component(gameObject)
        , _handle(INVALID_PHYSICS_HANDLE)
        , _ownsBody(false)
    {
    }

    void ICollider::OnAttach()
    {
        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        if (_handle.IsValid())
        {
            if (_ownsBody)
            {
                backend->DestroyBody(_handle);
            }
            _handle = INVALID_PHYSICS_HANDLE;
            _ownsBody = false;
        }

        const Rigidbody* rb = _gameObject.GetComponent<Rigidbody>();
        if (rb && !rb->IsDestroyed() && rb->GetHandle().IsValid())
        {
            _handle = rb->GetHandle();
            _ownsBody = false;

            Logger::Info(std::format("Collider attached to Rigidbody on GameObject: {}",
                _gameObject.GetName()));
        }
        else
        {
            const Positionable* positionable = _gameObject.GetPositionable();
            if (!positionable)
            {
                _gameObject.CreatePositionable();
                positionable = _gameObject.GetPositionable();
            }

            _handle = backend->CreateStaticBody(
                positionable->GetPosition(),
                positionable->GetRotation(),
                nullptr
            );

            _ownsBody = true;

            Logger::Info(std::format("Collider created static body for GameObject: {}",
                _gameObject.GetName()));
        }

        if (_handle.IsValid())
        {
            backend->RegisterCollider(_handle, this);
            AttachShape(backend);

            if (_isTrigger)
            {
                backend->SetIsTrigger(_handle, true);
            }
        }
    }

    void ICollider::OnDestroy()
    {
        if (!_handle.IsValid())
            return;

        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->UnregisterCollider(_handle, this);

            if (_ownsBody)
            {
                backend->DestroyBody(_handle);
            }
        }

        _handle = INVALID_PHYSICS_HANDLE;
        _ownsBody = false;
    }

    void ICollider::SetIsTrigger(bool isTrigger)
    {
        _isTrigger = isTrigger;

        if (!_handle.IsValid())
            return;

        if (auto* backend = Application::GetInstance().Get3DPhysicsBackend())
        {
            backend->SetIsTrigger(_handle, isTrigger);
        }
    }

    void ICollider::SetMaterial(const PhysicsMaterial& material)
    {
        _material = material;
        UpdateShapeGeometry();
    }

    void ICollider::SetOffset(const Math::Vector3& offset)
    {
        _offset = offset;
        UpdateShapeGeometry();
    }

    void ICollider::OnTransformChanged() const
    {
        if (!_ownsBody)
            return;

        static int moveCount = 0;
        if (++moveCount % 10 == 0)
        {
            Logger::Warn("Static collider being moved frequently. "
                "Consider using a Kinematic Rigidbody instead!");
        }

        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        const Positionable* positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            GetGameObject().CreatePositionable();
            positionable = GetGameObject().GetPositionable();
        }

        backend->SetStaticBodyTransform(
            _handle,
            positionable->GetPosition(),
            positionable->GetRotation()
        );
    }
}