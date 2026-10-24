#include <gtest/gtest.h>

#ifdef N2ENGINE_PHYSX_ENABLED

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include "../particles/FakePbdDriver.hpp"
#include "../softbody/FakeDeformableDriver.hpp"
#include "../softbody/SoftBodyTestSupport.hpp"
#include "engine/Health.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/IParticleBackend.hpp"
#include "engine/physics/ISoftBodyBackend.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"

// The PhysX physics backend's soft-body wiring with a fake driver (InstallDeformableDriverForTesting), on every leg
// that links PhysX (a CPU scene is initialised with physics.gpu off, so no CUDA call is made on any machine): the
// SoftBodies row (Running only while the backend exists), the soft-body failure chain, the split from the Particles
// row and the scene status, the read-back after each scene step, the release order, and physics.gpu=off unchanged.

using namespace N2Engine;
using namespace N2Engine::Physics;
using TestSupport::FakeDeformableDriver;
using TestSupport::FakePbdDriver;
using TestSupport::SoftBodyWarningCapture;

namespace
{
    /// A physics backend initialised with physics.gpu off, then given the fake drivers and told which GPU mode the
    /// project asked for: the state a real GPU scene would be in, reached without a GPU. The fakes are owned by the
    /// feature backends, so the pointers are only good while those live.
    struct Rig
    {
        explicit Rig(const GpuMode mode, const bool withParticles = false, const bool withSoft = true)
        {
            backend = std::make_unique<PhysXBackend>();
            backend->SetGpuMode(GpuMode::Off);
            if (!backend->Initialize())
            {
                return;
            }
            backend->SetGpuMode(mode); // only read again when a failure is recorded
            if (withParticles)
            {
                auto fake = std::make_unique<FakePbdDriver>();
                particles = fake.get();
                ready = backend->InstallParticleDriverForTesting(std::move(fake));
            }
            if (withSoft)
            {
                auto fake = std::make_unique<FakeDeformableDriver>();
                soft = fake.get();
                ready = backend->InstallDeformableDriverForTesting(std::move(fake)) && (ready || !withParticles);
            }
        }

        std::unique_ptr<PhysXBackend> backend;
        FakeDeformableDriver *soft = nullptr;
        FakePbdDriver *particles = nullptr;
        bool ready = false;
    };
}

TEST(PhysXSoftBodiesBackendTest, ARealPhysXSceneWithGpuOffHasNoSoftBodyBackend)
{
    PhysXBackend backend;
    backend.SetGpuMode(GpuMode::Off);
    ASSERT_TRUE(backend.Initialize());
    EXPECT_EQ(backend.GetSoftBodyBackend(), nullptr);
    EXPECT_FALSE(backend.SceneUsesGpuDynamics());
    EXPECT_FALSE(backend.HasCudaContextManager());
    const SubsystemStatus row = MakeSoftBodiesStatus(backend.GetSoftBodiesGpuStatus());
    EXPECT_EQ(row.name, "SoftBodies");
    EXPECT_EQ(row.state, SubsystemState::Disabled);
    EXPECT_EQ(row.detail, "physics.gpu is off (the CPU scene)");
    backend.Shutdown();
}

TEST(PhysXSoftBodiesBackendTest, TheSeamGivesAReadyGpuWithASoftBodyBackendAndTheRowSaysRunning)
{
    Rig rig(GpuMode::Auto);
    ASSERT_TRUE(rig.ready);
    ISoftBodyBackend *soft = rig.backend->GetSoftBodyBackend();
    ASSERT_NE(soft, nullptr);
    EXPECT_EQ(soft->GetName(), "PhysX");
    EXPECT_EQ(rig.backend->GetGpuStatus().state, GpuState::Ready);

    const SubsystemStatus row = MakeSoftBodiesStatus(rig.backend->GetSoftBodiesGpuStatus());
    EXPECT_EQ(row.state, SubsystemState::Running);
    EXPECT_EQ(row.detail, "PhysX GPU: Fake GPU");
    EXPECT_TRUE((EngineHealth{.subsystems = {row}}).IsHealthy());

    // One soft-body backend per scene
    EXPECT_FALSE(rig.backend->InstallDeformableDriverForTesting(std::make_unique<FakeDeformableDriver>()));
    EXPECT_EQ(rig.backend->GetSoftBodyBackend(), soft);
    rig.backend->Shutdown();
}

