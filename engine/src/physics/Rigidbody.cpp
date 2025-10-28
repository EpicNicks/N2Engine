#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObject.inl"
#include "engine/Positionable.hpp"
#include "engine/Logger.hpp"
#include "engine/physics/PhysicsHandle.hpp"

#include <format>

namespace N2Engine::Physics
{
    Rigidbody::Rigidbody(GameObject &gameObject)
        : Component(gameObject),
          _handle(Physics::INVALID_PHYSICS_HANDLE),
          _bodyType(BodyType::Dynamic),
          _mass(1.0f),
          _gravityEnabled(true),
          _initialized(false)
    {
    }

    Rigidbody::~Rigidbody() {}

    void Rigidbody::OnAttach()
    {
        if (_initialized)
            return;

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
        {
            Logger::Warn("Physics backend not available - Rigidbody disabled");
            return;
        }

        std::shared_ptr<Positionable> positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            GetGameObject().CreatePositionable();
            positionable = GetGameObject().GetPositionable();
        }

        // Create physics body based on type
        if (_bodyType == BodyType::Dynamic || _bodyType == BodyType::Kinematic)
        {
            _handle = backend->CreateDynamicBody(
                positionable->GetPosition(),
                positionable->GetRotation(),
                _mass,
                this, // Pass this component as the rigidbody pointer
                _bodyType == BodyType::Kinematic);

            if (_handle.IsValid())
            {
                Logger::Info(std::format("Created dynamic Rigidbody for GameObject: {}", GetGameObject().GetName()));
            }
        }
        else // BodyType::Static
        {
            _handle = backend->CreateStaticBody(
                positionable->GetPosition(),
                positionable->GetRotation(),
                this);

            if (_handle.IsValid())
            {
                Logger::Info(std::format("Created static Rigidbody for GameObject: {}", GetGameObject().GetName()));
            }
        }

        if (!_handle.IsValid())
        {
            Logger::Error(std::format("Failed to create physics body for GameObject: {}", GetGameObject().GetName()));
            return;
        }

        // Apply initial settings (only for dynamic bodies)
        if (_bodyType == BodyType::Dynamic)
        {
            backend->SetGravityEnabled(_handle, _gravityEnabled);
        }

        _initialized = true;

        std::vector<std::shared_ptr<ICollider>> colliders = GetGameObject().GetComponents<ICollider>();
        for (const auto &collider : colliders)
        {
            collider->OnAttach();
        }
    }

    void Rigidbody::OnDestroy()
    {
        if (!_handle.IsValid())
            return;

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->DestroyBody(_handle);
            Logger::Info(std::format("Destroyed Rigidbody for GameObject: {}", GetGameObject().GetName()));
        }

        _handle = Physics::INVALID_PHYSICS_HANDLE;
        _initialized = false;
    }

    // ========== Body Configuration ==========

    void Rigidbody::SetBodyType(BodyType type)
    {
        if (_bodyType == type)
            return;

        // If already initialized, we need to recreate the body
        if (_initialized)
        {
            Logger::Warn("Changing body type requires recreating physics body");
            OnDestroy();
            _bodyType = type;
            OnAttach();
        }
        else
        {
            _bodyType = type;
        }
    }

    void Rigidbody::SetMass(float mass)
    {
        if (mass <= 0.0f)
        {
            Logger::Warn("Mass must be positive, clamping to 0.001f");
            mass = 0.001f;
        }

        _mass = mass;

        if (!_handle.IsValid() || _bodyType != BodyType::Dynamic)
            return;

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->SetMass(_handle, mass);
        }
    }

    float Rigidbody::GetMass() const
    {
        if (!_handle.IsValid() || _bodyType != BodyType::Dynamic)
            return _mass;

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            return backend->GetMass(_handle);
        }

        return _mass;
    }

    void Rigidbody::SetGravityEnabled(bool enabled)
    {
        _gravityEnabled = enabled;

        if (!_handle.IsValid() || _bodyType != BodyType::Dynamic)
            return;

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->SetGravityEnabled(_handle, enabled);
        }
    }

    // ========== Forces and Motion ==========

    void Rigidbody::AddForce(const Math::Vector3 &force)
    {
        if (!_handle.IsValid() || _bodyType != BodyType::Dynamic)
        {
            if (_bodyType == BodyType::Static)
            {
                Logger::Warn("Cannot add force to static Rigidbody");
            }
            return;
        }

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->AddForce(_handle, force);
        }
    }

    void Rigidbody::AddImpulse(const Math::Vector3 &impulse)
    {
        if (!_handle.IsValid() || _bodyType != BodyType::Dynamic)
        {
            if (_bodyType == BodyType::Static)
            {
                Logger::Warn("Cannot add impulse to static Rigidbody");
            }
            return;
        }

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->AddImpulse(_handle, impulse);
        }
    }

    void Rigidbody::SetVelocity(const Math::Vector3 &velocity)
    {
        if (!_handle.IsValid() || _bodyType != BodyType::Dynamic)
        {
            if (_bodyType == BodyType::Static)
            {
                Logger::Warn("Cannot set velocity on static Rigidbody");
            }
            return;
        }

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->SetVelocity(_handle, velocity);
        }
    }

    void Rigidbody::SetAngularVelocity(const Math::Vector3 &velocity)
    {
        if (!_handle.IsValid() || _bodyType != BodyType::Dynamic)
        {
            if (_bodyType == BodyType::Static)
            {
                Logger::Warn("Cannot set angular velocity on static Rigidbody");
            }
            return;
        }

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            backend->SetAngularVelocity(_handle, velocity);
        }
    }

    // ========== Queries ==========

    Math::Vector3 Rigidbody::GetVelocity() const
    {
        if (!_handle.IsValid() || _bodyType != BodyType::Dynamic)
            return Math::Vector3::Zero();

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            return backend->GetVelocity(_handle);
        }

        return Math::Vector3::Zero();
    }

    Math::Vector3 Rigidbody::GetAngularVelocity() const
    {
        if (!_handle.IsValid() || _bodyType != BodyType::Dynamic)
            return Math::Vector3::Zero();

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (backend)
        {
            return backend->GetAngularVelocity(_handle);
        }

        return Math::Vector3::Zero();
    }

    void Rigidbody::OnTransformChanged()
    {
        if (!_initialized)
            return;

        // Only sync Transform → Physics for KINEMATIC bodies
        // Static bodies: shouldn't move (but we'll allow it with a warning)
        // Dynamic bodies: physics controls them, not transforms

        if (_bodyType == BodyType::Dynamic)
        {
            // Dynamic bodies are controlled BY physics, not by transform
            // Ignore manual transform changes
            return;
        }

        auto positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            GetGameObject().CreatePositionable();
            positionable = GetGameObject().GetPositionable();
        }

        const Transform &globalTransform = positionable->GetGlobalTransform();

        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        if (_bodyType == BodyType::Kinematic)
        {
            // KINEMATIC: Smooth script-controlled movement
            backend->SetBodyTransform(
                _handle,
                globalTransform.GetPosition(),
                globalTransform.GetRotation());
        }
        else if (_bodyType == BodyType::Static)
        {
            // STATIC: Allow but warn (expensive!)
            backend->SetStaticBodyTransform(
                _handle,
                globalTransform.GetPosition(),
                globalTransform.GetRotation());

#ifdef N2ENGINE_DEBUG
            Logger::Warn("Moving a Static Rigidbody! Consider using Kinematic type instead.");
#endif
        }
    }
}
