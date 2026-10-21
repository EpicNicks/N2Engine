#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "FakePbdDriver.hpp"
#include "engine/Logger.hpp"
#include "engine/particles/CpuParticleBackend.hpp"
#include "engine/particles/PbdParticleBackend.hpp"

// The PhysX PBD backend's logic against a fake driver, so it runs on every CI leg without a GPU: slot allocation,
// the cap, emission into the host mirror and its upload, ageing and compaction after a scene step, the material
// presets, the generation-counted handles, Clear/Destroy, and the soft failure paths. Nothing here touches PhysX
// or CUDA; the real driver is exercised by the GPU golden test on a machine with a GPU.

using namespace N2Engine;
using namespace N2Engine::Particles;
using Physics::ParticleParams;
using Physics::ParticleSystemDesc;
using Physics::ParticleSystemHandle;
using TestSupport::FakePbdDriver;

namespace
{
    /// Collects the warnings logged while it lives
    class WarningCapture
    {
    public:
        WarningCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn)
                {
                    warnings.emplace_back(message);
                }
            };
        }
        ~WarningCapture() { Logger::logEvent -= _id; }
        WarningCapture(const WarningCapture &) = delete;
        WarningCapture &operator=(const WarningCapture &) = delete;

        std::vector<std::string> warnings;

    private:
        std::size_t _id = 0;
    };

    /// A backend over a fake driver, which the test keeps a pointer to
    struct Rig
    {
        Rig()
        {
            auto fake = std::make_unique<FakePbdDriver>();
            driver = fake.get();
            backend = std::make_unique<PbdParticleBackend>(std::move(fake));
        }

        FakePbdDriver *driver = nullptr;
        std::unique_ptr<PbdParticleBackend> backend;
    };

    ParticleSystemDesc MakeDesc(const std::uint32_t maxParticles, const float lifetime = 1.0f,
                                const Physics::ParticleMaterial material = Physics::ParticleMaterial::Water)
    {
        ParticleSystemDesc desc;
        desc.maxParticles = maxParticles;
        desc.seed = 7;
        desc.material = material;
        desc.params.lifetimeMin = lifetime;
        desc.params.lifetimeMax = lifetime;
        return desc;
    }

    std::vector<Math::Vector3> Row(const int count, const float x0, const float y)
    {
        std::vector<Math::Vector3> points;
        for (int i = 0; i < count; ++i)
        {
            points.emplace_back(x0 + static_cast<float>(i), y, 0.0f);
        }
        return points;
    }

    std::vector<Math::Vector3> Still(const int count)
    {
        return std::vector<Math::Vector3>(static_cast<std::size_t>(count), Math::Vector3(0.0f, 0.0f, 0.0f));
    }

    std::vector<float> Xs(const PbdParticleBackend &backend, const ParticleSystemHandle handle)
    {
        std::vector<Math::Vector4> particles;
        backend.CopyPositions(handle, particles);
        std::vector<float> xs;
        for (const Math::Vector4 &p : particles)
        {
            xs.push_back(p.x);
        }
        return xs;
    }
}

// ---------------------------------------------------------------- creating systems and spawning

TEST(PbdParticleBackendTest, CreatesADriverSystemWithThePresetsMaterialAndAHandleOfItsGeneration)
{
    Rig rig;
    EXPECT_EQ(rig.backend->GetName(), "PhysX");
    EXPECT_EQ(rig.backend->GetDeviceName(), "Fake GPU");

    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(100));
    ASSERT_NE(handle, Physics::kInvalidParticleSystem);
    EXPECT_EQ(Physics::ParticleHandleGeneration(handle), rig.backend->GetGeneration());
    EXPECT_NE(rig.backend->GetGeneration(), 0u);
    EXPECT_TRUE(rig.backend->OwnsHandle(handle));
    EXPECT_EQ(rig.backend->GetSystemCount(), 1u);

    ASSERT_EQ(rig.driver->systems.size(), 1u);
    const auto &system = rig.driver->systems.begin()->second;
    EXPECT_EQ(system.desc.maxParticles, 100u);
    EXPECT_TRUE(system.desc.preset.fluid) << "Water is a fluid";
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 0u);
}

