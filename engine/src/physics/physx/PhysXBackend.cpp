#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/GameObject.hpp"
#include "engine/Component.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/Logger.hpp"
#include "engine/physics/Raycast.hpp"

#ifdef N2ENGINE_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#include <extensions/PxDefaultAllocator.h>
#include <extensions/PxDefaultErrorCallback.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#include <extensions/PxDefaultSimulationFilterShader.h>
#include <extensions/PxRigidBodyExt.h>

#include <format>
#include <algorithm>
#include <memory>
#include <vector>

using namespace physx;
#endif

namespace N2Engine::Physics
{
#ifdef N2ENGINE_PHYSX_ENABLED

    namespace
    {
        // Until the layer system (#4) exists, every shape is on every query layer, so any
        // non-zero layer mask hits it. Shapes used to have all-zero query data, which PhysX's
        // fixed-function query filter ((shape & mask) != 0) rejected for every mask.
        constexpr PxU32 AllQueryLayers = 0xFFFFFFFFu;

        // PxDefaultSimulationFilterShader only asks for touch notifications on trigger pairs, so
        // onContact never ran for solid contacts and OnCollisionEnter/Stay/Exit never fired
        PxFilterFlags CollisionEventFilterShader(
            PxFilterObjectAttributes attributes0, PxFilterData,
            PxFilterObjectAttributes attributes1, PxFilterData,
            PxPairFlags &pairFlags, const void *, PxU32)
        {
            if (PxFilterObjectIsTrigger(attributes0) || PxFilterObjectIsTrigger(attributes1))
            {
                pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
                return PxFilterFlag::eDEFAULT;
            }

            pairFlags = PxPairFlag::eCONTACT_DEFAULT
                        | PxPairFlag::eNOTIFY_TOUCH_FOUND
                        | PxPairFlag::eNOTIFY_TOUCH_LOST
                        | PxPairFlag::eNOTIFY_CONTACT_POINTS;
            return PxFilterFlag::eDEFAULT;
        }

        // Without a filter callback every raycast hit is blocking, so RaycastAll returned one hit
        class TouchAllHitsFilter final : public PxQueryFilterCallback
        {
        public:
            PxQueryHitType::Enum preFilter(const PxFilterData &, const PxShape *, const PxRigidActor *,
                                           PxHitFlags &) override
            {
                return PxQueryHitType::eTOUCH;
            }

            PxQueryHitType::Enum postFilter(const PxFilterData &, const PxQueryHit &, const PxShape *,
                                            const PxRigidActor *) override
            {
                return PxQueryHitType::eTOUCH;
            }
        };
    }

    PhysXBackend::PhysXBackend() = default;

    PhysXBackend::~PhysXBackend()
    {
        PhysXBackend::Shutdown();
    }

