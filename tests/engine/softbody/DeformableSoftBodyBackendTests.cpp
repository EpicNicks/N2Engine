#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <math/Vector3.hpp>

#include "FakeDeformableDriver.hpp"
#include "SoftBodyTestSupport.hpp"
#include "engine/physics/CpuSoftBodyBackend.hpp"
#include "engine/physics/ISoftBodyBackend.hpp"
#include "engine/physics/TetMeshData.hpp"
#include "engine/softbody/DeformableSoftBodyBackend.hpp"
#include "engine/softbody/TetSkinning.hpp"
#include "engine/softbody/Tetrahedraliser.hpp"

// The PhysX soft-body backend's logic (SoftBodies::DeformableSoftBodyBackend) against a fake driver, on every CI leg:
// handle generations and stale handles, create/destroy/slot reuse, the volume limit and refusals, the SetParams
// mapping, AddVelocity, read-back and skinning (with the same embedding the CPU backend uses), mirrored transforms,
// failure, and release order. No PhysX and no GPU.

using namespace N2Engine;
using namespace N2Engine::Physics;
using SoftBodies::DeformableSoftBodyBackend;
using SoftBodies::DeformableVec4;
using TestSupport::FakeDeformableDriver;
using TestSupport::MakeBoxDesc;
using TestSupport::SoftBodyWarningCapture;

namespace
{
    constexpr float kDt = 0.02f;

    struct Rig
    {
        Rig()
        {
            auto driver = std::make_unique<FakeDeformableDriver>();
            fake = driver.get();
            backend = std::make_unique<DeformableSoftBodyBackend>(std::move(driver));
        }

        /// One scene step: the fake device moves the volumes, then the backend reads them back
        void SceneStep(const float dt = kDt)
        {
            fake->Simulate(dt);
            backend->OnSceneStepped(dt);
        }

        FakeDeformableDriver *fake = nullptr; // owned by the backend: good only while it lives
        std::unique_ptr<DeformableSoftBodyBackend> backend;
    };

    /// The render vertices of a body skinned by hand from the nodes' device positions: every embedding applied to
    /// the ORIGINAL tetrahedra (so it holds for a mirrored transform too, whose backend swaps two corners)
    std::vector<Math::Vector3> ExpectedSurface(const FakeDeformableDriver &fake, const SoftBodies::DeformableVolumeId id,
                                               const TetMeshData &mesh)
    {
        std::vector<Math::Vector3> expected;
        for (const TetEmbedding &e : mesh.surfaceEmbedding)
        {
            Math::Vector3 sum(0.0f, 0.0f, 0.0f);
            for (int c = 0; c < 4; ++c)
            {
                const Math::Vector3 node = fake.DeviceNode(id, mesh.tets[e.tet][c]);
                sum.x += e.weights[c] * node.x;
                sum.y += e.weights[c] * node.y;
                sum.z += e.weights[c] * node.z;
            }
            expected.push_back(sum);
        }
        return expected;
    }

    void ExpectSameSurface(const std::vector<Math::Vector3> &actual, const std::vector<Math::Vector3> &expected,
                           const float tolerance = 1e-5f)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i)
        {
            EXPECT_NEAR(actual[i].x, expected[i].x, tolerance) << "vertex " << i;
            EXPECT_NEAR(actual[i].y, expected[i].y, tolerance) << "vertex " << i;
            EXPECT_NEAR(actual[i].z, expected[i].z, tolerance) << "vertex " << i;
        }
    }

    float MeanY(const std::vector<Math::Vector3> &points)
    {
        return TestSupport::Centroid(points).y;
    }
}

TEST(DeformableSoftBodyBackendTest, IsNamedPhysXAndReportsTheDevice)
{
    Rig rig;
    EXPECT_EQ(rig.backend->GetName(), "PhysX");
    EXPECT_EQ(rig.backend->GetDeviceName(), "Fake GPU");
    EXPECT_FALSE(rig.backend->HasFailed());
    EXPECT_NE(rig.backend->GetGeneration(), 0u);
}