TEST(PbdParticleBackendTest, SpawnWritesTheHostMirrorAndUploadsItAllToTheDevice)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10));
    const std::vector<Math::Vector3> positions = Row(3, 1.0f, 5.0f);
    const std::vector<Math::Vector3> velocities = {Math::Vector3(0, 1, 0), Math::Vector3(0, 2, 0),
                                                   Math::Vector3(0, 3, 0)};
    EXPECT_EQ(rig.backend->Spawn(handle, positions, velocities), 3u);
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 3u);

    const auto &system = rig.driver->systems.begin()->second;
    EXPECT_EQ(system.active, 3u);
    EXPECT_EQ(system.uploads, 1);
    EXPECT_EQ(system.lastDirty, kPbdPositions | kPbdVelocities | kPbdPhases);
    const PbdPreset &preset = system.desc.preset;
    const float mass = preset.density * preset.particleSpacing * preset.particleSpacing * preset.particleSpacing;
    for (std::size_t i = 0; i < 3; ++i)
    {
        // What reached the "device" is what was spawned: position, a positive inverse mass, velocity, the phase
        EXPECT_FLOAT_EQ(system.devicePositions[i].x, 1.0f + static_cast<float>(i));
        EXPECT_FLOAT_EQ(system.devicePositions[i].y, 5.0f);
        EXPECT_FLOAT_EQ(system.devicePositions[i].w, 1.0f / mass);
        EXPECT_FLOAT_EQ(system.deviceVelocities[i].y, static_cast<float>(i + 1));
        EXPECT_EQ(system.devicePhases[i], system.phase);
    }
}

TEST(PbdParticleBackendTest, SpawnIsCappedAtMaxParticlesAndReportsHowManyFit)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(4));
    EXPECT_EQ(rig.backend->Spawn(handle, Row(10, 0, 0), Still(10)), 4u) << "a burst larger than the buffer";
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 4u);
    EXPECT_EQ(rig.backend->Spawn(handle, Row(1, 0, 0), Still(1)), 0u) << "full";
    EXPECT_EQ(rig.driver->systems.begin()->second.active, 4u);
    // The shorter of the two spans limits it too
    const ParticleSystemHandle other = rig.backend->CreateSystem(MakeDesc(10));
    EXPECT_EQ(rig.backend->Spawn(other, Row(5, 0, 0), Still(2)), 2u);
}

TEST(PbdParticleBackendTest, ASystemOfZeroParticlesExistsWithoutADriverSystem)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(0));
    ASSERT_NE(handle, Physics::kInvalidParticleSystem);
    EXPECT_EQ(rig.driver->createCalls, 0);
    EXPECT_EQ(rig.backend->Spawn(handle, Row(3, 0, 0), Still(3)), 0u);
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 0u);
    rig.backend->OnSceneStepped(0.1f);
    rig.backend->Clear(handle);
    rig.backend->Destroy(handle);
    EXPECT_EQ(rig.backend->GetSystemCount(), 0u);
}

// ---------------------------------------------------------------- the scene step: read-back, ageing, compaction

TEST(PbdParticleBackendTest, AfterASceneStepThePositionsAreTheSimulatedOnes)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10, 100.0f));
    ASSERT_EQ(rig.backend->Spawn(handle, Row(2, 0.0f, 10.0f), Still(2)), 2u);

    for (int i = 0; i < 5; ++i)
    {
        rig.driver->Simulate(0.1f); // the scene steps the particles on the device
    }
    const float simulatedY = rig.driver->systems.begin()->second.devicePositions[0].y;
    ASSERT_LT(simulatedY, 9.0f);

    // Until the backend is told the scene stepped, it still shows the spawn positions
    std::vector<Math::Vector4> particles;
    rig.backend->CopyPositions(handle, particles);
    ASSERT_EQ(particles.size(), 2u);
    EXPECT_FLOAT_EQ(particles[0].y, 10.0f);

    rig.backend->OnSceneStepped(0.5f);
    rig.backend->CopyPositions(handle, particles);
    EXPECT_FLOAT_EQ(particles[0].y, simulatedY);
    EXPECT_FLOAT_EQ(particles[1].y, simulatedY);
    EXPECT_EQ(rig.driver->systems.begin()->second.readBacks, 1);
}

