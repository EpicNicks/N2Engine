#include "engine/physics/ICollider.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Logger.hpp"
#include "engine/serialization/MathSerialization.hpp" // needed for Vector3

#include <format>

#include "engine/common/ScriptUtils.hpp"

namespace N2Engine::Physics
{
    ICollider::ICollider(GameObject& gameObject)
        : SerializableComponent(gameObject)
        , _handle(INVALID_PHYSICS_HANDLE)
        , _ownsBody(false)
    {
        RegisterMember(NAMEOF(_isTrigger), _isTrigger);
        RegisterMember(NAMEOF(_material), _material);
        RegisterMember(NAMEOF(_offset), _offset);
    }

    void ICollider::OnAttach()
    {
        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        // Re-attaching (a Rigidbody was added, its body type changed, or the object changed scene):
        // take this collider's shapes off the old body first. Otherwise they stayed on a shared
        // Rigidbody actor as duplicates, and the backend kept pointers to shapes the old actor freed.
        if (_handle.IsValid())
        {
            backend->UnregisterCollider(_handle, this);
            if (_ownsBody)
            {
                backend->DestroyBody(_handle);
            }
            _handle = INVALID_PHYSICS_HANDLE;
            _ownsBody = false;
        }
        _shapesAttached = false; // removed above, or released with a destroyed body

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
            // Attached while disabled or on an inactive object: registered, but no shapes until enabled
            SetShapesAttached(IsActive());
        }
    }

    void ICollider::OnEnable()
    {
        SetShapesAttached(IsActive());
    }

    void ICollider::OnDisable()
    {
        // Also the first step of teardown (OnDestroy follows), when IsActive may still be true
        SetShapesAttached(false);
    }

    void ICollider::OnActiveFlagChanged()
    {
        SetShapesAttached(IsActive());
    }

    void ICollider::SetShapesAttached(const bool attached)
    {
        if (attached == _shapesAttached || !_handle.IsValid())
            return;

        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        if (attached)
        {
            AttachShape(backend);
            if (_isTrigger)
            {
                backend->SetIsTrigger(_handle, this, true);
            }
        }
        else
        {
            // Pairs through these shapes end with an Exit (the backend's forgotten pairs)
            backend->RemoveColliderShapes(_handle, this);
        }
        _shapesAttached = attached;
    }

    void ICollider::OnDestroy()
    {
        if (!_handle.IsValid())
            return;

        if (auto* backend = Application::GetInstance().Get3DPhysicsBackend())
        {
            backend->UnregisterCollider(_handle, this);

            if (_ownsBody)
            {
                backend->DestroyBody(_handle);
            }
        }

        _handle = INVALID_PHYSICS_HANDLE;
        _ownsBody = false;
        _shapesAttached = false;
    }

    void ICollider::SetIsTrigger(bool isTrigger)
    {
        _isTrigger = isTrigger;

        if (!_handle.IsValid())
            return;

        if (auto* backend = Application::GetInstance().Get3DPhysicsBackend())
        {
            backend->SetIsTrigger(_handle, this, isTrigger);
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
