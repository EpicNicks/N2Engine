#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/GameObject.hpp"
#include "engine/Component.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/Logger.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/Layers.hpp"

#ifdef N2ENGINE_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#include <extensions/PxDefaultAllocator.h>
#include <extensions/PxDefaultErrorCallback.h>
#include <extensions/PxDefaultCpuDispatcher.h>
#include <extensions/PxDefaultSimulationFilterShader.h>
#include <extensions/PxRigidBodyExt.h>

#include <format>
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <utility>
#include <vector>

using namespace physx;
#endif

namespace N2Engine::Physics
{
#ifdef N2ENGINE_PHYSX_ENABLED

    namespace
    {
        // A shape's layer as PhysX filter data. Query word0 is its layer bit, which PhysX's fixed-function
        // query filter ANDs with a query's layer mask. Simulation word0 is the same bit and word1 the
        // layers it collides with (its row of the Layers collision matrix).
        PxFilterData LayerQueryData(const int layer)
        {
            return PxFilterData(Layers::MaskOf(layer), 0, 0, 0);
        }

        PxFilterData LayerSimulationData(const int layer)
        {
            return PxFilterData(Layers::MaskOf(layer), Layers::CollisionMaskFor(layer), 0, 0);
        }

        // Each side has to accept the other's layer
        bool LayersCollide(const PxFilterData &a, const PxFilterData &b)
        {
            return (a.word0 & b.word1) != 0 && (b.word0 & a.word1) != 0;
        }

        int LayerOf(const ICollider *collider)
        {
            return collider ? collider->GetGameObject().GetLayer() : Layers::Default;
        }

        // PxDefaultSimulationFilterShader only asks for touch notifications on trigger pairs, so
        // onContact never ran for solid contacts and OnCollisionEnter/Stay/Exit never fired.
        // Reads nothing but its arguments: PhysX may call it from worker threads.
        PxFilterFlags CollisionEventFilterShader(
            PxFilterObjectAttributes attributes0, PxFilterData filterData0,
            PxFilterObjectAttributes attributes1, PxFilterData filterData1,
            PxPairFlags &pairFlags, const void *, PxU32)
        {
            // Before the trigger branch, so the matrix applies to triggers too (as in Unity). Suppressed
            // rather than killed: a change of filter data or resetFiltering asks again.
            if (!LayersCollide(filterData0, filterData1))
            {
                return PxFilterFlag::eSUPPRESS;
            }

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

        // Rigidbody::SetMass clamps, but a deserialized mass reaches the backend unchecked; zero would
        // make a dynamic body immovable and a negative or non-finite one is invalid in PhysX
        float SanitizedMass(const float mass)
        {
            if (mass > 0.0f && std::isfinite(mass))
            {
                return mass;
            }

            Logger::Warn(std::format("Invalid Rigidbody mass {}, using 1", mass));
            return 1.0f;
        }
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

        // Shapes re-filtered since the last step have their reports settled once this step is done
        _refiltering.swap(_pendingRefilter);
        _pendingRefilter.clear();
        // A deferred "lost" is settled by this step's reports for its shapes, so hold those back too
        for (const RefilterReport &deferred : _deferredLost)
        {
            _refiltering.insert(deferred.onA);
            _refiltering.insert(deferred.onB);
        }

        _scene->simulate(deltaTime);
        _scene->fetchResults(true);

        if (!_refiltering.empty())
        {
            ReconcileRefilteredPairs();
            _refiltering.clear();
        }
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

        for (const auto& event : _newCollisions)
        {
            delete event.data;
        }
        _newCollisions.clear();

        for (const auto& event : _endedCollisions)
        {
            delete event.data;
        }
        _endedCollisions.clear();

        for (const auto& event : _forgottenCollisions)
        {
            delete event.data;
        }
        _forgottenCollisions.clear();

        for (const auto& report : _refilterReports)
        {
            delete report.data;
        }
        _refilterReports.clear();
        for (const auto& deferred : _deferredLost)
        {
            delete deferred.data;
        }
        _deferredLost.clear();
        _pendingRefilter.clear();
        _refiltering.clear();

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

        const PhysicsBodyHandle handle = AllocateHandle();
        BodyData *data = GetBodyData(handle);
        data->actor = body;
        data->rigidbody = rigidbody;
        data->mass = SanitizedMass(mass);

        body->userData = new PhysicsBodyHandle(handle);

        // No shapes yet, so this gives the fallback; colliders attaching later derive the real inertia
        // and centre of mass
        UpdateMassProperties(handle);

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

            // Its shapes are freed with it; a later shape at the same address mustn't look re-filtered
            for (ICollider *collider : data->colliders)
            {
                if (const auto shapes = _colliderShapes.find(collider); shapes != _colliderShapes.end())
                {
                    for (const PxShape *shape : shapes->second)
                    {
                        _pendingRefilter.erase(shape);
                        DropDeferred(shape);
                    }
                }
            }

            data->actor->release();
            data->actor = nullptr;
        }