TEST(PbdParticleBackendTest, SpawningAfterAStepNeverRevertsTheLivingToTheirSpawnPositions)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10, 100.0f));
    ASSERT_EQ(rig.backend->Spawn(handle, Row(1, 0.0f, 10.0f), Still(1)), 1u);
    for (int i = 0; i < 5; ++i)
    {
        rig.driver->Simulate(0.1f);
    }
    rig.backend->OnSceneStepped(0.5f);
    const float fallenY = rig.driver->systems.begin()->second.devicePositions[0].y;
    ASSERT_LT(fallenY, 9.0f);

    // The mirror was refreshed by the step, so the upload this spawn makes carries the fallen particle's position
    ASSERT_EQ(rig.backend->Spawn(handle, Row(1, 5.0f, 10.0f), Still(1)), 1u);
    const auto &system = rig.driver->systems.begin()->second;
    EXPECT_EQ(system.active, 2u);
    EXPECT_FLOAT_EQ(system.devicePositions[0].y, fallenY);
    EXPECT_FLOAT_EQ(system.devicePositions[1].y, 10.0f);
}

TEST(PbdParticleBackendTest, ParticlesPastTheirLifetimeAreRemovedAndTheLastOneFillsTheSlot)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10, 1.0f));
    ASSERT_EQ(rig.backend->Spawn(handle, Row(3, 0.0f, 0.0f), Still(3)), 3u); // x = 0, 1, 2
    rig.backend->OnSceneStepped(0.6f);
    ASSERT_EQ(rig.backend->GetAliveCount(handle), 3u);
    ASSERT_EQ(rig.backend->Spawn(handle, Row(2, 10.0f, 0.0f), Still(2)), 2u); // x = 10, 11, 0.6 s younger

    rig.backend->OnSceneStepped(0.6f); // the first three are 1.2 s old, the last two 0.6 s
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 2u);
    // Slots 0..4 were [0, 1, 2, 10, 11]: removing from the front moves the last into the hole each time
    EXPECT_EQ(Xs(*rig.backend, handle), (std::vector<float>{11.0f, 10.0f}));

    // The device holds the compacted particles and the shrunken active count
    const auto &system = rig.driver->systems.begin()->second;
    EXPECT_EQ(system.active, 2u);
    EXPECT_FLOAT_EQ(system.devicePositions[0].x, 11.0f);
    EXPECT_FLOAT_EQ(system.devicePositions[1].x, 10.0f);

    // The survivors' ages follow them: 0.6 s into a 1 s life
    std::vector<Math::Vector4> particles;
    rig.backend->CopyPositions(handle, particles);
    EXPECT_NEAR(particles[0].w, 0.6f, 1e-5f);
    EXPECT_NEAR(particles[1].w, 0.6f, 1e-5f);

    rig.backend->OnSceneStepped(0.5f);
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 0u);
    EXPECT_EQ(rig.driver->systems.begin()->second.active, 0u);
}

TEST(PbdParticleBackendTest, ExpiredParticlesMakeRoomUnderTheCap)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(4, 1.0f));
    ASSERT_EQ(rig.backend->Spawn(handle, Row(6, 0, 0), Still(6)), 4u);
    EXPECT_EQ(rig.backend->Spawn(handle, Row(1, 0, 0), Still(1)), 0u);
    rig.backend->OnSceneStepped(1.1f);
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 0u);
    EXPECT_EQ(rig.backend->Spawn(handle, Row(6, 0, 0), Still(6)), 4u);
}

TEST(PbdParticleBackendTest, AStepWithNoDeathDoesNotUploadAndAnEmptySystemIsNotReadBack)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10, 100.0f));
    rig.backend->OnSceneStepped(0.1f);
    EXPECT_EQ(rig.driver->readBackCalls, 0) << "nothing alive, nothing to read";

    ASSERT_EQ(rig.backend->Spawn(handle, Row(2, 0, 0), Still(2)), 2u);
    const int uploads = rig.driver->uploadCalls;
    rig.backend->OnSceneStepped(0.1f);
    EXPECT_EQ(rig.driver->readBackCalls, 1);
    EXPECT_EQ(rig.driver->uploadCalls, uploads) << "nobody died: the device keeps its own state";
}

