#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "FakeGpuCompute.hpp"
#include "FakePhysicsBackend.hpp"
#include "engine/particles/CpuParticleBackend.hpp"
#include "engine/particles/ParticleBackendSelection.hpp"
#include "engine/particles/ParticleBillboards.hpp"
#include "engine/particles/ParticleEmitter.hpp"
#include "engine/particles/ParticleRandom.hpp"
#include "engine/particles/WebGpuParticleBackend.hpp"

// The CPU particle simulation, the emitter that feeds it, backend selection and the billboard mesh builder: all
// plain data in and out, no window, renderer or scene.

using namespace N2Engine;
using namespace N2Engine::Particles;
using Physics::ParticleParams;
using Physics::ParticleSystemDesc;
using Physics::ParticleSystemHandle;

namespace
{
    constexpr float kStep = 0.02f;

    ParticleSystemHandle Make(CpuParticleBackend &backend, const std::uint32_t maxParticles, const std::uint32_t seed,
                              const ParticleParams &params = {})
    {
        ParticleSystemDesc desc;
        desc.maxParticles = maxParticles;
        desc.seed = seed;
        desc.params = params;
        return backend.CreateSystem(desc);
    }

    /// The same emitter-and-backend loop a ParticleSystem runs, for `steps` fixed steps
    std::vector<Math::Vector4> RunScenario(const std::uint32_t seed, const int steps)
    {
        CpuParticleBackend backend;
        ParticleParams params;
        params.lifetimeMin = 0.5f;
        params.lifetimeMax = 1.5f;
        params.drag = 0.1f;
        const ParticleSystemHandle handle = Make(backend, 400, seed, params);

        ParticleEmitter emitter(seed);
        EmitterShapeSettings shape;
        shape.shapeRadius = 0.5f;
        shape.coneAngle = 40.0f;
        std::vector<Math::Vector3> positions;
        std::vector<Math::Vector3> velocities;
        for (int i = 0; i < steps; ++i)
        {
            backend.Step(kStep);
            const std::uint32_t due = emitter.Advance(150.0f, kStep);
            emitter.Generate(shape, Math::Vector3(1.0f, 2.0f, 3.0f), Math::Quaternion(), due, positions, velocities);
            backend.Spawn(handle, positions, velocities);
        }
        std::vector<Math::Vector4> out;
        backend.CopyPositions(handle, out);
        return out;
    }

    bool SameParticles(const std::vector<Math::Vector4> &a, const std::vector<Math::Vector4> &b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z || a[i].w != b[i].w)
            {
                return false;
            }
        }
        return true;
    }
}

// ---------------------------------------------------------------- ParticleRandom

TEST(ParticleRandomTest, TheSameSeedGivesTheSameSequence)
{
    ParticleRandom a(42);
    ParticleRandom b(42);
    ParticleRandom c(43);
    bool differs = false;
    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t va = a.NextU32();
        EXPECT_EQ(va, b.NextU32());
        differs = differs || va != c.NextU32();
    }
    EXPECT_TRUE(differs);
}

TEST(ParticleRandomTest, FloatsAreInTheHalfOpenUnitRangeAndRangeIsInclusiveOfItsBounds)
{
    ParticleRandom random(7);
    for (int i = 0; i < 2000; ++i)
    {
        const float f = random.NextFloat();
        EXPECT_GE(f, 0.0f);
        EXPECT_LT(f, 1.0f);
        const float r = random.Range(2.0f, 5.0f);
        EXPECT_GE(r, 2.0f);
        EXPECT_LE(r, 5.0f);
    }
    EXPECT_FLOAT_EQ(random.Range(3.0f, 3.0f), 3.0f);
    EXPECT_FLOAT_EQ(random.Range(3.0f, 1.0f), 3.0f) << "an inverted range gives its lower end";
}

// ---------------------------------------------------------------- CpuParticleBackend

TEST(CpuParticleBackendTest, IsNamedCpu)
{
    const CpuParticleBackend backend;
    EXPECT_EQ(backend.GetName(), "CPU");
}

TEST(CpuParticleBackendTest, TheSameSeedAndCallsGiveTheSameParticles)
{
    const std::vector<Math::Vector4> first = RunScenario(11, 120);
    const std::vector<Math::Vector4> second = RunScenario(11, 120);
    ASSERT_FALSE(first.empty());
    EXPECT_TRUE(SameParticles(first, second));

    const std::vector<Math::Vector4> other = RunScenario(12, 120);
    EXPECT_FALSE(SameParticles(first, other)) << "another seed gives other particles";
}

TEST(CpuParticleBackendTest, SystemsInOneBackendDoNotAffectEachOther)
{
    CpuParticleBackend backend;
    const ParticleSystemHandle a = Make(backend, 10, 1);
    const ParticleSystemHandle b = Make(backend, 10, 2);
    ASSERT_NE(a, Physics::kInvalidParticleSystem);
    ASSERT_NE(b, Physics::kInvalidParticleSystem);
    EXPECT_NE(a, b);

    const std::vector<Math::Vector3> positions(3, Math::Vector3(0.0f, 5.0f, 0.0f));
    const std::vector<Math::Vector3> velocities(3);
    EXPECT_EQ(backend.Spawn(a, positions, velocities), 3u);
    EXPECT_EQ(backend.GetAliveCount(a), 3u);
    EXPECT_EQ(backend.GetAliveCount(b), 0u);

    backend.Destroy(a);
    EXPECT_EQ(backend.GetAliveCount(a), 0u);
    EXPECT_EQ(backend.GetSystemCount(), 1u);
}

TEST(CpuParticleBackendTest, NeverHoldsMoreThanMaxParticlesAndReportsWhatItAccepted)
{
    CpuParticleBackend backend;
    const ParticleSystemHandle handle = Make(backend, 10, 1);
    const std::vector<Math::Vector3> positions(25);
    const std::vector<Math::Vector3> velocities(25);

    EXPECT_EQ(backend.Spawn(handle, positions, velocities), 10u) << "room for 10 of the 25";
    EXPECT_EQ(backend.GetAliveCount(handle), 10u);
    EXPECT_EQ(backend.Spawn(handle, positions, velocities), 0u) << "full";
    EXPECT_EQ(backend.GetAliveCount(handle), 10u);

    // Fewer velocities than positions: only the pairs
    backend.Clear(handle);
    const std::vector<Math::Vector3> fewer(4);
    EXPECT_EQ(backend.Spawn(handle, positions, fewer), 4u);
}

TEST(CpuParticleBackendTest, AMaxParticlesOfZeroAcceptsNothing)
{
    CpuParticleBackend backend;
    const ParticleSystemHandle handle = Make(backend, 0, 1);
    const std::vector<Math::Vector3> one(1);
    EXPECT_EQ(backend.Spawn(handle, one, one), 0u);
}

TEST(CpuParticleBackendTest, AHugeMaxParticlesIsClampedNotRefused)
{
    CpuParticleBackend backend;
    const ParticleSystemHandle handle = Make(backend, 0xFFFFFFFFu, 1);
    EXPECT_NE(handle, Physics::kInvalidParticleSystem);
    const std::vector<Math::Vector3> some(100);
    EXPECT_EQ(backend.Spawn(handle, some, some), 100u);
}

TEST(CpuParticleBackendTest, ParticlesDieAtTheirLifetimeAndTheirAgeFractionRises)
{
    CpuParticleBackend backend;
    ParticleParams params;
    params.gravity = Math::Vector3();
    params.lifetimeMin = 0.1f;
    params.lifetimeMax = 0.1f;
    const ParticleSystemHandle handle = Make(backend, 10, 1, params);
    const std::vector<Math::Vector3> positions(5);
    const std::vector<Math::Vector3> velocities(5);
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 5u);

    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 5u);
    EXPECT_FLOAT_EQ(out[0].w, 0.0f) << "born at age 0";

    backend.Step(kStep);
    backend.Step(kStep);
    backend.Step(kStep);
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 5u);
    EXPECT_NEAR(out[0].w, 0.6f, 0.01f);

    for (int i = 0; i < 4; ++i)
    {
        backend.Step(kStep);
    }
    EXPECT_EQ(backend.GetAliveCount(handle), 0u);
    backend.CopyPositions(handle, out);
    EXPECT_TRUE(out.empty());
}