TEST(PhysXSoftBodiesBackendTest, TheRowSaysRunningOnlyWhileTheBackendExists)
{
    // A Ready scene with only the particle feature: the SoftBodies row is not Running for a feature that isn't there
    Rig rig(GpuMode::Auto, true, false);
    ASSERT_TRUE(rig.backend != nullptr);
    ASSERT_NE(rig.particles, nullptr);
    EXPECT_EQ(rig.backend->GetGpuStatus().state, GpuState::Ready);
    EXPECT_EQ(rig.backend->GetSoftBodyBackend(), nullptr);
    const SubsystemStatus without = MakeSoftBodiesStatus(rig.backend->GetSoftBodiesGpuStatus());
    EXPECT_EQ(without.state, SubsystemState::Disabled);
    EXPECT_NE(without.detail.find("no PhysX soft-body backend"), std::string::npos) << without.detail;
    EXPECT_EQ(MakeParticlesStatus(rig.backend->GetParticlesGpuStatus()).state, SubsystemState::Running);

    // Installing the soft-body backend makes it Running, and leaves the scene and the Particles row as they were
    const GpuStatus sceneBefore = rig.backend->GetGpuStatus();
    const SubsystemStatus particlesBefore = MakeParticlesStatus(rig.backend->GetParticlesGpuStatus());
    ASSERT_TRUE(rig.backend->InstallDeformableDriverForTesting(std::make_unique<FakeDeformableDriver>()));
    EXPECT_EQ(MakeSoftBodiesStatus(rig.backend->GetSoftBodiesGpuStatus()).state, SubsystemState::Running);
    EXPECT_EQ(rig.backend->GetGpuStatus().detail, sceneBefore.detail);
    EXPECT_EQ(MakeParticlesStatus(rig.backend->GetParticlesGpuStatus()).detail, particlesBefore.detail);
    rig.backend->Shutdown();
}