        // Releasing the actor released its shapes; forget them so no collider keeps a freed PxShape*
        for (ICollider *collider : data->colliders)
        {
            _colliderShapes.erase(collider);
        }
        ForgetPairsWithBody(handle, true);

        data->active = false;
        data->rigidbody = nullptr;
        data->colliders.clear();
        _freeList.push_back(handle.index);
    }

    void PhysXBackend::SetBodyEnabled(const PhysicsBodyHandle handle, const bool enabled)
    {
        const BodyData *data = GetBodyData(handle);
        if (!data || !data->actor || !_scene)
        {
            return;
        }

        if (const bool inScene = data->actor->getScene() != nullptr; inScene == enabled)
        {
            return;
        }

        if (!enabled)
        {
            // Out of the scene it neither simulates nor shows up in queries. PhysX reports its lost
            // touches as removed-actor pairs, which the callbacks skip, so end them here.
            ForgetPairsWithBody(handle, false);
            _scene->removeActor(*data->actor);
            return;
        }

        // Back where its GameObject is now: it may have been moved while it was out
        if (const GameObject *owner = GetBodyOwner(handle))
        {
            if (const Positionable *positionable = owner->GetPositionable())
            {
                const Math::Vector3 position = positionable->GetPosition();
                const Math::Quaternion rotation = positionable->GetRotation();
                data->actor->setGlobalPose(PxTransform(
                    PxVec3(position.x, position.y, position.z),
                    PxQuat(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW())));
            }
        }
        _scene->addActor(*data->actor);
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
            // (RemoveColliderShapes also drops those shapes from the touch lists)
            RemoveColliderShapes(handle, collider);
            std::erase(data->colliders, collider);
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
        // A kinematic target needs a scene; a disabled body takes its pose when it's re-enabled
        if (!actor || !actor->getScene())
        {
            return;
        }
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
        // The layer of the collider's own GameObject, also for a collider on a child object
        const int layer = LayerOf(collider);
        shape->setQueryFilterData(LayerQueryData(layer));
        shape->setSimulationFilterData(LayerSimulationData(layer));
        shape->userData = collider; // lets contact and trigger events report which collider touched

        bodyData->actor->attachShape(*shape);

        // Recorded under the collider that asked for it (it used to be filed under whichever
        // collider registered last, so a second collider's shapes were attributed to the wrong one)
        if (collider)
        {
            _colliderShapes[collider].push_back(shape);
        }

        // The actor holds the only reference from here; detachShape frees the shape
        shape->release();

        UpdateMassProperties(body);
    }

    void PhysXBackend::UpdateMassProperties(const PhysicsBodyHandle body)
    {
        const BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
        {
            return;
        }

        auto *dynamic = data->actor->is<PxRigidDynamic>();
        if (!dynamic)
        {
            return; // static actors have no mass properties
        }

        // The Rigidbody's mass is authoritative (like Unity); the shapes, at uniform density, only give
        // the inertia tensor and centre of mass. Trigger shapes have no eSIMULATION_SHAPE flag and are
        // skipped (includeNonSimShapes = false), so a sensor doesn't make its body heavier or lopsided.
        bool hasSimulationShape = false;
        for (PxU32 i = 0, count = dynamic->getNbShapes(); i < count && !hasSimulationShape; ++i)
        {
            PxShape *shape = nullptr;
            dynamic->getShapes(&shape, 1, i);
            hasSimulationShape = shape && shape->getFlags().isSet(PxShapeFlag::eSIMULATION_SHAPE);
        }

        if (hasSimulationShape && PxRigidBodyExt::setMassAndUpdateInertia(*dynamic, data->mass, nullptr, false))
        {
            return;
        }

        // Nothing to derive them from. PhysX would keep the mass but use inertia (1,1,1) whatever the
        // mass; instead centre it on the actor and give it the inertia of a unit radius of gyration,
        // so it scales with the mass like a shaped body's does
        dynamic->setMass(data->mass);
        dynamic->setCMassLocalPose(PxTransform(PxIdentity));
        dynamic->setMassSpaceInertiaTensor(PxVec3(data->mass));
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

        // Triggers don't contribute mass, so toggling one moves the body's centre of mass and inertia
        UpdateMassProperties(body);
    }

    // ========== Layers ==========

    void PhysXBackend::SetColliderLayer(const PhysicsBodyHandle body, ICollider *collider, const int layer)
    {
        const BodyData *data = GetBodyData(body);
        const auto it = _colliderShapes.find(collider);
        if (!data || !data->actor || it == _colliderShapes.end() || it->second.empty())
        {
            return; // no shapes yet (or disabled): they take the object's layer when they're attached
        }

        for (PxShape *shape : it->second)
        {
            shape->setQueryFilterData(LayerQueryData(layer));
            shape->setSimulationFilterData(LayerSimulationData(layer));
        }
        RefilterShapes(*data->actor, it->second);
    }

