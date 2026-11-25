#include "engine/physics2d/Joint2D.hpp"

#include <cmath>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

#include "engine/Component.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Physics2D
{
    namespace
    {
        constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
        /// Box2D's revolute limit is within 0.99 pi
        constexpr float kHingeLimitDegrees = 178.0f;

        /// The 2D body of an object: its Rigidbody2D's, else a collider's own static body (invalid when it has none yet)
        PhysicsBodyHandle BodyOf(const GameObject &gameObject)
        {
            if (const Rigidbody2D *rigidbody = gameObject.GetComponent<Rigidbody2D>();
                rigidbody && !rigidbody->IsDestroyed() && rigidbody->GetHandle().IsValid())
            {
                return rigidbody->GetHandle();
            }
            for (const Collider2D *collider : gameObject.GetComponents<Collider2D>())
            {
                if (collider && !collider->IsDestroyed() && collider->GetHandle().IsValid())
                {
                    return collider->GetHandle();
                }
            }
            return Physics::INVALID_PHYSICS_HANDLE;
        }

        bool IsFiniteVector(const Math::Vector2 &v) { return std::isfinite(v.x) && std::isfinite(v.y); }

        /// A break threshold: 0 and infinity never break; negative or NaN is refused
        bool ReadThreshold(const float value, float &out, const char *what)
        {
            if (std::isnan(value) || value < 0.0f)
            {
                Logger::Warn(std::format("Joint2D: {} must be 0 or more (0 never breaks); it was not changed", what));
                return false;
            }
            out = std::isinf(value) ? 0.0f : value;
            return true;
        }
    }

    Joint2D::Joint2D(GameObject &gameObject)
        : SerializableComponent(gameObject)
    {
        RegisterMember(NAMEOF(_jointType), _jointType).Tooltip("What the joint allows: Fixed, Hinge, Slider, Distance, Wheel or Target");
        RegisterGameObjectRef(NAMEOF(_connectedBody), _connectedBody)
            .Tooltip("The object whose 2D body this one is joined to; empty joins it to the world (Target ignores it)");
        RegisterMember(NAMEOF(_anchor), _anchor).Tooltip("The joint's point on this body, in its local space");
        RegisterMember(NAMEOF(_connectedAnchor), _connectedAnchor)
            .Tooltip("The same point on the connected body (local space); the world position without one, and Target's target");
        RegisterMember(NAMEOF(_axis), _axis)
            .Tooltip("Slider or wheel direction, in this body's local space")
            .VisibleWhen(NAMEOF(_jointType), {"Slider", "Wheel"});
        RegisterMember(NAMEOF(_collideConnected), _collideConnected)
            .Tooltip("Whether the two joined bodies still collide with each other");
        RegisterMember(NAMEOF(_useLimits), _useLimits)
            .Tooltip("Limit a hinge's angle, or a slider's, wheel's or distance joint's length")
            .VisibleWhen(NAMEOF(_jointType), {"Hinge", "Slider", "Distance", "Wheel"});
        RegisterMember(NAMEOF(_limitMin), _limitMin)
            .DisplayName("Lower Limit")
            .Tooltip("Degrees for a hinge (-178 to 178), metres otherwise. A hinge, slider or wheel range must contain 0; a distance joint's lower limit is not negative.")
            .VisibleWhen(NAMEOF(_jointType), {"Hinge", "Slider", "Distance", "Wheel"})
            .VisibleWhen(NAMEOF(_useLimits), {true});
        RegisterMember(NAMEOF(_limitMax), _limitMax)
            .DisplayName("Upper Limit")
            .Tooltip("Degrees for a hinge, metres otherwise. Not below the lower limit.")
            .VisibleWhen(NAMEOF(_jointType), {"Hinge", "Slider", "Distance", "Wheel"})
            .VisibleWhen(NAMEOF(_useLimits), {true});
        RegisterMember(NAMEOF(_useSpring), _useSpring)
            .Tooltip("Make the joint springy: toward its zero pose (Hinge, Slider), its length (Distance), a soft weld (Fixed) or the suspension (Wheel)")
            .VisibleWhen(NAMEOF(_jointType), {"Fixed", "Hinge", "Slider", "Distance", "Wheel"});
        RegisterMember(NAMEOF(_springFrequency), _springFrequency)
            .DisplayName("Spring Frequency")
            .Tooltip("Hertz; higher is stiffer. Fixed takes 0 as rigid. Target always uses it.");
        RegisterMember(NAMEOF(_springDamping), _springDamping)
            .DisplayName("Spring Damping")
            .Tooltip("Damping ratio: 0 bounces, 1 is critically damped");
        RegisterMember(NAMEOF(_useMotor), _useMotor)
            .Tooltip("Drive the joint at a speed with a limited force")
            .VisibleWhen(NAMEOF(_jointType), {"Hinge", "Slider", "Distance", "Wheel"});
        RegisterMember(NAMEOF(_motorSpeed), _motorSpeed)
            .Tooltip("Degrees per second for a hinge or wheel, metres per second otherwise")
            .VisibleWhen(NAMEOF(_jointType), {"Hinge", "Slider", "Distance", "Wheel"})
            .VisibleWhen(NAMEOF(_useMotor), {true});
        RegisterMember(NAMEOF(_maxMotorForce), _maxMotorForce)
            .Tooltip("Newton-metres for a hinge or wheel, newtons otherwise")
            .VisibleWhen(NAMEOF(_jointType), {"Hinge", "Slider", "Distance", "Wheel"})
            .VisibleWhen(NAMEOF(_useMotor), {true});
        RegisterMember(NAMEOF(_maxForce), _maxForce)
            .Tooltip("The most force Target's spring may apply, in newtons")
            .VisibleWhen(NAMEOF(_jointType), {"Target"});
        RegisterMember(NAMEOF(_breakForce), _breakForce)
            .Tooltip("The reaction force (newtons) past which the joint breaks; 0 never breaks");
        RegisterMember(NAMEOF(_breakTorque), _breakTorque)
            .Tooltip("The reaction torque (newton-metres) past which the joint breaks; 0 never breaks");
    }

    std::string Joint2D::GetTypeName() const
    {
        return NAMEOF(Joint2D);
    }

    void Joint2D::OnAttach()
    {
        _attached = true; // made in the first FixedUpdate, as the 3D Joint
    }

    void Joint2D::OnDestroy()
    {
        Release();
        _attached = false;
    }

    void Joint2D::OnDisable()
    {
        Release();
    }

    void Joint2D::OnActiveFlagChanged()
    {
        if (!IsActive())
        {
            Release();
        }
    }

    void Joint2D::OnFixedUpdate()
    {
        Refresh();
    }

    void Joint2D::OnEditorFieldsChanged(const std::span<const std::string>)
    {
        Remake();
    }

    std::string Joint2D::DescribeProblem(const JointType2D type, const bool useLimits, const float minimum, const float maximum,
                                         const bool useSpring, const float frequency, const float damping)
    {
        if ((useSpring || type == JointType2D::Target) &&
            (!std::isfinite(frequency) || frequency < 0.0f || !std::isfinite(damping) || damping < 0.0f))
        {
            return "A joint's spring frequency and damping must be finite and not negative";
        }
        if (!useLimits || type == JointType2D::Fixed || type == JointType2D::Target)
        {
            return {};
        }
        if (!std::isfinite(minimum) || !std::isfinite(maximum))
        {
            return "Joint limits must be finite numbers";
        }
        if (minimum > maximum)
        {
            return "Joint lower limit must not be above the upper limit";
        }
        switch (type)
        {
        case JointType2D::Hinge:
            if (minimum < -kHingeLimitDegrees || maximum > kHingeLimitDegrees)
            {
                return "A 2D hinge's limits are degrees within -178 to 178";
            }
            [[fallthrough]];
        case JointType2D::Slider:
        case JointType2D::Wheel:
            if (minimum > 0.0f || maximum < 0.0f)
            {
                return "A hinge's, slider's or wheel's limits must contain 0 (the pose it was made at): lower <= 0 <= upper";
            }
            break;
        case JointType2D::Distance:
            if (minimum < 0.0f)
            {
                return "A distance joint's lower limit must not be negative";
            }
            break;
        default:
            break;
        }
        return {};
    }

    void Joint2D::SetEditorFields(const nlohmann::json &values, ReferenceResolver *resolver)
    {
        // Checked before anything is applied, on a copy of the fields the request touches
        const auto read = [&values](const std::string &name, auto current)
        {
            if (values.is_object() && values.contains(name))
            {
                try
                {
                    return values.at(name).get<decltype(current)>();
                }
                catch (const nlohmann::json::exception &)
                {
                }
            }
            return current;
        };
        const std::string problem = DescribeProblem(read(NAMEOF(_jointType), _jointType), read(NAMEOF(_useLimits), _useLimits),
                                                    read(NAMEOF(_limitMin), _limitMin), read(NAMEOF(_limitMax), _limitMax),
                                                    read(NAMEOF(_useSpring), _useSpring), read(NAMEOF(_springFrequency), _springFrequency),
                                                    read(NAMEOF(_springDamping), _springDamping));
        if (!problem.empty())
        {
            throw std::invalid_argument(problem);
        }
        for (const std::string &name : {NAMEOF(_motorSpeed), NAMEOF(_maxMotorForce), NAMEOF(_maxForce), NAMEOF(_breakForce), NAMEOF(_breakTorque)})
        {
            const float value = read(name, 0.0f);
            if (!std::isfinite(value) || (value < 0.0f && name != NAMEOF(_motorSpeed)))
            {
                throw std::invalid_argument("Joint2D: " + name + " must be a finite number, not negative");
            }
        }
        SerializableComponent::SetEditorFields(values, resolver);
    }

    void Joint2D::Remake()
    {
        Release();
        Refresh();
    }

    void Joint2D::SetJointType(const JointType2D type)
    {
        if (type == _jointType)
        {
            return;
        }
        if (const std::string problem = DescribeProblem(type, _useLimits, _limitMin, _limitMax, _useSpring, _springFrequency, _springDamping);
            !problem.empty())
        {
            Logger::Warn(std::format("Joint2D on '{}': {}; limits and spring are turned off", GetGameObject().GetName(), problem));
            _useLimits = false;
            _useSpring = false;
        }
        _jointType = type;
        Remake();
    }

    void Joint2D::SetConnectedBody(GameObject *connectedBody)
    {
        if (connectedBody == _connectedBody)
        {
            return;
        }
        _connectedBody = connectedBody;
        Remake();
    }

    void Joint2D::SetAnchor(const Math::Vector2 &anchor)
    {
        if (!IsFiniteVector(anchor))
        {
            Logger::Warn("Joint2D: the anchor must be finite; it was not changed");
            return;
        }
        _anchor = anchor;
        Remake();
    }

    void Joint2D::SetConnectedAnchor(const Math::Vector2 &anchor)
    {
        if (!IsFiniteVector(anchor))
        {
            Logger::Warn("Joint2D: the connected anchor must be finite; it was not changed");
            return;
        }
        _connectedAnchor = anchor;
        Remake();
    }

    void Joint2D::SetAxis(const Math::Vector2 &axis)
    {
        if (!IsFiniteVector(axis) || !(axis.x * axis.x + axis.y * axis.y > 0.0f))
        {
            Logger::Warn("Joint2D: the axis must be finite and not zero; it was not changed");
            return;
        }
        _axis = axis;
        Remake();
    }

    void Joint2D::SetCollideConnected(const bool collide)
    {
        _collideConnected = collide;
        ApplyLive();
    }

    void Joint2D::SetLimits(const bool enabled, const float minimum, const float maximum)
    {
        if (const std::string problem = DescribeProblem(_jointType, enabled, minimum, maximum, false, 0.0f, 0.0f); !problem.empty())
        {
            Logger::Warn(std::format("Joint2D on '{}': {}; the limits were not changed", GetGameObject().GetName(), problem));
            return;
        }
        _useLimits = enabled;
        _limitMin = minimum;
        _limitMax = maximum;
        ApplyLive();
    }

    void Joint2D::SetSpring(const bool enabled, const float frequency, const float damping)
    {
        if (!std::isfinite(frequency) || frequency < 0.0f || !std::isfinite(damping) || damping < 0.0f)
        {
            Logger::Warn(std::format("Joint2D on '{}': the spring frequency and damping must be finite and not negative; the spring "
                                     "was not changed", GetGameObject().GetName()));
            return;
        }
        _useSpring = enabled;
        _springFrequency = frequency;
        _springDamping = damping;
        ApplyLive();
    }

    void Joint2D::SetMotor(const bool enabled, const float speed, const float maxForce)
    {
        if (!std::isfinite(speed) || !std::isfinite(maxForce) || maxForce < 0.0f)
        {
            Logger::Warn(std::format("Joint2D on '{}': the motor speed must be finite and its force finite and not negative; the "
                                     "motor was not changed", GetGameObject().GetName()));
            return;
        }
        _useMotor = enabled;
        _motorSpeed = speed;
        _maxMotorForce = maxForce;
        ApplyLive();
    }

    void Joint2D::SetTarget(const Math::Vector2 &target)
    {
        if (!IsFiniteVector(target))
        {
            Logger::Warn("Joint2D: the target must be finite; it was not changed");
            return;
        }
        _connectedAnchor = target;
        if (_jointType == JointType2D::Target)
        {
            ApplyLive();
        }
        else
        {
            Remake();
        }
    }

    void Joint2D::SetMaxForce(const float force)
    {
        if (!std::isfinite(force) || force < 0.0f)
        {
            Logger::Warn("Joint2D: the maximum force must be finite and not negative; it was not changed");
            return;
        }
        _maxForce = force;
        ApplyLive();
    }

    void Joint2D::SetBreakForce(const float force)
    {
        if (ReadThreshold(force, _breakForce, "the break force"))
        {
            ApplyLive();
        }
    }

    void Joint2D::SetBreakTorque(const float torque)
    {
        if (ReadThreshold(torque, _breakTorque, "the break torque"))
        {
            ApplyLive();
        }
    }

    float Joint2D::GetJointAngle() const
    {
        const Physics2DWorld *world = LiveWorld();
        return world ? world->ReadJoint(_handle).angle / kDegreesToRadians : 0.0f;
    }

    float Joint2D::GetJointTranslation() const
    {
        const Physics2DWorld *world = LiveWorld();
        return world ? world->ReadJoint(_handle).translation : 0.0f;
    }

    Math::Vector2 Joint2D::GetReactionForce() const
    {
        const Physics2DWorld *world = LiveWorld();
        return world ? world->ReadJoint(_handle).reactionForce : Math::Vector2();
    }

    float Joint2D::GetReactionTorque() const
    {
        const Physics2DWorld *world = LiveWorld();
        return world ? world->ReadJoint(_handle).reactionTorque : 0.0f;
    }

    Physics2DWorld *Joint2D::LiveWorld() const
    {
        if (_handle == 0)
        {
            return nullptr;
        }
        const auto world = _world.lock();
        return world && world->JointHolds(_handle, BuildDesc(_self, _other)) ? world.get() : nullptr;
    }

    JointDesc2D Joint2D::BuildDesc(const PhysicsBodyHandle self, const PhysicsBodyHandle other) const
    {
        JointDesc2D desc;
        desc.type = _jointType;
        desc.self = self;
        desc.other = other;
        desc.anchor = _anchor;
        desc.connectedAnchor = _connectedAnchor;
        desc.axis = _axis;
        desc.collideConnected = _collideConnected;
        desc.useLimits = _useLimits;
        const bool angular = _jointType == JointType2D::Hinge;
        desc.limitMin = angular ? _limitMin * kDegreesToRadians : _limitMin;
        desc.limitMax = angular ? _limitMax * kDegreesToRadians : _limitMax;
        desc.useSpring = _useSpring || _jointType == JointType2D::Target;
        desc.springFrequency = _springFrequency;
        desc.springDamping = _springDamping;
        desc.useMotor = _useMotor;
        const bool turns = _jointType == JointType2D::Hinge || _jointType == JointType2D::Wheel;
        desc.motorSpeed = turns ? _motorSpeed * kDegreesToRadians : _motorSpeed;
        desc.maxMotorForce = _maxMotorForce;
        desc.maxForce = _maxForce;
        desc.breakForce = _breakForce;
        desc.breakTorque = _breakTorque;
        desc.owner = const_cast<Joint2D *>(this);
        return desc;
    }

    void Joint2D::ApplyLive()
    {
        if (Physics2DWorld *world = LiveWorld())
        {
            world->ConfigureJoint(_handle, BuildDesc(_self, _other));
        }
    }

    void Joint2D::Release()
    {
        if (_handle != 0)
        {
            // Only a joint that is still this component's: after a restore the handle may name a joint waiting for its owner
            if (Physics2DWorld *world = LiveWorld())
            {
                world->DestroyJoint(_handle);
            }
        }
        _handle = 0;
        _self = Physics::INVALID_PHYSICS_HANDLE;
        _other = Physics::INVALID_PHYSICS_HANDLE;
    }

    void Joint2D::Refresh()
    {
        if (!_attached || IsDestroyed() || !IsActive())
        {
            return;
        }
        const PhysicsBodyHandle self = BodyOf(GetGameObject());
        // Target pulls toward a world point: it has no connected body. A named object without a body yet is waited for.
        GameObject *connected = _jointType == JointType2D::Target ? nullptr : _connectedBody;
        const PhysicsBodyHandle other = connected ? BodyOf(*connected) : Physics::INVALID_PHYSICS_HANDLE;

        if (_handle != 0)
        {
            const auto world = _world.lock();
            if (world && self == _self && other == _other && world->JointHolds(_handle, BuildDesc(self, other)))
            {
                return;
            }
            Release(); // a body was destroyed or replaced, or a restore changed the joints
        }
        if (!self.IsValid() || (connected && !other.IsValid()))
        {
            return;
        }
        Scene *scene = GetGameObject().GetScene();
        if (!scene || (connected && connected->GetScene() != scene))
        {
            return;
        }
        const std::shared_ptr<Physics2DWorld> world = scene->GetOrCreatePhysics2DWorld();
        if (!world)
        {
            return;
        }
        const JointDesc2D desc = BuildDesc(self, other);
        // After a rollback restore or a join the world may hold this joint already, from the saved state: take it back
        _handle = world->ClaimJoint(desc);
        if (_handle == 0)
        {
            _handle = world->CreateJoint(desc);
        }
        if (_handle != 0)
        {
            _world = world;
            _self = self;
            _other = other;
        }
    }

    void Joint2D::OnJointBroken()
    {
        _handle = 0;
        _self = Physics::INVALID_PHYSICS_HANDLE;
        _other = Physics::INVALID_PHYSICS_HANDLE;
        GameObject &gameObject = GetGameObject();
        if (gameObject.IsDestroyed() || !gameObject.IsActiveInHierarchy())
        {
            return;
        }
        // Every component of the object, as for physics events: the list is snapshotted, and each one's lifetime checked
        const GameObject::Ptr keepAlive = std::const_pointer_cast<GameObject>(gameObject.weak_from_this().lock());
        const std::weak_ptr<const bool> self = GetLifetimeToken();
        std::vector<std::pair<Component *, std::weak_ptr<const bool>>> components;
        for (const auto &component : gameObject.GetAllComponents())
        {
            components.emplace_back(component.get(), component->GetLifetimeToken());
        }
        for (const auto &[component, lifetime] : components)
        {
            if (gameObject.IsDestroyed() || !gameObject.IsActiveInHierarchy() || self.expired())
            {
                return;
            }
            if (!lifetime.expired() && !component->IsDestroyed())
            {
                component->OnJointBreak2D(*this);
            }
        }
        if (!self.expired() && !IsDestroyed())
        {
            SetActive(false); // enabling it again makes a new joint
        }
    }
}
