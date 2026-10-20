#pragma once

// The scenario shared by the Jolt smoke test (JoltSmokeTests.cpp) and the web size probe (cmake/jolt/probe): it uses
// the vendored Jolt library alone, no engine type. Everything here is plain float math through Jolt's own code, a
// single-threaded job system and fixed inputs, so the state hash is a pure function of the library build. Jolt claims
// that, built with JPH_CROSS_PLATFORM_DETERMINISTIC, the hash is the same on every platform: kGoldenStateHash is the
// value every CI leg must print.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#include <Jolt/Jolt.h>

#include <Jolt/ConfigurationString.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

// The build must be the one the engine ships: these are static checks of the definitions Jolt::Jolt carries
// (cmake/jolt/CMakeLists.txt), so a target that links the library cannot compile with different ones
#ifndef JPH_CROSS_PLATFORM_DETERMINISTIC
#error "Jolt must be built with JPH_CROSS_PLATFORM_DETERMINISTIC (cmake/jolt/CMakeLists.txt sets it, always)"
#endif
#ifdef JPH_DOUBLE_PRECISION
#error "Jolt must be built in single precision"
#endif
#ifdef JPH_USE_FMADD
#error "Jolt must be built without fused multiply-add"
#endif
#ifdef JPH_DEBUG_RENDERER
#error "Jolt must be built without the debug renderer"
#endif
#if defined(JPH_PROFILE_ENABLED) || defined(JPH_EXTERNAL_PROFILE)
#error "Jolt must be built without the profiler"
#endif

namespace n2jolt
{

// The state hash of RunDeterminismScenario() every leg must produce. The value is the one the first green run printed
// on all legs; it changes only in a change that says why (a Jolt upgrade, a different scenario).
inline constexpr std::uint64_t kGoldenStateHash = 0x0000000000000000ULL;

inline constexpr int kDeterminismSteps = 480;
inline constexpr int kRestSteps = 240;
inline constexpr float kStep = 1.0f / 60.0f;

namespace Layers
{
inline constexpr JPH::ObjectLayer NonMoving = 0;
inline constexpr JPH::ObjectLayer Moving = 1;
inline constexpr JPH::uint Count = 2;
} // namespace Layers

namespace BroadPhaseLayers
{
inline constexpr JPH::BroadPhaseLayer NonMoving(0);
inline constexpr JPH::BroadPhaseLayer Moving(1);
inline constexpr JPH::uint Count = 2;
} // namespace BroadPhaseLayers

class LayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        // Static bodies only collide with moving ones; moving bodies collide with everything
        if (a == Layers::NonMoving)
            return b == Layers::Moving;
        return true;
    }
};

class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface
{
public:
    BroadPhaseLayerMap()
    {
        mObjectToBroadPhase[Layers::NonMoving] = BroadPhaseLayers::NonMoving;
        mObjectToBroadPhase[Layers::Moving] = BroadPhaseLayers::Moving;
    }

    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return mObjectToBroadPhase[layer]; }

private:
    JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::Count];
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override
    {
        if (layer == Layers::NonMoving)
            return broadPhaseLayer == BroadPhaseLayers::Moving;
        return true;
    }
};

// Jolt's global state (allocator, type factory, registered types) for the lifetime of the object
class Runtime
{
public:
    Runtime()
    {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    ~Runtime()
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
};

// FNV-1a over the bytes of 32-bit words: no std::hash, no library call whose result may differ between platforms
inline void HashWord(std::uint64_t& hash, std::uint32_t word)
{
    for (int i = 0; i < 4; ++i)
    {
        hash ^= static_cast<std::uint64_t>((word >> (8 * i)) & 0xFFu);
        hash *= 1099511628211ULL;
    }
}

inline void HashFloat(std::uint64_t& hash, float value)
{
    std::uint32_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    HashWord(hash, word);
}

// A physics system with a floor (a static box whose top face is the plane y = 0) and room for a few dozen bodies,
// stepped on the calling thread with a fixed time step
class World
{
public:
    World()
        : mTempAllocator(4 * 1024 * 1024)
        , mJobSystem(JPH::cMaxPhysicsJobs)
    {
        mSystem.Init(64, 0, 256, 256, mBroadPhaseLayers, mObjectVsBroadPhase, mLayerPairs);
        JPH::BodyInterface& bodies = mSystem.GetBodyInterface();
        JPH::BodyCreationSettings floorSettings(new JPH::BoxShape(JPH::Vec3(50.0f, 1.0f, 50.0f)), JPH::RVec3(0.0f, -1.0f, 0.0f),
                                        JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NonMoving);
        mFloor = bodies.CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);
    }