TEST(PbdParticleBackendTest, StepDoesNothingBecauseTheSceneSteps)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10, 0.5f));
    ASSERT_EQ(rig.backend->Spawn(handle, Row(2, 0, 0), Still(2)), 2u);
    for (int i = 0; i < 10; ++i)
    {
        rig.backend->Step(0.1f);
    }
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 2u) << "no ageing outside the scene step";
    EXPECT_EQ(rig.driver->readBackCalls, 0);
}

TEST(PbdParticleBackendTest, ANonFiniteOrNegativeStepIsIgnored)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10, 0.5f));
    ASSERT_EQ(rig.backend->Spawn(handle, Row(2, 0, 0), Still(2)), 2u);
    rig.backend->OnSceneStepped(std::nanf(""));
    rig.backend->OnSceneStepped(-1.0f);
    EXPECT_EQ(rig.driver->readBackCalls, 0);
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 2u);
}

TEST(PbdParticleBackendTest, TheSameSeedGivesTheSameLifetimes)
{
    const auto survivors = [](const std::uint32_t seed)
    {
        Rig rig;
        ParticleSystemDesc desc = MakeDesc(200, 1.0f);
        desc.params.lifetimeMax = 3.0f;
        desc.seed = seed;
        const ParticleSystemHandle handle = rig.backend->CreateSystem(desc);
        EXPECT_EQ(rig.backend->Spawn(handle, Row(200, 0, 0), Still(200)), 200u);
        rig.backend->OnSceneStepped(2.0f);
        return Xs(*rig.backend, handle);
    };
    const std::vector<float> a = survivors(5);
    EXPECT_EQ(a, survivors(5));
    EXPECT_GT(a.size(), 0u);
    EXPECT_LT(a.size(), 200u) << "lifetimes span 1 to 3 s, so some died at 2 s and some did not";
    EXPECT_NE(a, survivors(6));
}

// ---------------------------------------------------------------- Clear and Destroy

TEST(PbdParticleBackendTest, ClearEmptiesTheSystemAndItCanBeFilledAgain)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(4, 100.0f));
    ASSERT_EQ(rig.backend->Spawn(handle, Row(4, 0, 0), Still(4)), 4u);
    rig.backend->Clear(handle);
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 0u);
    EXPECT_EQ(rig.driver->systems.begin()->second.active, 0u) << "the device's active count follows";
    std::vector<Math::Vector4> particles(5);
    rig.backend->CopyPositions(handle, particles);
    EXPECT_TRUE(particles.empty());
    EXPECT_EQ(rig.backend->Spawn(handle, Row(4, 0, 0), Still(4)), 4u);

    rig.backend->Clear(0x1234); // unknown handles are ignored
    rig.backend->Clear(Physics::kInvalidParticleSystem);
}

TEST(PbdParticleBackendTest, DestroyReleasesTheDriverSystemOnceAndTheHandleIsDeadAfterwards)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(4));
    ASSERT_EQ(rig.backend->Spawn(handle, Row(2, 0, 0), Still(2)), 2u);
    rig.backend->Destroy(handle);
    EXPECT_EQ(rig.driver->destroyCalls, 1);
    EXPECT_TRUE(rig.driver->systems.empty());
    EXPECT_EQ(rig.backend->GetSystemCount(), 0u);

    rig.backend->Destroy(handle); // again: nothing
    EXPECT_EQ(rig.driver->destroyCalls, 1);
    EXPECT_EQ(rig.backend->Spawn(handle, Row(1, 0, 0), Still(1)), 0u);
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 0u);
    rig.backend->SetParams(handle, ParticleParams{});
    rig.backend->OnSceneStepped(0.1f);
    EXPECT_EQ(rig.driver->readBackCalls, 0);
}

TEST(PbdParticleBackendTest, DestroyingTheBackendReleasesEverySystem)
{
    auto fake = std::make_unique<FakePbdDriver>();
    FakePbdDriver *driver = fake.get();
    {
        PbdParticleBackend backend(std::move(fake));
        static_cast<void>(backend.CreateSystem(MakeDesc(4)));
        static_cast<void>(backend.CreateSystem(MakeDesc(8)));
        EXPECT_EQ(driver->systems.size(), 2u);
        EXPECT_EQ(backend.GetSystemCount(), 2u);
        backend.ReleaseAll();
        EXPECT_EQ(driver->destroyCalls, 2);
        EXPECT_EQ(backend.GetSystemCount(), 0u);
    }
}

