#pragma once

#include <math/Vector3.hpp>

#include "engine/common/ScriptUtils.hpp"
#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Physics
{
    enum class BodyType
    {
        Static, // Doesn't move (ground, walls)
        Dynamic, // Affected by forces (player, enemies, projectiles)
        Kinematic // Moves via transform, not affected by forces (moving platforms)
    };

    // literals as values because the mapping shouldn't change if one of the BodyType constants has a name change
    NLOHMANN_JSON_SERIALIZE_ENUM(BodyType, {
                                 { BodyType::Static, "Static" },
                                 { BodyType::Dynamic, "Dynamic"},
                                 { BodyType::Kinematic, "Kinematic"}
                                 })

    /**
     * Rigidbody component - adds physics simulation to a GameObject
     * Automatically creates a physics body when the component starts
     */
    class Rigidbody : public SerializableComponent
    {
    public:
        explicit Rigidbody(GameObject &gameObject);

        std::string GetTypeName() const override;

        void OnAttach() override;
        void OnDestroy() override;
        /// Like deactivating a Unity object: while the Rigidbody is disabled or its object inactive, the
        /// whole body (with its colliders) is out of the simulation and scene queries
        void OnEnable() override;
        void OnDisable() override;

        void SetBodyType(BodyType type);
        [[nodiscard]] BodyType GetBodyType() const { return _bodyType; }

        /// The body's total mass. Like Unity, it's authoritative: the inertia tensor and centre of mass
        /// are derived from the attached non-trigger colliders (at uniform density) and rescaled to it.
        void SetMass(float mass);
        [[nodiscard]] float GetMass() const;

        void SetGravityEnabled(bool enabled);
        [[nodiscard]] bool IsGravityEnabled() const { return _gravityEnabled; }

        void AddForce(const Math::Vector3 &force) const;
        void AddImpulse(const Math::Vector3 &impulse) const;
        void SetVelocity(const Math::Vector3 &velocity) const;
        void SetAngularVelocity(const Math::Vector3 &velocity) const;

        [[nodiscard]] Math::Vector3 GetVelocity() const;
        [[nodiscard]] Math::Vector3 GetAngularVelocity() const;

        [[nodiscard]] PhysicsBodyHandle GetHandle() const { return _handle; }
        [[nodiscard]] bool IsInitialized() const { return _initialized; }

        void OnCollisionEnter(const Collision &collision) override {}
        void OnCollisionStay(const Collision &collision) override {}
        void OnCollisionExit(const Collision &collision) override {}
        void OnTriggerEnter(Trigger trigger) override {}
        void OnTriggerStay(Trigger trigger) override {}
        void OnTriggerExit(Trigger trigger) override {}

        void OnTransformChanged() const;

    protected:
        void OnActiveFlagChanged() override;

    private:
        /// Destroys the physics body; colliders sharing it are left with a stale handle
        void DestroyPhysicsBody();
        /// Puts the body in or takes it out of the simulation (a no-op without a body)
        void SetBodyEnabled(bool enabled) const;

        PhysicsBodyHandle _handle;
        BodyType _bodyType = BodyType::Dynamic;
        float _mass = 1.0f;
        bool _gravityEnabled = true;
        bool _initialized = false;
    };
}
