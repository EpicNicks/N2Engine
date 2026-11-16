#pragma once

#include <math/Vector3.hpp>

#include "engine/physics/JointTypes.hpp"
#include "engine/physics/PhysicsHandle.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Physics
{
    class IPhysicsBackend;

    /**
     * Joint component: constrains this object's body to another object's body (or to the world).
     *
     * The body is the object's Rigidbody (or, for a static object, its collider's body). `connectedBody` names the other
     * object; empty holds this body to a fixed point of the world, `connectedAnchor` being that point in world space.
     * Needs a backend that implements PhysicsFeature::Joints (Jolt does); on another one the component warns once and
     * does nothing, so a scene still loads.
     *
     * The joint is made from the bodies' poses at that moment, in the first FixedUpdate in which both bodies exist (so it
     * does not matter in which order the Rigidbodies and the Joint were added or loaded): that pose is the joint's zero
     * (a hinge's angle 0, a slider's position 0). It is made again when either body is replaced (a changed body type, a
     * re-attached Rigidbody), ends with this component or either body, and is suspended while this component, its object or
     * either body is disabled. Joints are made in component update order, so the same scene gives the same joints in
     * the same order on every peer.
     *
     * Rollback (Jolt): SaveState, RestoreState and HashState cover the backend's joints, and RestoreState puts the joint
     * SET back: a joint made, ended or replaced inside the window (this component created, destroyed, disabled or
     * re-enabled, a jointed object spawned or despawned) is ended or made again from the SAVED settings, so its zero pose
     * is the saved one, not the bodies' poses at the restore. The joints this component makes are "owned": a joint the
     * restore made again waits for the component to take it back (in its next FixedUpdate), and is ended by the next
     * physics Update if no component does. The component set and each component's enabled flag are game state, not
     * rolled back by the engine: after a Restore the game's logic must bring the Joint components to what they were at the
     * restored tick (add or remove them, enable or disable them) before the next FixedUpdate, as it does for any
     * component state outside the replicated fields. A body replaced inside the window cannot come back (RestoreState
     * refuses, the only case).
     */
    class Joint final : public SerializableComponent
    {
    public:
        explicit Joint(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override;

        void OnAttach() override;
        void OnDestroy() override;
        void OnEnable() override;
        void OnDisable() override;
        /// This component's own flag changed (SetActive): the joint ends when it is switched off, and is made again by the
        /// first FixedUpdate after it is switched on
        void OnActiveFlagChanged() override;
        /// Makes the joint when both bodies exist, and again after either was replaced
        void OnFixedUpdate() override;
        /// The editor changed fields: the joint is made again from them
        void OnEditorFieldsChanged(std::span<const std::string> changed) override;

        /// What the joint is made of: changing one makes the joint again (from the bodies' poses then)
        void SetJointType(JointType type);
        [[nodiscard]] JointType GetJointType() const { return _jointType; }
        void SetConnectedBody(GameObject *connectedBody);
        [[nodiscard]] GameObject *GetConnectedBody() const { return _connectedBody; }
        /// On this object's body, in its local space
        void SetAnchor(const Math::Vector3 &anchor);
        [[nodiscard]] Math::Vector3 GetAnchor() const { return _anchor; }
        /// On the connected body, in its local space; with no connected body, the world position
        void SetConnectedAnchor(const Math::Vector3 &anchor);
        [[nodiscard]] Math::Vector3 GetConnectedAnchor() const { return _connectedAnchor; }
        /// Hinge axis or slider direction, in this body's local space
        void SetAxis(const Math::Vector3 &axis);
        [[nodiscard]] Math::Vector3 GetAxis() const { return _axis; }

        /// Live settings: applied to the existing joint without making it again
        void SetCollideConnected(bool collide);
        [[nodiscard]] bool GetCollideConnected() const { return _collideConnected; }
        /// Hinge: degrees. Slider and Distance: metres. A hinge's range holds 0 and a slider's holds 0 (the pose the
        /// joint was made at); Distance without limits holds the distance it was made at.
        void SetLimits(bool enabled, float minimum, float maximum);
        [[nodiscard]] bool GetUseLimits() const { return _useLimits; }
        [[nodiscard]] float GetLimitMin() const { return _limitMin; }
        [[nodiscard]] float GetLimitMax() const { return _limitMax; }

        /// The backend's joint, invalid while there is none (a body is missing, the backend has no joints)
        [[nodiscard]] PhysicsJointHandle GetHandle() const { return _handle; }

    private:
        /// Makes the joint when it should exist and does not, or replaces it when its bodies changed
        void Refresh();
        /// Ends the joint (the bodies stay)
        void Release();
        [[nodiscard]] JointDesc BuildDesc(PhysicsBodyHandle bodyA, PhysicsBodyHandle bodyB) const;
        void ApplyLive();

        JointType _jointType = JointType::Fixed;
        GameObject *_connectedBody = nullptr;
        Math::Vector3 _anchor = Math::Vector3::Zero;
        Math::Vector3 _connectedAnchor = Math::Vector3::Zero;
        Math::Vector3 _axis = Math::Vector3(1.0f, 0.0f, 0.0f);
        bool _collideConnected = false;
        bool _useLimits = false;
        float _limitMin = 0.0f;
        float _limitMax = 0.0f;

        PhysicsJointHandle _handle;
        PhysicsBodyHandle _bodyA; // the bodies the joint was made from
        PhysicsBodyHandle _bodyB;
        bool _attached = false;
        bool _warnedUnsupported = false;
    };
}