TEST(CpuParticleBackendTest, GravityPullsSemiImplicitly)
{
    CpuParticleBackend backend;
    ParticleParams params;
    params.gravity = Math::Vector3(0.0f, -10.0f, 0.0f);
    params.lifetimeMin = 100.0f;
    params.lifetimeMax = 100.0f;
    const ParticleSystemHandle handle = Make(backend, 4, 1, params);
    const std::vector<Math::Vector3> positions{Math::Vector3(1.0f, 0.0f, 0.0f)};
    const std::vector<Math::Vector3> velocities{Math::Vector3(2.0f, 0.0f, 0.0f)};
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 1u);

    for (int i = 0; i < 10; ++i)
    {
        backend.Step(kStep);
    }
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 1u);
    // v.y after n steps is -10 n dt and the position moves by v dt each step: y = -10 dt^2 (1 + 2 + ... + n)
    EXPECT_NEAR(out[0].y, -10.0f * kStep * kStep * 55.0f, 1e-4f);
    EXPECT_NEAR(out[0].x, 1.0f + 2.0f * kStep * 10.0f, 1e-4f);
    EXPECT_NEAR(out[0].z, 0.0f, 1e-6f);
}

TEST(CpuParticleBackendTest, DragSlowsParticlesDown)
{
    CpuParticleBackend backend;
    ParticleParams params;
    params.gravity = Math::Vector3();
    params.drag = 1.0f;
    params.lifetimeMin = 100.0f;
    params.lifetimeMax = 100.0f;
    const ParticleSystemHandle handle = Make(backend, 4, 1, params);
    const std::vector<Math::Vector3> positions{Math::Vector3()};
    const std::vector<Math::Vector3> velocities{Math::Vector3(10.0f, 0.0f, 0.0f)};
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 1u);
    for (int i = 0; i < 50; ++i)
    {
        backend.Step(kStep);
    }
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_LT(out[0].x, 9.0f) << "slower than the 10 units a second it started at";
    EXPECT_GT(out[0].x, 3.0f);
}

namespace
{
    /// A wall at x = 1 facing -x
    class Wall final : public Physics::IParticleCollider
    {
    public:
        bool Cast(const Math::Vector3 &from, const Math::Vector3 &direction, const float distance,
                  Physics::ParticleHit &hit) override
        {
            ++casts;
            if (!(direction.x > 0.0f) || from.x >= 1.0f)
            {
                return false;
            }
            const float along = (1.0f - from.x) / direction.x;
            if (along > distance)
            {
                return false;
            }
            hit.point = Math::Vector3(1.0f, from.y + direction.y * along, from.z + direction.z * along);
            hit.normal = Math::Vector3(-1.0f, 0.0f, 0.0f);
            return true;
        }
        int casts = 0;
    };

    /// One particle at the origin moving at (10, 5, 0) with no gravity into the wall, after `steps` steps
    struct IntoTheWall
    {
        Wall wall;
        CpuParticleBackend backend;
        ParticleSystemHandle handle{};
        std::vector<Math::Vector4> positions;
        std::vector<Math::Vector4> motion;

        explicit IntoTheWall(const float radius, const float bounce, const float dampen, const float loss,
                             const int steps = 10)
        {
            ParticleParams params;
            params.gravity = Math::Vector3();
            params.lifetimeMin = 100.0f;
            params.lifetimeMax = 100.0f;
            params.bounce = bounce;
            params.collider = &wall;
            params.collisionRadius = radius;
            params.collisionDampen = dampen;
            params.collisionLifetimeLoss = loss;
            handle = Make(backend, 4, 1, params);
            backend.Spawn(handle, std::vector<Math::Vector3>{Math::Vector3()},
                          std::vector<Math::Vector3>{Math::Vector3(10.0f, 5.0f, 0.0f)});
            for (int i = 0; i < steps; ++i)
            {
                backend.Step(kStep);
            }
            backend.CopyPositions(handle, positions);
            backend.CopyMotion(handle, motion);
        }
    };
}

TEST(CpuParticleBackendTest, AParticleBouncesOffACollider)
{
    const IntoTheWall run(0.0f, 0.5f, 0.0f, 0.0f);
    ASSERT_EQ(run.positions.size(), 1u);
    EXPECT_LT(run.positions[0].x, 1.0f) << "never through the wall";
    EXPECT_NEAR(run.motion[0].x, -5.0f, 1e-4f) << "half the speed into it, turned back";
    EXPECT_NEAR(run.motion[0].y, 5.0f, 1e-4f) << "the speed along it kept";
    EXPECT_EQ(run.wall.casts, 10) << "one cast per particle per step";
}

TEST(CpuParticleBackendTest, TheColliderRadiusDampeningAndLifetimeLossApply)
{
    const IntoTheWall wide(0.25f, 0.0f, 0.4f, 0.0f, 6);
    ASSERT_EQ(wide.positions.size(), 1u);
    EXPECT_LT(wide.positions[0].x, 0.751f) << "stopped a radius short";
    EXPECT_NEAR(wide.motion[0].x, 0.0f, 1e-4f) << "no bounce";
    EXPECT_NEAR(wide.motion[0].y, 3.0f, 1e-4f) << "40 percent of the speed along it lost, once";

    const IntoTheWall ageing(0.0f, 1.0f, 0.0f, 0.25f);
    ASSERT_EQ(ageing.positions.size(), 1u);
    EXPECT_NEAR(ageing.positions[0].w, 0.25f + 0.002f, 1e-3f) << "a quarter of its life, and the time passed";

    const IntoTheWall deadly(0.0f, 1.0f, 0.0f, 1.0f);
    EXPECT_TRUE(deadly.positions.empty()) << "the first hit removes it";
}

TEST(CpuParticleBackendTest, BadCollisionSettingsAreClamped)
{
    const IntoTheWall run(-3.0f, 0.5f, 7.0f, -1.0f);
    ASSERT_EQ(run.positions.size(), 1u);
    EXPECT_NEAR(run.motion[0].y, 0.0f, 1e-4f) << "dampening clamped to 1";
}

TEST(CpuParticleBackendTest, TheGroundPlaneBouncesAndNothingGoesBelowIt)
{
    CpuParticleBackend backend;
    ParticleParams params;
    params.gravity = Math::Vector3(0.0f, -10.0f, 0.0f);
    params.lifetimeMin = 100.0f;
    params.lifetimeMax = 100.0f;
    params.groundEnabled = true;
    params.groundHeight = 0.0f;
    params.bounce = 0.5f;
    const ParticleSystemHandle handle = Make(backend, 4, 1, params);
    const std::vector<Math::Vector3> positions{Math::Vector3(0.0f, 1.0f, 0.0f)};
    const std::vector<Math::Vector3> velocities{Math::Vector3()};
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 1u);

    bool touched = false;
    float highestAfterTouch = 0.0f;
    std::vector<Math::Vector4> out;
    for (int i = 0; i < 300; ++i)
    {
        backend.Step(kStep);
        backend.CopyPositions(handle, out);
        ASSERT_EQ(out.size(), 1u);
        EXPECT_GE(out[0].y, 0.0f) << "step " << i;
        if (out[0].y == 0.0f)
        {
            touched = true;
        }
        if (touched)
        {
            highestAfterTouch = std::max(highestAfterTouch, out[0].y);
        }
    }
    EXPECT_TRUE(touched);
    EXPECT_LT(highestAfterTouch, 0.6f) << "a bounce of 0.5 keeps a quarter of the height, not all of it";
}

TEST(CpuParticleBackendTest, WithoutTheGroundParticlesFallThroughIt)
{
    CpuParticleBackend backend;
    ParticleParams params;
    params.gravity = Math::Vector3(0.0f, -10.0f, 0.0f);
    params.lifetimeMin = 100.0f;
    params.lifetimeMax = 100.0f;
    const ParticleSystemHandle handle = Make(backend, 4, 1, params);
    const std::vector<Math::Vector3> positions{Math::Vector3(0.0f, 1.0f, 0.0f)};
    const std::vector<Math::Vector3> velocities{Math::Vector3()};
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 1u);
    for (int i = 0; i < 100; ++i)
    {
        backend.Step(kStep);
    }
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_LT(out[0].y, 0.0f);
}

TEST(CpuParticleBackendTest, SetParamsChangesTheSimulationAndBadValuesAreCleaned)
{
    CpuParticleBackend backend;
    ParticleParams params;
    params.gravity = Math::Vector3();
    params.lifetimeMin = 100.0f;
    params.lifetimeMax = 100.0f;
    const ParticleSystemHandle handle = Make(backend, 4, 1, params);
    const std::vector<Math::Vector3> positions{Math::Vector3()};
    const std::vector<Math::Vector3> velocities{Math::Vector3()};
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 1u);

    ParticleParams bad;
    bad.gravity = Math::Vector3(std::nanf(""), std::numeric_limits<float>::infinity(), 0.0f);
    bad.lifetimeMin = -3.0f;
    bad.lifetimeMax = std::nanf("");
    bad.drag = -5.0f;
    bad.bounce = 7.0f;
    backend.SetParams(handle, bad);
    for (int i = 0; i < 10; ++i)
    {
        backend.Step(kStep);
    }
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(std::isfinite(out[0].x) && std::isfinite(out[0].y) && std::isfinite(out[0].z));

    // The bad lifetimes became the shortest one (a millisecond), so a new particle is born and gone within a step,
    // while the old one (lifetime 100 s) lives on
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 1u);
    EXPECT_EQ(backend.GetAliveCount(handle), 2u);
    backend.Step(kStep);
    EXPECT_EQ(backend.GetAliveCount(handle), 1u);
}