TEST(DeformableSoftBodyBackendTest, HandlesCarryTheBackendsGenerationAndAnotherBackendsHandleIsIgnored)
{
    Rig a;
    Rig b;
    EXPECT_NE(a.backend->GetGeneration(), b.backend->GetGeneration());

    const SoftBodyHandle handle = a.backend->CreateBody(MakeBoxDesc(3));
    ASSERT_NE(handle, kInvalidSoftBody);
    EXPECT_EQ(SoftBodyHandleGeneration(handle), a.backend->GetGeneration());
    EXPECT_TRUE(a.backend->OwnsHandle(handle));
    EXPECT_FALSE(b.backend->OwnsHandle(handle));

    // B has a body of its own at the same index: A's handle must not reach it
    const SoftBodyHandle other = b.backend->CreateBody(MakeBoxDesc(3));
    ASSERT_NE(other, kInvalidSoftBody);
    EXPECT_EQ(SoftBodyHandleIndex(other), SoftBodyHandleIndex(handle));

    b.backend->Destroy(handle);
    SoftBodyParams params;
    params.stiffness = 5.0e5f;
    b.backend->SetParams(handle, params);
    b.backend->AddVelocity(handle, Math::Vector3(1.0f, 0.0f, 0.0f));
    std::vector<Math::Vector3> out(3);
    b.backend->CopySurfacePositions(handle, out);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(b.backend->GetBodyCount(), 1u);
    EXPECT_EQ(b.fake->destroyCalls, 0);
    EXPECT_EQ(b.fake->materialChanges, 0);
    EXPECT_EQ(b.fake->readBackCalls, 0);
    b.backend->CopySurfacePositions(other, out);
    EXPECT_FALSE(out.empty());

    // 0 is never a handle
    a.backend->Destroy(kInvalidSoftBody);
    EXPECT_EQ(a.backend->GetBodyCount(), 1u);
}

TEST(DeformableSoftBodyBackendTest, ARecreatedBackendAtTheSameAddressIgnoresTheOldHandle)
{
    alignas(DeformableSoftBodyBackend) unsigned char storage[sizeof(DeformableSoftBodyBackend)];
    auto *first = new (storage) DeformableSoftBodyBackend(std::make_unique<FakeDeformableDriver>());
    const SoftBodyHandle oldHandle = first->CreateBody(MakeBoxDesc(3));
    ASSERT_NE(oldHandle, kInvalidSoftBody);
    const std::uint32_t oldGeneration = first->GetGeneration();
    first->~DeformableSoftBodyBackend();

    auto *second = new (storage) DeformableSoftBodyBackend(std::make_unique<FakeDeformableDriver>());
    ASSERT_EQ(static_cast<void *>(second), static_cast<void *>(first)) << "the test needs the same address";
    EXPECT_NE(second->GetGeneration(), oldGeneration);
    const SoftBodyHandle newHandle = second->CreateBody(MakeBoxDesc(3));
    ASSERT_NE(newHandle, kInvalidSoftBody);
    EXPECT_EQ(SoftBodyHandleIndex(newHandle), SoftBodyHandleIndex(oldHandle)) << "the old handle would name this body";

    EXPECT_FALSE(second->OwnsHandle(oldHandle));
    second->Destroy(oldHandle);
    std::vector<Math::Vector3> out;
    second->CopySurfacePositions(oldHandle, out);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(second->GetBodyCount(), 1u);
    second->CopySurfacePositions(newHandle, out);
    EXPECT_FALSE(out.empty());
    second->~DeformableSoftBodyBackend();
}

TEST(DeformableSoftBodyBackendTest, CreateDestroyAndRoomReuse)
{
    Rig rig;
    rig.fake->maxVolumes = 2;
    const SoftBodyDesc desc = MakeBoxDesc(3);
    const SoftBodyHandle first = rig.backend->CreateBody(desc);
    const SoftBodyHandle second = rig.backend->CreateBody(desc);
    ASSERT_NE(first, kInvalidSoftBody);
    ASSERT_NE(second, kInvalidSoftBody);
    EXPECT_NE(first, second);
    EXPECT_EQ(rig.backend->GetBodyCount(), 2u);

    // A destroyed body frees its room (the driver's volume limit), and its handle stays dead: indexes aren't reused
    rig.backend->Destroy(first);
    EXPECT_EQ(rig.backend->GetBodyCount(), 1u);
    EXPECT_EQ(rig.fake->destroyCalls, 1);
    rig.backend->Destroy(first);
    EXPECT_EQ(rig.fake->destroyCalls, 1) << "destroying twice is ignored";
    const SoftBodyHandle third = rig.backend->CreateBody(desc);
    ASSERT_NE(third, kInvalidSoftBody) << "the room of the destroyed body is free again";
    EXPECT_NE(third, first);
    EXPECT_FALSE(rig.backend->HasFailed());

    std::vector<Math::Vector3> out;
    rig.backend->CopySurfacePositions(first, out);
    EXPECT_TRUE(out.empty());
    rig.backend->CopySurfacePositions(third, out);
    EXPECT_EQ(out.size(), desc.tetMesh->surfaceEmbedding.size());

    rig.backend->ReleaseAll();
    EXPECT_EQ(rig.backend->GetBodyCount(), 0u);
    EXPECT_TRUE(rig.fake->volumes.empty());
}

