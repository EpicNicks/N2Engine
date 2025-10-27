#include "engine/physics/ICollider.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"

#include <memory>

namespace N2Engine::Physics
{
    void Collider::OnAttach()
    {
        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        // Check if GameObject has a Rigidbody
        std::shared_ptr<Rigidbody> rb = _gameObject.GetComponent<Rigidbody>();

        if (rb && rb->GetHandle().IsValid())
        {
            // Use Rigidbody's existing body
            _handle = rb->GetHandle();
            _ownsBody = false;
        }
        else
        {
            // No Rigidbody - create our own static body
            std::shared_ptr<Positionable> positionable = _gameObject.GetPositionable();
            if (!positionable)
                return;

            _handle = backend->CreateStaticBody(
                positionable->GetPosition(),
                positionable->GetRotation(),
                nullptr // No Rigidbody, but Collider can receive callbacks
            );

            _ownsBody = true;
        }

        // Attach the collider shape
        if (_handle.IsValid())
        {
            AttachShape(backend);

            // Apply trigger setting
            if (_isTrigger)
            {
                backend->SetIsTrigger(_handle, true);
            }
        }
    }

    void Collider::OnDestroy()
    {
        if (!_handle.IsValid())
            return;

        // Only destroy body if we created it (not owned by Rigidbody)
        if (_ownsBody)
        {
            auto *backend = Application::GetInstance().Get3DPhysicsBackend();
            if (backend)
            {
                backend->DestroyBody(_handle);
            }
        }

        _handle = Physics::INVALID_PHYSICS_HANDLE;
        _ownsBody = false;
    }

    void Collider::SetIsTrigger(bool isTrigger)
    {
        _isTrigger = isTrigger;

        if (!_handle.IsValid())
            return;

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->SetIsTrigger(_handle, isTrigger);
        }
    }

    void Collider::SetMaterial(const Physics::PhysicsMaterial &material)
    {
        _material = material;
        // TODO: Update existing shapes if already created
    }

} // namespace N2Engine