TEST(CpuParticleBackendTest, ClearEmptiesAndUnknownHandlesAreIgnored)
{
    CpuParticleBackend backend;
    const ParticleSystemHandle handle = Make(backend, 10, 1);
    const std::vector<Math::Vector3> some(6);
    ASSERT_EQ(backend.Spawn(handle, some, some), 6u);
    backend.Clear(handle);
    EXPECT_EQ(backend.GetAliveCount(handle), 0u);

    constexpr ParticleSystemHandle unknown = 999;
    EXPECT_EQ(backend.Spawn(unknown, some, some), 0u);
    EXPECT_EQ(backend.GetAliveCount(unknown), 0u);
    backend.Clear(unknown);
    backend.SetParams(unknown, ParticleParams{});
    backend.Destroy(unknown);
    std::vector<Math::Vector4> out(3);
    backend.CopyPositions(unknown, out);
    EXPECT_TRUE(out.empty());
}

TEST(CpuParticleBackendTest, AStepOfNoTimeOrNonsenseChangesNothing)
{
    CpuParticleBackend backend;
    ParticleParams params;
    params.lifetimeMin = 1.0f;
    params.lifetimeMax = 1.0f;
    const ParticleSystemHandle handle = Make(backend, 10, 1, params);
    const std::vector<Math::Vector3> positions{Math::Vector3(0.0f, 1.0f, 0.0f)};
    const std::vector<Math::Vector3> velocities{Math::Vector3()};
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 1u);

    backend.Step(0.0f);
    backend.Step(-1.0f);
    backend.Step(std::nanf(""));
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].y, 1.0f);
    EXPECT_FLOAT_EQ(out[0].w, 0.0f);
}

TEST(CpuParticleBackendTest, CopyPositionsKeepsTheCallersCapacity)
{
    CpuParticleBackend backend;
    const ParticleSystemHandle handle = Make(backend, 100, 1);
    const std::vector<Math::Vector3> some(80);
    ASSERT_EQ(backend.Spawn(handle, some, some), 80u);
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 80u);
    const std::size_t capacity = out.capacity();
    backend.Clear(handle);
    ASSERT_EQ(backend.Spawn(handle, some, some), 80u);
    backend.CopyPositions(handle, out);
    EXPECT_EQ(out.capacity(), capacity) << "no reallocation once the vector has grown";
}

// ---------------------------------------------------------------- ParticleEmitter

TEST(ParticleEmitterTest, TheRateAccumulatesWithoutDrift)
{
    ParticleEmitter emitter;
    std::uint32_t total = 0;
    for (int i = 0; i < 500; ++i)
    {
        total += emitter.Advance(7.0f, kStep); // 10 seconds at 7 a second
    }
    EXPECT_NEAR(static_cast<float>(total), 70.0f, 1.0f);

    // A slow rate gives one particle every few steps, never a burst
    ParticleEmitter slow;
    std::uint32_t most = 0;
    std::uint32_t count = 0;
    for (int i = 0; i < 200; ++i)
    {
        const std::uint32_t due = slow.Advance(5.0f, kStep);
        most = std::max(most, due);
        count += due;
    }
    EXPECT_EQ(most, 1u);
    EXPECT_NEAR(static_cast<float>(count), 20.0f, 1.0f);
}

TEST(ParticleEmitterTest, NonsenseRatesAndStepsEmitNothing)
{
    ParticleEmitter emitter;
    EXPECT_EQ(emitter.Advance(0.0f, kStep), 0u);
    EXPECT_EQ(emitter.Advance(-5.0f, kStep), 0u);
    EXPECT_EQ(emitter.Advance(10.0f, 0.0f), 0u);
    EXPECT_EQ(emitter.Advance(10.0f, -1.0f), 0u);
    EXPECT_EQ(emitter.Advance(std::nanf(""), kStep), 0u);
    EXPECT_EQ(emitter.Advance(10.0f, std::numeric_limits<float>::infinity()), 0u);
    EXPECT_EQ(emitter.Advance(1e30f, 1e30f), 0u) << "an overflowing product emits nothing and doesn't poison later steps";
    EXPECT_EQ(emitter.Advance(10.0f, 0.1f), 1u);
}

TEST(ParticleEmitterTest, AHugeRateIsLimitedPerStep)
{
    ParticleEmitter emitter;
    EXPECT_EQ(emitter.Advance(1e9f, 1.0f), 4096u);
}

TEST(ParticleEmitterTest, ResetRateForgetsTheFraction)
{
    ParticleEmitter emitter;
    EXPECT_EQ(emitter.Advance(30.0f, kStep), 0u); // 0.6 so far
    emitter.ResetRate();
    EXPECT_EQ(emitter.Advance(30.0f, kStep), 0u) << "0.6 again, not 1.2";
}

TEST(ParticleEmitterTest, TheSameSeedGivesTheSameParticles)
{
    EmitterShapeSettings shape;
    shape.shapeRadius = 1.0f;
    std::vector<Math::Vector3> p1;
    std::vector<Math::Vector3> v1;
    std::vector<Math::Vector3> p2;
    std::vector<Math::Vector3> v2;
    ParticleEmitter a(5);
    ParticleEmitter b(5);
    a.Generate(shape, Math::Vector3(1.0f, 1.0f, 1.0f), Math::Quaternion(), 30, p1, v1);
    b.Generate(shape, Math::Vector3(1.0f, 1.0f, 1.0f), Math::Quaternion(), 30, p2, v2);
    ASSERT_EQ(p1.size(), 30u);
    ASSERT_EQ(v1.size(), 30u);
    for (std::size_t i = 0; i < 30; ++i)
    {
        EXPECT_EQ(p1[i].x, p2[i].x);
        EXPECT_EQ(p1[i].z, p2[i].z);
        EXPECT_EQ(v1[i].y, v2[i].y);
    }

    // Reset starts the sequence over
    a.Reset(5);
    std::vector<Math::Vector3> p3;
    std::vector<Math::Vector3> v3;
    a.Generate(shape, Math::Vector3(1.0f, 1.0f, 1.0f), Math::Quaternion(), 30, p3, v3);
    EXPECT_EQ(p3[7].x, p1[7].x);
}

TEST(ParticleEmitterTest, AStraightConeShootsAlongItsAxisAtTheChosenSpeeds)
{
    EmitterShapeSettings shape;
    shape.shape = EmitterShape::Cone;
    shape.coneAngle = 0.0f;
    shape.shapeRadius = 0.0f;
    shape.speedMin = 2.0f;
    shape.speedMax = 4.0f;
    ParticleEmitter emitter(3);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    emitter.Generate(shape, Math::Vector3(5.0f, 6.0f, 7.0f), Math::Quaternion(), 100, positions, velocities);
    ASSERT_EQ(positions.size(), 100u);
    for (std::size_t i = 0; i < 100; ++i)
    {
        EXPECT_NEAR(positions[i].x, 5.0f, 1e-5f);
        EXPECT_NEAR(positions[i].y, 6.0f, 1e-5f);
        EXPECT_NEAR(positions[i].z, 7.0f, 1e-5f);
        EXPECT_NEAR(velocities[i].x, 0.0f, 1e-5f);
        EXPECT_NEAR(velocities[i].z, 0.0f, 1e-5f);
        EXPECT_GE(velocities[i].y, 2.0f - 1e-5f);
        EXPECT_LE(velocities[i].y, 4.0f + 1e-5f);
    }
}

TEST(ParticleEmitterTest, ParticlesStayInsideTheConeAndItsBaseDisc)
{
    EmitterShapeSettings shape;
    shape.shape = EmitterShape::Cone;
    shape.coneAngle = 30.0f;
    shape.shapeRadius = 2.0f;
    shape.speedMin = 1.0f;
    shape.speedMax = 1.0f;
    ParticleEmitter emitter(9);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    emitter.Generate(shape, Math::Vector3(), Math::Quaternion(), 500, positions, velocities);
    const float cosLimit = std::cos(30.0f * std::numbers::pi_v<float> / 180.0f);
    float widest = 0.0f;
    for (std::size_t i = 0; i < 500; ++i)
    {
        const float speed = std::sqrt(velocities[i].x * velocities[i].x + velocities[i].y * velocities[i].y +
                                      velocities[i].z * velocities[i].z);
        EXPECT_NEAR(speed, 1.0f, 1e-4f);
        EXPECT_GE(velocities[i].y / speed, cosLimit - 1e-4f) << "within 30 degrees of +Y";
        EXPECT_NEAR(positions[i].y, 0.0f, 1e-5f) << "on the base plane";
        const float r = std::sqrt(positions[i].x * positions[i].x + positions[i].z * positions[i].z);
        EXPECT_LE(r, 2.0f + 1e-4f);
        widest = std::max(widest, r);
    }
    EXPECT_GT(widest, 1.5f) << "the disc is used, not just its centre";
}