TEST(DeformableSoftBodyBackendTest, TheDriversVolumeLimitIsRecordedAsAFailureAndTheBackendStopsOfferingGpuWork)
{
    Rig rig; // the fake holds 4095 volumes, as a PhysX scene does
    ASSERT_EQ(rig.fake->maxVolumes, 4095u);
    const SoftBodyDesc desc = MakeBoxDesc(2);
    for (int i = 0; i < 4095; ++i)
    {
        ASSERT_NE(rig.backend->CreateBody(desc), kInvalidSoftBody) << "body " << i;
    }
    EXPECT_FALSE(rig.backend->HasFailed());

    SoftBodyWarningCapture warnings;
    EXPECT_EQ(rig.backend->CreateBody(desc), kInvalidSoftBody) << "the 4096th volume";
    EXPECT_TRUE(rig.backend->HasFailed());
    EXPECT_NE(rig.backend->GetFailureReason().find("most deformable volumes"), std::string::npos)
        << rig.backend->GetFailureReason();
    EXPECT_EQ(warnings.Count("PhysX GPU soft bodies"), 1);

    // After that it makes nothing more; its owner (the physics backend) drops it and releases the volumes
    EXPECT_EQ(rig.backend->CreateBody(desc), kInvalidSoftBody);
    EXPECT_EQ(rig.fake->createCalls, 4096);
    EXPECT_EQ(warnings.Count("PhysX GPU soft bodies"), 1) << "logged once";
    rig.backend->ReleaseAll();
    EXPECT_TRUE(rig.fake->volumes.empty());
}

TEST(DeformableSoftBodyBackendTest, AMalformedOrTooBigBodyIsRefusedWithoutFailingTheBackend)
{
    Rig rig;

    SoftBodyDesc none;
    EXPECT_EQ(rig.backend->CreateBody(none), kInvalidSoftBody) << "no tetrahedral mesh";

    SoftBodyDesc nonFinite = MakeBoxDesc(3);
    nonFinite.localToWorld[3] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(rig.backend->CreateBody(nonFinite), kInvalidSoftBody);

    auto tooMany = std::make_shared<TetMeshData>(*MakeBoxDesc(3).tetMesh);
    const std::array<std::uint32_t, 4> tet = tooMany->tets.front();
    tooMany->tets.assign(kMaxTetsPerBody + 1, tet);
    SoftBodyDesc big;
    big.tetMesh = tooMany;
    EXPECT_EQ(rig.backend->CreateBody(big), kInvalidSoftBody) << "more than kMaxTetsPerBody tetrahedra";

    auto badIndex = std::make_shared<TetMeshData>(*MakeBoxDesc(3).tetMesh);
    badIndex->tets[0][2] = static_cast<std::uint32_t>(badIndex->positions.size());
    SoftBodyDesc broken;
    broken.tetMesh = badIndex;
    EXPECT_EQ(rig.backend->CreateBody(broken), kInvalidSoftBody) << "a node index out of range";

    auto badEmbedding = std::make_shared<TetMeshData>(*MakeBoxDesc(3).tetMesh);
    badEmbedding->surfaceEmbedding[0].tet = static_cast<std::uint32_t>(badEmbedding->tets.size());
    SoftBodyDesc embedded;
    embedded.tetMesh = badEmbedding;
    EXPECT_EQ(rig.backend->CreateBody(embedded), kInvalidSoftBody) << "an embedding tetrahedron out of range";

    EXPECT_EQ(rig.fake->createCalls, 0) << "the driver was never asked";
    EXPECT_FALSE(rig.backend->HasFailed()) << "the CPU backend refuses these too: not a GPU failure";
    EXPECT_NE(rig.backend->CreateBody(MakeBoxDesc(3)), kInvalidSoftBody);
}

TEST(DeformableSoftBodyBackendTest, ADriverRefusalIsRecordedOnceWithItsReason)
{
    Rig rig;
    rig.fake->failCreate = true;
    SoftBodyWarningCapture warnings;
    EXPECT_EQ(rig.backend->CreateBody(MakeBoxDesc(3)), kInvalidSoftBody);
    EXPECT_TRUE(rig.backend->HasFailed());
    EXPECT_EQ(rig.backend->GetFailureReason(), "fake: out of GPU memory");
    EXPECT_EQ(warnings.Count("fake: out of GPU memory"), 1);

    EXPECT_EQ(rig.backend->CreateBody(MakeBoxDesc(3)), kInvalidSoftBody);
    EXPECT_EQ(rig.fake->createCalls, 1) << "nothing more is asked of the GPU";
    EXPECT_EQ(warnings.Count("PhysX GPU soft bodies"), 1);
}