// ---------------------------------------------------------------- material presets and parameters

TEST(PbdParticleBackendTest, EachPresetMapsToItsOwnPbdParameters)
{
    const PbdPreset water = GetPbdPreset(Physics::ParticleMaterial::Water);
    const PbdPreset sand = GetPbdPreset(Physics::ParticleMaterial::Sand);
    const PbdPreset smoke = GetPbdPreset(Physics::ParticleMaterial::Smoke);

    // Water flows: fluid constraints, viscosity, surface tension and cohesion, little friction
    EXPECT_TRUE(water.fluid);
    EXPECT_GT(water.material.viscosity, 0.0f);
    EXPECT_GT(water.material.surfaceTension, 0.0f);
    EXPECT_GT(water.material.cohesion, 0.0f);
    // Sand piles up: not a fluid, no cohesion or viscosity, far more friction than water
    EXPECT_FALSE(sand.fluid);
    EXPECT_EQ(sand.material.cohesion, 0.0f);
    EXPECT_EQ(sand.material.viscosity, 0.0f);
    EXPECT_GT(sand.material.friction, water.material.friction * 5.0f);
    // Smoke drifts: far more damping than the others, no cohesion, lighter than water
    EXPECT_GT(smoke.material.damping, water.material.damping * 5.0f);
    EXPECT_EQ(smoke.material.cohesion, 0.0f);
    EXPECT_LT(smoke.density, water.density);
    for (const PbdPreset &preset : {water, sand, smoke})
    {
        EXPECT_GT(preset.particleSpacing, 0.0f);
        EXPECT_GT(preset.density, 0.0f);
        EXPECT_GT(preset.material.cflCoefficient, 0.0f);
    }

    // The system gets the one it asked for
    Rig rig;
    static_cast<void>(rig.backend->CreateSystem(MakeDesc(4, 1.0f, Physics::ParticleMaterial::Sand)));
    static_cast<void>(rig.backend->CreateSystem(MakeDesc(4, 1.0f, Physics::ParticleMaterial::Smoke)));
    ASSERT_EQ(rig.driver->systems.size(), 2u);
    auto it = rig.driver->systems.begin();
    EXPECT_FALSE(it->second.desc.preset.fluid);
    EXPECT_FLOAT_EQ(it->second.desc.material.friction, sand.material.friction);
    ++it;
    EXPECT_FLOAT_EQ(it->second.desc.material.damping, smoke.material.damping);
}

TEST(PbdParticleBackendTest, GravityIsTheShareOfTheSceneGravityTheParametersAskFor)
{
    const PbdPreset preset = GetPbdPreset(Physics::ParticleMaterial::Water);
    const Math::Vector3 scene(0.0f, -10.0f, 0.0f);
    EXPECT_FLOAT_EQ(ComposePbdMaterial(preset, Math::Vector3(0, -10, 0), scene, 0).gravityScale, 1.0f);
    EXPECT_FLOAT_EQ(ComposePbdMaterial(preset, Math::Vector3(0, -5, 0), scene, 0).gravityScale, 0.5f);
    EXPECT_FLOAT_EQ(ComposePbdMaterial(preset, Math::Vector3(0, 0, 0), scene, 0).gravityScale, 0.0f);
    EXPECT_FLOAT_EQ(ComposePbdMaterial(preset, Math::Vector3(0, 10, 0), scene, 0).gravityScale, 0.0f)
        << "upwards can't be made: PBD gravity scale is never negative";
    EXPECT_FLOAT_EQ(ComposePbdMaterial(preset, Math::Vector3(0, -10, 0), Math::Vector3(0, 0, 0), 0).gravityScale, 0.0f)
        << "a scene without gravity";
    EXPECT_FLOAT_EQ(ComposePbdMaterial(preset, Math::Vector3(0, -20, 0), scene, 0).gravityScale, 2.0f);

    // The system's drag is added to the preset's damping
    EXPECT_FLOAT_EQ(ComposePbdMaterial(preset, Math::Vector3(0, -10, 0), scene, 0.25f).damping,
                    preset.material.damping + 0.25f);
    EXPECT_FLOAT_EQ(ComposePbdMaterial(preset, Math::Vector3(0, -10, 0), scene, std::nanf("")).damping,
                    preset.material.damping);
}