TEST(ParticleEmitterTest, ASphereStartsInsideItAndMovesAwayFromItsCentre)
{
    EmitterShapeSettings shape;
    shape.shape = EmitterShape::Sphere;
    shape.shapeRadius = 3.0f;
    shape.speedMin = 2.0f;
    shape.speedMax = 2.0f;
    ParticleEmitter emitter(21);
    const Math::Vector3 origin(10.0f, 0.0f, -4.0f);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    emitter.Generate(shape, origin, Math::Quaternion(), 300, positions, velocities);
    for (std::size_t i = 0; i < 300; ++i)
    {
        const Math::Vector3 offset(positions[i].x - origin.x, positions[i].y - origin.y, positions[i].z - origin.z);
        const float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
        EXPECT_LE(distance, 3.0f + 1e-4f);
        if (distance > 0.05f)
        {
            // The velocity points the way the offset does
            const float dot = offset.x * velocities[i].x + offset.y * velocities[i].y + offset.z * velocities[i].z;
            EXPECT_GT(dot, 0.0f);
        }
    }
}

TEST(ParticleEmitterTest, TheEmittersRotationTurnsTheConeAxis)
{
    EmitterShapeSettings shape;
    shape.coneAngle = 0.0f;
    shape.speedMin = 1.0f;
    shape.speedMax = 1.0f;
    ParticleEmitter emitter(2);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    // A quarter turn about Z takes +Y to the X axis (-X for a right-handed rotation): the cone leaves along it
    const Math::Quaternion turn =
        Math::Quaternion::FromAxisAngle(Math::Vector3(0.0f, 0.0f, 1.0f), std::numbers::pi_v<float> / 2.0f);
    emitter.Generate(shape, Math::Vector3(), turn, 5, positions, velocities);
    ASSERT_EQ(velocities.size(), 5u);
    for (const Math::Vector3 &v : velocities)
    {
        EXPECT_NEAR(std::abs(v.x), 1.0f, 1e-4f);
        EXPECT_NEAR(v.y, 0.0f, 1e-4f);
    }
}

TEST(ParticleEmitterTest, GeneratingZeroOrWithBadSettingsGivesNothingOrFiniteParticles)
{
    ParticleEmitter emitter;
    std::vector<Math::Vector3> positions(4);
    std::vector<Math::Vector3> velocities(4);
    emitter.Generate(EmitterShapeSettings{}, Math::Vector3(), Math::Quaternion(), 0, positions, velocities);
    EXPECT_TRUE(positions.empty());
    EXPECT_TRUE(velocities.empty());

    EmitterShapeSettings bad;
    bad.shapeRadius = std::nanf("");
    bad.coneAngle = std::numeric_limits<float>::infinity();
    bad.speedMin = 5.0f;
    bad.speedMax = 1.0f; // inverted: the minimum
    emitter.Generate(bad, Math::Vector3(), Math::Quaternion(), 10, positions, velocities);
    ASSERT_EQ(positions.size(), 10u);
    for (std::size_t i = 0; i < 10; ++i)
    {
        EXPECT_TRUE(std::isfinite(positions[i].x) && std::isfinite(positions[i].y) && std::isfinite(positions[i].z));
        EXPECT_TRUE(std::isfinite(velocities[i].x) && std::isfinite(velocities[i].y) &&
                    std::isfinite(velocities[i].z));
    }
}

// ---------------------------------------------------------------- backend selection

TEST(ParticleBackendSelectionTest, CpuIsAlwaysTheCpuBackend)
{
    TestSupport::FakePhysicsBackend withGpu(true);
    for (Physics::IPhysicsBackend *physics : {static_cast<Physics::IPhysicsBackend *>(nullptr),
                                              static_cast<Physics::IPhysicsBackend *>(&withGpu)})
    {
        const BackendSelection selection = SelectParticleBackend(ParticleBackend::CPU, physics);
        EXPECT_EQ(selection.gpu, nullptr);
        EXPECT_FALSE(selection.fellBack);
        EXPECT_TRUE(selection.reason.empty());
    }
}

TEST(ParticleBackendSelectionTest, AutoTakesTheGpuBackendWhenThereIsOneAndTheCpuBackendQuietlyOtherwise)
{
    TestSupport::FakePhysicsBackend withGpu(true);
    const BackendSelection gpu = SelectParticleBackend(ParticleBackend::Auto, &withGpu);
    EXPECT_EQ(gpu.gpu, &withGpu.gpu);
    EXPECT_FALSE(gpu.fellBack);

    TestSupport::FakePhysicsBackend without(false, {Physics::GpuState::Disabled, "physics.gpu is off"});
    const BackendSelection cpu = SelectParticleBackend(ParticleBackend::Auto, &without);
    EXPECT_EQ(cpu.gpu, nullptr);
    EXPECT_FALSE(cpu.fellBack) << "Auto choosing the CPU is not a fallback to warn about";

    const BackendSelection none = SelectParticleBackend(ParticleBackend::Auto, nullptr);
    EXPECT_EQ(none.gpu, nullptr);
    EXPECT_FALSE(none.fellBack);
}

TEST(ParticleBackendSelectionTest, PhysXWithoutAGpuBackendFallsBackWithAReasonThatNamesTheProbe)
{
    TestSupport::FakePhysicsBackend withGpu(true);
    const BackendSelection gpu = SelectParticleBackend(ParticleBackend::PhysX, &withGpu);
    EXPECT_EQ(gpu.gpu, &withGpu.gpu);
    EXPECT_FALSE(gpu.fellBack);

    TestSupport::FakePhysicsBackend without(false, {Physics::GpuState::Unavailable, "no CUDA device"});
    const BackendSelection fallback = SelectParticleBackend(ParticleBackend::PhysX, &without);
    EXPECT_EQ(fallback.gpu, nullptr);
    EXPECT_TRUE(fallback.fellBack);
    EXPECT_NE(fallback.reason.find("no CUDA device"), std::string::npos) << fallback.reason;
    EXPECT_NE(fallback.reason.find("CPU"), std::string::npos) << fallback.reason;

    const BackendSelection noPhysics = SelectParticleBackend(ParticleBackend::PhysX, nullptr);
    EXPECT_EQ(noPhysics.gpu, nullptr);
    EXPECT_TRUE(noPhysics.fellBack);
    EXPECT_FALSE(noPhysics.reason.empty());
}

TEST(ParticleBackendSelectionTest, WebGpuTakesTheRenderersComputeWheneverItIsUsable)
{
    TestSupport::FakeGpuCompute compute;
    TestSupport::FakePhysicsBackend withGpu(true);
    // An explicit WebGPU request ignores the PhysX GPU backend and the project's physics.gpu (autoUsesCompute false)
    const BackendSelection selection = SelectParticleBackend(ParticleBackend::WebGPU, &withGpu, &compute, false);
    EXPECT_EQ(selection.compute, &compute);
    EXPECT_EQ(selection.gpu, nullptr);
    EXPECT_FALSE(selection.fellBack);
    EXPECT_TRUE(selection.reason.empty());
}

TEST(ParticleBackendSelectionTest, WebGpuWithoutComputeFallsBackToTheCpuWithAReason)
{
    TestSupport::FakeGpuCompute lost(false);
    for (Renderer::Common::IGpuCompute *compute : {static_cast<Renderer::Common::IGpuCompute *>(nullptr),
                                                   static_cast<Renderer::Common::IGpuCompute *>(&lost)})
    {
        const BackendSelection selection = SelectParticleBackend(ParticleBackend::WebGPU, nullptr, compute, true);
        EXPECT_EQ(selection.compute, nullptr);
        EXPECT_EQ(selection.gpu, nullptr);
        EXPECT_TRUE(selection.fellBack);
        EXPECT_NE(selection.reason.find("backend is WebGPU"), std::string::npos) << selection.reason;
        EXPECT_NE(selection.reason.find("CPU backend"), std::string::npos) << selection.reason;
    }
}