    bool PhysXBackend::Initialize()
    {
        Logger::Info("Initializing PhysX Backend...");

        // 1. Create foundation. The allocator and error callback are members so they outlive the
        // foundation: as function-local statics they were destroyed at exit before the backend released it.
        _foundation = PxCreateFoundation(PX_PHYSICS_VERSION, _allocator, _errorCallback);
        if (!_foundation)
        {
            Logger::Error("PxCreateFoundation failed!");
            return false;
        }

        // 2. Create PVD (PhysX Visual Debugger) - optional
#if N2ENGINE_PHYSX_HAS_PVD
        _pvd = PxCreatePvd(*_foundation);
        if (_pvd)
        {
            if (PxPvdTransport *transport = PxDefaultPvdSocketTransportCreate("127.0.0.1", 5425, 10))
            {
                _pvd->connect(*transport, PxPvdInstrumentationFlag::eALL);
                Logger::Info("PhysX Visual Debugger connected");
            }
        }
#else
        Logger::Info("PhysX Visual Debugger not available (PVD headers not found)");
        physx::PxPvd *_pvd = nullptr; // Local variable for compatibility
#endif
        // 3. Create physics SDK
        PxTolerancesScale scale;
        _physics = PxCreatePhysics(PX_PHYSICS_VERSION, *_foundation, scale, true, _pvd);
        if (!_physics)
        {
            Logger::Error("PxCreatePhysics failed!");
            Shutdown();
            return false;
        }

        // 4. Create default material
        _defaultMaterial = _physics->createMaterial(0.5f, 0.5f, 0.6f);

        // 5. Create scene
        PxSceneDesc sceneDesc(_physics->getTolerancesScale());
        sceneDesc.gravity = PxVec3(0.0f, -9.81f, 0.0f);

        _dispatcher = PxDefaultCpuDispatcherCreate(4);
        sceneDesc.cpuDispatcher = _dispatcher;
        sceneDesc.filterShader = CollisionEventFilterShader;

        // Set this backend as the simulation event callback
        sceneDesc.simulationEventCallback = this;

        // PhysX 5 performance flags
        sceneDesc.flags |= PxSceneFlag::eENABLE_PCM;
        sceneDesc.flags |= PxSceneFlag::eENABLE_STABILIZATION;

        _scene = _physics->createScene(sceneDesc);
        if (!_scene)
        {
            Logger::Error("PxCreateScene failed!");
            Shutdown();
            return false;
        }

        // Setup PVD scene
        if (_pvd)
        {
            if (PxPvdSceneClient *pvdClient = _scene->getScenePvdClient())
            {
                pvdClient->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONSTRAINTS, true);
                pvdClient->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONTACTS, true);
                pvdClient->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_SCENEQUERIES, true);
            }
        }

        Logger::Info("PhysX initialized successfully!");
        return true;
    }

    void PhysXBackend::Update(const float deltaTime)
    {
        if (!_scene)
        {
            return;
        }

        _scene->simulate(deltaTime);
        _scene->fetchResults(true);
    }

    void PhysXBackend::Shutdown()
    {
        Logger::Info("Shutting down PhysX...");

        for (const auto& bodyData : _bodies)
        {
            if (bodyData.active && bodyData.actor)
            {
                if (bodyData.actor->userData)
                {
                    delete static_cast<PhysicsBodyHandle*>(bodyData.actor->userData);
                    bodyData.actor->userData = nullptr;
                }

                bodyData.actor->release();
            }
        }
        _bodies.clear();
        _freeList.clear();

        _colliderShapes.clear();

        for (const auto& material : _materialCache | std::views::values)
        {
            material->release();
        }
        _materialCache.clear();

        for (auto& [pair, data] : _newCollisions)
        {
            delete data;
        }
        _newCollisions.clear();

        for (auto& [pair, data] : _endedCollisions)
        {
            delete data;
        }
        _endedCollisions.clear();

        if (_scene)
            _scene->release();
        if (_dispatcher)
            _dispatcher->release();
        if (_defaultMaterial)
            _defaultMaterial->release();
        if (_physics)
            _physics->release();

#if N2ENGINE_PHYSX_HAS_PVD
        if (_pvd)
        {
            _pvd->release();
            _pvd = nullptr;
        }
#endif

        if (_foundation)
            _foundation->release();

        _scene = nullptr;
        _dispatcher = nullptr;
        _defaultMaterial = nullptr;
        _physics = nullptr;
        _foundation = nullptr;
    }

    // ========== Deferred Scene Modifications ==========

    void PhysXBackend::SetGravity(const Math::Vector3 &gravity)
    {
        _pendingChanges[ChangeType::SetGravity] = [this, gravity]()
        {
            _scene->setGravity(PxVec3(gravity.x, gravity.y, gravity.z));
            _currentGravity = gravity;
            Logger::Info(std::format("Gravity applied: ({}, {}, {})", gravity.x, gravity.y, gravity.z));
        };
    }

    Math::Vector3 PhysXBackend::GetGravity() const
    {
        return _currentGravity;
    }

    void PhysXBackend::ApplyPendingChanges()
    {
        for (auto &action : _pendingChanges | std::views::values)
        {
            action();
        }
        _pendingChanges.clear();
    }

    // ========== Handle Management ==========

    PhysicsBodyHandle PhysXBackend::AllocateHandle()
    {
        PhysicsBodyHandle handle;

        if (!_freeList.empty())
        {
            handle.index = _freeList.back();
            _freeList.pop_back();
            _bodies[handle.index].generation++;
        }
        else
        {
            handle.index = static_cast<uint32_t>(_bodies.size());
            _bodies.emplace_back();
        }

        handle.generation = _bodies[handle.index].generation;
        _bodies[handle.index].active = true;

        return handle;
    }

    PhysXBackend::BodyData* PhysXBackend::GetBodyData(PhysicsBodyHandle handle)
    {
        if (handle.index >= _bodies.size())
            return nullptr;

        BodyData &data = _bodies[handle.index];

        if (!data.active || data.generation != handle.generation)
            return nullptr;

        return &data;
    }

    const PhysXBackend::BodyData* PhysXBackend::GetBodyData(PhysicsBodyHandle handle) const
    {
        if (handle.index >= _bodies.size())
            return nullptr;

        const BodyData &data = _bodies[handle.index];

        if (!data.active || data.generation != handle.generation)
            return nullptr;

        return &data;
    }

    // ========== Body Creation ==========

    PhysicsBodyHandle PhysXBackend::CreateDynamicBody(const Math::Vector3 &position, const Math::Quaternion &rotation,
                                                      float mass, Rigidbody *rigidbody, bool isKinematic)
    {
        const PxTransform transform(
            PxVec3(position.x, position.y, position.z),
            PxQuat(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()));

        PxRigidDynamic *body = _physics->createRigidDynamic(transform);
        if (!body)
        {
            Logger::Error("Failed to create dynamic body");
            return INVALID_PHYSICS_HANDLE;
        }
        body->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, isKinematic);

        PxRigidBodyExt::setMassAndUpdateInertia(*body, mass);

        const PhysicsBodyHandle handle = AllocateHandle();
        BodyData *data = GetBodyData(handle);
        data->actor = body;
        data->rigidbody = rigidbody;

        body->userData = new PhysicsBodyHandle(handle);

        _scene->addActor(*body);

        return handle;
    }

    PhysicsBodyHandle PhysXBackend::CreateStaticBody(const Math::Vector3 &position, const Math::Quaternion &rotation,
                                                     Rigidbody *rigidbody)
    {
        const PxTransform transform(PxVec3(position.x, position.y, position.z),
                                    PxQuat(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()));
        PxRigidStatic *body = _physics->createRigidStatic(transform);

        if (!body)
        {
            Logger::Error("Failed to create static body");
            return INVALID_PHYSICS_HANDLE;
        }

        const PhysicsBodyHandle handle = AllocateHandle();
        BodyData *data = GetBodyData(handle);
        data->actor = body;
        data->rigidbody = rigidbody;

        body->userData = new PhysicsBodyHandle(handle);

        _scene->addActor(*body);

        return handle;
    }

    void PhysXBackend::DestroyBody(PhysicsBodyHandle handle)
    {
        BodyData *data = GetBodyData(handle);
        if (!data)
            return;

        if (data->actor)
        {
            if (data->actor->userData)
            {
                delete static_cast<PhysicsBodyHandle*>(data->actor->userData);
                data->actor->userData = nullptr;
            }

            data->actor->release();
            data->actor = nullptr;
        }

        // Releasing the actor released its shapes; forget them so no collider keeps a freed PxShape*
        for (ICollider *collider : data->colliders)
        {
            _colliderShapes.erase(collider);
        }
        ForgetPairsWithBody(handle);

        data->active = false;
        data->rigidbody = nullptr;
        data->colliders.clear();
        _freeList.push_back(handle.index);
    }

    // ========== Component Registration ==========

    void PhysXBackend::RegisterCollider(PhysicsBodyHandle handle, ICollider *collider)
    {
        if (BodyData *data = GetBodyData(handle); data && collider)
        {
            // Only add if not already registered
            if (const auto it = std::ranges::find(data->colliders, collider); it == data->colliders.end())
            {
                data->colliders.push_back(collider);
            }
        }
    }

    void PhysXBackend::UnregisterCollider(PhysicsBodyHandle handle, ICollider* collider)
    {
        if (BodyData* data = GetBodyData(handle); data && collider)
        {
            // A removed collider must take its shapes with it; on a shared Rigidbody actor they'd
            // otherwise stay behind as ghost colliders
            RemoveColliderShapes(handle, collider);
            std::erase(data->colliders, collider);

            // The detached shape's touch-lost arrives flagged eREMOVED_SHAPE_*, which the callbacks skip,
            // so end this body's pairs now; shapes that are still touching re-enter on the next step
            if (data->actor)
            {
                ForgetPairsWithBody(handle);
            }
        }

        // Also when the body is already gone (its actor released these shapes)
        _colliderShapes.erase(collider);
    }

    // ========== Transform Updates (for Transform -> Physics syncing) ==========
    void PhysXBackend::SetBodyTransform(
        PhysicsBodyHandle handle,
        const Math::Vector3 &position,
        const Math::Quaternion &rotation)
    {
        // Validate handle
        if (!handle.IsValid() || handle.index >= _bodies.size())
        {
            return;
        }

        const BodyData &bodyData = _bodies[handle.index];
        if (!bodyData.active || bodyData.generation != handle.generation)
        {
            return;
        }

        PxRigidActor *actor = bodyData.actor;
        if (auto *dynamic = actor->is<PxRigidDynamic>())
        {
            // For kinematic bodies, use setKinematicTarget for smooth interpolation
            const PxTransform target(
                PxVec3(position.x, position.y, position.z),
                PxQuat(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()));
            dynamic->setKinematicTarget(target);
        }
    }

    void PhysXBackend::SetStaticBodyTransform(
        const PhysicsBodyHandle handle,
        const Math::Vector3 &position,
        const Math::Quaternion &rotation)
    {
        // Validate handle
        if (!handle.IsValid() || handle.index >= _bodies.size())
        {
            return;
        }

        const BodyData &bodyData = _bodies[handle.index];
        if (!bodyData.active || bodyData.generation != handle.generation)
        {
            return;
        }

        if (PxRigidActor *actor = bodyData.actor; actor->is<PxRigidStatic>())
        {
            // Direct position update for static bodies (expensive - rebuilds broadphase!)
            const PxTransform transform(
                PxVec3(position.x, position.y, position.z),
                PxQuat(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()));
            actor->setGlobalPose(transform);
        }
    }

    PxMaterial* PhysXBackend::GetOrCreateMaterial(const PhysicsMaterial &material)
    {
        const MaterialKey key{material.staticFriction, material.dynamicFriction, material.restitution};

        if (const auto it = _materialCache.find(key); it != _materialCache.end())
        {
            return it->second;
        }

        PxMaterial *pxMaterial = _physics->createMaterial(
            material.staticFriction,
            material.dynamicFriction,
            material.restitution);

        if (pxMaterial)
        {
            _materialCache[key] = pxMaterial;
        }

        return pxMaterial;
    }

    void PhysXBackend::AttachColliderShape(
        const PhysicsBodyHandle body,
        ICollider *collider,
        const PxGeometry &geometry,
        const PxTransform &localPose,
        const PhysicsMaterial &material)
    {
        const BodyData *bodyData = GetBodyData(body);
        if (!bodyData || !bodyData->actor)
        {
            return;
        }

        const PxMaterial *pxMaterial = GetOrCreateMaterial(material);
        if (!pxMaterial)
        {
            pxMaterial = _defaultMaterial;
        }

        PxShape *shape = _physics->createShape(geometry, *pxMaterial, true);
        if (!shape)
        {
            Logger::Error("Failed to create collider shape");
            return;
        }
        shape->setLocalPose(localPose);
        shape->setQueryFilterData(PxFilterData(AllQueryLayers, 0, 0, 0));

        bodyData->actor->attachShape(*shape);

        // Recorded under the collider that asked for it (it used to be filed under whichever
        // collider registered last, so a second collider's shapes were attributed to the wrong one)
        if (collider)
        {
            _colliderShapes[collider].push_back(shape);
        }

        // The actor holds the only reference from here; detachShape frees the shape
        shape->release();
    }

    void PhysXBackend::AddSphereCollider(
        const PhysicsBodyHandle body,
        ICollider *collider,
        const float radius,
        const Math::Vector3 &localOffset,
        const PhysicsMaterial &material)
    {
        AttachColliderShape(body, collider, PxSphereGeometry(radius),
                            PxTransform(PxVec3(localOffset.x, localOffset.y, localOffset.z)), material);
    }

    void PhysXBackend::AddBoxCollider(
        const PhysicsBodyHandle body,
        ICollider *collider,
        const Math::Vector3 &halfExtents,
        const Math::Vector3 &localOffset,
        const PhysicsMaterial &material)
    {
        AttachColliderShape(body, collider, PxBoxGeometry(halfExtents.x, halfExtents.y, halfExtents.z),
                            PxTransform(PxVec3(localOffset.x, localOffset.y, localOffset.z)), material);
    }

    void PhysXBackend::AddCapsuleCollider(
        const PhysicsBodyHandle body,
        ICollider *collider,
        const float radius,
        const float height,
        const Math::Vector3 &localOffset,
        const PhysicsMaterial &material)
    {
        const float halfHeight = std::max(0.01f, (height - 2.0f * radius) * 0.5f);
        const PxQuat rotation(PxHalfPi, PxVec3(0, 0, 1)); // PhysX capsules lie along X; ours stand on Y
        AttachColliderShape(body, collider, PxCapsuleGeometry(radius, halfHeight),
                            PxTransform(PxVec3(localOffset.x, localOffset.y, localOffset.z), rotation), material);
    }

    void PhysXBackend::SetIsTrigger(const PhysicsBodyHandle body, ICollider *collider, const bool isTrigger)
    {
        if (!GetBodyData(body))
        {
            return;
        }

        const auto it = _colliderShapes.find(collider);
        if (it == _colliderShapes.end())
        {
            return;
        }

        // Only this collider's shapes: a trigger sphere must not turn the object's solid box into a trigger
        for (PxShape *shape : it->second)
        {
            if (isTrigger)
            {
                shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false);
                shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, true);
            }
            else
            {
                shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, false);
                shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, true);
            }
        }
    }

    // ========== Forces and Motion ==========

    void PhysXBackend::AddForce(const PhysicsBodyHandle body, const Math::Vector3 &force)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
        {
            return;
        }

        if (auto *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->addForce(PxVec3(force.x, force.y, force.z));
        }
    }

    void PhysXBackend::AddImpulse(PhysicsBodyHandle body, const Math::Vector3 &impulse)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
        {
            return;
        }

        if (auto *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->addForce(PxVec3(impulse.x, impulse.y, impulse.z), PxForceMode::eIMPULSE);
        }
    }

    void PhysXBackend::SetVelocity(PhysicsBodyHandle body, const Math::Vector3 &velocity)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
        {
            return;
        }

        if (auto *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->setLinearVelocity(PxVec3(velocity.x, velocity.y, velocity.z));
        }
    }

    void PhysXBackend::SetAngularVelocity(PhysicsBodyHandle body, const Math::Vector3 &velocity)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
        {
            return;
        }

        if (auto *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->setAngularVelocity(PxVec3(velocity.x, velocity.y, velocity.z));
        }
    }

    // ========== Queries ==========

    Math::Vector3 PhysXBackend::GetPosition(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
        {
            return Math::Vector3::Zero;
        }

        const PxTransform transform = data->actor->getGlobalPose();
        return {transform.p.x, transform.p.y, transform.p.z};
    }

    Math::Quaternion PhysXBackend::GetRotation(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
        {
            return Math::Quaternion::Identity;
        }

        const PxTransform transform = data->actor->getGlobalPose();
        return {transform.q.w, transform.q.x, transform.q.y, transform.q.z};
    }

    Math::Vector3 PhysXBackend::GetVelocity(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
        {
            return Math::Vector3::Zero;
        }

        if (const PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            const PxVec3 vel = dynamic->getLinearVelocity();
            return {vel.x, vel.y, vel.z};
        }

        return Math::Vector3::Zero;
    }

    Math::Vector3 PhysXBackend::GetAngularVelocity(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
        {
            return Math::Vector3::Zero;
        }

        if (const PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            const PxVec3 vel = dynamic->getAngularVelocity();
            return {vel.x, vel.y, vel.z};
        }

        return Math::Vector3::Zero;
    }

    // ========== Properties ==========

    void PhysXBackend::SetMass(PhysicsBodyHandle body, float mass)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
        {
            return;
        }

        if (auto *dynamic = data->actor->is<PxRigidDynamic>())
        {
            PxRigidBodyExt::setMassAndUpdateInertia(*dynamic, mass);
        }
    }

    float PhysXBackend::GetMass(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
        {
            return 0.0f;
        }

        if (const PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            return dynamic->getMass();
        }

        return 0.0f;
    }

    void PhysXBackend::SetGravityEnabled(PhysicsBodyHandle body, bool enabled)
    {
        BodyData *data = GetBodyData(body);
        if (!data)
        {
            return;
        }

        if (auto *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !enabled);
        }
    }

    // ========== Transform Syncing ==========

    void PhysXBackend::SyncTransforms()
    {
        for (const auto &bodyData : _bodies)
        {
            if (!bodyData.active || !bodyData.actor || !bodyData.rigidbody)
            {
                continue;
            }

            // Only simulated dynamic bodies. Kinematic bodies are driven by their transform; writing the
            // physics pose back would overwrite a script's move before PhysX consumed it.
            const auto *dynamic = bodyData.actor->is<PxRigidDynamic>();
            if (!dynamic || dynamic->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC))
            {
                continue;
            }

            // Get transform from PhysX
            const PxTransform pxTransform = bodyData.actor->getGlobalPose();

            // Convert to engine types
            Math::Vector3 position(pxTransform.p.x, pxTransform.p.y, pxTransform.p.z);
            Math::Quaternion rotation(pxTransform.q.w, pxTransform.q.x, pxTransform.q.y, pxTransform.q.z);

            // Update GameObject transform
            if (Positionable *positionable = bodyData.rigidbody->GetGameObject().GetPositionable())
            {
                positionable->SetPositionAndRotation(position, rotation);
            }
        }
    }

    // ========== Collision Detection Callbacks ==========

    void PhysXBackend::onContact(
        const PxContactPairHeader &pairHeader,
        const PxContactPair *pairs,
        PxU32 nbPairs)
    {
        // A released actor's pointer is invalid here. Its pairs were already dropped in DestroyBody.
        if (pairHeader.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 | PxContactPairHeaderFlag::eREMOVED_ACTOR_1))
        {
            return;
        }

        auto *handleA = static_cast<PhysicsBodyHandle*>(pairHeader.actors[0]->userData);
        auto *handleB = static_cast<PhysicsBodyHandle*>(pairHeader.actors[1]->userData);

        if (!handleA || !handleB)
        {
            return;
        }

        for (PxU32 i = 0; i < nbPairs; i++)
        {
            const PxContactPair &cp = pairs[i];
            CollisionPair pair{*handleA, *handleB};

            // Create base collision data
            auto *baseCollisionData = new Collision();

            // Extract contact points from PhysX
            PxContactPairPoint contactPoints[64];
            PxU32 numContacts = cp.extractContacts(contactPoints, 64);

            for (PxU32 j = 0; j < numContacts; j++)
            {
                ContactPoint contact;
                contact.point = Math::Vector3(
                    contactPoints[j].position.x,
                    contactPoints[j].position.y,
                    contactPoints[j].position.z);
                contact.normal = Math::Vector3(
                    contactPoints[j].normal.x,
                    contactPoints[j].normal.y,
                    contactPoints[j].normal.z);
                contact.separation = contactPoints[j].separation;
                contact.normalImpulse = contactPoints[j].impulse.magnitude();

                baseCollisionData->contacts.push_back(contact);
            }

            // Calculate total impulse
            Math::Vector3 totalImpulse = Math::Vector3::Zero;
            for (const auto &contact : baseCollisionData->contacts)
            {
                totalImpulse = totalImpulse + (contact.normal * contact.normalImpulse);
            }
            baseCollisionData->impulse = totalImpulse;

            // Get relative velocity
            auto *dynA = pairHeader.actors[0]->is<PxRigidDynamic>();
            auto *dynB = pairHeader.actors[1]->is<PxRigidDynamic>();

            PxVec3 velA = dynA ? dynA->getLinearVelocity() : PxVec3(0);
            PxVec3 velB = dynB ? dynB->getLinearVelocity() : PxVec3(0);
            PxVec3 relVel = velA - velB;
            baseCollisionData->relativeVelocity = Math::Vector3(relVel.x, relVel.y, relVel.z);

            // Check event type
            if (cp.events & PxPairFlag::eNOTIFY_TOUCH_FOUND)
            {
                // Collision started
                _newCollisions.push_back({pair, baseCollisionData});
                _activeCollisions.insert(pair);
            }
            else if (cp.events & PxPairFlag::eNOTIFY_TOUCH_LOST)
            {
                // Collision ended
                _endedCollisions.push_back({pair, baseCollisionData});
                _activeCollisions.erase(pair);
            }
            else
            {
                // Event we don't care about - clean up
                delete baseCollisionData;
            }
        }
    }

    void PhysXBackend::onTrigger(PxTriggerPair *pairs, const PxU32 count)
    {
        for (PxU32 i = 0; i < count; i++)
        {
            // Removed shapes may belong to released actors (e.g. an object destroyed inside a trigger
            // volume); their pairs were dropped when the body or collider went away
            if (pairs[i].flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER | PxTriggerPairFlag::eREMOVED_SHAPE_OTHER))
            {
                continue;
            }

            const auto *triggerHandle = static_cast<PhysicsBodyHandle*>(pairs[i].triggerActor->userData);
            const auto *otherHandle = static_cast<PhysicsBodyHandle*>(pairs[i].otherActor->userData);

            if (!triggerHandle || !otherHandle)
                continue;

            CollisionPair pair{*triggerHandle, *otherHandle};

            if (pairs[i].status & PxPairFlag::eNOTIFY_TOUCH_FOUND)
            {
                _newTriggers.push_back({pair});
                _activeTriggers.insert(pair);
            }
            else if (pairs[i].status & PxPairFlag::eNOTIFY_TOUCH_LOST)
            {
                _endedTriggers.push_back({pair});
                _activeTriggers.erase(pair);
            }
        }
    }

    // Helper to create collision data with resolved object references
    Collision PhysXBackend::CreateCollisionData(
        const CollisionPair &pair,
        const Collision &baseData,
        bool isForBodyA)
    {
        Collision collision = baseData;

        BodyData *thisData = isForBodyA ? GetBodyData(pair.bodyA) : GetBodyData(pair.bodyB);
        BodyData *otherData = isForBodyA ? GetBodyData(pair.bodyB) : GetBodyData(pair.bodyA);

        // Set this object's references
        if (thisData)
        {
            if (thisData->rigidbody)
            {
                collision.gameObject = &thisData->rigidbody->GetGameObject();
                collision.rigidbody = thisData->rigidbody;
            }
            else if (!thisData->colliders.empty())
            {
                collision.gameObject = &thisData->colliders[0]->GetGameObject();
                collision.rigidbody = nullptr;
            }
        }

        // Set other object's references
        if (otherData)
        {
            if (otherData->rigidbody)
            {
                collision.otherGameObject = &otherData->rigidbody->GetGameObject();
                collision.otherRigidbody = otherData->rigidbody;
            }
            else if (!otherData->colliders.empty())
            {
                collision.otherGameObject = &otherData->colliders[0]->GetGameObject();
                collision.otherRigidbody = nullptr;
            }
        }

        // Flip normals for body B
        if (!isForBodyA)
        {
            for (auto &contact : collision.contacts)
            {
                contact.normal = contact.normal * -1.0f;
            }
        }

        return collision;
    }

    Trigger PhysXBackend::CreateTriggerData(const CollisionPair &pair, bool isForBodyA)
    {
        Trigger trigger;

        BodyData *thisData = isForBodyA ? GetBodyData(pair.bodyA) : GetBodyData(pair.bodyB);
        BodyData *otherData = isForBodyA ? GetBodyData(pair.bodyB) : GetBodyData(pair.bodyA);

        // Set this object's references
        if (thisData)
        {
            if (thisData->rigidbody)
            {
                trigger.gameObject = &thisData->rigidbody->GetGameObject();
                trigger.rigidbody = thisData->rigidbody;
            }
            else if (!thisData->colliders.empty())
            {
                trigger.gameObject = &thisData->colliders[0]->GetGameObject();
                trigger.rigidbody = nullptr;
            }
        }

        // Set other object's references
        if (otherData)
        {
            if (otherData->rigidbody)
            {
                trigger.otherGameObject = &otherData->rigidbody->GetGameObject();
                trigger.otherRigidbody = otherData->rigidbody;
            }
            else if (!otherData->colliders.empty())
            {
                trigger.otherGameObject = &otherData->colliders[0]->GetGameObject();
                trigger.otherRigidbody = nullptr;
            }
        }

        return trigger;
    }

    GameObject *PhysXBackend::GetBodyOwner(const PhysicsBodyHandle handle)
    {
        const BodyData *data = GetBodyData(handle);
        if (!data)
        {
            return nullptr;
        }
        if (data->rigidbody)
        {
            return &data->rigidbody->GetGameObject();
        }
        if (!data->colliders.empty())
        {
            return &data->colliders.front()->GetGameObject();
        }
        return nullptr;
    }

    void PhysXBackend::DispatchToBody(const PhysicsBodyHandle handle, const std::function<void(Component &)> &fn)
    {
        GameObject *owner = GetBodyOwner(handle);
        if (!owner)
        {
            return; // destroyed this frame, or never belonged to a GameObject
        }

        // Snapshot: a handler can add or remove components, which would invalidate the live vector
        std::vector<Component *> components;
        for (const auto &component : owner->GetAllComponents())
        {
            components.push_back(component.get());
        }

        for (Component *component : components)
        {
            // A handler may have destroyed this body (e.g. removed its Rigidbody) or other components
            if (GetBodyOwner(handle) != owner)
            {
                return;
            }
            const auto &live = owner->GetAllComponents();
            if (std::ranges::none_of(live, [component](const auto &c) { return c.get() == component; }) ||
                component->IsDestroyed())
            {
                continue;
            }
            fn(*component);
        }
    }

    void PhysXBackend::ForgetPairsWithBody(const PhysicsBodyHandle handle)
    {
        const auto involves = [handle](const CollisionPair &pair)
        {
            return pair.bodyA == handle || pair.bodyB == handle;
        };
        // The other body still gets its Exit (as in Unity): PhysX's touch-lost for a released actor or
        // shape is skipped by the callbacks, so nothing else would report it. DispatchToBody skips a
        // side that no longer exists.
        for (const CollisionPair &pair : _activeCollisions)
        {
            if (involves(pair))
            {
                _endedCollisions.push_back({pair, new Collision()});
            }
        }
        for (const CollisionPair &pair : _activeTriggers)
        {
            if (involves(pair))
            {
                _endedTriggers.push_back({pair});
            }
        }
        std::erase_if(_activeCollisions, involves);
        std::erase_if(_activeTriggers, involves);
    }

    void PhysXBackend::ProcessCollisionCallbacks()
    {
        // Every event goes to *each* body's own GameObject (A's components get A's view, B's get B's).
        // This used to send both to B's first collider, which could be null or empty.
        // Take the event lists first so handlers can't disturb what's being iterated.

        // ===== OnCollisionEnter =====
        std::vector<CollisionEvent> newCollisions;
        newCollisions.swap(_newCollisions);
        for (const auto &event : newCollisions)
        {
            const Collision forA = CreateCollisionData(event.pair, *event.data, true);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnCollisionEnter(forA); });
            const Collision forB = CreateCollisionData(event.pair, *event.data, false);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnCollisionEnter(forB); });
            delete event.data;
        }

        // ===== OnCollisionStay =====
        const std::vector<CollisionPair> activeCollisions(_activeCollisions.begin(), _activeCollisions.end());
        for (const auto &pair : activeCollisions)
        {
            if (!_activeCollisions.contains(pair))
            {
                continue; // a handler destroyed one of the bodies
            }
            Collision baseData{};
            const Collision forA = CreateCollisionData(pair, baseData, true);
            DispatchToBody(pair.bodyA, [&forA](Component &c) { c.OnCollisionStay(forA); });
            const Collision forB = CreateCollisionData(pair, baseData, false);
            DispatchToBody(pair.bodyB, [&forB](Component &c) { c.OnCollisionStay(forB); });
        }

        // ===== OnCollisionExit =====
        std::vector<CollisionEvent> endedCollisions;
        endedCollisions.swap(_endedCollisions);
        for (const auto &event : endedCollisions)
        {
            const Collision forA = CreateCollisionData(event.pair, *event.data, true);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnCollisionExit(forA); });
            const Collision forB = CreateCollisionData(event.pair, *event.data, false);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnCollisionExit(forB); });
            delete event.data;
        }

        // ===== OnTriggerEnter =====
        std::vector<TriggerEvent> newTriggers;
        newTriggers.swap(_newTriggers);
        for (const auto &event : newTriggers)
        {
            const Trigger forA = CreateTriggerData(event.pair, true);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnTriggerEnter(forA); });
            const Trigger forB = CreateTriggerData(event.pair, false);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnTriggerEnter(forB); });
        }

        // ===== OnTriggerStay =====
        const std::vector<CollisionPair> activeTriggers(_activeTriggers.begin(), _activeTriggers.end());
        for (const auto &pair : activeTriggers)
        {
            if (!_activeTriggers.contains(pair))
            {
                continue;
            }
            const Trigger forA = CreateTriggerData(pair, true);
            DispatchToBody(pair.bodyA, [&forA](Component &c) { c.OnTriggerStay(forA); });
            const Trigger forB = CreateTriggerData(pair, false);
            DispatchToBody(pair.bodyB, [&forB](Component &c) { c.OnTriggerStay(forB); });
        }

        // ===== OnTriggerExit =====
        std::vector<TriggerEvent> endedTriggers;
        endedTriggers.swap(_endedTriggers);
        for (const auto &event : endedTriggers)
        {
            const Trigger forA = CreateTriggerData(event.pair, true);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnTriggerExit(forA); });
            const Trigger forB = CreateTriggerData(event.pair, false);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnTriggerExit(forB); });
        }
    }

    void PhysXBackend::RemoveColliderShapes(PhysicsBodyHandle body, ICollider *collider)
    {
        if (!collider)
        {
            return;
        }

        BodyData *bodyData = GetBodyData(body);
        if (!bodyData || !bodyData->actor)
        {
            return;
        }

        const auto it = _colliderShapes.find(collider);
        if (it == _colliderShapes.end())
        {
            return;
        }

        // detachShape drops the actor's reference, which is the only one (AttachColliderShape released
        // its own), so the shape is freed here. The extra release() that used to follow freed it twice.
        for (PxShape *shape : it->second)
        {
            bodyData->actor->detachShape(*shape);
        }

        it->second.clear();
    }

    void PhysXBackend::UpdateSphereCollider(
        PhysicsBodyHandle body,
        ICollider *collider,
        float radius,
        const Math::Vector3 &localOffset,
        const PhysicsMaterial &material)
    {
        if (!collider)
        {
            return;
        }

        const auto it = _colliderShapes.find(collider);
        if (it == _colliderShapes.end() || it->second.empty())
        {
            return;
        }

        PxMaterial *pxMaterial = GetOrCreateMaterial(material);
        if (!pxMaterial)
        {
            pxMaterial = _defaultMaterial;
        }

        PxSphereGeometry sphere(radius);
        const PxTransform localPose(PxVec3(localOffset.x, localOffset.y, localOffset.z));

        for (PxShape *shape : it->second)
        {
            shape->setGeometry(sphere);
            shape->setLocalPose(localPose);

            PxMaterial *materials[] = {pxMaterial};
            shape->setMaterials(materials, 1);
        }
    }

    void PhysXBackend::UpdateBoxCollider(
        PhysicsBodyHandle body,
        ICollider *collider,
        const Math::Vector3 &halfExtents,
        const Math::Vector3 &localOffset,
        const PhysicsMaterial &material)
    {
        if (!collider)
            return;

        auto it = _colliderShapes.find(collider);
        if (it == _colliderShapes.end() || it->second.empty())
            return;

        PxMaterial *pxMaterial = GetOrCreateMaterial(material);
        if (!pxMaterial)
            pxMaterial = _defaultMaterial;

        PxBoxGeometry box(halfExtents.x, halfExtents.y, halfExtents.z);
        PxTransform localPose(PxVec3(localOffset.x, localOffset.y, localOffset.z));

        for (PxShape *shape : it->second)
        {
            shape->setGeometry(box);
            shape->setLocalPose(localPose);

            PxMaterial *materials[] = {pxMaterial};
            shape->setMaterials(materials, 1);
        }
    }

    void PhysXBackend::UpdateCapsuleCollider(
        PhysicsBodyHandle body,
        ICollider *collider,
        float radius,
        float height,
        const Math::Vector3 &localOffset,
        const PhysicsMaterial &material)
    {
        if (!collider)
            return;

        auto it = _colliderShapes.find(collider);
        if (it == _colliderShapes.end() || it->second.empty())
            return;

        PxMaterial *pxMaterial = GetOrCreateMaterial(material);
        if (!pxMaterial)
            pxMaterial = _defaultMaterial;

        float halfHeight = (height - 2.0f * radius) * 0.5f;
        halfHeight = std::max(0.01f, halfHeight);

        PxCapsuleGeometry capsule(radius, halfHeight);

        PxQuat rotation(PxHalfPi, PxVec3(0, 0, 1));
        PxTransform localPose(PxVec3(localOffset.x, localOffset.y, localOffset.z), rotation);

        for (PxShape *shape : it->second)
        {
            shape->setGeometry(capsule);
            shape->setLocalPose(localPose);

            PxMaterial *materials[] = {pxMaterial};
            shape->setMaterials(materials, 1);
        }
    }

    void PhysXBackend::FillRaycastHit(RaycastHit &hit, const PxRaycastHit &pxHit) const
    {
        hit.hit = true;
        hit.point = Math::Vector3(pxHit.position.x, pxHit.position.y, pxHit.position.z);
        hit.normal = Math::Vector3(pxHit.normal.x, pxHit.normal.y, pxHit.normal.z);
        hit.distance = pxHit.distance;

        if (pxHit.actor && pxHit.actor->userData)
        {
            const auto *handle = static_cast<PhysicsBodyHandle*>(pxHit.actor->userData);
            hit.bodyHandle = *handle;

            if (const BodyData *bodyData = GetBodyData(*handle))
            {
                if (bodyData->rigidbody)
                {
                    hit.gameObject = &bodyData->rigidbody->GetGameObject();
                    hit.rigidbody = bodyData->rigidbody;
                }
                else if (!bodyData->colliders.empty())
                {
                    hit.gameObject = &bodyData->colliders[0]->GetGameObject();
                    hit.collider = bodyData->colliders[0];
                }
            }
        }
    }

    bool PhysXBackend::Raycast(
        const Math::Vector3 &origin,
        const Math::Vector3 &direction,
        RaycastHit &hit,
        float maxDistance,
        uint32_t layerMask)
    {
        if (!_scene)
        {
            hit.hit = false;
            return false;
        }

        Math::Vector3 dir = direction.Normalized();
        PxVec3 pxOrigin(origin.x, origin.y, origin.z);
        PxVec3 pxDir(dir.x, dir.y, dir.z);

        PxRaycastBuffer hitBuffer;
        PxQueryFilterData filterData;
        filterData.data.word0 = layerMask;

        bool status = _scene->raycast(pxOrigin, pxDir, maxDistance, hitBuffer, PxHitFlag::eDEFAULT, filterData);

        if (status && hitBuffer.hasBlock)
        {
            FillRaycastHit(hit, hitBuffer.block);
            return true;
        }

        hit.hit = false;
        return false;
    }

    int PhysXBackend::RaycastAll(
        const Math::Vector3 &origin,
        const Math::Vector3 &direction,
        std::vector<RaycastHit> &hits,
        float maxDistance,
        uint32_t layerMask)
    {
        hits.clear();

        if (!_scene)
            return 0;

        Math::Vector3 dir = direction.Normalized();
        PxVec3 pxOrigin(origin.x, origin.y, origin.z);
        PxVec3 pxDir(dir.x, dir.y, dir.z);

        constexpr PxU32 maxHits = 256;
        PxRaycastHit hitBuffer[maxHits];
        PxQueryFilterData filterData;
        filterData.data.word0 = layerMask;

        PxRaycastBuffer raycastBuffer(hitBuffer, maxHits);

        // Report every hit as touching; otherwise PhysX keeps only the closest blocking one
        TouchAllHitsFilter touchAll;
        filterData.flags |= PxQueryFlag::ePREFILTER;

        if (_scene->raycast(pxOrigin, pxDir, maxDistance, raycastBuffer, PxHitFlag::eDEFAULT, filterData, &touchAll))
        {
            for (PxU32 i = 0; i < raycastBuffer.nbTouches; i++)
            {
                RaycastHit hit;
                FillRaycastHit(hit, raycastBuffer.touches[i]);
                hits.push_back(hit);
            }

            if (raycastBuffer.hasBlock)
            {
                RaycastHit hit;
                FillRaycastHit(hit, raycastBuffer.block);
                hits.push_back(hit);
            }
        }

        std::ranges::sort(hits, [](const RaycastHit &a, const RaycastHit &b)
        {
            return a.distance < b.distance;
        });

        return static_cast<int>(hits.size());
    }

    bool PhysXBackend::SphereCast(
        const Math::Vector3 &origin,
        const float radius,
        const Math::Vector3 &direction,
        RaycastHit &hit,
        const float maxDistance,
        const uint32_t layerMask)
    {
        if (!_scene)
        {
            hit.hit = false;
            return false;
        }

        const Math::Vector3 dir = direction.Normalized();
        const PxVec3 pxOrigin(origin.x, origin.y, origin.z);
        const PxVec3 pxDir(dir.x, dir.y, dir.z);

        const PxSphereGeometry sphere(radius);
        const PxTransform pose(pxOrigin);

        PxSweepBuffer hitBuffer;
        PxQueryFilterData filterData;
        filterData.data.word0 = layerMask;

        const bool status = _scene->sweep(sphere, pose, pxDir, maxDistance, hitBuffer, PxHitFlag::eDEFAULT, filterData);
        if (status && hitBuffer.hasBlock)
        {
            hit.hit = true;
            hit.point = Math::Vector3(hitBuffer.block.position.x, hitBuffer.block.position.y,
                                      hitBuffer.block.position.z);
            hit.normal = Math::Vector3(hitBuffer.block.normal.x, hitBuffer.block.normal.y, hitBuffer.block.normal.z);
            hit.distance = hitBuffer.block.distance;

            if (hitBuffer.block.actor && hitBuffer.block.actor->userData)
            {
                const auto *handle = static_cast<PhysicsBodyHandle*>(hitBuffer.block.actor->userData);
                hit.bodyHandle = *handle;

                if (const BodyData *bodyData = GetBodyData(*handle))
                {
                    if (bodyData->rigidbody)
                    {
                        hit.gameObject = &bodyData->rigidbody->GetGameObject();
                        hit.rigidbody = bodyData->rigidbody;
                    }
                    else if (!bodyData->colliders.empty())
                    {
                        hit.gameObject = &bodyData->colliders[0]->GetGameObject();
                        hit.collider = bodyData->colliders[0];
                    }
                }
            }
            return true;
        }

        hit.hit = false;
        return false;
    }

    // Stub implementations for unused callbacks
    void PhysXBackend::onConstraintBreak(PxConstraintInfo *constraints, PxU32 count) {}
    void PhysXBackend::onWake(PxActor **actors, PxU32 count) {}
    void PhysXBackend::onSleep(PxActor **actors, PxU32 count) {}

    void PhysXBackend::onAdvance(const PxRigidBody *const *bodyBuffer, const PxTransform *poseBuffer,
                                 const PxU32 count) {}