TEST(PbdParticleBackendTest, SetParamsChangesTheDriversMaterialOnlyWhenItDiffers)
{
    Rig rig;
    ParticleSystemDesc desc = MakeDesc(4, 1.0f);
    desc.params.gravity = Math::Vector3(0.0f, -10.0f, 0.0f);
    const ParticleSystemHandle handle = rig.backend->CreateSystem(desc);
    const auto &system = rig.driver->systems.begin()->second;
    EXPECT_FLOAT_EQ(system.desc.material.gravityScale, 1.0f);

    ParticleParams params = desc.params;
    for (int i = 0; i < 5; ++i)
    {
        rig.backend->SetParams(handle, params); // every fixed step, unchanged
    }
    EXPECT_EQ(system.materialChanges, 0);

    params.gravity = Math::Vector3(0.0f, -5.0f, 0.0f);
    rig.backend->SetParams(handle, params);
    rig.backend->SetParams(handle, params);
    EXPECT_EQ(system.materialChanges, 1);
    EXPECT_FLOAT_EQ(system.material.gravityScale, 0.5f);

    params.drag = 0.4f;
    rig.backend->SetParams(handle, params);
    EXPECT_EQ(system.materialChanges, 2);
    EXPECT_FLOAT_EQ(system.material.damping, system.desc.preset.material.damping + 0.4f);
}

TEST(PbdParticleBackendTest, NewLifetimesFollowTheLatestParams)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10, 100.0f));
    ParticleParams params;
    params.lifetimeMin = 0.5f;
    params.lifetimeMax = 0.5f;
    rig.backend->SetParams(handle, params);
    ASSERT_EQ(rig.backend->Spawn(handle, Row(2, 0, 0), Still(2)), 2u);
    rig.backend->OnSceneStepped(0.6f);
    EXPECT_EQ(rig.backend->GetAliveCount(handle), 0u);
}

// ---------------------------------------------------------------- generation-counted handles

TEST(PbdParticleBackendTest, TwoBackendsHaveDifferentGenerationsSoTheirFirstHandlesDiffer)
{
    Rig a;
    Rig b;
    EXPECT_NE(a.backend->GetGeneration(), b.backend->GetGeneration());
    const ParticleSystemHandle ha = a.backend->CreateSystem(MakeDesc(4));
    const ParticleSystemHandle hb = b.backend->CreateSystem(MakeDesc(4));
    EXPECT_EQ(Physics::ParticleHandleIndex(ha), Physics::ParticleHandleIndex(hb)) << "both are the first system";
    EXPECT_NE(ha, hb);
    EXPECT_TRUE(a.backend->OwnsHandle(ha));
    EXPECT_FALSE(a.backend->OwnsHandle(hb));
    EXPECT_FALSE(b.backend->OwnsHandle(ha));
    EXPECT_FALSE(a.backend->OwnsHandle(Physics::kInvalidParticleSystem));
}

TEST(PbdParticleBackendTest, AHandleOfAnotherBackendIsIgnoredNeverReadAsOneOfThisBackendsSystems)
{
    Rig a;
    Rig b;
    const ParticleSystemHandle ha = a.backend->CreateSystem(MakeDesc(4, 100.0f));
    const ParticleSystemHandle hb = b.backend->CreateSystem(MakeDesc(4, 100.0f));
    ASSERT_EQ(Physics::ParticleHandleIndex(ha), Physics::ParticleHandleIndex(hb));
    ASSERT_EQ(a.backend->Spawn(ha, Row(2, 0, 0), Still(2)), 2u);

    // b has a system with the same index; a's handle must not reach it
    EXPECT_EQ(b.backend->GetAliveCount(ha), 0u);
    EXPECT_EQ(b.backend->Spawn(ha, Row(3, 0, 0), Still(3)), 0u);
    b.backend->Clear(ha);
    b.backend->SetParams(ha, ParticleParams{});
    b.backend->Destroy(ha);
    std::vector<Math::Vector4> particles(3);
    b.backend->CopyPositions(ha, particles);
    EXPECT_TRUE(particles.empty());
    EXPECT_EQ(b.backend->GetSystemCount(), 1u);
    EXPECT_EQ(b.driver->destroyCalls, 0);
    EXPECT_EQ(b.backend->GetAliveCount(hb), 0u) << "b's own system is untouched";
    EXPECT_EQ(a.backend->GetAliveCount(ha), 2u);
}

