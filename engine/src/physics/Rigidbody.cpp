#include "engine/physics/Rigidbody.hpp"
#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
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

        std::shared_ptr<Positionable> positionable = _gameObject.GetPositionable();
        if (!positionable)
        {
            Logger::Error("Rigidbody requires a Transform component");
            return;
        }

        // Create physics body based on type
        if (_bodyType == BodyType::Dynamic)
        {
            _handle = backend->CreateDynamicBody(
                positionable->GetPosition(),
                positionable->GetRotation(),
                _mass,
                this // Pass this component as the rigidbody pointer
            );

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

}