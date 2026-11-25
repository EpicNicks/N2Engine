#pragma once

#include <memory>
#include <span>
#include <string>

#include <math/Vector2.hpp>

#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics2d/Physics2DTypes.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Physics2D
{
    /**
     * Joint2D: joins this object's 2D body to another object's 2D body, or to the world. The 2D twin of Physics::Joint, with
     * the same fields and lifecycle (docs/physics2d.html#joints).
     *
     * The body is the object's Rigidbody2D, or a collider's own static body. `connectedBody` names the other object; empty
     * holds this body to the world, `connectedAnchor` being the world point (for Target, the point it pulls toward).
     *
     * The joint is made in the first FixedUpdate in which both bodies exist, from their poses then: that pose is its zero (a
     * hinge's angle 0, a slider's translation 0, a distance joint's length). It is made again when either body is replaced or
     * a field that shapes it changes (type, connected body, anchors, axis), ends with this component or either body, and is
     * suspended while this component, its object or the connected object is disabled. The live settings (collideConnected,
     * limits, spring, motor, Target's target and force, break thresholds) change the existing joint.
     *
     * Breaking: with a finite break force or torque, a joint whose reaction goes past it in a step ends after that step; every
     * component of this object gets OnJointBreak2D(*this), then this component is disabled (SetActive(false)). Enabling it
     * again makes a new joint from the poses then.
     *
     * Rollback: the joints are in the 2D world's state. After a restore or a late join, the restored joints wait for their
     * owners, and this component takes its own back in its next FixedUpdate (Physics2DWorld::ClaimJoint), keeping the saved
     * zero pose. As for 3D joints, the component set and enabled flags are game state the game brings back itself.
     */
    class Joint2D final : public SerializableComponent
    {
    public:
        explicit Joint2D(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override;

        void OnAttach() override;
        void OnDestroy() override;
        void OnDisable() override;
        void OnActiveFlagChanged() override;
        /// Makes the joint when both bodies exist, and again after either was replaced
        void OnFixedUpdate() override;
        /// The editor changed fields: the joint is made again from them
        void OnEditorFieldsChanged(std::span<const std::string> changed) override;
        /// The editor's field edit, with the settings checked as a whole (DescribeProblem); a bad request throws
        /// std::invalid_argument and is not applied
        void SetEditorFields(const nlohmann::json &values, ReferenceResolver *resolver) override;

        // ===== What the joint is made of: changing one makes it again =====
        void SetJointType(JointType2D type);
        [[nodiscard]] JointType2D GetJointType() const { return _jointType; }
        void SetConnectedBody(GameObject *connectedBody);
        [[nodiscard]] GameObject *GetConnectedBody() const { return _connectedBody; }
        /// On this body, in its local space
        void SetAnchor(const Math::Vector2 &anchor);
        [[nodiscard]] Math::Vector2 GetAnchor() const { return _anchor; }
        /// On the connected body, in its local space; with no connected body (and for Target), the world point
        void SetConnectedAnchor(const Math::Vector2 &anchor);
        [[nodiscard]] Math::Vector2 GetConnectedAnchor() const { return _connectedAnchor; }
        /// Slider and Wheel: the direction, in this body's local space
        void SetAxis(const Math::Vector2 &axis);
        [[nodiscard]] Math::Vector2 GetAxis() const { return _axis; }

        // ===== Live settings =====
        void SetCollideConnected(bool collide);
        [[nodiscard]] bool GetCollideConnected() const { return _collideConnected; }
        /// Hinge: degrees within [-178, 178] holding 0. Slider and Wheel: metres holding 0. Distance: metres, lower >= 0.
        void SetLimits(bool enabled, float minimum, float maximum);
        [[nodiscard]] bool GetUseLimits() const { return _useLimits; }
        [[nodiscard]] float GetLimitMin() const { return _limitMin; }
        [[nodiscard]] float GetLimitMax() const { return _limitMax; }
        /// Frequency in hertz (> 0 for a spring; Fixed takes 0 as rigid) and damping ratio (>= 0). Distance: a spring toward
        /// the length it was made at. Hinge and Slider: toward the zero pose. Wheel: the suspension. Fixed: a soft weld.
        /// Target always uses its spring (5 Hz, 0.7 until set).
        void SetSpring(bool enabled, float frequency, float damping);
        [[nodiscard]] bool GetUseSpring() const { return _useSpring; }
        [[nodiscard]] float GetSpringFrequency() const { return _springFrequency; }
        [[nodiscard]] float GetSpringDamping() const { return _springDamping; }
        /// Hinge and Wheel: degrees per second and newton-metres. Slider and Distance: metres per second and newtons.
        void SetMotor(bool enabled, float speed, float maxForce);
        [[nodiscard]] bool GetUseMotor() const { return _useMotor; }
        [[nodiscard]] float GetMotorSpeed() const { return _motorSpeed; }
        [[nodiscard]] float GetMaxMotorForce() const { return _maxMotorForce; }
        /// Target: the world point the anchor is pulled toward (the connected anchor), without making the joint again
        void SetTarget(const Math::Vector2 &target);
        [[nodiscard]] Math::Vector2 GetTarget() const { return _connectedAnchor; }
        /// Target: the most force the spring may apply (newtons)
        void SetMaxForce(float force);
        [[nodiscard]] float GetMaxForce() const { return _maxForce; }
        /// The reaction force (newtons) and torque (newton-metres) past which the joint breaks. 0 (the default) never breaks, and
        /// so does infinity, which is stored as 0 (scenes are JSON, which has no infinity).
        void SetBreakForce(float force);
        [[nodiscard]] float GetBreakForce() const { return _breakForce; }
        void SetBreakTorque(float torque);
        [[nodiscard]] float GetBreakTorque() const { return _breakTorque; }

        // ===== Reading =====
        /// The world holds this component's joint now
        [[nodiscard]] bool IsCreated() const { return _handle != 0; }
        [[nodiscard]] JointHandle2D GetHandle() const { return _handle; }
        /// Hinge: degrees from the zero pose (0 without a joint)
        [[nodiscard]] float GetJointAngle() const;
        /// Slider and Wheel: metres along the axis; Distance: the current length (0 without a joint)
        [[nodiscard]] float GetJointTranslation() const;
        [[nodiscard]] Math::Vector2 GetReactionForce() const;
        [[nodiscard]] float GetReactionTorque() const;

        /// Why these settings cannot be used, or an empty string
        [[nodiscard]] static std::string DescribeProblem(JointType2D type, bool useLimits, float minimum, float maximum,
                                                         bool useSpring, float frequency, float damping);

        /// The world calls it after the step in which the joint broke (it is already ended)
        void OnJointBroken();

    private:
        void Refresh();
        void Release();
        void Remake();
        void ApplyLive();
        [[nodiscard]] JointDesc2D BuildDesc(PhysicsBodyHandle self, PhysicsBodyHandle other) const;
        [[nodiscard]] Physics2DWorld *LiveWorld() const;

        JointType2D _jointType = JointType2D::Fixed;
        GameObject *_connectedBody = nullptr;
        Math::Vector2 _anchor;
        Math::Vector2 _connectedAnchor;
        Math::Vector2 _axis{1.0f, 0.0f};
        bool _collideConnected = false;
        bool _useLimits = false;
        float _limitMin = 0.0f;
        float _limitMax = 0.0f;
        bool _useSpring = false;
        float _springFrequency = 5.0f;
        float _springDamping = 0.7f;
        bool _useMotor = false;
        float _motorSpeed = 0.0f;
        float _maxMotorForce = 0.0f;
        float _maxForce = 1000.0f;
        float _breakForce = 0.0f;  // 0: never breaks
        float _breakTorque = 0.0f;

        std::weak_ptr<Physics2DWorld> _world;
        JointHandle2D _handle = 0;
        PhysicsBodyHandle _self;
        PhysicsBodyHandle _other;
        bool _attached = false;
    };
}
