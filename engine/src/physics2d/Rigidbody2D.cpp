#include "engine/physics2d/Rigidbody2D.hpp"

#include <format>

#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Physics2D
{
    Rigidbody2D::Rigidbody2D(GameObject &gameObject)
        : SerializableComponent(gameObject)
    {
        RegisterMember(NAMEOF(_bodyType), _bodyType);
        RegisterMember(NAMEOF(_useAutoMass), _useAutoMass);
        RegisterMember(NAMEOF(_mass), _mass);
        RegisterMember(NAMEOF(_linearDamping), _linearDamping);
        RegisterMember(NAMEOF(_angularDamping), _angularDamping);
        RegisterMember(NAMEOF(_gravityScale), _gravityScale);
        RegisterMember(NAMEOF(_fixedRotation), _fixedRotation);
        RegisterMember(NAMEOF(_bullet), _bullet);
    }

    std::string Rigidbody2D::GetTypeName() const
    {
        return NAMEOF(Rigidbody2D);
    }

    Physics2DWorld *Rigidbody2D::Live() const
    {
        const auto world = _world.lock();
        return world && _handle.IsValid() ? world.get() : nullptr;
    }

    void Rigidbody2D::OnAttach()
    {
        Scene *scene = GetGameObject().GetScene();
        const std::shared_ptr<Physics2DWorld> world = scene ? scene->GetOrCreatePhysics2DWorld() : nullptr;
        if (!world)
        {
            return; // a build without Box2D: the component keeps its data
        }
        if (_handle.IsValid() && _world.lock() == world)
        {
            return;
        }
        DestroyBody(); // it had a body in another scene's world

        if (!GetGameObject().GetPositionable())
        {
            GetGameObject().CreatePositionable();
        }

        BodyDesc2D desc;
        desc.type = _bodyType;
        desc.owner = &GetGameObject();
        desc.rigidbody = this;
        desc.linearVelocity = _pendingLinearVelocity;
        desc.angularVelocity = _pendingAngularVelocity;
        desc.linearDamping = _linearDamping;
        desc.angularDamping = _angularDamping;
        desc.gravityScale = _gravityScale;
        desc.fixedRotation = _fixedRotation;
        desc.bullet = _bullet;
        desc.enabled = IsActive();
        _handle = world->CreateBody(desc);
        _world = world;
        _pendingLinearVelocity = Math::Vector2();
        _pendingAngularVelocity = 0.0f;
        world->SetMass(_handle, _useAutoMass, _mass);

        // Colliders attached before this body move onto it
        for (auto *collider : GetGameObject().GetComponents<Collider2D>())
        {
            if (!collider->IsDestroyed())
            {
                collider->OnAttach();
            }
        }
    }

    void Rigidbody2D::DestroyBody()
    {
        if (Physics2DWorld *world = Live())
        {
            world->DestroyBody(_handle);
        }
        _handle = Physics::INVALID_PHYSICS_HANDLE;
        _world.reset();
    }

    void Rigidbody2D::OnDestroy()
    {
        const bool hadBody = Live() != nullptr;
        DestroyBody();

        // As with 3D: colliders that stay after the Rigidbody2D is removed become static colliders. Skipped when the whole
        // object is being destroyed, since its colliders are about to go too.
        if (!hadBody || GetGameObject().IsDestroyed())
        {
            return;
        }
        for (auto *collider : GetGameObject().GetComponents<Collider2D>())
        {
            if (!collider->IsDestroyed())
            {
                collider->OnAttach();
            }
        }
    }

    void Rigidbody2D::SetBodyEnabled(const bool enabled) const
    {
        if (Physics2DWorld *world = Live())
        {
            world->SetBodyEnabled(_handle, enabled);
        }
    }

    void Rigidbody2D::OnEnable()
    {
        SetBodyEnabled(IsActive());
    }

    void Rigidbody2D::OnDisable()
    {
        SetBodyEnabled(false);
    }

    void Rigidbody2D::OnActiveFlagChanged()
    {
        SetBodyEnabled(IsActive());
    }

    void Rigidbody2D::SetBodyType(const BodyType2D type)
    {
        _bodyType = type;
        if (Physics2DWorld *world = Live())
        {
            world->SetBodyType(_handle, type);
        }
    }

    void Rigidbody2D::SetMass(const float mass)
    {
        if (!(mass > 0.0f))
        {
            Logger::Warn(std::format("Rigidbody2D on '{}': the mass must be above zero, not {}", GetGameObject().GetName(), mass));
            return;
        }
        _mass = mass;
        _useAutoMass = false;
        if (Physics2DWorld *world = Live())
        {
            world->SetMass(_handle, false, mass);
        }
    }

    void Rigidbody2D::UseAutoMass()
    {
        _useAutoMass = true;
        if (Physics2DWorld *world = Live())
        {
            world->SetMass(_handle, true, _mass);
        }
    }

    float Rigidbody2D::GetMass() const
    {
        const Physics2DWorld *world = Live();
        return world ? world->GetMass(_handle) : _mass;
    }

    void Rigidbody2D::SetLinearVelocity(const Math::Vector2 &velocity)
    {
        if (Physics2DWorld *world = Live())
        {
            world->SetLinearVelocity(_handle, velocity);
        }
        else
        {
            _pendingLinearVelocity = velocity;
        }
    }

    Math::Vector2 Rigidbody2D::GetLinearVelocity() const
    {
        const Physics2DWorld *world = Live();
        return world ? world->GetLinearVelocity(_handle) : _pendingLinearVelocity;
    }

    void Rigidbody2D::SetAngularVelocity(const float radiansPerSecond)
    {
        if (Physics2DWorld *world = Live())
        {
            world->SetAngularVelocity(_handle, radiansPerSecond);
        }
        else
        {
            _pendingAngularVelocity = radiansPerSecond;
        }
    }

    float Rigidbody2D::GetAngularVelocity() const
    {
        const Physics2DWorld *world = Live();
        return world ? world->GetAngularVelocity(_handle) : _pendingAngularVelocity;
    }

    void Rigidbody2D::SetLinearDamping(const float damping)
    {
        _linearDamping = damping;
        if (Physics2DWorld *world = Live())
        {
            world->SetDamping(_handle, _linearDamping, _angularDamping);
        }
    }

    void Rigidbody2D::SetAngularDamping(const float damping)
    {
        _angularDamping = damping;
        if (Physics2DWorld *world = Live())
        {
            world->SetDamping(_handle, _linearDamping, _angularDamping);
        }
    }

    void Rigidbody2D::SetGravityScale(const float scale)
    {
        _gravityScale = scale;
        if (Physics2DWorld *world = Live())
        {
            world->SetGravityScale(_handle, scale);
        }
    }

    void Rigidbody2D::SetFixedRotation(const bool fixed)
    {
        _fixedRotation = fixed;
        if (Physics2DWorld *world = Live())
        {
            world->SetFixedRotation(_handle, fixed);
        }
    }

    void Rigidbody2D::SetBullet(const bool bullet)
    {
        _bullet = bullet;
        if (Physics2DWorld *world = Live())
        {
            world->SetBullet(_handle, bullet);
        }
    }

    void Rigidbody2D::AddForce(const Math::Vector2 &force) const
    {
        if (Physics2DWorld *world = Live())
        {
            world->AddForce(_handle, force);
        }
    }

    void Rigidbody2D::AddImpulse(const Math::Vector2 &impulse) const
    {
        if (Physics2DWorld *world = Live())
        {
            world->AddImpulse(_handle, impulse);
        }
    }

    void Rigidbody2D::AddTorque(const float torque) const
    {
        if (Physics2DWorld *world = Live())
        {
            world->AddTorque(_handle, torque);
        }
    }

    void Rigidbody2D::AddAngularImpulse(const float impulse) const
    {
        if (Physics2DWorld *world = Live())
        {
            world->AddAngularImpulse(_handle, impulse);
        }
    }

    void Rigidbody2D::OnTransformChanged() const
    {
        Physics2DWorld *world = Live();
        if (world && !world->IsSyncingTransforms(_handle))
        {
            world->MarkTransformDirty(_handle);
        }
    }
}