TEST(ParticleBackendSelectionTest, AutoPrefersPhysXThenTheRenderersComputeWhenAllowedThenTheCpu)
{
    TestSupport::FakeGpuCompute compute;
    TestSupport::FakePhysicsBackend withGpu(true);
    const BackendSelection physx = SelectParticleBackend(ParticleBackend::Auto, &withGpu, &compute, true);
    EXPECT_EQ(physx.gpu, &withGpu.gpu) << "PhysX first";
    EXPECT_EQ(physx.compute, nullptr);

    TestSupport::FakePhysicsBackend without(false, {Physics::GpuState::Disabled, "physics.gpu is off"});
    const BackendSelection webGpu = SelectParticleBackend(ParticleBackend::Auto, &without, &compute, true);
    EXPECT_EQ(webGpu.gpu, nullptr);
    EXPECT_EQ(webGpu.compute, &compute) << "then the renderer's compute";
    EXPECT_FALSE(webGpu.fellBack);

    const BackendSelection noPhysics = SelectParticleBackend(ParticleBackend::Auto, nullptr, &compute, true);
    EXPECT_EQ(noPhysics.compute, &compute) << "the browser: no physics GPU at all";

    // physics.gpu off (or physics.deterministic on): Auto stays on the CPU, quietly
    const BackendSelection notAllowed = SelectParticleBackend(ParticleBackend::Auto, &without, &compute, false);
    EXPECT_EQ(notAllowed.compute, nullptr);
    EXPECT_EQ(notAllowed.gpu, nullptr);
    EXPECT_FALSE(notAllowed.fellBack);

    TestSupport::FakeGpuCompute lost(false);
    const BackendSelection unusable = SelectParticleBackend(ParticleBackend::Auto, &without, &lost, true);
    EXPECT_EQ(unusable.compute, nullptr);
    EXPECT_FALSE(unusable.fellBack);
}

TEST(ParticleBackendSelectionTest, AutoStaysOnTheCpuOnASoftwareDeviceButWebGpuStillTakesIt)
{
    // A fallback adapter (SwiftShader in a browser) is the CPU emulating a GPU: the CPU backend is faster there
    TestSupport::FakeGpuCompute compute;
    compute.software = true;
    const BackendSelection automatic = SelectParticleBackend(ParticleBackend::Auto, nullptr, &compute, true);
    EXPECT_EQ(automatic.compute, nullptr);
    EXPECT_FALSE(automatic.fellBack) << "Auto on the CPU is never a fallback to warn about";
    const BackendSelection explicitly = SelectParticleBackend(ParticleBackend::WebGPU, nullptr, &compute, false);
    EXPECT_EQ(explicitly.compute, &compute) << "asked for by name, it runs there anyway";
}

TEST(ParticleBackendSelectionTest, PhysXAndCpuNeverTakeTheRenderersCompute)
{
    TestSupport::FakeGpuCompute compute;
    const BackendSelection cpu = SelectParticleBackend(ParticleBackend::CPU, nullptr, &compute, true);
    EXPECT_EQ(cpu.compute, nullptr);
    EXPECT_FALSE(cpu.fellBack);
    const BackendSelection physx = SelectParticleBackend(ParticleBackend::PhysX, nullptr, &compute, true);
    EXPECT_EQ(physx.compute, nullptr);
    EXPECT_TRUE(physx.fellBack) << "PhysX asked and not there: the CPU, with a reason";
}

TEST(ParticleBackendSelectionTest, AWebGpuBackendOnAComputeThatRunsNothingIsNotHealthy)
{
    // What the component checks before it uses the backend: a compute whose kernels can't be made gives a backend that
    // refuses systems, so the system starts on the CPU instead
    TestSupport::FakeGpuCompute compute;
    const auto lifetime = std::make_shared<const int>(0);
    WebGpuParticleBackend backend(&compute, lifetime);
    EXPECT_FALSE(backend.IsHealthy());
    EXPECT_EQ(backend.GetName(), "WebGPU");
    EXPECT_EQ(backend.CreateSystem(ParticleSystemDesc{}), Physics::kInvalidParticleSystem);
}

TEST(ParticleBackendSelectionTest, TheEnumSavesByName)
{
    const std::vector<std::string> options = FieldTraits<ParticleBackend>::EnumOptions();
    const std::vector<std::string> expected = {"Auto", "PhysX", "CPU", "WebGPU"};
    EXPECT_EQ(options, expected);
    EXPECT_EQ(nlohmann::json(ParticleBackend::PhysX).get<std::string>(), "PhysX");
    EXPECT_EQ(nlohmann::json("CPU").get<ParticleBackend>(), ParticleBackend::CPU);
}

TEST(ParticleUsageTest, DescribesTheLiveSystemsPerBackend)
{
    // Other tests of the program may have systems alive: compare against what is there now
    const int cpu = ParticleUsage::GetCount(false);
    const int gpu = ParticleUsage::GetCount(true);
    ParticleUsage::Add(false);
    ParticleUsage::Add(false);
    ParticleUsage::Add(true);
    EXPECT_EQ(ParticleUsage::GetCount(false), cpu + 2);
    EXPECT_EQ(ParticleUsage::GetCount(true), gpu + 1);
    const std::string text = ParticleUsage::Describe();
    EXPECT_NE(text.find("ParticleSystems:"), std::string::npos) << text;
    EXPECT_NE(text.find("CPU"), std::string::npos) << text;
    EXPECT_NE(text.find("PhysX"), std::string::npos) << text;
    ParticleUsage::Remove(false);
    ParticleUsage::Remove(false);
    ParticleUsage::Remove(true);
    EXPECT_EQ(ParticleUsage::GetCount(false), cpu);
    EXPECT_EQ(ParticleUsage::GetCount(true), gpu);
    if (cpu == 0 && gpu == 0)
    {
        EXPECT_TRUE(ParticleUsage::Describe().empty());
    }
}

TEST(ParticleUsageTest, CountsTheWebGpuBackendOnItsOwn)
{
    const int cpu = ParticleUsage::GetCount(false);
    const int gpu = ParticleUsage::GetCount(true);
    const int webGpu = ParticleUsage::GetWebGpuCount();
    ParticleUsage::AddWebGpu();
    ParticleUsage::AddWebGpu();
    EXPECT_EQ(ParticleUsage::GetWebGpuCount(), webGpu + 2);
    EXPECT_EQ(ParticleUsage::GetCount(false), cpu) << "not counted as CPU";
    EXPECT_EQ(ParticleUsage::GetCount(true), gpu) << "not counted as PhysX";
    const std::string text = ParticleUsage::Describe();
    EXPECT_NE(text.find("WebGPU"), std::string::npos) << text;
    if (cpu == 0 && gpu == 0 && webGpu == 0)
    {
        EXPECT_EQ(text, "ParticleSystems: 2 WebGPU");
        ParticleUsage::Add(false);
        EXPECT_EQ(ParticleUsage::Describe(), "ParticleSystems: 1 CPU, 2 WebGPU");
        ParticleUsage::Remove(false);
    }
    ParticleUsage::RemoveWebGpu();
    ParticleUsage::RemoveWebGpu();
    EXPECT_EQ(ParticleUsage::GetWebGpuCount(), webGpu);
}

// ---------------------------------------------------------------- billboards