TEST(PhysXSoftBodiesBackendTest, ADriverFailureDropsTheBackendAndSetsOnlyTheSoftBodiesStatus)
{
    struct Case
    {
        GpuMode mode;
        GpuState state;
        SubsystemState row;
        bool healthy;
    };
    // auto: no usable GPU soft bodies is not an error; on: the project asked for the GPU, so it is
    for (const Case c : {Case{GpuMode::Auto, GpuState::Unavailable, SubsystemState::Disabled, true},
                         Case{GpuMode::On, GpuState::Failed, SubsystemState::Failed, false}})
    {
        Rig rig(c.mode, true);
        ASSERT_TRUE(rig.ready);
        IParticleBackend *particles = rig.backend->GetParticleBackend();
        ASSERT_NE(particles, nullptr);
        ISoftBodyBackend *soft = rig.backend->GetSoftBodyBackend();
        ASSERT_NE(soft, nullptr);

        const SoftBodyHandle handle = soft->CreateBody(TestSupport::MakeBoxDesc(3));
        ASSERT_NE(handle, kInvalidSoftBody);
        const GpuStatus sceneBefore = rig.backend->GetGpuStatus();
        const SubsystemStatus particlesBefore = MakeParticlesStatus(rig.backend->GetParticlesGpuStatus());
        ASSERT_EQ(particlesBefore.state, SubsystemState::Running);

        // The GPU refuses a transfer (AddVelocity reads the velocities back; the driver dies with the backend, so it
        // is not touched again). The physics backend is not stepped here: with the fake particle driver installed it
        // would ask a CPU scene for a particle sync.
        SoftBodyWarningCapture warnings;
        rig.soft->failReadBack = true;
        soft->AddVelocity(handle, Math::Vector3(1.0f, 0.0f, 0.0f));

        EXPECT_EQ(rig.backend->GetSoftBodyBackend(), nullptr) << ToString(c.mode) << ": the backend is dropped";
        const GpuStatus status = rig.backend->GetSoftBodiesGpuStatus();
        EXPECT_EQ(status.state, c.state) << ToString(c.mode);
        EXPECT_NE(status.detail.find("GPU soft bodies unavailable: fake: read-back failed"), std::string::npos)
            << status.detail;
        const SubsystemStatus row = MakeSoftBodiesStatus(status);
        EXPECT_EQ(row.state, c.row);
        EXPECT_EQ((EngineHealth{.subsystems = {row}}).IsHealthy(), c.healthy);
        EXPECT_EQ(warnings.Count("GPU soft bodies unavailable"), 1) << "logged once, not on every look";
        EXPECT_EQ(warnings.Count("GPU particles unavailable"), 0);
        EXPECT_EQ(rig.backend->GetSoftBodyBackend(), nullptr);
        EXPECT_EQ(warnings.Count("GPU soft bodies unavailable"), 1) << "still once";

        // The scene status (Physics.IsGpuAvailable follows it) and the Particles row did not move; the particle
        // backend lives on
        EXPECT_EQ(rig.backend->GetGpuStatus().state, sceneBefore.state) << ToString(c.mode);
        EXPECT_EQ(rig.backend->GetGpuStatus().detail, sceneBefore.detail);
        const SubsystemStatus particlesAfter = MakeParticlesStatus(rig.backend->GetParticlesGpuStatus());
        EXPECT_EQ(particlesAfter.state, SubsystemState::Running);
        EXPECT_EQ(particlesAfter.detail, particlesBefore.detail);
        EXPECT_EQ(rig.backend->GetParticleBackend(), particles);

        // Installing a fresh driver after the failure restores the SoftBodies row
        ASSERT_TRUE(rig.backend->InstallDeformableDriverForTesting(std::make_unique<FakeDeformableDriver>()));
        EXPECT_NE(rig.backend->GetSoftBodyBackend(), nullptr) << ToString(c.mode);
        EXPECT_EQ(MakeSoftBodiesStatus(rig.backend->GetSoftBodiesGpuStatus()).state, SubsystemState::Running);
        rig.backend->Shutdown();
    }
}

TEST(PhysXSoftBodiesBackendTest, ARefusedBodyOrAFailedTransferNeverTouchesTheParticlesRowOrTheScene)
{
    Rig rig(GpuMode::Auto, true);
    ASSERT_TRUE(rig.ready);
    IParticleBackend *particles = rig.backend->GetParticleBackend();
    ASSERT_NE(particles, nullptr);
    const GpuStatus sceneBefore = rig.backend->GetGpuStatus();
    const SubsystemStatus particlesBefore = MakeParticlesStatus(rig.backend->GetParticlesGpuStatus());

    // A refused body: the volume cap or an out-of-memory GPU
    rig.soft->failCreate = true;
    ISoftBodyBackend *soft = rig.backend->GetSoftBodyBackend();
    ASSERT_NE(soft, nullptr);
    EXPECT_EQ(soft->CreateBody(TestSupport::MakeBoxDesc(3)), kInvalidSoftBody);
    EXPECT_EQ(rig.backend->GetSoftBodyBackend(), nullptr);
    EXPECT_EQ(rig.backend->GetSoftBodiesGpuStatus().state, GpuState::Unavailable);

    EXPECT_EQ(rig.backend->GetGpuStatus().state, sceneBefore.state);
    EXPECT_EQ(rig.backend->GetGpuStatus().detail, sceneBefore.detail);
    EXPECT_EQ(MakeParticlesStatus(rig.backend->GetParticlesGpuStatus()).state, particlesBefore.state);
    EXPECT_EQ(MakeParticlesStatus(rig.backend->GetParticlesGpuStatus()).detail, particlesBefore.detail);
    EXPECT_EQ(rig.backend->GetParticleBackend(), particles);
    rig.backend->Shutdown();
}