    ~World()
    {
        JPH::BodyInterface& bodies = mSystem.GetBodyInterface();
        for (const JPH::BodyID& id : mDynamic)
        {
            bodies.RemoveBody(id);
            bodies.DestroyBody(id);
        }
        bodies.RemoveBody(mFloor);
        bodies.DestroyBody(mFloor);
    }

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    JPH::BodyID AddBox(JPH::Vec3 halfExtent, JPH::RVec3 position, JPH::Quat rotation)
    {
        JPH::BodyCreationSettings settings(new JPH::BoxShape(halfExtent), position, rotation, JPH::EMotionType::Dynamic,
                                           Layers::Moving);
        return Add(settings);
    }

    JPH::BodyID AddSphere(float radius, JPH::RVec3 position, JPH::Vec3 velocity)
    {
        JPH::BodyCreationSettings settings(new JPH::SphereShape(radius), position, JPH::Quat::sIdentity(),
                                           JPH::EMotionType::Dynamic, Layers::Moving);
        const JPH::BodyID id = Add(settings);
        mSystem.GetBodyInterface().SetLinearVelocity(id, velocity);
        return id;
    }

    // One step of the fixed time step; false when Jolt reports that one of its buffers was too small
    bool Step()
    {
        const JPH::EPhysicsUpdateError error = mSystem.Update(kStep, 1, &mTempAllocator, &mJobSystem);
        return error == JPH::EPhysicsUpdateError::None;
    }

    JPH::BodyInterface& Bodies() { return mSystem.GetBodyInterface(); }

    // The state of every dynamic body in creation order: position, rotation, linear and angular velocity, as the bits of
    // the floats. A body that went to sleep reports zero velocities, which is part of the state too.
    std::uint64_t HashState()
    {
        std::uint64_t hash = 14695981039346656037ULL;
        JPH::BodyInterface& bodies = mSystem.GetBodyInterface();
        for (const JPH::BodyID& id : mDynamic)
        {
            const JPH::RVec3 position = bodies.GetCenterOfMassPosition(id);
            const JPH::Quat rotation = bodies.GetRotation(id);
            const JPH::Vec3 linear = bodies.GetLinearVelocity(id);
            const JPH::Vec3 angular = bodies.GetAngularVelocity(id);
            HashFloat(hash, static_cast<float>(position.GetX()));
            HashFloat(hash, static_cast<float>(position.GetY()));
            HashFloat(hash, static_cast<float>(position.GetZ()));
            HashFloat(hash, rotation.GetX());
            HashFloat(hash, rotation.GetY());
            HashFloat(hash, rotation.GetZ());
            HashFloat(hash, rotation.GetW());
            HashFloat(hash, linear.GetX());
            HashFloat(hash, linear.GetY());
            HashFloat(hash, linear.GetZ());
            HashFloat(hash, angular.GetX());
            HashFloat(hash, angular.GetY());
            HashFloat(hash, angular.GetZ());
        }
        return hash;
    }

    // True when every number of every dynamic body is finite
    bool AllFinite()
    {
        JPH::BodyInterface& bodies = mSystem.GetBodyInterface();
        for (const JPH::BodyID& id : mDynamic)
        {
            const JPH::RVec3 position = bodies.GetCenterOfMassPosition(id);
            const JPH::Vec3 linear = bodies.GetLinearVelocity(id);
            if (!std::isfinite(static_cast<float>(position.GetX())) || !std::isfinite(static_cast<float>(position.GetY())) ||
                !std::isfinite(static_cast<float>(position.GetZ())) || !std::isfinite(linear.GetX()) ||
                !std::isfinite(linear.GetY()) || !std::isfinite(linear.GetZ()))
                return false;
        }
        return true;
    }