TEST(ParticleBillboardsTest, BuildsOneCameraFacingQuadPerParticle)
{
    Math::Vector4 a;
    a.x = 1.0f;
    a.y = 2.0f;
    a.z = 3.0f;
    a.w = 0.0f;
    Math::Vector4 b;
    b.x = -4.0f;
    b.y = 0.0f;
    b.z = 0.0f;
    b.w = 1.0f;
    const std::vector<Math::Vector4> particles{a, b};

    BillboardStyle style;
    style.startSize = 2.0f;
    style.endSize = 4.0f;
    style.startColor = Common::Color(1.0f, 0.0f, 0.0f, 1.0f);
    style.endColor = Common::Color(0.0f, 0.0f, 1.0f, 0.0f);
    Renderer::Common::MeshData mesh;
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style);

    ASSERT_EQ(mesh.vertices.size(), 8u);
    ASSERT_EQ(mesh.indices.size(), 12u);
    const std::vector<std::uint32_t> expectedIndices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    EXPECT_EQ(mesh.indices, expectedIndices);

    // The first particle: size 2 (a half edge of 1) around (1, 2, 3), on the camera's right and up axes (+X, +Y)
    const auto &v = mesh.vertices;
    EXPECT_FLOAT_EQ(v[0].position[0], 0.0f);
    EXPECT_FLOAT_EQ(v[0].position[1], 1.0f);
    EXPECT_FLOAT_EQ(v[1].position[0], 2.0f);
    EXPECT_FLOAT_EQ(v[1].position[1], 1.0f);
    EXPECT_FLOAT_EQ(v[2].position[0], 2.0f);
    EXPECT_FLOAT_EQ(v[2].position[1], 3.0f);
    EXPECT_FLOAT_EQ(v[3].position[0], 0.0f);
    EXPECT_FLOAT_EQ(v[3].position[1], 3.0f);
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_FLOAT_EQ(v[static_cast<std::size_t>(i)].position[2], 3.0f);
        EXPECT_FLOAT_EQ(v[static_cast<std::size_t>(i)].normal[2], 1.0f) << "back at a camera looking down -Z";
        EXPECT_FLOAT_EQ(v[static_cast<std::size_t>(i)].color[0], 1.0f) << "the start colour at age 0";
        EXPECT_FLOAT_EQ(v[static_cast<std::size_t>(i)].color[3], 1.0f);
    }
    // uvs: bottom-left, bottom-right, top-right, top-left
    EXPECT_FLOAT_EQ(v[0].texCoord[0], 0.0f);
    EXPECT_FLOAT_EQ(v[0].texCoord[1], 0.0f);
    EXPECT_FLOAT_EQ(v[2].texCoord[0], 1.0f);
    EXPECT_FLOAT_EQ(v[2].texCoord[1], 1.0f);

    // The second is at the end of its life: size 4, the end colour
    EXPECT_FLOAT_EQ(v[4].position[0], -6.0f);
    EXPECT_FLOAT_EQ(v[4].position[1], -2.0f);
    EXPECT_FLOAT_EQ(v[6].position[0], -2.0f);
    EXPECT_FLOAT_EQ(v[6].position[1], 2.0f);
    EXPECT_FLOAT_EQ(v[4].color[0], 0.0f);
    EXPECT_FLOAT_EQ(v[4].color[2], 1.0f);
    EXPECT_FLOAT_EQ(v[4].color[3], 0.0f);
}

TEST(ParticleBillboardsTest, InterpolatesSizeAndColourByAge)
{
    Math::Vector4 half;
    half.w = 0.5f;
    BillboardStyle style;
    style.startSize = 2.0f;
    style.endSize = 4.0f;
    style.startColor = Common::Color(1.0f, 1.0f, 1.0f, 1.0f);
    style.endColor = Common::Color(0.0f, 0.5f, 1.0f, 0.0f);
    Renderer::Common::MeshData mesh;
    const std::vector<Math::Vector4> particles{half};
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style);
    ASSERT_EQ(mesh.vertices.size(), 4u);
    EXPECT_FLOAT_EQ(mesh.vertices[1].position[0] - mesh.vertices[0].position[0], 3.0f) << "an edge of 3";
    EXPECT_FLOAT_EQ(mesh.vertices[0].color[0], 0.5f);
    EXPECT_FLOAT_EQ(mesh.vertices[0].color[1], 0.75f);
    EXPECT_FLOAT_EQ(mesh.vertices[0].color[2], 1.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[0].color[3], 0.5f);
}

TEST(ParticleBillboardsTest, QuadsFaceTheCamerasAxes)
{
    // A camera looking down +X with +Z to its right and +Y up
    Rendering::CameraBasis camera;
    camera.right = Math::Vector3(0.0f, 0.0f, 1.0f);
    camera.up = Math::Vector3(0.0f, 1.0f, 0.0f);
    camera.forward = Math::Vector3(1.0f, 0.0f, 0.0f);
    BillboardStyle style;
    style.startSize = 2.0f;
    style.endSize = 2.0f;
    Renderer::Common::MeshData mesh;
    const std::vector<Math::Vector4> particles{Math::Vector4()};
    BuildBillboards(mesh, particles, camera, style);
    ASSERT_EQ(mesh.vertices.size(), 4u);
    for (const auto &vertex : mesh.vertices)
    {
        EXPECT_FLOAT_EQ(vertex.position[0], 0.0f) << "the quad lies in the plane the camera faces";
        EXPECT_FLOAT_EQ(vertex.normal[0], -1.0f);
    }
    EXPECT_FLOAT_EQ(mesh.vertices[1].position[2], 1.0f) << "bottom-right is on the camera's right";
    EXPECT_FLOAT_EQ(mesh.vertices[3].position[1], 1.0f) << "top-left is up";
}

TEST(ParticleBillboardsTest, RebuildingKeepsTheCapacityAndOnlyWritesNewIndices)
{
    Renderer::Common::MeshData mesh;
    BillboardStyle style;
    std::vector<Math::Vector4> particles(50);
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style);
    ASSERT_EQ(mesh.vertices.size(), 200u);
    ASSERT_EQ(mesh.indices.size(), 300u);
    const auto *vertexData = mesh.vertices.data();
    const auto *indexData = mesh.indices.data();

    particles.resize(20);
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style);
    EXPECT_EQ(mesh.vertices.size(), 80u);
    EXPECT_EQ(mesh.indices.size(), 120u);
    EXPECT_EQ(mesh.vertices.data(), vertexData) << "shrinking reuses the allocation";
    EXPECT_EQ(mesh.indices.data(), indexData);
    EXPECT_EQ(mesh.indices[119], 79u);

    particles.resize(50);
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style);
    EXPECT_EQ(mesh.vertices.data(), vertexData) << "growing back within the capacity doesn't allocate either";
    EXPECT_EQ(mesh.indices.size(), 300u);
    EXPECT_EQ(mesh.indices[299], 199u);
    EXPECT_EQ(mesh.indices[0], 0u);

    particles.clear();
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style);
    EXPECT_TRUE(mesh.vertices.empty());
    EXPECT_TRUE(mesh.indices.empty());
}

TEST(ParticleBillboardsTest, ANegativeSizeCountsAsZero)
{
    BillboardStyle style;
    style.startSize = -3.0f;
    style.endSize = -3.0f;
    Renderer::Common::MeshData mesh;
    const std::vector<Math::Vector4> particles{Math::Vector4()};
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style);
    ASSERT_EQ(mesh.vertices.size(), 4u);
    EXPECT_FLOAT_EQ(mesh.vertices[0].position[0], mesh.vertices[2].position[0]);
}

// ---------------------------------------------------------------- issue #445: shapes, rotation, motion, kills, render modes

