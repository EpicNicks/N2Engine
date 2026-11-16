#include "engine/physics/Joint.hpp"

#include <format>

#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/MathSerialization.hpp" // needed for Vector3

namespace N2Engine::Physics
{
    namespace
    {
        constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

        /// The body of an object: its Rigidbody's, else the static body of a collider (invalid when it has none yet)
        PhysicsBodyHandle BodyOf(const GameObject &gameObject)
        {
            if (const Rigidbody *rigidbody = gameObject.GetComponent<Rigidbody>();
                rigidbody && !rigidbody->IsDestroyed() && rigidbody->GetHandle().IsValid())
            {
                return rigidbody->GetHandle();
            }
            for (const ICollider *collider : gameObject.GetComponents<ICollider>())
            {
                if (collider && !collider->IsDestroyed() && collider->GetHandle().IsValid())
                {
                    return collider->GetHandle();
                }
            }
            return INVALID_PHYSICS_HANDLE;
        }
    }

    Joint::Joint(GameObject &gameObject)
        : SerializableComponent(gameObject)
    {
        RegisterMember(NAMEOF(_jointType), _jointType).Tooltip("What the joint allows: Fixed, Hinge, Ball, Slider or Distance");
        RegisterGameObjectRef(NAMEOF(_connectedBody), _connectedBody)
            .Tooltip("The object whose body this one is joined to; empty joins it to the world");
        RegisterMember(NAMEOF(_anchor), _anchor).Tooltip("The joint's point on this body, in its local space");
        RegisterMember(NAMEOF(_connectedAnchor), _connectedAnchor)
            .Tooltip("The same point on the connected body (local space); the world position without one");
        RegisterMember(NAMEOF(_axis), _axis).Tooltip("Hinge axis or slider direction, in this body's local space");
        RegisterMember(NAMEOF(_collideConnected), _collideConnected)
            .Tooltip("Whether the two joined bodies still collide with each other");
        RegisterMember(NAMEOF(_useLimits), _useLimits).Tooltip("Limit a hinge's angle, or a slider's or distance joint's length");
        RegisterMember(NAMEOF(_limitMin), _limitMin).Tooltip("Lower limit: degrees for a hinge, metres otherwise");
        RegisterMember(NAMEOF(_limitMax), _limitMax).Tooltip("Upper limit: degrees for a hinge, metres otherwise");
    }

    std::string Joint::GetTypeName() const
    {
        return NAMEOF(Joint);
    }

    void Joint::OnAttach()
    {
        _attached = true;
        // Made in the first FixedUpdate, like OnEnable (see there)
    }

    void Joint::OnDestroy()
    {
        Release();
        _attached = false;
    }

    void Joint::OnEnable()
    {
        // Not made here: a pooled object that is enabled again has its bodies put at their spawn pose only after this, and
        // the joint's zero is the bodies' poses when it is made. The first FixedUpdate makes it, the same way for an
        // object that is new and one that is brought back.
    }

    void Joint::OnDisable()
    {
        Release();
    }

    void Joint::OnActiveFlagChanged()
    {
        if (!IsActive())
        {
            Release();
        }
    }

    void Joint::OnFixedUpdate()
    {
        Refresh();
    }

    void Joint::OnEditorFieldsChanged(const std::span<const std::string>)
    {
        Release();
        Refresh();
    }

    void Joint::SetJointType(const JointType type)
    {
        if (type == _jointType)
        {
            return;
        }
        _jointType = type;
        Release();
        Refresh();
    }

    void Joint::SetConnectedBody(GameObject *connectedBody)
    {
        if (connectedBody == _connectedBody)
        {
            return;
        }
        _connectedBody = connectedBody;
        Release();
        Refresh();
    }

    void Joint::SetAnchor(const Math::Vector3 &anchor)
    {
        _anchor = anchor;
        Release();
        Refresh();
    }

    void Joint::SetConnectedAnchor(const Math::Vector3 &anchor)
    {
        _connectedAnchor = anchor;
        Release();
        Refresh();
    }