    void PhysXBackend::RefreshCollisionMatrix()
    {
        // Only shapes whose row actually changed, grouped by actor for resetFiltering
        std::unordered_map<PxRigidActor *, std::vector<PxShape *>> changed;
        for (const auto &shapes : _colliderShapes | std::views::values)
        {
            for (PxShape *shape : shapes)
            {
                PxFilterData filter = shape->getSimulationFilterData();
                // word0 is the layer's bit, so its index is the layer
                const uint32_t collidesWith = filter.word0 != 0 ? Layers::CollisionMaskFor(std::countr_zero(filter.word0)) : 0u;
                if (filter.word1 == collidesWith)
                {
                    continue;
                }
                filter.word1 = collidesWith;
                shape->setSimulationFilterData(filter);
                if (PxRigidActor *actor = shape->getActor())
                {
                    changed[actor].push_back(shape);
                }
            }
        }

        for (auto &[actor, shapes] : changed)
        {
            RefilterShapes(*actor, shapes);
        }
    }

    void PhysXBackend::RefilterShapes(PxRigidActor &actor, const std::vector<PxShape *> &shapes)
    {
        // resetFiltering is invalid for an actor outside the scene (a disabled body); it's filtered
        // afresh, with the new data, when it's added back
        if (!_scene || actor.getScene() != _scene || shapes.empty())
        {
            return;
        }

        // Existing pairs keep their old filtering until this: PhysX reports them lost, then found again
        // if the new data still allows them (see ReconcileRefilteredPairs)
        _scene->resetFiltering(actor, shapes.data(), static_cast<PxU32>(shapes.size()));
        _pendingRefilter.insert(shapes.begin(), shapes.end());

        // resetFiltering wakes only this actor. A sleeping partner (e.g. a ball resting on a floor whose
        // layer changed) would let PhysX re-find the pair only once something woke it, so wake it now.
        const auto isReset = [&shapes](const PxShape *shape) { return std::ranges::find(shapes, shape) != shapes.end(); };
        for (const TouchMap *touches : {&_collisionTouches, &_triggerTouches})
        {
            for (const auto &shapePairs : *touches | std::views::values)
            {
                for (const ShapePair &touching : shapePairs)
                {
                    if (isReset(touching.onA))
                    {
                        WakeIfAsleep(touching.onB);
                    }
                    else if (isReset(touching.onB))
                    {
                        WakeIfAsleep(touching.onA);
                    }
                }
            }
        }
    }