namespace
{
    float Length3(const Math::Vector3 &v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    Math::Vector4 Particle(const float x, const float y, const float z, const float age)
    {
        Math::Vector4 v;
        v.x = x;
        v.y = y;
        v.z = z;
        v.w = age;
        return v;
    }
}

TEST(ParticleEmitterTest, ACircleStartsInItsDiscInTheXYPlaneAndMovesOutwardsInThatPlane)
{
    EmitterShapeSettings shape;
    shape.shape = EmitterShape::Circle;
    shape.shapeRadius = 2.0f;
    shape.speedMin = 1.0f;
    shape.speedMax = 1.0f;
    ParticleEmitter emitter(4);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    emitter.Generate(shape, Math::Vector3(1.0f, 1.0f, 3.0f), Math::Quaternion(), 300, positions, velocities);
    ASSERT_EQ(positions.size(), 300u);
    float nearest = 10.0f;
    for (std::size_t i = 0; i < 300; ++i)
    {
        EXPECT_NEAR(positions[i].z, 3.0f, 1e-5f) << "in the XY plane";
        EXPECT_NEAR(velocities[i].z, 0.0f, 1e-5f);
        const float dx = positions[i].x - 1.0f;
        const float dy = positions[i].y - 1.0f;
        const float r = std::sqrt(dx * dx + dy * dy);
        EXPECT_LE(r, 2.0f + 1e-4f);
        nearest = std::min(nearest, r);
        if (r > 0.05f)
        {
            EXPECT_GT(dx * velocities[i].x + dy * velocities[i].y, 0.0f) << "outwards";
        }
        EXPECT_NEAR(Length3(velocities[i]), 1.0f, 1e-4f);
    }
    EXPECT_LT(nearest, 1.0f) << "the disc is filled, not just its rim";

    // From the edge: every particle on the rim
    shape.emitFromEdge = true;
    emitter.Generate(shape, Math::Vector3(), Math::Quaternion(), 100, positions, velocities);
    for (const Math::Vector3 &p : positions)
    {
        EXPECT_NEAR(std::sqrt(p.x * p.x + p.y * p.y), 2.0f, 1e-4f);
    }
}

TEST(ParticleEmitterTest, AnEdgeStartsOnItsLineAndShootsUpWithinTheFanInTheXYPlane)
{
    EmitterShapeSettings shape;
    shape.shape = EmitterShape::Edge;
    shape.shapeRadius = 3.0f;
    shape.coneAngle = 20.0f;
    shape.speedMin = 2.0f;
    shape.speedMax = 2.0f;
    ParticleEmitter emitter(5);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    emitter.Generate(shape, Math::Vector3(), Math::Quaternion(), 300, positions, velocities);
    const float cosLimit = std::cos(20.0f * std::numbers::pi_v<float> / 180.0f);
    float widest = 0.0f;
    for (std::size_t i = 0; i < 300; ++i)
    {
        EXPECT_NEAR(positions[i].y, 0.0f, 1e-5f);
        EXPECT_NEAR(positions[i].z, 0.0f, 1e-5f);
        EXPECT_LE(std::abs(positions[i].x), 3.0f + 1e-4f);
        widest = std::max(widest, std::abs(positions[i].x));
        EXPECT_NEAR(velocities[i].z, 0.0f, 1e-5f) << "stays in the 2D plane";
        EXPECT_GE(velocities[i].y / 2.0f, cosLimit - 1e-4f) << "within 20 degrees of +Y";
    }
    EXPECT_GT(widest, 2.5f);
}

TEST(ParticleEmitterTest, ABoxStartsInsideItOrOnItsFacesAndShootsAlongTheCone)
{
    EmitterShapeSettings shape;
    shape.shape = EmitterShape::Box;
    shape.boxSize = Math::Vector3(4.0f, 2.0f, 6.0f);
    shape.coneAngle = 0.0f;
    shape.speedMin = 1.0f;
    shape.speedMax = 1.0f;
    ParticleEmitter emitter(6);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    emitter.Generate(shape, Math::Vector3(), Math::Quaternion(), 400, positions, velocities);
    bool inner = false;
    for (std::size_t i = 0; i < 400; ++i)
    {
        const Math::Vector3 &p = positions[i];
        EXPECT_LE(std::abs(p.x), 2.0f + 1e-4f);
        EXPECT_LE(std::abs(p.y), 1.0f + 1e-4f);
        EXPECT_LE(std::abs(p.z), 3.0f + 1e-4f);
        inner = inner || (std::abs(p.x) < 1.5f && std::abs(p.y) < 0.5f && std::abs(p.z) < 2.5f);
        EXPECT_NEAR(velocities[i].y, 1.0f, 1e-4f) << "a cone of 0 degrees: straight up";
    }
    EXPECT_TRUE(inner) << "the volume is used";

    shape.emitFromEdge = true;
    emitter.Generate(shape, Math::Vector3(), Math::Quaternion(), 400, positions, velocities);
    int onEachAxis[3] = {0, 0, 0};
    for (const Math::Vector3 &p : positions)
    {
        const bool onX = std::abs(std::abs(p.x) - 2.0f) < 1e-4f;
        const bool onY = std::abs(std::abs(p.y) - 1.0f) < 1e-4f;
        const bool onZ = std::abs(std::abs(p.z) - 3.0f) < 1e-4f;
        EXPECT_TRUE(onX || onY || onZ) << "on a face: " << p.x << ", " << p.y << ", " << p.z;
        onEachAxis[0] += onX ? 1 : 0;
        onEachAxis[1] += onY ? 1 : 0;
        onEachAxis[2] += onZ ? 1 : 0;
    }
    for (const int count : onEachAxis)
    {
        EXPECT_GT(count, 20) << "every pair of faces is used";
    }
}

TEST(ParticleEmitterTest, FromTheEdgeTheSphereUsesItsSurfaceAndTheConeItsRim)
{
    EmitterShapeSettings shape;
    shape.shape = EmitterShape::Sphere;
    shape.shapeRadius = 1.5f;
    shape.emitFromEdge = true;
    ParticleEmitter emitter(8);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    emitter.Generate(shape, Math::Vector3(), Math::Quaternion(), 100, positions, velocities);
    for (const Math::Vector3 &p : positions)
    {
        EXPECT_NEAR(Length3(p), 1.5f, 1e-4f);
    }
    shape.shape = EmitterShape::Cone;
    emitter.Generate(shape, Math::Vector3(), Math::Quaternion(), 100, positions, velocities);
    for (const Math::Vector3 &p : positions)
    {
        EXPECT_NEAR(std::sqrt(p.x * p.x + p.z * p.z), 1.5f, 1e-4f);
        EXPECT_NEAR(p.y, 0.0f, 1e-5f);
    }
}

TEST(ParticleEmitterTest, SpinsComeFromTheirRangesWithoutChangingThePositionsOrVelocities)
{
    EmitterShapeSettings shape;
    shape.shapeRadius = 1.0f;
    shape.coneAngle = 30.0f;
    shape.startRotationMin = 90.0f;
    shape.startRotationMax = 180.0f;
    shape.angularVelocityMin = -45.0f;
    shape.angularVelocityMax = 45.0f;
    ParticleEmitter withSpins(11);
    ParticleEmitter without(11);
    std::vector<Math::Vector3> positionsA, velocitiesA, positionsB, velocitiesB;
    std::vector<Math::Vector2> spins;
    withSpins.Generate(shape, Math::Vector3(), Math::Quaternion(), 50, positionsA, velocitiesA, &spins);
    without.Generate(shape, Math::Vector3(), Math::Quaternion(), 50, positionsB, velocitiesB);
    ASSERT_EQ(spins.size(), 50u);
    const float deg = std::numbers::pi_v<float> / 180.0f;
    for (std::size_t i = 0; i < 50; ++i)
    {
        EXPECT_EQ(positionsA[i].x, positionsB[i].x) << "the rotations have a random stream of their own";
        EXPECT_EQ(velocitiesA[i].y, velocitiesB[i].y);
        EXPECT_GE(spins[i].x, 90.0f * deg - 1e-5f);
        EXPECT_LE(spins[i].x, 180.0f * deg + 1e-5f);
        EXPECT_GE(spins[i].y, -45.0f * deg - 1e-5f);
        EXPECT_LE(spins[i].y, 45.0f * deg + 1e-5f);
    }
}

TEST(CpuParticleBackendTest, CopyMotionGivesTheVelocityAndTheRotationAtTheParticlesAge)
{
    CpuParticleBackend backend;
    ParticleParams params;
    params.gravity = Math::Vector3(0.0f, 0.0f, 0.0f);
    params.lifetimeMin = 10.0f;
    params.lifetimeMax = 10.0f;
    const ParticleSystemHandle handle = Make(backend, 10, 1, params);
    const std::vector<Math::Vector3> positions{Math::Vector3(0.0f, 0.0f, 0.0f), Math::Vector3(1.0f, 0.0f, 0.0f)};
    const std::vector<Math::Vector3> velocities{Math::Vector3(3.0f, 0.0f, 0.0f), Math::Vector3(0.0f, -2.0f, 0.0f)};
    const std::vector<Math::Vector2> spins{Math::Vector2(0.5f, 2.0f)}; // the second particle has none
    ASSERT_EQ(backend.SpawnWithSpin(handle, positions, velocities, spins), 2u);
    for (int i = 0; i < 25; ++i)
    {
        backend.Step(kStep);
    }
    std::vector<Math::Vector4> motion;
    backend.CopyMotion(handle, motion);
    ASSERT_EQ(motion.size(), 2u);
    EXPECT_FLOAT_EQ(motion[0].x, 3.0f);
    EXPECT_NEAR(motion[0].w, 0.5f + 2.0f * 0.5f, 1e-4f);
    EXPECT_FLOAT_EQ(motion[1].y, -2.0f);
    EXPECT_FLOAT_EQ(motion[1].w, 0.0f) << "no spin given: no rotation";

    backend.Clear(handle);
    backend.CopyMotion(handle, motion);
    EXPECT_TRUE(motion.empty());
    backend.CopyMotion(ParticleSystemHandle{12345}, motion);
    EXPECT_TRUE(motion.empty()) << "an unknown handle";
}

TEST(CpuParticleBackendTest, KillRemovesTheParticlesInsideARegionAndKeepsTheRest)
{
    CpuParticleBackend backend;
    ParticleParams params;
    params.lifetimeMin = 10.0f;
    params.lifetimeMax = 10.0f;
    const ParticleSystemHandle handle = Make(backend, 100, 1, params);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    std::vector<Math::Vector2> spins;
    for (int i = 0; i < 10; ++i)
    {
        positions.emplace_back(static_cast<float>(i), 0.0f, static_cast<float>(i % 2) * 10.0f);
        velocities.emplace_back(0.0f, 0.0f, 0.0f);
        spins.emplace_back(static_cast<float>(i), 0.0f); // the rotation tells which particle survived
    }
    ASSERT_EQ(backend.SpawnWithSpin(handle, positions, velocities, spins), 10u);

    using Physics::ParticleRegion;
    // A circle in the XY plane takes particles at any z: x = 0, 1, 2
    EXPECT_EQ(backend.Kill(handle, ParticleRegion{ParticleRegion::Shape::Circle, Math::Vector3(0.0f, 0.0f, 0.0f), 2.0f,
                                                  Math::Vector3()}),
              3u);
    // A sphere does not: x = 3 to 5 near z = 0 means 4 only (3 and 5 are at z = 10)
    EXPECT_EQ(backend.Kill(handle, ParticleRegion{ParticleRegion::Shape::Sphere, Math::Vector3(4.0f, 0.0f, 0.0f), 1.5f,
                                                  Math::Vector3()}),
              1u);
    // A box: x 6 to 7 (z 0 and 10)
    EXPECT_EQ(backend.Kill(handle, ParticleRegion{ParticleRegion::Shape::Box, Math::Vector3(6.5f, 0.0f, 5.0f), 0.0f,
                                                  Math::Vector3(0.6f, 1.0f, 6.0f)}),
              2u);
    // Nothing in a negative radius, nor for an unknown handle
    EXPECT_EQ(backend.Kill(handle, ParticleRegion{ParticleRegion::Shape::Sphere, Math::Vector3(8.0f, 0.0f, 0.0f), -1.0f,
                                                  Math::Vector3()}),
              0u);
    EXPECT_EQ(backend.Kill(ParticleSystemHandle{12345}, ParticleRegion{}), 0u);

    EXPECT_EQ(backend.GetAliveCount(handle), 4u);
    std::vector<Math::Vector4> out;
    std::vector<Math::Vector4> motion;
    backend.CopyPositions(handle, out);
    backend.CopyMotion(handle, motion);
    ASSERT_EQ(out.size(), 4u);
    ASSERT_EQ(motion.size(), 4u);
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        EXPECT_FLOAT_EQ(out[i].x, motion[i].w) << "positions and rotations moved together";
        EXPECT_TRUE(out[i].x == 3.0f || out[i].x == 5.0f || out[i].x == 8.0f || out[i].x == 9.0f) << out[i].x;
    }
}