    std::size_t DynamicCount() const { return mDynamic.size(); }

private:
    JPH::BodyID Add(const JPH::BodyCreationSettings& settings)
    {
        const JPH::BodyID id = mSystem.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
        mDynamic.push_back(id);
        return id;
    }

    BroadPhaseLayerMap mBroadPhaseLayers;
    ObjectVsBroadPhaseFilter mObjectVsBroadPhase;
    LayerPairFilter mLayerPairs;
    JPH::TempAllocatorImpl mTempAllocator;
    JPH::JobSystemSingleThreaded mJobSystem;
    JPH::PhysicsSystem mSystem;
    JPH::BodyID mFloor;
    std::vector<JPH::BodyID> mDynamic;
};

struct RestResult
{
    bool stepsOk = false;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float speed = 0.0f;
};

// One box dropped from 5 m onto the floor: where it stops
inline RestResult RunRestScenario()
{
    Runtime runtime;
    World world;
    const JPH::BodyID box = world.AddBox(JPH::Vec3(0.5f, 0.5f, 0.5f), JPH::RVec3(0.0f, 5.0f, 0.0f), JPH::Quat::sIdentity());
    RestResult result;
    result.stepsOk = true;
    for (int i = 0; i < kRestSteps; ++i)
        result.stepsOk = world.Step() && result.stepsOk;
    const JPH::RVec3 position = world.Bodies().GetCenterOfMassPosition(box);
    result.x = static_cast<float>(position.GetX());
    result.y = static_cast<float>(position.GetY());
    result.z = static_cast<float>(position.GetZ());
    result.speed = world.Bodies().GetLinearVelocity(box).Length();
    return result;
}

struct DeterminismResult
{
    bool stepsOk = false;
    bool finite = false;
    std::size_t bodies = 0;
    std::uint64_t hashAfter60 = 0;
    std::uint64_t hashAfter240 = 0;
    std::uint64_t hash = 0;
};

// Six bodies that touch each other and the floor: three boxes in a leaning stack, a box dropped in a turned pose
// (the rotation comes from Jolt's own Quat::sRotation, so its sin and cos are in the result), a sphere that rolls
// away and a sphere that lands on the tumbling box. kDeterminismSteps steps of 1/60 s.
inline DeterminismResult RunDeterminismScenario()
{
    Runtime runtime;
    World world;
    for (int i = 0; i < 3; ++i)
    {
        const float fi = static_cast<float>(i);
        world.AddBox(JPH::Vec3(0.5f, 0.5f, 0.5f), JPH::RVec3(-3.0f + 0.05f * fi, 0.5f + 1.02f * fi, 0.0f),
                     JPH::Quat::sIdentity());
    }
    world.AddBox(JPH::Vec3(0.6f, 0.4f, 0.5f), JPH::RVec3(3.0f, 2.0f, 0.0f),
                 JPH::Quat::sRotation(JPH::Vec3(1.0f, 2.0f, 3.0f).Normalized(), 0.6f));
    world.AddSphere(0.5f, JPH::RVec3(0.0f, 3.0f, 0.0f), JPH::Vec3(2.0f, 0.0f, 1.0f));
    world.AddSphere(0.4f, JPH::RVec3(3.2f, 5.0f, 0.1f), JPH::Vec3(0.0f, 0.0f, 0.0f));

    DeterminismResult result;
    result.stepsOk = true;
    result.bodies = world.DynamicCount();
    for (int step = 1; step <= kDeterminismSteps; ++step)
    {
        result.stepsOk = world.Step() && result.stepsOk;
        if (step == 60)
            result.hashAfter60 = world.HashState();
        else if (step == 240)
            result.hashAfter240 = world.HashState();
    }
    result.finite = world.AllFinite();
    result.hash = world.HashState();
    return result;
}

} // namespace n2jolt
