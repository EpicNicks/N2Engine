#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/Logger.hpp"

#ifdef N2ENGINE_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#include <extensions/PxDefaultAllocator.h>
#include <extensions/PxDefaultErrorCallback.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#include <extensions/PxDefaultSimulationFilterShader.h>
#include <extensions/PxRigidBodyExt.h>

#include <format>
#include <algorithm>

using namespace physx;
#endif

namespace N2Engine::Physics
{

#ifdef N2ENGINE_PHYSX_ENABLED

    PhysXBackend::PhysXBackend()
        : _foundation(nullptr),
          _physics(nullptr),
          _scene(nullptr),
          _dispatcher(nullptr),
          _defaultMaterial(nullptr)
    {
    }

    PhysXBackend::~PhysXBackend()
    {
        Shutdown();
    }

    bool PhysXBackend::Initialize()
    {
        Logger::Info("Initializing PhysX Backend...");

        // 1. Create foundation
        static PxDefaultAllocator allocator;
        static PxDefaultErrorCallback errorCallback;

        _foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, errorCallback);
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
            PxPvdTransport *transport = PxDefaultPvdSocketTransportCreate("127.0.0.1", 5425, 10);
            if (transport)
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
        sceneDesc.filterShader = PxDefaultSimulationFilterShader;

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
            PxPvdSceneClient *pvdClient = _scene->getScenePvdClient();
            if (pvdClient)
            {
                pvdClient->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONSTRAINTS, true);
                pvdClient->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONTACTS, true);
                pvdClient->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_SCENEQUERIES, true);
            }
        }

        Logger::Info("PhysX initialized successfully!");
        return true;
    }

    void PhysXBackend::Update(float deltaTime)
    {
        if (!_scene)
            return;

        _scene->simulate(deltaTime);
        _scene->fetchResults(true);
    }

    void PhysXBackend::Shutdown()
    {
        Logger::Info("Shutting down PhysX...");

        // Release all bodies
        for (auto &bodyData : _bodies)
        {
            if (bodyData.active && bodyData.actor)
            {
                // Clean up user data
                if (bodyData.actor->userData)
                {
                    delete static_cast<PhysicsBodyHandle *>(bodyData.actor->userData);
                    bodyData.actor->userData = nullptr;
                }

                bodyData.actor->release();
            }
        }
        _bodies.clear();
        _freeList.clear();

        // Release cached materials
        for (auto &[key, material] : _materialCache)
        {
            material->release();
        }
        _materialCache.clear();

        // Clean up collision events
        for (auto &event : _newCollisions)
        {
            delete event.data;
        }
        _newCollisions.clear();

        for (auto &event : _endedCollisions)
        {
            delete event.data;
        }
        _endedCollisions.clear();

        // Release PhysX objects
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
            PxPvdSceneClient *pvdClient = _scene->getScenePvdClient();
            if (pvdClient)
            {
                pvdClient->setScenePvdFlag(PxPvdSceneFlag::eTRANSMIT_CONSTRAINTS, false);
                // ... other PVD cleanup ...
            }
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
        for (auto &[type, action] : _pendingChanges)
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

    PhysXBackend::BodyData *PhysXBackend::GetBodyData(PhysicsBodyHandle handle)
    {
        if (handle.index >= _bodies.size())
            return nullptr;

        BodyData &data = _bodies[handle.index];

        if (!data.active || data.generation != handle.generation)
            return nullptr;

        return &data;
    }

    const PhysXBackend::BodyData *PhysXBackend::GetBodyData(PhysicsBodyHandle handle) const
    {
        if (handle.index >= _bodies.size())
            return nullptr;

        const BodyData &data = _bodies[handle.index];

        if (!data.active || data.generation != handle.generation)
            return nullptr;

        return &data;
    }

    // ========== Body Creation ==========

    PhysicsBodyHandle PhysXBackend::CreateDynamicBody(const Math::Vector3 &position, const Math::Quaternion &rotation, float mass, Rigidbody *rigidbody, bool isKinematic)
    {
        PxTransform transform(
            PxVec3(position.x, position.y, position.z),
            PxQuat(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()));

        PxRigidDynamic *body = _physics->createRigidDynamic(transform);
        body->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, isKinematic);
        if (!body)
        {
            Logger::Error("Failed to create dynamic body");
            return INVALID_PHYSICS_HANDLE;
        }

        PxRigidBodyExt::setMassAndUpdateInertia(*body, mass);

        PhysicsBodyHandle handle = AllocateHandle();
        BodyData *data = GetBodyData(handle);
        data->actor = body;
        data->rigidbody = rigidbody;

        body->userData = new PhysicsBodyHandle(handle);

        _scene->addActor(*body);

        return handle;
    }

    PhysicsBodyHandle PhysXBackend::CreateStaticBody(const Math::Vector3 &position, const Math::Quaternion &rotation, Rigidbody *rigidbody)
    {
        PxTransform transform(PxVec3(position.x, position.y, position.z), PxQuat(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()));
        PxRigidStatic *body = _physics->createRigidStatic(transform);

        if (!body)
        {
            Logger::Error("Failed to create static body");
            return INVALID_PHYSICS_HANDLE;
        }

        PhysicsBodyHandle handle = AllocateHandle();
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
                delete static_cast<PhysicsBodyHandle *>(data->actor->userData);
                data->actor->userData = nullptr;
            }

            data->actor->release();
            data->actor = nullptr;
        }

        data->active = false;
        data->rigidbody = nullptr;
        data->colliders.clear();
        _freeList.push_back(handle.index);
    }

    // ========== Component Registration ==========

    void PhysXBackend::RegisterCollider(PhysicsBodyHandle handle, ICollider *collider)
    {
        BodyData *data = GetBodyData(handle);
        if (data && collider)
        {
            // Only add if not already registered
            auto it = std::find(data->colliders.begin(), data->colliders.end(), collider);
            if (it == data->colliders.end())
            {
                data->colliders.push_back(collider);
            }
        }
    }

    void PhysXBackend::UnregisterCollider(PhysicsBodyHandle handle, ICollider *collider)
    {
        BodyData *data = GetBodyData(handle);
        if (data && collider)
        {
            auto it = std::find(data->colliders.begin(), data->colliders.end(), collider);
            if (it != data->colliders.end())
            {
                data->colliders.erase(it);
            }
        }
    }

    // ========== Transform Updates (for Transform -> Physics syncing) ==========
    void PhysXBackend::SetBodyTransform(
        PhysicsBodyHandle handle,
        const Math::Vector3 &position,
        const Math::Quaternion &rotation)
    {
        // Validate handle
        if (!handle.IsValid() || handle.index >= _bodies.size())
            return;

        BodyData &bodyData = _bodies[handle.index];
        if (!bodyData.active || bodyData.generation != handle.generation)
            return;

        PxRigidActor *actor = bodyData.actor;
        if (auto *dynamic = actor->is<PxRigidDynamic>())
        {
            // For kinematic bodies, use setKinematicTarget for smooth interpolation
            PxTransform target(
                PxVec3(position.x, position.y, position.z),
                PxQuat(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()));
            dynamic->setKinematicTarget(target);
        }
    }

    void PhysXBackend::SetStaticBodyTransform(
        PhysicsBodyHandle handle,
        const Math::Vector3 &position,
        const Math::Quaternion &rotation)
    {
        // Validate handle
        if (!handle.IsValid() || handle.index >= _bodies.size())
            return;

        BodyData &bodyData = _bodies[handle.index];
        if (!bodyData.active || bodyData.generation != handle.generation)
            return;

        PxRigidActor *actor = bodyData.actor;
        if (actor->is<PxRigidStatic>())
        {
            // Direct position update for static bodies (expensive - rebuilds broadphase!)
            PxTransform transform(
                PxVec3(position.x, position.y, position.z),
                PxQuat(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()));
            actor->setGlobalPose(transform);
        }
    }

    // ========== Material Management ==========

    PxMaterial *PhysXBackend::GetOrCreateMaterial(const PhysicsMaterial &material)
    {
        MaterialKey key{material.staticFriction, material.dynamicFriction, material.restitution};

        auto it = _materialCache.find(key);
        if (it != _materialCache.end())
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

    // ========== Shape Attachment ==========

    void PhysXBackend::AddSphereCollider(
        PhysicsBodyHandle body,
        float radius,
        const Math::Vector3 &localOffset,
        const PhysicsMaterial &material)
    {
        BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
            return;

        PxMaterial *pxMaterial = GetOrCreateMaterial(material);
        if (!pxMaterial)
            pxMaterial = _defaultMaterial;

        PxSphereGeometry sphere(radius);
        PxTransform localPose(PxVec3(localOffset.x, localOffset.y, localOffset.z));

        PxShape *shape = _physics->createShape(sphere, *pxMaterial, true);
        shape->setLocalPose(localPose);

        data->actor->attachShape(*shape);
        shape->release();
    }

    void PhysXBackend::AddBoxCollider(
        PhysicsBodyHandle body,
        const Math::Vector3 &halfExtents,
        const Math::Vector3 &localOffset,
        const PhysicsMaterial &material)
    {
        BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
            return;

        PxMaterial *pxMaterial = GetOrCreateMaterial(material);
        if (!pxMaterial)
            pxMaterial = _defaultMaterial;

        PxBoxGeometry box(halfExtents.x, halfExtents.y, halfExtents.z);
        PxTransform localPose(PxVec3(localOffset.x, localOffset.y, localOffset.z));

        PxShape *shape = _physics->createShape(box, *pxMaterial, true);
        shape->setLocalPose(localPose);

        data->actor->attachShape(*shape);
        shape->release();
    }

    void PhysXBackend::AddCapsuleCollider(
        PhysicsBodyHandle body,
        float radius,
        float height,
        const Math::Vector3 &localOffset,
        const PhysicsMaterial &material)
    {
        BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
            return;

        PxMaterial *pxMaterial = GetOrCreateMaterial(material);
        if (!pxMaterial)
            pxMaterial = _defaultMaterial;

        float halfHeight = (height - 2.0f * radius) * 0.5f;
        PxCapsuleGeometry capsule(radius, halfHeight);

        PxQuat rotation(PxHalfPi, PxVec3(0, 0, 1));
        PxTransform localPose(PxVec3(localOffset.x, localOffset.y, localOffset.z), rotation);

        PxShape *shape = _physics->createShape(capsule, *pxMaterial, true);
        shape->setLocalPose(localPose);

        data->actor->attachShape(*shape);
        shape->release();
    }

    // ========== Trigger Configuration ==========

    void PhysXBackend::SetIsTrigger(PhysicsBodyHandle body, bool isTrigger)
    {
        BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
            return;

        // Get all shapes attached to this actor
        PxU32 numShapes = data->actor->getNbShapes();
        std::vector<PxShape *> shapes(numShapes);
        data->actor->getShapes(shapes.data(), numShapes);

        // Set trigger flag on all shapes
        for (PxShape *shape : shapes)
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

    void PhysXBackend::AddForce(PhysicsBodyHandle body, const Math::Vector3 &force)
    {
        BodyData *data = GetBodyData(body);
        if (!data)
            return;

        if (PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->addForce(PxVec3(force.x, force.y, force.z));
        }
    }

    void PhysXBackend::AddImpulse(PhysicsBodyHandle body, const Math::Vector3 &impulse)
    {
        BodyData *data = GetBodyData(body);
        if (!data)
            return;

        if (PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->addForce(PxVec3(impulse.x, impulse.y, impulse.z), PxForceMode::eIMPULSE);
        }
    }

    void PhysXBackend::SetVelocity(PhysicsBodyHandle body, const Math::Vector3 &velocity)
    {
        BodyData *data = GetBodyData(body);
        if (!data)
            return;

        if (PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->setLinearVelocity(PxVec3(velocity.x, velocity.y, velocity.z));
        }
    }

    void PhysXBackend::SetAngularVelocity(PhysicsBodyHandle body, const Math::Vector3 &velocity)
    {
        BodyData *data = GetBodyData(body);
        if (!data)
            return;

        if (PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->setAngularVelocity(PxVec3(velocity.x, velocity.y, velocity.z));
        }
    }

    // ========== Queries ==========

    Math::Vector3 PhysXBackend::GetPosition(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
            return Math::Vector3::Zero();

        PxTransform transform = data->actor->getGlobalPose();
        return Math::Vector3(transform.p.x, transform.p.y, transform.p.z);
    }

    Math::Quaternion PhysXBackend::GetRotation(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
            return Math::Quaternion::Identity();

        PxTransform transform = data->actor->getGlobalPose();
        return Math::Quaternion(transform.q.w, transform.q.x, transform.q.y, transform.q.z);
    }

    Math::Vector3 PhysXBackend::GetVelocity(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
            return Math::Vector3::Zero();

        if (const PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            PxVec3 vel = dynamic->getLinearVelocity();
            return Math::Vector3(vel.x, vel.y, vel.z);
        }

        return Math::Vector3::Zero();
    }

    Math::Vector3 PhysXBackend::GetAngularVelocity(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
            return Math::Vector3::Zero();

        if (const PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            PxVec3 vel = dynamic->getAngularVelocity();
            return Math::Vector3(vel.x, vel.y, vel.z);
        }

        return Math::Vector3::Zero();
    }

    // ========== Properties ==========

    void PhysXBackend::SetMass(PhysicsBodyHandle body, float mass)
    {
        BodyData *data = GetBodyData(body);
        if (!data)
            return;

        if (PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            PxRigidBodyExt::setMassAndUpdateInertia(*dynamic, mass);
        }
    }

    float PhysXBackend::GetMass(PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data)
            return 0.0f;

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
            return;

        if (PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            dynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !enabled);
        }
    }

    // ========== Transform Syncing ==========

    void PhysXBackend::SyncTransforms()
    {
        for (auto &bodyData : _bodies)
        {
            if (!bodyData.active || !bodyData.actor || !bodyData.rigidbody)
                continue;

            // Only sync dynamic bodies
            PxRigidDynamic *dynamic = bodyData.actor->is<PxRigidDynamic>();
            if (!dynamic)
                continue;

            // Get transform from PhysX
            PxTransform pxTransform = bodyData.actor->getGlobalPose();

            // Convert to engine types
            Math::Vector3 position(pxTransform.p.x, pxTransform.p.y, pxTransform.p.z);
            Math::Quaternion rotation(pxTransform.q.w, pxTransform.q.x, pxTransform.q.y, pxTransform.q.z);

            // Update GameObject transform
            std::shared_ptr<N2Engine::Positionable> positionable = bodyData.rigidbody->GetGameObject().GetPositionable();
            if (positionable)
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
        PhysicsBodyHandle *handleA = static_cast<PhysicsBodyHandle *>(pairHeader.actors[0]->userData);
        PhysicsBodyHandle *handleB = static_cast<PhysicsBodyHandle *>(pairHeader.actors[1]->userData);

        if (!handleA || !handleB)
            return;

        for (PxU32 i = 0; i < nbPairs; i++)
        {
            const PxContactPair &cp = pairs[i];
            CollisionPair pair{*handleA, *handleB};

            // Create base collision data
            Collision *baseCollisionData = new Collision();

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
            Math::Vector3 totalImpulse = Math::Vector3::Zero();
            for (const auto &contact : baseCollisionData->contacts)
            {
                totalImpulse = totalImpulse + (contact.normal * contact.normalImpulse);
            }
            baseCollisionData->impulse = totalImpulse;

            // Get relative velocity
            PxRigidDynamic *dynA = pairHeader.actors[0]->is<PxRigidDynamic>();
            PxRigidDynamic *dynB = pairHeader.actors[1]->is<PxRigidDynamic>();

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

    void PhysXBackend::onTrigger(PxTriggerPair *pairs, PxU32 count)
    {
        for (PxU32 i = 0; i < count; i++)
        {
            PhysicsBodyHandle *triggerHandle = static_cast<PhysicsBodyHandle *>(pairs[i].triggerActor->userData);
            PhysicsBodyHandle *otherHandle = static_cast<PhysicsBodyHandle *>(pairs[i].otherActor->userData);

            if (!triggerHandle || !otherHandle)
                continue;

            CollisionPair pair{*triggerHandle, *otherHandle};

            if (pairs[i].status & PxPairFlag::eNOTIFY_TOUCH_FOUND)
            {
                _newTriggers.push_back({pair});
            }
            else if (pairs[i].status & PxPairFlag::eNOTIFY_TOUCH_LOST)
            {
                _endedTriggers.push_back({pair});
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

    Trigger PhysXBackend::CreateTriggerData(
        const CollisionPair &pair,
        bool isForBodyA)
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

    void PhysXBackend::ProcessCollisionCallbacks()
    {
        // ===== OnCollisionEnter =====
        for (const auto &event : _newCollisions)
        {
            BodyData *dataA = GetBodyData(event.pair.bodyA);
            BodyData *dataB = GetBodyData(event.pair.bodyB);

            // Call on Body A's components
            if (dataA)
            {
                Collision collisionForA = CreateCollisionData(event.pair, *event.data, true);

                // Call on Rigidbody if present
                if (dataA->rigidbody)
                {
                    dataA->rigidbody->OnCollisionEnter(collisionForA);
                }

                // Call on all Colliders
                for (ICollider *collider : dataA->colliders)
                {
                    if (collider)
                    {
                        collider->OnCollisionEnter(collisionForA);
                    }
                }
            }

            // Call on Body B's components
            if (dataB)
            {
                Collision collisionForB = CreateCollisionData(event.pair, *event.data, false);

                // Call on Rigidbody if present
                if (dataB->rigidbody)
                {
                    dataB->rigidbody->OnCollisionEnter(collisionForB);
                }

                // Call on all Colliders
                for (ICollider *collider : dataB->colliders)
                {
                    if (collider)
                    {
                        collider->OnCollisionEnter(collisionForB);
                    }
                }
            }

            // Clean up
            delete event.data;
        }
        _newCollisions.clear();

        // ===== OnCollisionStay =====
        for (const auto &pair : _activeCollisions)
        {
            BodyData *dataA = GetBodyData(pair.bodyA);
            BodyData *dataB = GetBodyData(pair.bodyB);

            // Create minimal collision data for Stay events
            Collision baseData;

            if (dataA)
            {
                Collision collisionForA = CreateCollisionData(pair, baseData, true);

                if (dataA->rigidbody)
                {
                    dataA->rigidbody->OnCollisionStay(collisionForA);
                }

                for (ICollider *collider : dataA->colliders)
                {
                    if (collider)
                    {
                        collider->OnCollisionStay(collisionForA);
                    }
                }
            }

            if (dataB)
            {
                Collision collisionForB = CreateCollisionData(pair, baseData, false);

                if (dataB->rigidbody)
                {
                    dataB->rigidbody->OnCollisionStay(collisionForB);
                }

                for (ICollider *collider : dataB->colliders)
                {
                    if (collider)
                    {
                        collider->OnCollisionStay(collisionForB);
                    }
                }
            }
        }

        // ===== OnCollisionExit =====
        for (const auto &event : _endedCollisions)
        {
            BodyData *dataA = GetBodyData(event.pair.bodyA);
            BodyData *dataB = GetBodyData(event.pair.bodyB);

            if (dataA)
            {
                Collision collisionForA = CreateCollisionData(event.pair, *event.data, true);

                if (dataA->rigidbody)
                {
                    dataA->rigidbody->OnCollisionExit(collisionForA);
                }

                for (ICollider *collider : dataA->colliders)
                {
                    if (collider)
                    {
                        collider->OnCollisionExit(collisionForA);
                    }
                }
            }

            if (dataB)
            {
                Collision collisionForB = CreateCollisionData(event.pair, *event.data, false);

                if (dataB->rigidbody)
                {
                    dataB->rigidbody->OnCollisionExit(collisionForB);
                }

                for (ICollider *collider : dataB->colliders)
                {
                    if (collider)
                    {
                        collider->OnCollisionExit(collisionForB);
                    }
                }
            }

            delete event.data;
        }
        _endedCollisions.clear();

        // ===== OnTriggerEnter =====
        for (const auto &event : _newTriggers)
        {
            BodyData *dataA = GetBodyData(event.pair.bodyA);
            BodyData *dataB = GetBodyData(event.pair.bodyB);

            if (dataA)
            {
                Trigger triggerForA = CreateTriggerData(event.pair, true);

                if (dataA->rigidbody)
                {
                    dataA->rigidbody->OnTriggerEnter(triggerForA);
                }

                for (ICollider *collider : dataA->colliders)
                {
                    if (collider)
                    {
                        collider->OnTriggerEnter(triggerForA);
                    }
                }
            }

            if (dataB)
            {
                Trigger triggerForB = CreateTriggerData(event.pair, false);

                if (dataB->rigidbody)
                {
                    dataB->rigidbody->OnTriggerEnter(triggerForB);
                }

                for (ICollider *collider : dataB->colliders)
                {
                    if (collider)
                    {
                        collider->OnTriggerEnter(triggerForB);
                    }
                }
            }
        }
        _newTriggers.clear();

        // ===== OnTriggerExit =====
        for (const auto &event : _endedTriggers)
        {
            BodyData *dataA = GetBodyData(event.pair.bodyA);
            BodyData *dataB = GetBodyData(event.pair.bodyB);

            if (dataA)
            {
                Trigger triggerForA = CreateTriggerData(event.pair, true);

                if (dataA->rigidbody)
                {
                    dataA->rigidbody->OnTriggerExit(triggerForA);
                }

                for (ICollider *collider : dataA->colliders)
                {
                    if (collider)
                    {
                        collider->OnTriggerExit(triggerForA);
                    }
                }
            }

            if (dataB)
            {
                Trigger triggerForB = CreateTriggerData(event.pair, false);

                if (dataB->rigidbody)
                {
                    dataB->rigidbody->OnTriggerExit(triggerForB);
                }

                for (ICollider *collider : dataB->colliders)
                {
                    if (collider)
                    {
                        collider->OnTriggerExit(triggerForB);
                    }
                }
            }
        }
        _endedTriggers.clear();
    }

    // Stub implementations for unused callbacks
    void PhysXBackend::onConstraintBreak(PxConstraintInfo *constraints, PxU32 count) {}
    void PhysXBackend::onWake(PxActor **actors, PxU32 count) {}
    void PhysXBackend::onSleep(PxActor **actors, PxU32 count) {}
    void PhysXBackend::onAdvance(const PxRigidBody *const *bodyBuffer, const PxTransform *poseBuffer, const PxU32 count) {}

#else

    // ===== Stub Implementation When PhysX is Not Available =====

    PhysXBackend::~PhysXBackend() {}
    bool PhysXBackend::Initialize() { return false; }
    void PhysXBackend::Update(float) {}
    void PhysXBackend::Shutdown() {}
    void PhysXBackend::ApplyPendingChanges() {}
    void PhysXBackend::SyncTransforms() {}
    void PhysXBackend::ProcessCollisionCallbacks() {}

    PhysicsBodyHandle PhysXBackend::CreateDynamicBody(const Math::Vector3 &, const Math::Quaternion &, float, Rigidbody *)
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
    void PhysXBackend::AddSphereCollider(PhysicsBodyHandle, float, const Math::Vector3 &, const PhysicsMaterial &) {}
    void PhysXBackend::AddBoxCollider(PhysicsBodyHandle, const Math::Vector3 &, const Math::Vector3 &, const PhysicsMaterial &) {}
    void PhysXBackend::AddCapsuleCollider(PhysicsBodyHandle, float, float, const Math::Vector3 &, const PhysicsMaterial &) {}
    void PhysXBackend::SetIsTrigger(PhysicsBodyHandle, bool) {}
    void PhysXBackend::AddForce(PhysicsBodyHandle, const Math::Vector3 &) {}
    void PhysXBackend::AddImpulse(PhysicsBodyHandle, const Math::Vector3 &) {}
    void PhysXBackend::SetVelocity(PhysicsBodyHandle, const Math::Vector3 &) {}
    void PhysXBackend::SetAngularVelocity(PhysicsBodyHandle, const Math::Vector3 &) {}
    Math::Vector3 PhysXBackend::GetPosition(PhysicsBodyHandle) { return Math::Vector3::Zero(); }
    Math::Quaternion PhysXBackend::GetRotation(PhysicsBodyHandle) { return Math::Quaternion::Identity(); }
    Math::Vector3 PhysXBackend::GetVelocity(PhysicsBodyHandle) { return Math::Vector3::Zero(); }
    Math::Vector3 PhysXBackend::GetAngularVelocity(PhysicsBodyHandle) { return Math::Vector3::Zero(); }
    void PhysXBackend::SetMass(PhysicsBodyHandle, float) {}
    float PhysXBackend::GetMass(PhysicsBodyHandle) { return 0.0f; }
    void PhysXBackend::SetGravityEnabled(PhysicsBodyHandle, bool) {}
    void PhysXBackend::SetGravity(const Math::Vector3 &) {}
    Math::Vector3 PhysXBackend::GetGravity() const { return Math::Vector3(0.0f, -9.81f, 0.0f); }

#endif

}