TEST(ParticleBillboardsTest, AStretchedQuadLiesAlongTheVelocityAsTheCameraSeesIt)
{
    BillboardStyle style;
    style.startSize = 1.0f;
    style.endSize = 1.0f;
    style.mode = ParticleRenderMode::Stretched;
    style.lengthScale = 2.0f;
    style.speedScale = 0.5f;
    const std::vector<Math::Vector4> particles{Particle(0.0f, 0.0f, 0.0f, 0.0f)};
    // Moving along +X (and towards the camera, which the camera can't see) at speed 4
    const std::vector<Math::Vector4> motion{Particle(4.0f, 0.0f, 3.0f, 1.0f)};
    Renderer::Common::MeshData mesh;
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style, motion);
    ASSERT_EQ(mesh.vertices.size(), 4u);
    const auto &v = mesh.vertices;
    // up = +X, right = forward x up = (0, 0, -1) x (1, 0, 0) = (0, -1, 0); length 1 * 2 + 5 * 0.5 = 4.5, width 1
    EXPECT_NEAR(v[0].position[0], -2.25f, 1e-5f) << "bottom-left: the tail";
    EXPECT_NEAR(v[0].position[1], 0.5f, 1e-5f);
    EXPECT_NEAR(v[2].position[0], 2.25f, 1e-5f) << "top-right: the front";
    EXPECT_NEAR(v[2].position[1], -0.5f, 1e-5f);
    for (const auto &vertex : v)
    {
        EXPECT_NEAR(vertex.position[2], 0.0f, 1e-5f) << "in the plane the camera faces";
        EXPECT_FLOAT_EQ(vertex.normal[2], 1.0f);
    }
    EXPECT_FLOAT_EQ(v[2].texCoord[1], 1.0f) << "v = 1 at the front";

    // Moving straight at the camera, or without motion: a plain billboard
    const std::vector<Math::Vector4> head{Particle(0.0f, 0.0f, 5.0f, 0.0f)};
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style, head);
    EXPECT_NEAR(mesh.vertices[2].position[0], 0.5f, 1e-5f);
    EXPECT_NEAR(mesh.vertices[2].position[1], 0.5f, 1e-5f);
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style);
    EXPECT_NEAR(mesh.vertices[2].position[0], 0.5f, 1e-5f);
}

TEST(ParticleBillboardsTest, TheRotationTurnsTheQuadCounterClockwiseAsTheCameraSeesIt)
{
    BillboardStyle style;
    style.startSize = 2.0f;
    style.endSize = 2.0f;
    const std::vector<Math::Vector4> particles{Particle(0.0f, 0.0f, 0.0f, 0.0f)};
    const std::vector<Math::Vector4> motion{Particle(0.0f, 0.0f, 0.0f, std::numbers::pi_v<float> / 2.0f)};
    Renderer::Common::MeshData mesh;
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style, motion);
    // A quarter turn: bottom-right (1, -1) goes to (1, 1)
    EXPECT_NEAR(mesh.vertices[1].position[0], 1.0f, 1e-5f);
    EXPECT_NEAR(mesh.vertices[1].position[1], 1.0f, 1e-5f);
    EXPECT_NEAR(mesh.vertices[3].position[0], -1.0f, 1e-5f) << "top-left (-1, 1) goes to (-1, -1)";
    EXPECT_NEAR(mesh.vertices[3].position[1], -1.0f, 1e-5f);

    // A Stretched quad is not rotated
    style.mode = ParticleRenderMode::Stretched;
    style.lengthScale = 1.0f;
    const std::vector<Math::Vector4> moving{Particle(0.0f, 2.0f, 0.0f, std::numbers::pi_v<float> / 2.0f)};
    BuildBillboards(mesh, particles, Rendering::CameraBasis{}, style, moving);
    EXPECT_NEAR(mesh.vertices[2].position[0], 1.0f, 1e-5f) << "along +Y, as an unrotated billboard";
    EXPECT_NEAR(mesh.vertices[2].position[1], 1.0f, 1e-5f);
}

TEST(ParticleBillboardsTest, HorizontalQuadsLieFlatAndVerticalOnesStandUpright)
{
    BillboardStyle style;
    style.startSize = 2.0f;
    style.endSize = 2.0f;
    style.mode = ParticleRenderMode::HorizontalBillboard;
    // A camera looking down +X with +Z to its right
    Rendering::CameraBasis camera;
    camera.right = Math::Vector3(0.0f, 0.0f, 1.0f);
    camera.up = Math::Vector3(0.0f, 1.0f, 0.0f);
    camera.forward = Math::Vector3(1.0f, 0.0f, 0.0f);
    const std::vector<Math::Vector4> particles{Particle(0.0f, 3.0f, 0.0f, 0.0f)};
    Renderer::Common::MeshData mesh;
    BuildBillboards(mesh, particles, camera, style);
    for (const auto &vertex : mesh.vertices)
    {
        EXPECT_FLOAT_EQ(vertex.position[1], 3.0f) << "flat, whatever the camera";
        EXPECT_FLOAT_EQ(vertex.normal[1], 1.0f) << "facing up";
    }
    EXPECT_FLOAT_EQ(mesh.vertices[2].position[0], 1.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[2].position[2], -1.0f);

    // Vertical: a camera tilted down still gets an upright quad turned towards it
    style.mode = ParticleRenderMode::VerticalBillboard;
    camera.up = Math::Vector3(-0.6f, 0.8f, 0.0f);
    camera.forward = Math::Vector3(0.8f, 0.6f, 0.0f);
    BuildBillboards(mesh, particles, camera, style);
    for (const auto &vertex : mesh.vertices)
    {
        EXPECT_NEAR(vertex.position[0], 0.0f, 1e-5f) << "upright in the plane facing the camera's heading";
        EXPECT_NEAR(vertex.normal[0], -1.0f, 1e-5f);
        EXPECT_NEAR(vertex.normal[1], 0.0f, 1e-5f);
    }
    EXPECT_NEAR(mesh.vertices[2].position[1], 4.0f, 1e-5f) << "top-right is straight up";
    EXPECT_NEAR(mesh.vertices[2].position[2], 1.0f, 1e-5f);
}

TEST(ParticleBillboardsTest, TheRenderModeSavesByName)
{
    EXPECT_EQ(nlohmann::json(ParticleRenderMode::Stretched).get<std::string>(), "Stretched");
    EXPECT_EQ(nlohmann::json("VerticalBillboard").get<ParticleRenderMode>(), ParticleRenderMode::VerticalBillboard);
    EXPECT_EQ(nlohmann::json(EmitterShape::Box).get<std::string>(), "Box");
    EXPECT_EQ(nlohmann::json("Circle").get<EmitterShape>(), EmitterShape::Circle);
}