#else

    // ===== Stub Implementation When PhysX is Not Available =====

    PhysXBackend::~PhysXBackend() {}
    bool PhysXBackend::Initialize() { return false; }
    void PhysXBackend::Update(float) {}
    void PhysXBackend::Shutdown() {}
    void PhysXBackend::ApplyPendingChanges() {}
    void PhysXBackend::SyncTransforms() {}
    void PhysXBackend::ProcessCollisionCallbacks() {}

    PhysicsBodyHandle PhysXBackend::CreateDynamicBody(const Math::Vector3 &, const Math::Quaternion &, float,
                                                      Rigidbody *)
    {
        return INVALID_PHYSICS_HANDLE;
    }

    PhysicsBodyHandle PhysXBackend::CreateStaticBody(const Math::Vector3 &, const Math::Quaternion &, Rigidbody *)
    {
        return INVALID_PHYSICS_HANDLE;
    }

    void PhysXBackend::DestroyBody(PhysicsBodyHandle) {}
    void PhysXBackend::RegisterCollider(PhysicsBodyHandle, Collider *) {}
    void PhysXBackend::UnregisterCollider(PhysicsBodyHandle, Collider *) {}
    void PhysXBackend::AddSphereCollider(PhysicsBodyHandle, ICollider *, float, const Math::Vector3 &,
                                         const PhysicsMaterial &) {}
    void PhysXBackend::AddBoxCollider(PhysicsBodyHandle, ICollider *, const Math::Vector3 &, const Math::Vector3 &,
                                      const PhysicsMaterial &) {}
    void PhysXBackend::AddCapsuleCollider(PhysicsBodyHandle, ICollider *, float, float, const Math::Vector3 &,
                                          const PhysicsMaterial &) {}
    void PhysXBackend::SetIsTrigger(PhysicsBodyHandle, ICollider *, bool) {}
    void PhysXBackend::AddForce(PhysicsBodyHandle, const Math::Vector3 &) {}
    void PhysXBackend::AddImpulse(PhysicsBodyHandle, const Math::Vector3 &) {}
    void PhysXBackend::SetVelocity(PhysicsBodyHandle, const Math::Vector3 &) {}
    void PhysXBackend::SetAngularVelocity(PhysicsBodyHandle, const Math::Vector3 &) {}
    Math::Vector3 PhysXBackend::GetPosition(PhysicsBodyHandle) { return Math::Vector3::Zero; }
    Math::Quaternion PhysXBackend::GetRotation(PhysicsBodyHandle) { return Math::Quaternion::Identity; }
    Math::Vector3 PhysXBackend::GetVelocity(PhysicsBodyHandle) { return Math::Vector3::Zero; }
    Math::Vector3 PhysXBackend::GetAngularVelocity(PhysicsBodyHandle) { return Math::Vector3::Zero; }
    void PhysXBackend::SetMass(PhysicsBodyHandle, float) {}
    float PhysXBackend::GetMass(PhysicsBodyHandle) { return 0.0f; }
    void PhysXBackend::SetGravityEnabled(PhysicsBodyHandle, bool) {}
    void PhysXBackend::SetGravity(const Math::Vector3 &) {}
    Math::Vector3 PhysXBackend::GetGravity() const { return Math::Vector3(0.0f, -9.81f, 0.0f); }

#endif
}