    void PhysXBackend::WakeIfAsleep(const PxShape *shape)
    {
        PxRigidActor *actor = shape ? shape->getActor() : nullptr;
        PxRigidDynamic *dynamic = actor ? actor->is<PxRigidDynamic>() : nullptr;
        // wakeUp is invalid for kinematic actors and actors outside a scene
        if (dynamic && dynamic->getScene() && !dynamic->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC) &&
            dynamic->isSleeping())
        {
            dynamic->wakeUp();
        }
    }

    void PhysXBackend::DropDeferred(const PxShape *shape)
    {
        std::erase_if(_deferredLost, [shape](const RefilterReport &deferred)
        {
            if (deferred.onA != shape && deferred.onB != shape)
            {
                return false;
            }
            delete deferred.data;
            return true;
        });
    }

    bool PhysXBackend::IsRefiltering(const PxShape *a, const PxShape *b) const
    {
        return !_refiltering.empty() && (_refiltering.contains(a) || _refiltering.contains(b));
    }

    void PhysXBackend::ReconcileRefilteredPairs()
    {
        std::vector<RefilterReport> reports;
        reports.swap(_refilterReports);

        // Each shape pair ends up as its last report says: a pair reported lost and found again is
        // still touching, so it gets neither Exit nor Enter
        using ShapeKey = std::pair<const PxShape *, const PxShape *>;
        const auto key = [](const RefilterReport &report) -> ShapeKey
        {
            return std::less<>{}(report.onA, report.onB) ? ShapeKey{report.onA, report.onB}
                                                         : ShapeKey{report.onB, report.onA};
        };
        std::map<ShapeKey, size_t> last;
        for (size_t i = 0; i < reports.size(); ++i)
        {
            last[key(reports[i])] = i;
        }

        // "Lost" reports held over from the last step, waiting for a "found"
        std::vector<RefilterReport> previouslyDeferred;
        previouslyDeferred.swap(_deferredLost);
        std::set<ShapeKey> wasDeferred;
        for (const RefilterReport &deferred : previouslyDeferred)
        {
            wasDeferred.insert(key(deferred));
        }

        const auto isTouching = [](const TouchMap &touches, const RefilterReport &report)
        {
            const auto it = touches.find(report.pair);
            if (it == touches.end())
            {
                return false;
            }
            const bool flipped = it->first.bodyA != report.pair.bodyA;
            const PxShape *onA = flipped ? report.onB : report.onA;
            const PxShape *onB = flipped ? report.onA : report.onB;
            return std::ranges::any_of(it->second, [&](const ShapePair &p) { return p.onA == onA && p.onB == onB; });
        };

        for (size_t i = 0; i < reports.size(); ++i)
        {
            RefilterReport &report = reports[i];
            if (last[key(report)] != i)
            {
                delete report.data;
                continue;
            }

            TouchMap &touches = report.trigger ? _triggerTouches : _collisionTouches;

            // Lost, but the new filter data still allows the pair: PhysX may report it found again only
            // in the next step (e.g. a partner that was asleep). Wait a step before calling it an Exit.
            if (!report.found && !wasDeferred.contains(key(report)) && isTouching(touches, report) &&
                LayersCollide(report.onA->getSimulationFilterData(), report.onB->getSimulationFilterData()))
            {
                WakeIfAsleep(report.onA);
                WakeIfAsleep(report.onB);
                _deferredLost.push_back(report); // takes report.data
                continue;
            }

            const bool changed = report.found
                                     ? AddTouch(touches, report.pair, report.onA, report.onB)
                                     : RemoveTouch(touches, report.pair, report.onA, report.onB);
            if (!changed)
            {
                delete report.data; // no change for the body pair (still touching, or other shapes are)
                continue;
            }

            if (report.trigger)
            {
                const TriggerEvent event{report.pair, ColliderOf(report.onA), ColliderOf(report.onB)};
                (report.found ? _newTriggers : _endedTriggers).push_back(event);
            }
            else
            {
                (report.found ? _newCollisions : _endedCollisions).push_back({report.pair, report.data});
            }
        }

        // A deferred "lost" with no report for its pair in a whole further step was a real end: Exit now.
        // One with a report was settled above ("found": still touching; "lost" again: ended there).
        for (RefilterReport &deferred : previouslyDeferred)
        {
            TouchMap &touches = deferred.trigger ? _triggerTouches : _collisionTouches;
            if (last.contains(key(deferred)) || !RemoveTouch(touches, deferred.pair, deferred.onA, deferred.onB))
            {
                delete deferred.data;
                continue;
            }
            if (deferred.trigger)
            {
                _endedTriggers.push_back({deferred.pair, ColliderOf(deferred.onA), ColliderOf(deferred.onB)});
            }
            else
            {
                _endedCollisions.push_back({deferred.pair, deferred.data});
            }
        }

        // In case PhysX didn't report a touch it no longer allows as lost: end it here, so a pair the
        // matrix now rules out always gets its Exit
        const auto ruledOut = [this](const ShapePair &shapes)
        {
            return (_refiltering.contains(shapes.onA) || _refiltering.contains(shapes.onB)) &&
                   !LayersCollide(shapes.onA->getSimulationFilterData(), shapes.onB->getSimulationFilterData());
        };
        for (auto it = _collisionTouches.begin(); it != _collisionTouches.end();)
        {
            const auto removed = std::ranges::find_if(it->second, ruledOut);
            if (removed == it->second.end())
            {
                ++it;
                continue;
            }
            const ShapePair lastPair = *removed;
            std::erase_if(it->second, ruledOut);
            if (!it->second.empty())
            {
                ++it;
                continue;
            }
            auto *data = new Collision();
            data->collider = ColliderOf(lastPair.onA);
            data->otherCollider = ColliderOf(lastPair.onB);
            _endedCollisions.push_back({it->first, data});
            it = _collisionTouches.erase(it);
        }
        for (auto it = _triggerTouches.begin(); it != _triggerTouches.end();)
        {
            const auto removed = std::ranges::find_if(it->second, ruledOut);
            if (removed == it->second.end())
            {
                ++it;
                continue;
            }
            const ShapePair lastPair = *removed;
            std::erase_if(it->second, ruledOut);
            if (!it->second.empty())
            {
                ++it;
                continue;
            }
            _endedTriggers.push_back({it->first, ColliderOf(lastPair.onA), ColliderOf(lastPair.onB)});
            it = _triggerTouches.erase(it);
        }
    }

    // ========== Forces and Motion ==========

    void PhysXBackend::AddForce(const PhysicsBodyHandle body, const Math::Vector3 &force)
    {
        const BodyData *data = GetBodyData(body);
        if (!data || !data->actor->getScene()) // PhysX rejects forces on a body outside a scene (disabled)
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
        if (!data || !data->actor->getScene())
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
        BodyData *data = GetBodyData(body);
        if (!data)
        {
            return;
        }

        data->mass = SanitizedMass(mass);
        UpdateMassProperties(body);
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

    Math::Vector3 PhysXBackend::GetInertiaTensor(const PhysicsBodyHandle body) const
    {
        const BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
        {
            return Math::Vector3::Zero;
        }

        if (const PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            const PxVec3 inertia = dynamic->getMassSpaceInertiaTensor();
            return {inertia.x, inertia.y, inertia.z};
        }

        return Math::Vector3::Zero;
    }

    Math::Vector3 PhysXBackend::GetCenterOfMass(const PhysicsBodyHandle body) const
    {
        const BodyData *data = GetBodyData(body);
        if (!data || !data->actor)
        {
            return Math::Vector3::Zero;
        }

        if (const PxRigidDynamic *dynamic = data->actor->is<PxRigidDynamic>())
        {
            const PxVec3 center = dynamic->getCMassLocalPose().p;
            return {center.x, center.y, center.z};
        }

        return Math::Vector3::Zero;
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
            // A disabled body (out of the scene) didn't move; its object may be moved by script meanwhile
            if (!bodyData.active || !bodyData.actor || !bodyData.rigidbody || !bodyData.actor->getScene())
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

            // A removed shape's pointer is invalid; it was already dropped from the touch lists
            if (cp.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 | PxContactPairFlag::eREMOVED_SHAPE_1))
            {
                continue;
            }

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

            baseCollisionData->collider = ColliderOf(cp.shapes[0]);      // actors[0] is pair.bodyA
            baseCollisionData->otherCollider = ColliderOf(cp.shapes[1]);

            const bool found = cp.events.isSet(PxPairFlag::eNOTIFY_TOUCH_FOUND);
            if ((found || cp.events.isSet(PxPairFlag::eNOTIFY_TOUCH_LOST)) && IsRefiltering(cp.shapes[0], cp.shapes[1]))
            {
                _refilterReports.push_back({pair, cp.shapes[0], cp.shapes[1], found, false, baseCollisionData});
                continue; // settled after the step
            }

            // Enter on the body pair's first touching shape pair, Exit on its last
            if ((cp.events & PxPairFlag::eNOTIFY_TOUCH_FOUND) &&
                AddTouch(_collisionTouches, pair, cp.shapes[0], cp.shapes[1]))
            {
                _newCollisions.push_back({pair, baseCollisionData});
            }
            else if ((cp.events & PxPairFlag::eNOTIFY_TOUCH_LOST) &&
                     RemoveTouch(_collisionTouches, pair, cp.shapes[0], cp.shapes[1]))
            {
                _endedCollisions.push_back({pair, baseCollisionData});
            }
            else
            {
                delete baseCollisionData; // another shape pair of these bodies started/stopped touching
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
            const PxShape *triggerShape = pairs[i].triggerShape;
            const PxShape *otherShape = pairs[i].otherShape;
            const TriggerEvent event{pair, ColliderOf(triggerShape), ColliderOf(otherShape)};

            if (IsRefiltering(triggerShape, otherShape))
            {
                const bool found = static_cast<bool>(pairs[i].status & PxPairFlag::eNOTIFY_TOUCH_FOUND);
                _refilterReports.push_back({pair, triggerShape, otherShape, found, true, nullptr});
                continue; // settled after the step
            }

            if ((pairs[i].status & PxPairFlag::eNOTIFY_TOUCH_FOUND) &&
                AddTouch(_triggerTouches, pair, triggerShape, otherShape))
            {
                _newTriggers.push_back(event);
            }
            else if ((pairs[i].status & PxPairFlag::eNOTIFY_TOUCH_LOST) &&
                     RemoveTouch(_triggerTouches, pair, triggerShape, otherShape))
            {
                _endedTriggers.push_back(event);
            }
        }
    }

    void PhysXBackend::DescribeSide(const PhysicsBodyHandle handle, const std::weak_ptr<GameObject> &fallbackOwner,
                                    GameObject *&gameObject, Rigidbody *&rigidbody) const
    {
        if (const BodyData *data = GetBodyData(handle))
        {
            if (data->rigidbody)
            {
                gameObject = &data->rigidbody->GetGameObject();
                rigidbody = data->rigidbody;
            }
            else if (!data->colliders.empty())
            {
                gameObject = &data->colliders[0]->GetGameObject();
                rigidbody = nullptr;
            }
            return;
        }

        // The body is gone (destroyed or re-created); the object it belonged to may still be around
        const auto owner = fallbackOwner.lock();
        gameObject = owner && !owner->IsDestroyed() ? owner.get() : nullptr;
        rigidbody = nullptr;
    }

    // Helper to create collision data with resolved object references
    Collision PhysXBackend::CreateCollisionData(
        const CollisionPair &pair,
        const Collision &baseData,
        bool isForBodyA,
        const PairOwners &owners)
    {
        Collision collision = baseData;

        DescribeSide(isForBodyA ? pair.bodyA : pair.bodyB, isForBodyA ? owners.a : owners.b,
                     collision.gameObject, collision.rigidbody);
        DescribeSide(isForBodyA ? pair.bodyB : pair.bodyA, isForBodyA ? owners.b : owners.a,
                     collision.otherGameObject, collision.otherRigidbody);

        // baseData's colliders are A's and B's; validate them, then swap for B's view
        collision.collider = LiveCollider(pair.bodyA, baseData.collider);
        collision.otherCollider = LiveCollider(pair.bodyB, baseData.otherCollider);

        // Everything directional is from A's point of view; B sees it the other way round
        if (!isForBodyA)
        {
            std::swap(collision.collider, collision.otherCollider);
            for (auto &contact : collision.contacts)
            {
                contact.normal = contact.normal * -1.0f;
            }
            collision.impulse = collision.impulse * -1.0f;
            collision.relativeVelocity = collision.relativeVelocity * -1.0f;
        }

        return collision;
    }

    Trigger PhysXBackend::CreateTriggerData(const CollisionPair &pair, ICollider *colliderA, ICollider *colliderB,
                                            bool isForBodyA, const PairOwners &owners)
    {
        Trigger trigger;
        trigger.collider = LiveCollider(isForBodyA ? pair.bodyA : pair.bodyB, isForBodyA ? colliderA : colliderB);
        trigger.otherCollider = LiveCollider(isForBodyA ? pair.bodyB : pair.bodyA, isForBodyA ? colliderB : colliderA);

        DescribeSide(isForBodyA ? pair.bodyA : pair.bodyB, isForBodyA ? owners.a : owners.b,
                     trigger.gameObject, trigger.rigidbody);
        DescribeSide(isForBodyA ? pair.bodyB : pair.bodyA, isForBodyA ? owners.b : owners.a,
                     trigger.otherGameObject, trigger.otherRigidbody);

        return trigger;
    }

    PhysXBackend::PairOwners PhysXBackend::OwnersOf(const CollisionPair &pair) const
    {
        const auto ownerOf = [this](const PhysicsBodyHandle handle) -> std::weak_ptr<GameObject>
        {
            GameObject *owner = GetBodyOwner(handle);
            return owner ? owner->weak_from_this() : std::weak_ptr<GameObject>{};
        };
        return {ownerOf(pair.bodyA), ownerOf(pair.bodyB)};
    }

    GameObject *PhysXBackend::GetBodyOwner(const PhysicsBodyHandle handle) const
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

    void PhysXBackend::DispatchToBody(const PhysicsBodyHandle handle, const std::function<void(Component &)> &fn,
                                      const std::weak_ptr<GameObject> &fallbackOwner)
    {
        GameObject *owner = GetBodyOwner(handle);
        const bool viaBody = owner != nullptr;
        std::shared_ptr<GameObject> keepAlive;
        if (!viaBody)
        {
            // The body is gone. Its GameObject still hears about a forgotten pair unless it's being destroyed.
            keepAlive = fallbackOwner.lock();
            if (!keepAlive || keepAlive->IsDestroyed())
            {
                return; // destroyed this frame, or never belonged to a GameObject
            }
            owner = keepAlive.get();
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
            if (viaBody ? GetBodyOwner(handle) != owner : owner->IsDestroyed())
            {
                return;
            }
            const auto &live = owner->GetAllComponents();
            if (std::ranges::none_of(live, [component](const auto &c) { return c.get() == component; }))
            {
                continue;
            }
            // Like Unity: an inactive object gets nothing (its body has left the simulation, and it got
            // OnDisable instead), but a disabled component on an active object still does, so a script
            // that disables itself keeps a balanced Enter/Exit count
            if (component->IsDestroyed() || !owner->IsActiveInHierarchy())
            {
                continue;
            }
            fn(*component);
        }
    }

    bool PhysXBackend::AddTouch(TouchMap &touches, const CollisionPair &pair,
                                const PxShape *shapeOnA, const PxShape *shapeOnB)
    {
        auto [it, inserted] = touches.try_emplace(pair);
        // The key may have been stored the other way round; keep shapes oriented to the key
        if (it->first.bodyA != pair.bodyA)
        {
            std::swap(shapeOnA, shapeOnB);
        }
        auto &shapes = it->second;
        if (std::ranges::none_of(shapes, [&](const ShapePair &p) { return p.onA == shapeOnA && p.onB == shapeOnB; }))
        {
            shapes.push_back({shapeOnA, shapeOnB});
        }
        return shapes.size() == 1 && inserted;
    }

    bool PhysXBackend::RemoveTouch(TouchMap &touches, const CollisionPair &pair,
                                   const PxShape *shapeOnA, const PxShape *shapeOnB)
    {
        const auto it = touches.find(pair);
        if (it == touches.end())
        {
            return false; // already forgotten (body destroyed or shape removed)
        }
        if (it->first.bodyA != pair.bodyA)
        {
            std::swap(shapeOnA, shapeOnB);
        }
        std::erase_if(it->second, [&](const ShapePair &p) { return p.onA == shapeOnA && p.onB == shapeOnB; });
        if (!it->second.empty())
        {
            return false; // other shapes of these bodies are still touching
        }
        touches.erase(it);
        return true;
    }

    void PhysXBackend::ForgetShape(const PxShape *shape)
    {
        // A body pair whose last touching shape pair goes ends now: PhysX's touch-lost for the removed
        // shape is skipped, so the other body would otherwise never get Exit, and a re-created shape
        // would fire a second Enter. Both shapes are still alive here (released after this).
        _pendingRefilter.erase(shape);
        DropDeferred(shape);
        const auto involves = [shape](const ShapePair &p) { return p.onA == shape || p.onB == shape; };
        for (auto it = _collisionTouches.begin(); it != _collisionTouches.end();)
        {
            const auto removed = std::ranges::find_if(it->second, involves);
            if (removed == it->second.end())
            {
                ++it;
                continue;
            }
            const ShapePair last = *removed;
            std::erase_if(it->second, involves);
            if (!it->second.empty())
            {
                ++it;
                continue;
            }
            auto *data = new Collision();
            data->collider = ColliderOf(last.onA); // LiveCollider nulls the removed one at dispatch
            data->otherCollider = ColliderOf(last.onB);
            // Owners too: a collider-only object's body is destroyed right after its shapes are removed
            _forgottenCollisions.push_back({it->first, data, OwnersOf(it->first)});
            it = _collisionTouches.erase(it);
        }
        for (auto it = _triggerTouches.begin(); it != _triggerTouches.end();)
        {
            const auto removed = std::ranges::find_if(it->second, involves);
            if (removed == it->second.end())
            {
                ++it;
                continue;
            }
            const ShapePair last = *removed;
            std::erase_if(it->second, involves);
            if (!it->second.empty())
            {
                ++it;
                continue;
            }
            _forgottenTriggers.push_back({it->first, ColliderOf(last.onA), ColliderOf(last.onB),
                                          OwnersOf(it->first)});
            it = _triggerTouches.erase(it);
        }
    }

    ICollider *PhysXBackend::ColliderOf(const PxShape *shape)
    {
        return shape ? static_cast<ICollider *>(shape->userData) : nullptr;
    }

    ICollider *PhysXBackend::LiveCollider(const PhysicsBodyHandle body, ICollider *collider) const
    {
        const BodyData *data = GetBodyData(body);
        return data && collider && std::ranges::find(data->colliders, collider) != data->colliders.end()
                   ? collider
                   : nullptr;
    }

    void PhysXBackend::ForgetPairsWithBody(const PhysicsBodyHandle handle, const bool shapesReleased)
    {
        const auto involves = [handle](const CollisionPair &pair)
        {
            return pair.bodyA == handle || pair.bodyB == handle;
        };
        // Both sides get their Exit: PhysX's touch-lost for a released or removed actor is skipped by the
        // callbacks, so nothing else would report it. If this body is being destroyed, its side is reached
        // through its recorded GameObject (when that survives, e.g. the body is being re-created), and its
        // shapes are already released, so only the other side's collider is looked up.
        const auto colliderOn = [handle, shapesReleased](const CollisionPair &pair, const ShapePair &shapes,
                                                         const bool sideA)
        {
            const bool thisIsA = pair.bodyA == handle;
            return !shapesReleased || sideA != thisIsA ? ColliderOf(sideA ? shapes.onA : shapes.onB) : nullptr;
        };
        for (auto it = _collisionTouches.begin(); it != _collisionTouches.end();)
        {
            if (!involves(it->first) || it->second.empty())
            {
                it = involves(it->first) ? _collisionTouches.erase(it) : std::next(it);
                continue;
            }
            auto *data = new Collision();
            data->collider = colliderOn(it->first, it->second.front(), true);
            data->otherCollider = colliderOn(it->first, it->second.front(), false);
            _forgottenCollisions.push_back({it->first, data, OwnersOf(it->first)});
            it = _collisionTouches.erase(it);
        }
        for (auto it = _triggerTouches.begin(); it != _triggerTouches.end();)
        {
            if (!involves(it->first) || it->second.empty())
            {
                it = involves(it->first) ? _triggerTouches.erase(it) : std::next(it);
                continue;
            }
            _forgottenTriggers.push_back({it->first, colliderOn(it->first, it->second.front(), true),
                                          colliderOn(it->first, it->second.front(), false),
                                          OwnersOf(it->first)});
            it = _triggerTouches.erase(it);
        }
    }

    void PhysXBackend::ProcessCollisionCallbacks()
    {
        // Every event goes to *each* body's own GameObject (A's components get A's view, B's get B's).
        // This used to send both to B's first collider, which could be null or empty.
        // Take the event lists first so handlers can't disturb what's being iterated.

        // ===== Exits for pairs ended by removing a body or collider =====
        // First, so a collider re-created in the same step reports Exit(old) before Enter(new)
        DispatchCollisionExits(_forgottenCollisions);
        DispatchTriggerExits(_forgottenTriggers);

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
        std::vector<CollisionPair> activeCollisions;
        for (const auto &key : _collisionTouches | std::views::keys)
        {
            activeCollisions.push_back(key);
        }
        for (const auto &pair : activeCollisions)
        {
            const auto touching = _collisionTouches.find(pair);
            if (touching == _collisionTouches.end() || touching->second.empty())
            {
                continue; // a handler destroyed one of the bodies or removed the touching collider
            }
            Collision baseData{};
            baseData.collider = ColliderOf(touching->second.front().onA);
            baseData.otherCollider = ColliderOf(touching->second.front().onB);
            const Collision forA = CreateCollisionData(pair, baseData, true);
            DispatchToBody(pair.bodyA, [&forA](Component &c) { c.OnCollisionStay(forA); });
            const Collision forB = CreateCollisionData(pair, baseData, false);
            DispatchToBody(pair.bodyB, [&forB](Component &c) { c.OnCollisionStay(forB); });
        }

        // ===== OnCollisionExit =====
        DispatchCollisionExits(_endedCollisions);

        // ===== OnTriggerEnter =====
        std::vector<TriggerEvent> newTriggers;
        newTriggers.swap(_newTriggers);
        for (const auto &event : newTriggers)
        {
            const Trigger forA = CreateTriggerData(event.pair, event.colliderA, event.colliderB, true);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnTriggerEnter(forA); });
            const Trigger forB = CreateTriggerData(event.pair, event.colliderA, event.colliderB, false);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnTriggerEnter(forB); });
        }

        // ===== OnTriggerStay =====
        std::vector<CollisionPair> activeTriggers;
        for (const auto &key : _triggerTouches | std::views::keys)
        {
            activeTriggers.push_back(key);
        }
        for (const auto &pair : activeTriggers)
        {
            const auto touching = _triggerTouches.find(pair);
            if (touching == _triggerTouches.end() || touching->second.empty())
            {
                continue;
            }
            ICollider *colliderA = ColliderOf(touching->second.front().onA);
            ICollider *colliderB = ColliderOf(touching->second.front().onB);
            const Trigger forA = CreateTriggerData(pair, colliderA, colliderB, true);
            DispatchToBody(pair.bodyA, [&forA](Component &c) { c.OnTriggerStay(forA); });
            const Trigger forB = CreateTriggerData(pair, colliderA, colliderB, false);
            DispatchToBody(pair.bodyB, [&forB](Component &c) { c.OnTriggerStay(forB); });
        }

        // ===== OnTriggerExit =====
        DispatchTriggerExits(_endedTriggers);
    }

    void PhysXBackend::DispatchCollisionExits(std::vector<CollisionEvent> &queue)
    {
        std::vector<CollisionEvent> events;
        events.swap(queue);
        for (const auto &event : events)
        {
            const Collision forA = CreateCollisionData(event.pair, *event.data, true, event.owners);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnCollisionExit(forA); }, event.owners.a);
            const Collision forB = CreateCollisionData(event.pair, *event.data, false, event.owners);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnCollisionExit(forB); }, event.owners.b);
            delete event.data;
        }
    }

    void PhysXBackend::DispatchTriggerExits(std::vector<TriggerEvent> &queue)
    {
        std::vector<TriggerEvent> events;
        events.swap(queue);
        for (const auto &event : events)
        {
            const Trigger forA = CreateTriggerData(event.pair, event.colliderA, event.colliderB, true, event.owners);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnTriggerExit(forA); }, event.owners.a);
            const Trigger forB = CreateTriggerData(event.pair, event.colliderA, event.colliderB, false, event.owners);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnTriggerExit(forB); }, event.owners.b);
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
            ForgetShape(shape);
            bodyData->actor->detachShape(*shape);
        }

        it->second.clear();
        UpdateMassProperties(body);
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

        UpdateMassProperties(body); // a new size or offset moves the inertia and centre of mass
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

        UpdateMassProperties(body);
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

        UpdateMassProperties(body);
    }

    void PhysXBackend::FillRaycastHit(RaycastHit &hit, const PxLocationHit &location,
                                      const PxActorShape &actorShape) const
    {
        hit = RaycastHit{}; // a reused hit must not keep the previous hit's body, rigidbody or collider
        hit.hit = true;
        hit.point = Math::Vector3(location.position.x, location.position.y, location.position.z);
        hit.normal = Math::Vector3(location.normal.x, location.normal.y, location.normal.z);
        hit.distance = location.distance;

        if (actorShape.actor && actorShape.actor->userData)
        {
            const auto *handle = static_cast<PhysicsBodyHandle*>(actorShape.actor->userData);
            hit.bodyHandle = *handle;
            DescribeSide(*handle, {}, hit.gameObject, hit.rigidbody);
            // The collider whose shape was hit (not just the body's first), as the contact events report it
            hit.collider = LiveCollider(*handle, ColliderOf(actorShape.shape));
        }
    }

    bool PhysXBackend::Raycast(
        const Math::Vector3 &origin,
        const Math::Vector3 &direction,
        RaycastHit &hit,
        float maxDistance,
        uint32_t layerMask)
    {
        hit = RaycastHit{};
        // An all-zero query filter turns PhysX's layer test off (it would hit everything); no layers, no hit
        if (!_scene || layerMask == 0)
        {
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
            FillRaycastHit(hit, hitBuffer.block, hitBuffer.block);
            return true;
        }

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

        if (!_scene || layerMask == 0) // as in Raycast
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
                FillRaycastHit(hit, raycastBuffer.touches[i], raycastBuffer.touches[i]);
                hits.push_back(hit);
            }

            if (raycastBuffer.hasBlock)
            {
                RaycastHit hit;
                FillRaycastHit(hit, raycastBuffer.block, raycastBuffer.block);
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
        hit = RaycastHit{};
        if (!_scene || layerMask == 0) // as in Raycast
        {
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
            FillRaycastHit(hit, hitBuffer.block, hitBuffer.block);
            return true;
        }

        return false;
    }

    // Stub implementations for unused callbacks
    void PhysXBackend::onConstraintBreak(PxConstraintInfo *constraints, PxU32 count) {}
    void PhysXBackend::onWake(PxActor **actors, PxU32 count) {}
    void PhysXBackend::onSleep(PxActor **actors, PxU32 count) {}

    void PhysXBackend::onAdvance(const PxRigidBody *const *bodyBuffer, const PxTransform *poseBuffer,
                                 const PxU32 count) {}

#endif
}