TEST(PhysXSoftBodiesBackendTest, AParticleFailureLeavesTheSoftBodiesRowAndTheBodiesAlone)
{
    Rig rig(GpuMode::Auto, true);
    ASSERT_TRUE(rig.ready);
    ISoftBodyBackend *soft = rig.backend->GetSoftBodyBackend();
    ASSERT_NE(soft, nullptr);
    const SoftBodyHandle body = soft->CreateBody(TestSupport::MakeBoxDesc(3));
    ASSERT_NE(body, kInvalidSoftBody);
    const SubsystemStatus softBefore = MakeSoftBodiesStatus(rig.backend->GetSoftBodiesGpuStatus());
    ASSERT_EQ(softBefore.state, SubsystemState::Running);

    IParticleBackend *particles = rig.backend->GetParticleBackend();
    ASSERT_NE(particles, nullptr);
    ParticleSystemDesc desc;
    desc.maxParticles = 16;
    const ParticleSystemHandle handle = particles->CreateSystem(desc);
    ASSERT_NE(handle, kInvalidParticleSystem);
    rig.particles->failUpload = true;
    const Math::Vector3 at(0.0f, 1.0f, 0.0f);
    const Math::Vector3 velocity(0.0f, 0.0f, 0.0f);
    particles->Spawn(handle, std::span<const Math::Vector3>(&at, 1), std::span<const Math::Vector3>(&velocity, 1));
    EXPECT_EQ(rig.backend->GetParticleBackend(), nullptr) << "the particle backend gave up";
    EXPECT_EQ(rig.backend->GetParticlesGpuStatus().state, GpuState::Unavailable);

    // ... and the soft-body feature carries on
    EXPECT_EQ(rig.backend->GetSoftBodyBackend(), soft);
    const SubsystemStatus softAfter = MakeSoftBodiesStatus(rig.backend->GetSoftBodiesGpuStatus());
    EXPECT_EQ(softAfter.state, SubsystemState::Running);
    EXPECT_EQ(softAfter.detail, softBefore.detail);
    EXPECT_EQ(rig.backend->GetGpuStatus().state, GpuState::Ready);
    std::vector<Math::Vector3> surface;
    soft->CopySurfacePositions(body, surface);
    EXPECT_FALSE(surface.empty());
    rig.backend->Shutdown();
}

TEST(PhysXSoftBodiesBackendTest, AReadBackFailureAfterASceneStepDropsTheBackendOnce)
{
    for (const GpuMode mode : {GpuMode::Auto, GpuMode::On})
    {
        Rig rig(mode);
        ASSERT_TRUE(rig.ready);
        ISoftBodyBackend *soft = rig.backend->GetSoftBodyBackend();
        ASSERT_NE(soft, nullptr);
        ASSERT_NE(soft->CreateBody(TestSupport::MakeBoxDesc(3)), kInvalidSoftBody);
        rig.backend->Update(0.02f);
        EXPECT_EQ(rig.soft->readBackCalls, 1);
        EXPECT_NE(rig.backend->GetSoftBodyBackend(), nullptr);

        SoftBodyWarningCapture warnings;
        rig.soft->failReadBack = true;
        rig.backend->Update(0.02f); // the step runs, then the read-back fails

        EXPECT_EQ(rig.backend->GetSoftBodyBackend(), nullptr) << ToString(mode);
        EXPECT_EQ(rig.backend->GetSoftBodiesGpuStatus().state,
                  mode == GpuMode::On ? GpuState::Failed : GpuState::Unavailable);
        EXPECT_EQ(rig.backend->GetGpuStatus().state, GpuState::Ready) << "the scene itself carries on";
        EXPECT_EQ(warnings.Count("GPU soft bodies unavailable"), 1);
        rig.backend->Update(0.02f);
        rig.backend->Update(0.02f);
        EXPECT_EQ(warnings.Count("GPU soft bodies unavailable"), 1) << "once";
        rig.backend->Shutdown();
    }
}

