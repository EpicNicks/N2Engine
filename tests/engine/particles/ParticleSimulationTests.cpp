#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "FakePhysicsBackend.hpp"
#include "engine/particles/CpuParticleBackend.hpp"
#include "engine/particles/ParticleBackendSelection.hpp"
#include "engine/particles/ParticleBillboards.hpp"
#include "engine/particles/ParticleEmitter.hpp"
#include "engine/particles/ParticleRandom.hpp"

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

TEST(ParticleBackendSelectionTest, TheEnumSavesByName)
{
    const std::vector<std::string> options = FieldTraits<ParticleBackend>::EnumOptions();
    const std::vector<std::string> expected = {"Auto", "PhysX", "CPU"};
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