TEST(DeformableSoftBodyBackendTest, AVolumeThatDoesNotMatchItsMeshIsRefusedAndReleased)
{
    // A driver whose node map is the wrong size (a bug on its side) must not leave a body that reads out of range
    Rig rig;
    rig.fake->shortNodeMap = true;
    SoftBodyWarningCapture warnings;
    EXPECT_EQ(rig.backend->CreateBody(MakeBoxDesc(3)), kInvalidSoftBody);
    EXPECT_TRUE(rig.backend->HasFailed());
    EXPECT_NE(rig.backend->GetFailureReason().find("does not match"), std::string::npos)
        << rig.backend->GetFailureReason();
    EXPECT_EQ(rig.fake->destroyCalls, 1) << "the volume the driver made is released";
    EXPECT_TRUE(rig.fake->volumes.empty());
    EXPECT_EQ(rig.backend->GetBodyCount(), 0u);
    EXPECT_EQ(warnings.Count("PhysX GPU soft bodies"), 1);
}

TEST(DeformableSoftBodyBackendTest, TheDriverGetsTheWorldSpaceBodyDensityAndMaterial)
{
    Rig rig;
    SoftBodyDesc desc = MakeBoxDesc(3, Math::Vector3(1.0f, 2.0f, 3.0f));
    desc.localToWorld[0] = 2.0f; // x scaled
    desc.params.density = 250.0f;
    desc.params.stiffness = 3.0e5f;
    desc.params.poisson = 0.4f;
    desc.params.friction = 0.7f;
    desc.params.damping = 0.25f;
    desc.params.solverIterations = 3;
    desc.params.materialModel = SoftMaterialModel::NeoHookean;
    desc.params.selfCollision = true;
    const SoftBodyHandle handle = rig.backend->CreateBody(desc);
    ASSERT_NE(handle, kInvalidSoftBody);

    const FakeDeformableDriver::Volume &volume = rig.fake->volumes.begin()->second;
    const TetMeshData &mesh = *desc.tetMesh;
    ASSERT_EQ(volume.rest.size(), mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i)
    {
        EXPECT_NEAR(volume.rest[i].x, 2.0f * mesh.positions[i].x + 1.0f, 1e-5f);
        EXPECT_NEAR(volume.rest[i].y, mesh.positions[i].y + 2.0f, 1e-5f);
        EXPECT_NEAR(volume.rest[i].z, mesh.positions[i].z + 3.0f, 1e-5f);
    }
    EXPECT_EQ(volume.tets.size(), mesh.tets.size());
    EXPECT_FLOAT_EQ(volume.density, 250.0f);
    EXPECT_FLOAT_EQ(volume.material.youngsModulus, 3.0e5f);
    EXPECT_FLOAT_EQ(volume.material.poisson, 0.4f);
    EXPECT_FLOAT_EQ(volume.material.dynamicFriction, 0.7f);
    EXPECT_FLOAT_EQ(volume.material.linearDamping, 0.25f);
    EXPECT_EQ(volume.material.solverIterations, 3u * DeformableSoftBodyBackend::kIterationsPerSolverIteration);
    EXPECT_EQ(volume.material.materialModel, SoftMaterialModel::NeoHookean);
    EXPECT_TRUE(volume.material.selfCollision);
}

TEST(DeformableSoftBodyBackendTest, SetParamsMapsToTheMaterialAndOnlyChangesAreSent)
{
    Rig rig;
    SoftBodyDesc desc = MakeBoxDesc(3);
    const SoftBodyHandle handle = rig.backend->CreateBody(desc);
    ASSERT_NE(handle, kInvalidSoftBody);
    FakeDeformableDriver::Volume &volume = rig.fake->volumes.begin()->second;

    // The component sends its parameters every fixed step: unchanged ones reach the driver zero times
    rig.backend->SetParams(handle, desc.params);
    rig.backend->SetParams(handle, desc.params);
    EXPECT_EQ(rig.fake->materialChanges, 0);

    SoftBodyParams params = desc.params;
    params.stiffness = 2.0e5f;
    params.poisson = 0.45f;
    params.friction = 0.9f;
    params.damping = 0.5f;
    params.solverIterations = 5;
    params.materialModel = SoftMaterialModel::NeoHookean;
    params.selfCollision = true;
    rig.backend->SetParams(handle, params);
    EXPECT_EQ(rig.fake->materialChanges, 1);
    EXPECT_FLOAT_EQ(volume.material.youngsModulus, 2.0e5f);
    EXPECT_FLOAT_EQ(volume.material.poisson, 0.45f);
    EXPECT_FLOAT_EQ(volume.material.dynamicFriction, 0.9f);
    EXPECT_FLOAT_EQ(volume.material.linearDamping, 0.5f);
    EXPECT_FLOAT_EQ(volume.material.elasticityDamping, 0.0f);
    EXPECT_EQ(volume.material.solverIterations, 20u);
    EXPECT_EQ(volume.material.materialModel, SoftMaterialModel::NeoHookean);
    EXPECT_TRUE(volume.material.selfCollision);

    rig.backend->SetParams(handle, params);
    EXPECT_EQ(rig.fake->materialChanges, 1) << "the same again sends nothing";

    // The ground plane and gravity belong to the CPU backend (the GPU body collides with the scene's colliders and
    // falls with the scene's gravity), and the density is read when the body is made
    SoftBodyParams cpuOnly = params;
    cpuOnly.groundEnabled = true;
    cpuOnly.groundHeight = 4.0f;
    cpuOnly.gravity = Math::Vector3(0.0f, -1.0f, 0.0f);
    cpuOnly.density = 5.0f;
    rig.backend->SetParams(handle, cpuOnly);
    EXPECT_EQ(rig.fake->materialChanges, 1);
    EXPECT_FLOAT_EQ(volume.density, desc.params.density);

    // Out-of-range and non-finite values are clamped
    SoftBodyParams wild = params;
    wild.poisson = 0.9f;
    wild.friction = 7.0f;
    wild.stiffness = std::numeric_limits<float>::quiet_NaN();
    wild.damping = -3.0f;
    wild.solverIterations = 1000;
    rig.backend->SetParams(handle, wild);
    EXPECT_FLOAT_EQ(volume.material.poisson, 0.49f);
    EXPECT_FLOAT_EQ(volume.material.dynamicFriction, 1.0f);
    EXPECT_FLOAT_EQ(volume.material.youngsModulus, SoftBodyParams{}.stiffness) << "NaN means the default";
    EXPECT_FLOAT_EQ(volume.material.linearDamping, 0.0f);
    EXPECT_EQ(volume.material.solverIterations, 255u);
    wild.solverIterations = 0;
    rig.backend->SetParams(handle, wild);
    EXPECT_EQ(volume.material.solverIterations, DeformableSoftBodyBackend::kIterationsPerSolverIteration);
}

