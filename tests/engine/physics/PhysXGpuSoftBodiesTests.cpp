#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "../softbody/SoftBodyTestSupport.hpp"
#include "engine/Health.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/IParticleBackend.hpp"
#include "engine/physics/ISoftBodyBackend.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/physics/physx/PhysXGpuSupport.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/softbody/Tetrahedraliser.hpp"

#if N2ENGINE_PHYSX_GPU_PROBE
#include <PxPhysicsAPI.h>
#include <extensions/PxDefaultErrorCallback.h>
#endif

// The GPU golden test for soft bodies: real PhysX deformable volumes on a real CUDA device. It is registered with
// ctest (it is part of engine_tests) but SKIPPED unless the machine can run it: a build with the PhysX GPU library
// (Windows), not a GitHub Actions runner, PxGetSuggestedCudaDeviceOrdinal() >= 0 and a GPU scene (with its soft-body
// backend) that actually came up. So it reports "skipped" on every CI leg and does its work on a Windows PC with an
// NVIDIA GPU. See docs/soft-bodies.html ("Running the GPU test") and docs/gpu-softbodies-checklist.html.

using namespace N2Engine;
using namespace N2Engine::Physics;

namespace
{
    [[maybe_unused]] constexpr float kStep = 1.0f / 60.0f;

#if N2ENGINE_PHYSX_GPU_PROBE
    /// Empty when the test can run, else why it is skipped
    std::string SkipReason()
    {
        // CI never loads the GPU library: GitHub Actions runners have no GPU, so the test is skipped before any CUDA call
        char *ci = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&ci, &length, "GITHUB_ACTIONS") == 0 && ci != nullptr)
        {
            std::free(ci);
            return "GitHub Actions has no GPU; run this test on a Windows PC with an NVIDIA GPU";
        }
        physx::PxDefaultErrorCallback errors;
        if (PxGetSuggestedCudaDeviceOrdinal(errors) < 0)
        {
            return "no CUDA-capable NVIDIA GPU (PxGetSuggestedCudaDeviceOrdinal < 0)";
        }
        return {};
    }

    struct Extent
    {
        Math::Vector3 min{1e30f, 1e30f, 1e30f};
        Math::Vector3 max{-1e30f, -1e30f, -1e30f};
        Math::Vector3 size() const { return Math::Vector3(max.x - min.x, max.y - min.y, max.z - min.z); }
    };

    Extent ExtentOf(const std::vector<Math::Vector3> &points)
    {
        Extent extent;
        for (const Math::Vector3 &p : points)
        {
            extent.min = Math::Vector3(std::min(extent.min.x, p.x), std::min(extent.min.y, p.y),
                                       std::min(extent.min.z, p.z));
            extent.max = Math::Vector3(std::max(extent.max.x, p.x), std::max(extent.max.y, p.y),
                                       std::max(extent.max.z, p.z));
        }
        return extent;
    }

    bool AllFinite(const std::vector<Math::Vector3> &points)
    {
        return std::ranges::all_of(points,
                                   [](const Math::Vector3 &p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); });
    }

    /// A static floor whose top is the plane y = 0
    void AddFloor(PhysXBackend &backend)
    {
        const PhysicsBodyHandle floor = backend.CreateStaticBody(Math::Vector3(0.0f, 0.0f, 0.0f), Math::Quaternion(), nullptr);
        ASSERT_NE(floor, INVALID_PHYSICS_HANDLE);
        backend.AddBoxCollider(floor, nullptr, Math::Vector3(20.0f, 0.5f, 20.0f), Math::Vector3(0.0f, -0.5f, 0.0f),
                               PhysicsMaterial::Default());
    }

    SoftBodyDesc CubeBody(const Math::Vector3 &position)
    {
        return TestSupport::MakeBoxDesc(6, position);
    }

    SoftBodyDesc SphereBody(const Math::Vector3 &position)
    {
        const Renderer::Common::MeshData sphere = Rendering::Mesh::MakeSphere();
        SoftBodies::TetSettings settings;
        settings.resolution = 6;
        SoftBodyDesc desc;
        desc.tetMesh = SoftBodies::Tetrahedralise(sphere.vertices, sphere.indices, settings).mesh;
        desc.localToWorld[3] = position.x;
        desc.localToWorld[7] = position.y;
        desc.localToWorld[11] = position.z;
        return desc;
    }