TEST(PhysXSoftBodiesBackendTest, EverySceneStepReadsEveryBodyBackOnce)
{
    Rig rig(GpuMode::Auto);
    ASSERT_TRUE(rig.ready);
    ISoftBodyBackend *soft = rig.backend->GetSoftBodyBackend();
    ASSERT_NE(soft, nullptr);
    const SoftBodyDesc desc = TestSupport::MakeBoxDesc(3);
    ASSERT_NE(soft->CreateBody(desc), kInvalidSoftBody);
    ASSERT_NE(soft->CreateBody(desc), kInvalidSoftBody);
    EXPECT_EQ(rig.soft->readBackCalls, 0) << "creating reads nothing back";

    for (int i = 0; i < 3; ++i)
    {
        rig.backend->Update(0.02f);
    }
    EXPECT_EQ(rig.soft->readBackCalls, 6) << "3 steps, 2 bodies";
    rig.backend->Shutdown();
}

TEST(PhysXSoftBodiesBackendTest, ShutdownReleasesTheBodiesBeforeTheDriverAndTheScene)
{
    std::vector<std::string> log;
    PhysXBackend backend;
    backend.SetGpuMode(GpuMode::Off);
    ASSERT_TRUE(backend.Initialize());
    backend.SetGpuMode(GpuMode::Auto);
    auto driver = std::make_unique<FakeDeformableDriver>();
    driver->log = &log;
    ASSERT_TRUE(backend.InstallDeformableDriverForTesting(std::move(driver)));
    ISoftBodyBackend *soft = backend.GetSoftBodyBackend();
    ASSERT_NE(soft, nullptr);
    ASSERT_NE(soft->CreateBody(TestSupport::MakeBoxDesc(3)), kInvalidSoftBody);
    ASSERT_NE(soft->CreateBody(TestSupport::MakeBoxDesc(3)), kInvalidSoftBody);

    backend.Shutdown();
    const std::vector<std::string> expected = {"create 1", "create 2", "destroy 1", "destroy 2", "driver destroyed"};
    EXPECT_EQ(log, expected) << "the volumes go, then the driver, all before the scene";
    EXPECT_EQ(backend.GetSoftBodyBackend(), nullptr);
    EXPECT_EQ(MakeSoftBodiesStatus(backend.GetSoftBodiesGpuStatus()).state, SubsystemState::Disabled)
        << "no backend, no Running row";
}

namespace
{
    // A box that falls under gravity on a CPU scene: its height after N steps
    std::vector<float> FallingBox(PhysXBackend &backend, const int steps)
    {
        const PhysicsBodyHandle body = backend.CreateDynamicBody(Math::Vector3(0.0f, 10.0f, 0.0f), Math::Quaternion(),
                                                                  2.0f, nullptr, false);
        backend.AddBoxCollider(body, nullptr, Math::Vector3(0.5f, 0.5f, 0.5f), Math::Vector3(0.0f, 0.0f, 0.0f),
                               PhysicsMaterial::Default());
        std::vector<float> heights;
        for (int i = 0; i < steps; ++i)
        {
            backend.Update(1.0f / 60.0f);
            heights.push_back(backend.GetPosition(body).y);
        }
        return heights;
    }
}

TEST(PhysXSoftBodiesBackendTest, PhysicsGpuOffKeepsTheCpuSceneAsItWas)
{
    // The soft-body wiring adds nothing to a CPU scene: the same rigid body falls identically with and without it
    PhysXBackend plain;
    plain.SetGpuMode(GpuMode::Off);
    ASSERT_TRUE(plain.Initialize());
    EXPECT_EQ(plain.GetSoftBodyBackend(), nullptr);
    const std::vector<float> expected = FallingBox(plain, 30);
    plain.Shutdown();

    Rig wired(GpuMode::Auto); // the soft-body backend only (a particle fake would ask a CPU scene for a particle sync)
    ASSERT_TRUE(wired.ready);
    const std::vector<float> actual = FallingBox(*wired.backend, 30);
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        EXPECT_FLOAT_EQ(actual[i], expected[i]) << "step " << i;
    }
    EXPECT_LT(actual.back(), 10.0f - 0.5f) << "it fell";
    wired.backend->Shutdown();
}

#endif