TEST(DeformableSoftBodyBackendTest, AddVelocityAddsToEveryNodeThroughTheVelocityMirrorOnly)
{
    Rig rig;
    const SoftBodyHandle handle = rig.backend->CreateBody(MakeBoxDesc(3));
    ASSERT_NE(handle, kInvalidSoftBody);
    rig.SceneStep(); // the nodes now have a velocity of gravity * dt
    FakeDeformableDriver::Volume &volume = rig.fake->volumes.begin()->second;
    const std::vector<DeformableVec4> positionsBefore = volume.deviceCollision;
    ASSERT_EQ(volume.velocityReadBacks, 0);

    rig.backend->AddVelocity(handle, Math::Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(volume.velocityReadBacks, 1) << "read-modify-write: the velocities are read back first";
    EXPECT_EQ(volume.uploads, 1);
    EXPECT_EQ(volume.lastDirty, SoftBodies::kDeformSimVelocities) << "only the velocities are marked dirty";
    EXPECT_EQ(volume.allDirty, SoftBodies::kDeformSimVelocities);
    for (const DeformableVec4 &v : volume.deviceSimVelocities)
    {
        EXPECT_NEAR(v.x, 1.0f, 1e-5f);
        EXPECT_NEAR(v.y, -10.0f * kDt + 2.0f, 1e-5f);
        EXPECT_NEAR(v.z, 3.0f, 1e-5f);
    }
    for (std::size_t i = 0; i < positionsBefore.size(); ++i)
    {
        EXPECT_EQ(volume.deviceCollision[i].x, positionsBefore[i].x) << "positions are not touched";
        EXPECT_EQ(volume.deviceCollision[i].y, positionsBefore[i].y);
    }

    // A second push adds again
    rig.backend->AddVelocity(handle, Math::Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_NEAR(volume.deviceSimVelocities.front().x, 2.0f, 1e-5f);

    // The next scene step moves the body by the new velocity
    const float x = rig.fake->DeviceNode(rig.fake->volumes.begin()->first, 0).x;
    rig.SceneStep();
    EXPECT_GT(rig.fake->DeviceNode(rig.fake->volumes.begin()->first, 0).x, x + 1.0f * kDt);
}

TEST(DeformableSoftBodyBackendTest, AddVelocityLeavesMasslessNodesAndBadInputAlone)
{
    Rig rig;
    const SoftBodyHandle handle = rig.backend->CreateBody(MakeBoxDesc(3));
    ASSERT_NE(handle, kInvalidSoftBody);
    FakeDeformableDriver::Volume &volume = rig.fake->volumes.begin()->second;
    volume.hostSimPositions[0].w = 0.0f; // inverse mass 0: a node that doesn't move

    rig.backend->AddVelocity(handle, Math::Vector3(0.0f, 5.0f, 0.0f));
    EXPECT_EQ(volume.deviceSimVelocities[0].y, 0.0f);
    EXPECT_NEAR(volume.deviceSimVelocities[1].y, 5.0f, 1e-6f);

    const int uploads = volume.uploads;
    rig.backend->AddVelocity(handle, Math::Vector3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f));
    rig.backend->AddVelocity(handle, Math::Vector3(0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f));
    rig.backend->AddVelocity(kInvalidSoftBody, Math::Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(volume.uploads, uploads);
    EXPECT_FALSE(rig.backend->HasFailed());
}

TEST(DeformableSoftBodyBackendTest, AnAddVelocityTransferFailureFailsTheBackend)
{
    {
        Rig rig;
        const SoftBodyHandle handle = rig.backend->CreateBody(MakeBoxDesc(3));
        rig.fake->failUpload = true;
        rig.backend->AddVelocity(handle, Math::Vector3(1.0f, 0.0f, 0.0f));
        EXPECT_TRUE(rig.backend->HasFailed());
        EXPECT_EQ(rig.backend->GetFailureReason(), "fake: upload failed");
    }
    {
        Rig rig;
        const SoftBodyHandle handle = rig.backend->CreateBody(MakeBoxDesc(3));
        rig.fake->failReadBack = true;
        rig.backend->AddVelocity(handle, Math::Vector3(1.0f, 0.0f, 0.0f));
        EXPECT_TRUE(rig.backend->HasFailed());
        EXPECT_EQ(rig.backend->GetFailureReason(), "fake: read-back failed");
    }
}

TEST(DeformableSoftBodyBackendTest, StartsAtTheRestPoseWithoutAnyGpuCall)
{
    Rig rig;
    const Renderer::Common::MeshData box = TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f);
    SoftBodies::TetSettings settings;
    settings.resolution = 3;
    SoftBodyDesc desc;
    desc.tetMesh = SoftBodies::Tetrahedralise(box.vertices, box.indices, settings).mesh;
    ASSERT_NE(desc.tetMesh, nullptr);
    desc.localToWorld[3] = 1.0f;
    desc.localToWorld[7] = 2.0f;
    desc.localToWorld[11] = 3.0f;
    const SoftBodyHandle handle = rig.backend->CreateBody(desc);
    ASSERT_NE(handle, kInvalidSoftBody);

    std::vector<Math::Vector3> surface;
    rig.backend->CopySurfacePositions(handle, surface);
    ASSERT_EQ(surface.size(), box.vertices.size());
    for (std::size_t i = 0; i < surface.size(); ++i)
    {
        EXPECT_NEAR(surface[i].x, box.vertices[i].position[0] + 1.0f, 1e-4f) << "vertex " << i;
        EXPECT_NEAR(surface[i].y, box.vertices[i].position[1] + 2.0f, 1e-4f) << "vertex " << i;
        EXPECT_NEAR(surface[i].z, box.vertices[i].position[2] + 3.0f, 1e-4f) << "vertex " << i;
    }
    EXPECT_EQ(rig.fake->readBackCalls, 0);
    EXPECT_EQ(rig.fake->uploadCalls, 0);
}

TEST(DeformableSoftBodyBackendTest, TheRestPoseIsTheCpuBackendsExactly)
{
    // Both backends skin with SoftBodies::SkinSurface from the same transform: at rest they agree
    Rig rig;
    CpuSoftBodyBackend cpu;
    SoftBodyDesc desc = MakeBoxDesc(4, Math::Vector3(0.5f, 1.5f, -2.0f));
    desc.localToWorld[0] = 1.5f;
    desc.localToWorld[5] = 0.5f;
    const SoftBodyHandle gpuHandle = rig.backend->CreateBody(desc);
    const SoftBodyHandle cpuHandle = cpu.CreateBody(desc);
    ASSERT_NE(gpuHandle, kInvalidSoftBody);
    ASSERT_NE(cpuHandle, kInvalidSoftBody);
    std::vector<Math::Vector3> fromGpu;
    std::vector<Math::Vector3> fromCpu;
    rig.backend->CopySurfacePositions(gpuHandle, fromGpu);
    cpu.CopySurfacePositions(cpuHandle, fromCpu);
    ExpectSameSurface(fromGpu, fromCpu, 1e-6f);
}

TEST(DeformableSoftBodyBackendTest, ReadBackSkinsTheRenderVerticesFromTheNodes)
{
    for (const bool reorder : {false, true})
    {
        Rig rig;
        rig.fake->reorderVertices = reorder; // cooking may order the vertices differently: the node map covers it
        const SoftBodyDesc desc = MakeBoxDesc(3, Math::Vector3(0.0f, 10.0f, 0.0f));
        const SoftBodyHandle handle = rig.backend->CreateBody(desc);
        ASSERT_NE(handle, kInvalidSoftBody);
        const SoftBodies::DeformableVolumeId id = rig.fake->volumes.begin()->first;

        std::vector<Math::Vector3> surface;
        rig.backend->CopySurfacePositions(handle, surface);
        const float y0 = MeanY(surface);

        for (int step = 1; step <= 5; ++step)
        {
            rig.SceneStep();
            EXPECT_EQ(rig.fake->volumes.begin()->second.readBacks, step) << "one read-back per body per scene step";
            rig.backend->CopySurfacePositions(handle, surface);
            ExpectSameSurface(surface, ExpectedSurface(*rig.fake, id, *desc.tetMesh));
        }
        EXPECT_LT(MeanY(surface), y0 - 0.02f) << "it fell with the fake scene's gravity";
        EXPECT_EQ(rig.fake->uploadCalls, 0) << "reading back never uploads";

        // The cached nodes follow the engine's mesh order, whatever the driver's
        std::vector<Math::Vector3> nodes;
        rig.backend->CopyNodePositions(handle, nodes);
        ASSERT_EQ(nodes.size(), desc.tetMesh->positions.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            const Math::Vector3 expected = rig.fake->DeviceNode(id, i);
            EXPECT_NEAR(nodes[i].y, expected.y, 1e-6f);
        }
    }
}

TEST(DeformableSoftBodyBackendTest, EveryLiveBodyIsReadBackOncePerSceneStep)
{
    Rig rig;
    const SoftBodyDesc desc = MakeBoxDesc(3);
    const SoftBodyHandle a = rig.backend->CreateBody(desc);
    const SoftBodyHandle b = rig.backend->CreateBody(desc);
    ASSERT_NE(a, kInvalidSoftBody);
    ASSERT_NE(b, kInvalidSoftBody);
    rig.SceneStep();
    rig.SceneStep();
    rig.backend->Destroy(a);
    rig.SceneStep();
    EXPECT_EQ(rig.fake->readBackCalls, 5) << "2 + 2 + 1";
    EXPECT_EQ(rig.fake->volumes.begin()->second.readBacks, 3);
}

TEST(DeformableSoftBodyBackendTest, StepDoesNothingBecauseTheSceneStepsTheVolumes)
{
    Rig rig;
    const SoftBodyHandle handle = rig.backend->CreateBody(MakeBoxDesc(3));
    std::vector<Math::Vector3> before;
    rig.backend->CopySurfacePositions(handle, before);
    for (int i = 0; i < 5; ++i)
    {
        rig.backend->Step(kDt);
    }
    EXPECT_EQ(rig.fake->readBackCalls, 0);
    EXPECT_EQ(rig.fake->uploadCalls, 0);
    std::vector<Math::Vector3> after;
    rig.backend->CopySurfacePositions(handle, after);
    ExpectSameSurface(after, before, 0.0f);
}

TEST(DeformableSoftBodyBackendTest, MirroredAndNonUniformTransformsGiveTheDriverPositiveTetrahedraAndTheRightPicture)
{
    // Mirrored (negative determinant) = an odd number of negative axes; (-1,1,-2) is not mirrored but non-uniform
    const float scales[5][3] = {{-1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, -2.0f}, {1.0f, -1.0f, 1.0f},
                                {-1.0f, -1.0f, -1.0f}, {3.0f, 0.5f, 1.0f}};
    for (const auto &scale : scales)
    {
        Rig rig;
        SoftBodyDesc desc = MakeBoxDesc(3);
        desc.localToWorld[0] = scale[0];
        desc.localToWorld[5] = scale[1];
        desc.localToWorld[10] = scale[2];
        desc.localToWorld[3] = 1.0f;
        desc.localToWorld[7] = 2.0f;
        desc.localToWorld[11] = 3.0f;
        const SoftBodyHandle handle = rig.backend->CreateBody(desc);
        ASSERT_NE(handle, kInvalidSoftBody);

        // Every tetrahedron the driver got is positive in world space
        const FakeDeformableDriver::Volume &volume = rig.fake->volumes.begin()->second;
        for (std::size_t t = 0; t < volume.tets.size(); ++t)
        {
            const auto &tet = volume.tets[t];
            EXPECT_GT(SoftBodies::TetVolume6(volume.rest[tet[0]], volume.rest[tet[1]], volume.rest[tet[2]],
                                             volume.rest[tet[3]]),
                      0.0f)
                << "tetrahedron " << t << " of scale " << scale[0] << ", " << scale[1] << ", " << scale[2];
        }

        // And the render vertices sit at their transformed rest positions, at rest and after a step
        const TetMeshData &mesh = *desc.tetMesh;
        std::vector<Math::Vector3> surface;
        rig.backend->CopySurfacePositions(handle, surface);
        ASSERT_EQ(surface.size(), mesh.surfaceEmbedding.size());
        for (std::size_t i = 0; i < surface.size(); ++i)
        {
            const TetEmbedding &e = mesh.surfaceEmbedding[i];
            Math::Vector3 local(0.0f, 0.0f, 0.0f);
            for (int c = 0; c < 4; ++c)
            {
                const Math::Vector3 &p = mesh.positions[mesh.tets[e.tet][c]];
                local.x += e.weights[c] * p.x;
                local.y += e.weights[c] * p.y;
                local.z += e.weights[c] * p.z;
            }
            EXPECT_NEAR(surface[i].x, scale[0] * local.x + 1.0f, 1e-4f) << "vertex " << i;
            EXPECT_NEAR(surface[i].y, scale[1] * local.y + 2.0f, 1e-4f) << "vertex " << i;
            EXPECT_NEAR(surface[i].z, scale[2] * local.z + 3.0f, 1e-4f) << "vertex " << i;
        }
        rig.SceneStep();
        rig.backend->CopySurfacePositions(handle, surface);
        ExpectSameSurface(surface, ExpectedSurface(*rig.fake, rig.fake->volumes.begin()->first, mesh));
    }
}

TEST(DeformableSoftBodyBackendTest, CopySurfacePositionsAllocatesNothingOnceTheVectorHasGrown)
{
    Rig rig;
    const SoftBodyHandle handle = rig.backend->CreateBody(MakeBoxDesc(4));
    std::vector<Math::Vector3> out;
    rig.backend->CopySurfacePositions(handle, out);
    ASSERT_FALSE(out.empty());
    const Math::Vector3 *data = out.data();
    const std::size_t capacity = out.capacity();
    for (int i = 0; i < 5; ++i)
    {
        rig.SceneStep();
        rig.backend->CopySurfacePositions(handle, out);
    }
    EXPECT_EQ(out.data(), data);
    EXPECT_EQ(out.capacity(), capacity);
}

TEST(DeformableSoftBodyBackendTest, ABodyThatBlowsUpKeepsItsLastGoodShape)
{
    Rig rig;
    const SoftBodyHandle handle = rig.backend->CreateBody(MakeBoxDesc(3));
    rig.SceneStep();
    std::vector<Math::Vector3> good;
    rig.backend->CopySurfacePositions(handle, good);

    rig.fake->volumes.begin()->second.deviceCollision[0].x = std::numeric_limits<float>::quiet_NaN();
    rig.backend->OnSceneStepped(kDt);
    std::vector<Math::Vector3> after;
    rig.backend->CopySurfacePositions(handle, after);
    ExpectSameSurface(after, good, 0.0f);
    for (const Math::Vector3 &p : after)
    {
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    }
    EXPECT_FALSE(rig.backend->HasFailed()) << "a bad shape is not a GPU failure";
}

TEST(DeformableSoftBodyBackendTest, AReadBackFailureSwitchesTheGpuBackendOffOnce)
{
    Rig rig;
    const SoftBodyHandle handle = rig.backend->CreateBody(MakeBoxDesc(3));
    ASSERT_NE(handle, kInvalidSoftBody);
    rig.SceneStep();

    SoftBodyWarningCapture warnings;
    rig.fake->failReadBack = true;
    rig.backend->OnSceneStepped(kDt);
    EXPECT_TRUE(rig.backend->HasFailed());
    EXPECT_EQ(rig.backend->GetFailureReason(), "fake: read-back failed");
    EXPECT_EQ(warnings.Count("PhysX GPU soft bodies"), 1);

    const int readBacks = rig.fake->readBackCalls;
    rig.backend->OnSceneStepped(kDt);
    rig.backend->OnSceneStepped(kDt);
    EXPECT_EQ(rig.fake->readBackCalls, readBacks) << "no GPU work after a failure";
    EXPECT_EQ(rig.backend->CreateBody(MakeBoxDesc(3)), kInvalidSoftBody);
    EXPECT_EQ(rig.fake->createCalls, 1);
    EXPECT_EQ(warnings.Count("PhysX GPU soft bodies"), 1);

    // Its owner releases the bodies
    rig.backend->ReleaseAll();
    EXPECT_EQ(rig.backend->GetBodyCount(), 0u);
    EXPECT_TRUE(rig.fake->volumes.empty());
}

TEST(DeformableSoftBodyBackendTest, VolumesAreReleasedBeforeTheDriverGoes)
{
    std::vector<std::string> log;
    {
        auto driver = std::make_unique<FakeDeformableDriver>();
        driver->log = &log;
        DeformableSoftBodyBackend backend(std::move(driver));
        const SoftBodyDesc desc = MakeBoxDesc(3);
        ASSERT_NE(backend.CreateBody(desc), kInvalidSoftBody);
        ASSERT_NE(backend.CreateBody(desc), kInvalidSoftBody);
    }
    const std::vector<std::string> expected = {"create 1", "create 2", "destroy 1", "destroy 2", "driver destroyed"};
    EXPECT_EQ(log, expected) << "every volume is destroyed while the driver (and so the scene it uses) is alive";
}