#endif
}

TEST(PhysXGpuSoftBodiesGoldenTest, BodiesFallRestOnTheFloorAndReleaseCleanly)
{
#if N2ENGINE_PHYSX_GPU_PROBE
    if (const std::string reason = SkipReason(); !reason.empty())
    {
        GTEST_SKIP() << reason;
    }

    PhysXBackend backend;
    backend.SetGpuMode(GpuMode::On);
    ASSERT_TRUE(backend.Initialize());
    ISoftBodyBackend *soft = backend.GetSoftBodyBackend();
    if (backend.GetGpuStatus().state != GpuState::Ready || soft == nullptr)
    {
        GTEST_SKIP() << "the GPU scene did not come up (is PhysXGpu_64.dll next to the test executable?): "
                     << backend.GetGpuStatus().detail;
    }
    EXPECT_EQ(soft->GetName(), "PhysX");
    const SubsystemStatus row = MakeSoftBodiesStatus(backend.GetSoftBodiesGpuStatus());
    EXPECT_EQ(row.state, SubsystemState::Running) << row.detail;
    EXPECT_NE(row.detail.find("PhysX GPU:"), std::string::npos) << row.detail;
    std::cout << "[  GPU     ] " << backend.GetGpuStatus().detail << "\n";

    AddFloor(backend);

    // --- a cube and a sphere, 3 m above the floor ---------------------------------------------------------------
    const SoftBodyHandle cube = soft->CreateBody(CubeBody(Math::Vector3(-2.0f, 3.0f, 0.0f)));
    ASSERT_NE(cube, kInvalidSoftBody) << "PhysX could not create a deformable volume on this GPU: "
                                      << backend.GetSoftBodiesGpuStatus().detail;
    const SoftBodyHandle sphere = soft->CreateBody(SphereBody(Math::Vector3(2.0f, 3.0f, 0.0f)));
    ASSERT_NE(sphere, kInvalidSoftBody);
    EXPECT_NE(cube, sphere);
    EXPECT_TRUE(soft->OwnsHandle(cube));

    // The bodies exist at their rest shape before the first step
    std::vector<Math::Vector3> cubeSurface;
    std::vector<Math::Vector3> sphereSurface;
    soft->CopySurfacePositions(cube, cubeSurface);
    soft->CopySurfacePositions(sphere, sphereSurface);
    ASSERT_FALSE(cubeSurface.empty());
    ASSERT_FALSE(sphereSurface.empty());
    const float cubeStartY = TestSupport::Centroid(cubeSurface).y;
    const float sphereStartY = TestSupport::Centroid(sphereSurface).y;
    EXPECT_NEAR(cubeStartY, 3.0f, 0.1f);
    EXPECT_NEAR(sphereStartY, 3.0f, 0.1f);
    const Extent cubeRest = ExtentOf(cubeSurface);
    EXPECT_NEAR(cubeRest.size().y, 1.0f, 0.05f);

    // Half a second of simulation: both fall under the scene's gravity
    for (int i = 0; i < 30; ++i)
    {
        backend.Update(kStep);
    }
    soft->CopySurfacePositions(cube, cubeSurface);
    soft->CopySurfacePositions(sphere, sphereSurface);
    ASSERT_TRUE(AllFinite(cubeSurface)) << "the cube's positions are not finite";
    ASSERT_TRUE(AllFinite(sphereSurface)) << "the sphere's positions are not finite";
    EXPECT_GT(cubeStartY - TestSupport::Centroid(cubeSurface).y, 0.4f) << "free fall for 0.5 s is about 1.2 m";
    EXPECT_GT(sphereStartY - TestSupport::Centroid(sphereSurface).y, 0.4f);

    // Four more seconds: they land on the floor and come to rest above it without exploding
    for (int i = 0; i < 240; ++i)
    {
        backend.Update(kStep);
        if (i % 30 == 0)
        {
            soft->CopySurfacePositions(cube, cubeSurface);
            soft->CopySurfacePositions(sphere, sphereSurface);
            ASSERT_TRUE(AllFinite(cubeSurface)) << "step " << i;
            ASSERT_TRUE(AllFinite(sphereSurface)) << "step " << i;
        }
    }
    soft->CopySurfacePositions(cube, cubeSurface);
    soft->CopySurfacePositions(sphere, sphereSurface);
    const Extent cubeEnd = ExtentOf(cubeSurface);
    const Extent sphereEnd = ExtentOf(sphereSurface);
    EXPECT_GT(cubeEnd.min.y, -0.15f) << "the cube did not sink through the floor";
    EXPECT_GT(sphereEnd.min.y, -0.15f) << "the sphere did not sink through the floor";
    EXPECT_LT(cubeEnd.min.y, 0.3f) << "and it came down to it";
    EXPECT_LT(sphereEnd.min.y, 0.3f);
    EXPECT_LT(TestSupport::Centroid(cubeSurface).y, 1.0f);
    // Bounded volume change: a squashed 1 m cube stays within sane bounds of its rest size
    EXPECT_GT(cubeEnd.size().y, 0.3f);
    EXPECT_LT(cubeEnd.size().y, 1.3f);
    EXPECT_LT(cubeEnd.size().x, 1.6f);
    EXPECT_LT(cubeEnd.size().z, 1.6f);
    EXPECT_LT(sphereEnd.size().x, 1.6f);
    std::cout << "[  GPU     ] cube at rest: height " << cubeEnd.size().y << ", sphere lowest point "
              << sphereEnd.min.y << "\n";

    // --- AddVelocity moves a body ------------------------------------------------------------------------------
    const float x = TestSupport::Centroid(cubeSurface).x;
    soft->AddVelocity(cube, Math::Vector3(4.0f, 0.0f, 0.0f));
    for (int i = 0; i < 30; ++i)
    {
        backend.Update(kStep);
    }
    soft->CopySurfacePositions(cube, cubeSurface);
    ASSERT_TRUE(AllFinite(cubeSurface));
    EXPECT_GT(TestSupport::Centroid(cubeSurface).x, x + 0.3f) << "a push of 4 m/s moves the body";

    // --- parameters can change on a live body ------------------------------------------------------------------
    SoftBodyParams params;
    params.stiffness = 5.0e4f;
    params.materialModel = SoftMaterialModel::NeoHookean;
    soft->SetParams(sphere, params);
    backend.Update(kStep);
    soft->CopySurfacePositions(sphere, sphereSurface);
    EXPECT_TRUE(AllFinite(sphereSurface));

    // --- Destroy: a destroyed body's handle is dead ------------------------------------------------------------
    soft->Destroy(cube);
    soft->CopySurfacePositions(cube, cubeSurface);
    EXPECT_TRUE(cubeSurface.empty());
    backend.Update(kStep);
    soft->CopySurfacePositions(sphere, sphereSurface);
    EXPECT_FALSE(sphereSurface.empty()) << "the other body is unaffected";
    soft->Destroy(sphere);

    // --- exit releases cleanly: shutting the backend down with a body still alive must not crash or hang -------
    const SoftBodyHandle left = soft->CreateBody(CubeBody(Math::Vector3(0.0f, 3.0f, 0.0f)));
    ASSERT_NE(left, kInvalidSoftBody);
    backend.Update(kStep);
    backend.Shutdown();
    EXPECT_EQ(backend.GetSoftBodyBackend(), nullptr) << "the soft-body backend goes with the scene";

    // A second backend starts from nothing and the old handle means nothing to it
    PhysXBackend again;
    again.SetGpuMode(GpuMode::On);
    ASSERT_TRUE(again.Initialize());
    ASSERT_NE(again.GetSoftBodyBackend(), nullptr);
    std::vector<Math::Vector3> none;
    again.GetSoftBodyBackend()->CopySurfacePositions(left, none);
    EXPECT_TRUE(none.empty());
    again.Shutdown();
#else
    GTEST_SKIP() << "needs a PhysX build with the GPU library (Windows); this build has none";
#endif
}

