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
     * Not in a rollback snapshot yet: the backend's SaveState and HashState do not cover joints.
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
        /// Makes the joint when both bodies exist, and again after either was replaced
        void OnFixedUpdate() override;
        /// The editor changed fields: the joint is made again from them
        void OnEditorFieldsChanged(std::span<const std::string> changed) override;
        /// The editor's field edit, with the limits checked as a whole (see ValidateLimits): a request that touches useLimits,
        /// limitMin or limitMax and leaves them inconsistent throws std::invalid_argument, which the editor shows and
        /// does not apply. A request that only changes the type or other fields is never refused for old limits.
        void SetEditorFields(const nlohmann::json &values, ReferenceResolver *resolver) override;

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

        /// Why the limits cannot be used, or an empty string: with limits on, both finite, lower <= upper, a hinge's
        /// within [-180, 180] degrees and holding 0, a slider's holding 0, a distance joint's lower >= 0
        [[nodiscard]] static std::string DescribeLimitProblem(JointType type, bool useLimits, float minimum, float maximum);

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