TEST(PbdParticleBackendTest, ABackendRecreatedAtTheSameAddressDoesNotKnowTheOldHandles)
{
    std::optional<PbdParticleBackend> slot;
    slot.emplace(std::make_unique<FakePbdDriver>());
    const PbdParticleBackend *address = &*slot;
    const ParticleSystemHandle old = slot->CreateSystem(MakeDesc(4, 100.0f));
    ASSERT_EQ(slot->Spawn(old, Row(2, 0, 0), Still(2)), 2u);
    const std::uint32_t oldGeneration = slot->GetGeneration();

    slot.reset();
    slot.emplace(std::make_unique<FakePbdDriver>());
    ASSERT_EQ(&*slot, address) << "constructed in the same storage";
    EXPECT_NE(slot->GetGeneration(), oldGeneration);

    // The new backend's first system has the same index as the old handle's, and a different generation
    const ParticleSystemHandle fresh = slot->CreateSystem(MakeDesc(4, 100.0f));
    ASSERT_EQ(Physics::ParticleHandleIndex(fresh), Physics::ParticleHandleIndex(old));
    EXPECT_NE(fresh, old);
    EXPECT_FALSE(slot->OwnsHandle(old));
    EXPECT_EQ(slot->GetAliveCount(old), 0u);
    EXPECT_EQ(slot->Spawn(old, Row(2, 0, 0), Still(2)), 0u);
    EXPECT_EQ(slot->GetAliveCount(fresh), 0u) << "the stale spawn did not land in the new system";
    slot->Destroy(old);
    EXPECT_EQ(slot->GetSystemCount(), 1u) << "the stale destroy did not remove the new system";
}

TEST(PbdParticleBackendTest, TheCpuBackendsHandlesAreNotThePbdBackendsEither)
{
    Rig rig;
    CpuParticleBackend cpu;
    const ParticleSystemHandle fromCpu = cpu.CreateSystem(MakeDesc(4, 100.0f));
    const ParticleSystemHandle fromPbd = rig.backend->CreateSystem(MakeDesc(4, 100.0f));
    EXPECT_EQ(Physics::ParticleHandleGeneration(fromCpu), cpu.GetGeneration());
    EXPECT_NE(cpu.GetGeneration(), rig.backend->GetGeneration());
    EXPECT_FALSE(rig.backend->OwnsHandle(fromCpu));
    EXPECT_FALSE(cpu.OwnsHandle(fromPbd));
    EXPECT_EQ(cpu.Spawn(fromPbd, Row(2, 0, 0), Still(2)), 0u);
    EXPECT_EQ(rig.backend->Spawn(fromCpu, Row(2, 0, 0), Still(2)), 0u);
}

TEST(ParticleHandleTest, PacksTheGenerationAndTheIndexAndNeverIssuesGenerationZero)
{
    const ParticleSystemHandle handle = Physics::MakeParticleHandle(0xABCDu, 0x1234u);
    EXPECT_EQ(Physics::ParticleHandleGeneration(handle), 0xABCDu);
    EXPECT_EQ(Physics::ParticleHandleIndex(handle), 0x1234u);
    EXPECT_NE(handle, Physics::kInvalidParticleSystem);
    EXPECT_NE(Physics::NextParticleBackendGeneration(), 0u);
    const auto first = Physics::NextParticleBackendGeneration();
    const auto second = Physics::NextParticleBackendGeneration();
    EXPECT_NE(first, second);
}

// ---------------------------------------------------------------- soft failure