TEST(PhysXGpuSoftBodiesGoldenTest, ASoftBodyAndPbdParticlesCoexistInOneScene)
{
#if N2ENGINE_PHYSX_GPU_PROBE
    if (const std::string reason = SkipReason(); !reason.empty())
    {
        GTEST_SKIP() << reason;
    }

    PhysXBackend backend;
    backend.SetGpuMode(GpuMode::On);
    ASSERT_TRUE(backend.Initialize());
    ISoftBodyBackend *soft = backend.GetSoftBodyBackend();
    IParticleBackend *particles = backend.GetParticleBackend();
    if (backend.GetGpuStatus().state != GpuState::Ready || soft == nullptr || particles == nullptr)
    {
        GTEST_SKIP() << "the GPU scene did not come up (is PhysXGpu_64.dll next to the test executable?): "
                     << backend.GetGpuStatus().detail;
    }
    AddFloor(backend);

    const SoftBodyHandle body = soft->CreateBody(CubeBody(Math::Vector3(0.0f, 3.0f, 0.0f)));
    ASSERT_NE(body, kInvalidSoftBody) << "PhysX could not create a deformable volume on this GPU";

    ParticleSystemDesc desc;
    desc.maxParticles = 64;
    desc.material = ParticleMaterial::Water;
    desc.params.lifetimeMin = 100.0f;
    desc.params.lifetimeMax = 100.0f;
    const ParticleSystemHandle water = particles->CreateSystem(desc);
    ASSERT_NE(water, kInvalidParticleSystem) << "PhysX could not create a PBD particle system next to a soft body";
    std::vector<Math::Vector3> positions;
    for (int x = 0; x < 4; ++x)
    {
        for (int z = 0; z < 4; ++z)
        {
            positions.emplace_back(3.0f + 0.12f * static_cast<float>(x), 6.0f, 0.12f * static_cast<float>(z));
        }
    }
    const std::vector<Math::Vector3> velocities(positions.size());
    ASSERT_EQ(particles->Spawn(water, positions, velocities), positions.size());

    std::vector<Math::Vector3> surface;
    soft->CopySurfacePositions(body, surface);
    const float startY = TestSupport::Centroid(surface).y;
    std::vector<Math::Vector4> before;
    particles->CopyPositions(water, before);

    for (int i = 0; i < 60; ++i)
    {
        backend.Update(kStep);
    }
    soft->CopySurfacePositions(body, surface);
    ASSERT_TRUE(AllFinite(surface));
    EXPECT_LT(TestSupport::Centroid(surface).y, startY - 0.3f) << "the soft body falls";
    std::vector<Math::Vector4> after;
    particles->CopyPositions(water, after);
    ASSERT_EQ(after.size(), before.size());
    float fall = 0.0f;
    for (std::size_t i = 0; i < after.size(); ++i)
    {
        ASSERT_TRUE(std::isfinite(after[i].x) && std::isfinite(after[i].y) && std::isfinite(after[i].z));
        fall += before[i].y - after[i].y;
    }
    EXPECT_GT(fall / static_cast<float>(after.size()), 0.5f) << "and so do the particles";

    // Neither feature failed, and the scene is still the GPU scene
    EXPECT_EQ(backend.GetGpuStatus().state, GpuState::Ready);
    EXPECT_EQ(backend.GetParticlesGpuStatus().state, GpuState::Ready);
    EXPECT_EQ(backend.GetSoftBodiesGpuStatus().state, GpuState::Ready);

    soft->Destroy(body);
    particles->Destroy(water);
    backend.Update(kStep);
    backend.Shutdown();
#else
    GTEST_SKIP() << "needs a PhysX build with the GPU library (Windows); this build has none";
#endif
}