    void Joint::SetAxis(const Math::Vector3 &axis)
    {
        _axis = axis;
        Release();
        Refresh();
    }

    void Joint::SetCollideConnected(const bool collide)
    {
        _collideConnected = collide;
        ApplyLive();
    }

    void Joint::SetLimits(const bool enabled, const float minimum, const float maximum)
    {
        _useLimits = enabled;
        _limitMin = minimum;
        _limitMax = maximum;
        ApplyLive();
    }

    JointDesc Joint::BuildDesc(const PhysicsBodyHandle bodyA, const PhysicsBodyHandle bodyB) const
    {
        JointDesc desc;
        desc.type = _jointType;
        desc.bodyA = bodyA;
        desc.bodyB = bodyB;
        desc.anchorA = _anchor;
        desc.anchorB = _connectedAnchor;
        desc.axis = _axis;
        desc.collideConnected = _collideConnected;
        desc.useLimits = _useLimits;
        // A hinge's limits are degrees in the component and radians in the backend
        const float scale = _jointType == JointType::Hinge ? kDegreesToRadians : 1.0f;
        desc.limitMin = _limitMin * scale;
        desc.limitMax = _limitMax * scale;
        return desc;
    }

    void Joint::ApplyLive()
    {
        if (!_handle.IsValid())
        {
            return;
        }
        if (auto *backend = Application::GetInstance().Get3DPhysicsBackend())
        {
            backend->ConfigureJoint(_handle, BuildDesc(_bodyA, _bodyB));
        }
    }

    void Joint::Release()
    {
        if (_handle.IsValid())
        {
            // Only a joint that is still this component's: after a rollback restore the handle may be stale, and its slot
            // may hold another joint now
            if (auto *backend = Application::GetInstance().Get3DPhysicsBackend();
                backend && backend->JointHolds(_handle, _bodyA, _bodyB))
            {
                backend->DestroyJoint(_handle);
            }
        }
        _handle = INVALID_PHYSICS_JOINT_HANDLE;
        _bodyA = INVALID_PHYSICS_HANDLE;
        _bodyB = INVALID_PHYSICS_HANDLE;
    }

    void Joint::Refresh()
    {
        if (!_attached || IsDestroyed() || !IsActive())
        {
            return;
        }
        auto *backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
        {
            return;
        }
        if (!backend->GetImplementedFeatures().Has(PhysicsFeature::Joints))
        {
            if (!_warnedUnsupported)
            {
                _warnedUnsupported = true;
                Logger::Warn(std::format("Joint on GameObject '{}' ignored: the physics backend ({}) does not implement joints",
                                         GetGameObject().GetName(), backend->BackendId()));
            }
            return;
        }

        const PhysicsBodyHandle bodyA = BodyOf(GetGameObject());
        // A named connected object without a body yet is waited for, not replaced by the world
        const PhysicsBodyHandle bodyB = _connectedBody ? BodyOf(*_connectedBody) : INVALID_PHYSICS_HANDLE;

        if (_handle.IsValid())
        {
            if (bodyA == _bodyA && bodyB == _bodyB && backend->JointHolds(_handle, bodyA, bodyB))
            {
                return;
            }
            Release(); // a body was destroyed or replaced, or a restore changed the joints and the handle is stale
        }
        if (!bodyA.IsValid() || (_connectedBody && !bodyB.IsValid()))
        {
            return;
        }

        JointDesc desc = BuildDesc(bodyA, bodyB);
        desc.owned = true;
        // After a rollback restore the backend may hold this joint already, made again from its saved state: take it back
        // (it keeps the saved zero pose) instead of making a new one from the bodies' poses now
        _handle = backend->ClaimJoint(desc);
        if (!_handle.IsValid())
        {
            _handle = backend->CreateJoint(desc);
        }
        if (_handle.IsValid())
        {
            _bodyA = bodyA;
            _bodyB = bodyB;
        }
    }
}