TEST(PbdParticleBackendTest, ADriverThatRefusesASystemFailsTheBackendOnceAndSoftly)
{
    Rig rig;
    rig.driver->failCreate = true;
    WarningCapture warnings;

    EXPECT_EQ(rig.backend->CreateSystem(MakeDesc(10)), Physics::kInvalidParticleSystem);
    EXPECT_TRUE(rig.backend->HasFailed());
    EXPECT_NE(rig.backend->GetFailureReason().find("out of GPU memory"), std::string::npos);
    EXPECT_EQ(warnings.warnings.size(), 1u);

    // From then on it makes no driver call and says nothing more
    EXPECT_EQ(rig.backend->CreateSystem(MakeDesc(10)), Physics::kInvalidParticleSystem);
    EXPECT_EQ(rig.driver->createCalls, 1);
    EXPECT_EQ(warnings.warnings.size(), 1u);
}

TEST(PbdParticleBackendTest, AFailedUploadFailsTheBackendAndStopsFurtherSpawning)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10, 100.0f));
    WarningCapture warnings;
    rig.driver->failUpload = true;
    static_cast<void>(rig.backend->Spawn(handle, Row(2, 0, 0), Still(2)));
    EXPECT_TRUE(rig.backend->HasFailed());
    EXPECT_EQ(warnings.warnings.size(), 1u);
    EXPECT_EQ(rig.backend->Spawn(handle, Row(2, 0, 0), Still(2)), 0u);
    EXPECT_EQ(rig.driver->uploadCalls, 1);
}

TEST(PbdParticleBackendTest, AFailedReadBackFailsTheBackendAndLaterStepsDoNothing)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10, 100.0f));
    ASSERT_EQ(rig.backend->Spawn(handle, Row(2, 0, 0), Still(2)), 2u);
    WarningCapture warnings;
    rig.driver->failReadBack = true;
    rig.backend->OnSceneStepped(0.1f);
    EXPECT_TRUE(rig.backend->HasFailed());
    EXPECT_EQ(warnings.warnings.size(), 1u);
    rig.backend->OnSceneStepped(0.1f);
    EXPECT_EQ(rig.driver->readBackCalls, 1);
    EXPECT_EQ(warnings.warnings.size(), 1u);
}

TEST(PbdParticleBackendTest, AFailedBackendStillReleasesWhatItHolds)
{
    Rig rig;
    const ParticleSystemHandle handle = rig.backend->CreateSystem(MakeDesc(10));
    rig.driver->failCreate = true;
    static_cast<void>(rig.backend->CreateSystem(MakeDesc(10)));
    ASSERT_TRUE(rig.backend->HasFailed());

    rig.backend->Destroy(handle);
    EXPECT_EQ(rig.driver->destroyCalls, 1);
    EXPECT_TRUE(rig.driver->systems.empty());
}

TEST(PbdParticleBackendTest, ADriverWithATooSmallMirrorIsRefused)
{
    class SmallDriver final : public IPbdDriver
    {
    public:
        [[nodiscard]] std::string GetDeviceName() const override { return "small"; }
        [[nodiscard]] Math::Vector3 GetSceneGravity() const override { return Math::Vector3(0, -10, 0); }
        [[nodiscard]] PbdSystemId CreateSystem(const PbdSystemDesc &, std::string &) override { return 1; }
        void DestroySystem(PbdSystemId) override { ++destroyed; }
        [[nodiscard]] PbdStaging GetStaging(PbdSystemId) override { return {}; }
        [[nodiscard]] std::uint32_t GetPhase(PbdSystemId) const override { return 0; }
        void SetMaterial(PbdSystemId, const PbdMaterialParams &) override {}
        [[nodiscard]] bool Upload(PbdSystemId, std::uint32_t, std::uint32_t, std::string &) override { return true; }
        [[nodiscard]] bool ReadBack(PbdSystemId, std::uint32_t, std::string &) override { return true; }
        int destroyed = 0;
    };

    auto driver = std::make_unique<SmallDriver>();
    SmallDriver *raw = driver.get();
    PbdParticleBackend backend(std::move(driver));
    WarningCapture warnings;
    EXPECT_EQ(backend.CreateSystem(MakeDesc(10)), Physics::kInvalidParticleSystem);
    EXPECT_TRUE(backend.HasFailed());
    EXPECT_EQ(raw->destroyed, 1) << "the half-made system is released";
}
