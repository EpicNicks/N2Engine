#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/IParticleBackend.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/physics/physx/PhysXGpuSupport.hpp"

#if N2ENGINE_PHYSX_GPU_PROBE
#include <PxPhysicsAPI.h>
#include <extensions/PxDefaultErrorCallback.h>
#endif

// The GPU golden test: real PhysX PBD particles on a real CUDA device. It is registered with ctest (it is part of
// engine_tests) but SKIPPED unless the machine can run it: a build with the PhysX GPU library (Windows), not a
// GitHub Actions runner, PxGetSuggestedCudaDeviceOrdinal() >= 0 and a GPU scene that actually came up. So it
// reports "skipped" on every CI leg and does its work on a Windows PC with an NVIDIA GPU. See docs/particles.html
// ("Running the GPU test") for how to run it and what to expect.

using namespace N2Engine;
using namespace N2Engine::Physics;

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    std::vector<Math::Vector3> Block(const int perSide, const Math::Vector3 &origin, const float spacing)
    {
        std::vector<Math::Vector3> points;
        for (int x = 0; x < perSide; ++x)
        {
            for (int y = 0; y < perSide; ++y)
            {
                for (int z = 0; z < perSide; ++z)
                {
                    points.emplace_back(origin.x + static_cast<float>(x) * spacing,
                                        origin.y + static_cast<float>(y) * spacing,
                                        origin.z + static_cast<float>(z) * spacing);
                }
            }
        }
        return points;
    }

    float MeanY(const std::vector<Math::Vector4> &particles)
    {
        float sum = 0.0f;
        for (const Math::Vector4 &p : particles)
        {
            sum += p.y;
        }
        return particles.empty() ? 0.0f : sum / static_cast<float>(particles.size());
    }
}

