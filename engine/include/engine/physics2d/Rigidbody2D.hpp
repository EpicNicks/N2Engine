#pragma once

#include <memory>

#include <math/Vector2.hpp>

#include "engine/physics2d/Physics2DTypes.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Physics2D
{
    class Physics2DWorld;

    /// Adds 2D physics to a GameObject. The body uses the object's world x, y and rotation about z; z is left alone. Velocity
    /// set before the body exists (before the component attaches) is kept and applied when it does.
    class Rigidbody2D : public SerializableComponent
    {
    public:
        explicit Rigidbody2D(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override;

        void OnAttach() override;
        void OnDestroy() override;
        void OnEnable() override;
        void OnDisable() override;

        void SetBodyType(BodyType2D type);
        [[nodiscard]] BodyType2D GetBodyType() const { return _bodyType; }

        /// Overrides the mass worked out from the colliders' density; the inertia is rescaled to it
        void SetMass(float mass);
        /// Goes back to the mass worked out from the colliders' density
        void UseAutoMass();
        [[nodiscard]] bool UsesAutoMass() const { return _useAutoMass; }
        [[nodiscard]] float GetMass() const;

        void SetLinearVelocity(const Math::Vector2 &velocity);
        [[nodiscard]] Math::Vector2 GetLinearVelocity() const;
        void SetAngularVelocity(float radiansPerSecond);
        [[nodiscard]] float GetAngularVelocity() const;

        void SetLinearDamping(float damping);
        [[nodiscard]] float GetLinearDamping() const { return _linearDamping; }
        void SetAngularDamping(float damping);
        [[nodiscard]] float GetAngularDamping() const { return _angularDamping; }
        void SetGravityScale(float scale);
        [[nodiscard]] float GetGravityScale() const { return _gravityScale; }
        void SetFixedRotation(bool fixed);
        [[nodiscard]] bool IsFixedRotation() const { return _fixedRotation; }
        /// Continuous collision against other moving bodies, for fast projectiles
        void SetBullet(bool bullet);
        [[nodiscard]] bool IsBullet() const { return _bullet; }

        void AddForce(const Math::Vector2 &force) const;
        void AddImpulse(const Math::Vector2 &impulse) const;
        void AddTorque(float torque) const;
        void AddAngularImpulse(float impulse) const;

        [[nodiscard]] Physics::PhysicsBodyHandle GetHandle() const { return _handle; }
        /// The world the body is in, or null without a body
        [[nodiscard]] std::shared_ptr<Physics2DWorld> GetWorld() const { return _world.lock(); }

        void OnTransformChanged() const;

    protected:
        void OnActiveFlagChanged() override;

    private:
        void DestroyBody();
        void SetBodyEnabled(bool enabled) const;
        /// The world and handle when the body exists, else null
        [[nodiscard]] Physics2DWorld *Live() const;

        // A weak reference: the Scene owns the world, and this component can outlive it when its object moves
        std::weak_ptr<Physics2DWorld> _world;
        Physics::PhysicsBodyHandle _handle;
        BodyType2D _bodyType = BodyType2D::Dynamic;
        bool _useAutoMass = true;
        float _mass = 1.0f;
        float _linearDamping = 0.0f;
        float _angularDamping = 0.0f;
        float _gravityScale = 1.0f;
        bool _fixedRotation = false;
        bool _bullet = false;
        Math::Vector2 _pendingLinearVelocity;
        float _pendingAngularVelocity = 0.0f;
    };
}