TEST(PhysXGpuParticlesGoldenTest, SpawnedParticlesFallRespectTheCapAndReleaseCleanly)
{
#if N2ENGINE_PHYSX_GPU_PROBE
    // CI never loads the GPU library: GitHub Actions runners have no GPU, so the test is skipped before any CUDA call
    char *ci = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&ci, &length, "GITHUB_ACTIONS") == 0 && ci != nullptr)
    {
        std::free(ci);
        GTEST_SKIP() << "GitHub Actions has no GPU; run this test on a Windows PC with an NVIDIA GPU";
    }

    physx::PxDefaultErrorCallback errors;
    if (PxGetSuggestedCudaDeviceOrdinal(errors) < 0)
    {
        GTEST_SKIP() << "no CUDA-capable NVIDIA GPU (PxGetSuggestedCudaDeviceOrdinal < 0)";
    }

    PhysXBackend backend;
    backend.SetGpuMode(GpuMode::On);
    ASSERT_TRUE(backend.Initialize());
    IParticleBackend *particles = backend.GetParticleBackend();
    if (backend.GetGpuStatus().state != GpuState::Ready || particles == nullptr)
    {
        GTEST_SKIP() << "the GPU scene did not come up (is PhysXGpu_64.dll next to the test executable?): "
                     << backend.GetGpuStatus().detail;
    }
    EXPECT_EQ(particles->GetName(), "PhysX");
    EXPECT_NE(backend.GetGpuStatus().detail.find("PhysX GPU:"), std::string::npos) << backend.GetGpuStatus().detail;
    std::cout << "[  GPU     ] " << backend.GetGpuStatus().detail << "\n";

    // --- a water system: 5x5x5 = 125 particles offered to a buffer of 100 -------------------------------------
    ParticleSystemDesc desc;
    desc.maxParticles = 100;
    desc.material = ParticleMaterial::Water;
    desc.params.lifetimeMin = 100.0f; // nothing expires during the test
    desc.params.lifetimeMax = 100.0f;
    const ParticleSystemHandle water = particles->CreateSystem(desc);
    ASSERT_NE(water, kInvalidParticleSystem) << "PhysX could not create a PBD particle system on this GPU";
    EXPECT_NE(water, 0u);

    const std::vector<Math::Vector3> positions = Block(5, Math::Vector3(0.0f, 20.0f, 0.0f), 0.12f);
    const std::vector<Math::Vector3> velocities(positions.size(), Math::Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_EQ(positions.size(), 125u);
    EXPECT_EQ(particles->Spawn(water, positions, velocities), 100u) << "the cap: 100 of the 125 fit";
    EXPECT_EQ(particles->GetAliveCount(water), 100u);
    EXPECT_EQ(particles->Spawn(water, positions, velocities), 0u) << "full";

    std::vector<Math::Vector4> before;
    particles->CopyPositions(water, before);
    ASSERT_EQ(before.size(), 100u);
    const float startY = MeanY(before);
    EXPECT_NEAR(startY, 20.0f + 0.12f * 2.0f, 0.3f);

    // Half a second of simulation, with no colliders: the particles fall under gravity
    for (int i = 0; i < 30; ++i)
    {
        backend.Update(kStep);
    }
    std::vector<Math::Vector4> after;
    particles->CopyPositions(water, after);
    ASSERT_EQ(after.size(), 100u) << "none expired, none were lost";
    for (const Math::Vector4 &p : after)
    {
        ASSERT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    }
    const float fallen = startY - MeanY(after);
    EXPECT_GT(fallen, 0.5f) << "free fall for 0.5 s is about 1.2 m; at least half a metre of it must show";
    EXPECT_LT(fallen, 3.0f) << "and not far more than gravity gives";

    // --- a second system whose particles expire ----------------------------------------------------------------
    ParticleSystemDesc shortLived = desc;
    shortLived.maxParticles = 16;
    shortLived.params.lifetimeMin = 0.1f;
    shortLived.params.lifetimeMax = 0.1f;
    shortLived.material = ParticleMaterial::Sand;
    const ParticleSystemHandle sand = particles->CreateSystem(shortLived);
    ASSERT_NE(sand, kInvalidParticleSystem);
    const std::vector<Math::Vector3> few = Block(2, Math::Vector3(5.0f, 20.0f, 0.0f), 0.1f);
    ASSERT_EQ(particles->Spawn(sand, few, std::vector<Math::Vector3>(few.size())), few.size());
    EXPECT_EQ(particles->GetAliveCount(sand), few.size());
    for (int i = 0; i < 12; ++i)
    {
        backend.Update(kStep);
    }
    EXPECT_EQ(particles->GetAliveCount(sand), 0u) << "0.2 s is past a 0.1 s lifetime";
    EXPECT_EQ(particles->GetAliveCount(water), 100u) << "the long-lived system is unaffected";

    // --- Clear and Destroy -------------------------------------------------------------------------------------
    particles->Clear(water);
    EXPECT_EQ(particles->GetAliveCount(water), 0u);
    EXPECT_EQ(particles->Spawn(water, positions, velocities), 100u) << "refilled after Clear";
    backend.Update(kStep);
    EXPECT_EQ(particles->GetAliveCount(water), 100u);

    particles->Destroy(water);
    particles->Destroy(sand);
    EXPECT_EQ(particles->GetAliveCount(water), 0u);
    EXPECT_EQ(particles->Spawn(water, positions, velocities), 0u) << "a destroyed system's handle is dead";
    backend.Update(kStep); // stepping a scene with no particle systems left

    // --- exit releases cleanly: shutting the backend down with a system still alive must not crash or hang ------
    const ParticleSystemHandle left = particles->CreateSystem(desc);
    ASSERT_NE(left, kInvalidParticleSystem);
    ASSERT_EQ(particles->Spawn(left, few, std::vector<Math::Vector3>(few.size())), few.size());
    backend.Update(kStep);
    backend.Shutdown();
    EXPECT_EQ(backend.GetParticleBackend(), nullptr) << "the particle backend goes with the scene";

    // A second backend starts from nothing and the old handle means nothing to it
    PhysXBackend again;
    again.SetGpuMode(GpuMode::On);
    ASSERT_TRUE(again.Initialize());
    ASSERT_NE(again.GetParticleBackend(), nullptr);
    EXPECT_EQ(again.GetParticleBackend()->GetAliveCount(left), 0u);
    again.Shutdown();
#else
    GTEST_SKIP() << "needs a PhysX build with the GPU library (Windows); this build has none";
#endif
}
