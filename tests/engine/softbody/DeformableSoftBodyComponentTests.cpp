#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>

#include <math/Vector3.hpp>

#include "../particles/FakePhysicsBackend.hpp"
#include "SoftBodyTestSupport.hpp"
#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Time.hpp"
#include "engine/physics/SoftBody.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/softbody/SoftBodyBackendSelection.hpp"
#include "engine/softbody/Tetrahedraliser.hpp"

// The SoftBody component over the PhysX soft-body backend's real logic (SoftBodies::DeformableSoftBodyBackend) with a
// fake driver, on every CI leg: Auto and PhysX pick it, CPU never does, the material model and self collision reach
// the driver, a refusal or a later failure restarts the body on the CPU with one warning, and a replaced backend's
// handles are not the new one's. The fake physics backend's Update plays the scene step (the fake device moves, the
// backend reads back), then the component's OnFixedUpdate skins the picture.

using namespace N2Engine;
using Physics::SoftBody;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using TestSupport::SoftBodyWarningCapture;

namespace
{
    constexpr float kDt = 0.02f;

    struct Rig
    {
        GameObject::Ptr object;
        SoftBody *body = nullptr;
        MeshRenderer *renderer = nullptr;
    };

    class DeformableComponentTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));
            SoftBodies::TetCache::Instance().Clear();
        }

        void TearDown() override
        {
            _rigs.clear();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            SoftBodies::TetCache::Instance().Clear();
        }

        /// A fake physics backend (a Ready GPU scene) with the PhysX soft-body logic over a fake driver, installed as
        /// the application's
        TestSupport::FakePhysicsBackend *InstallPhysics()
        {
            auto fake = std::make_unique<TestSupport::FakePhysicsBackend>(
                false, Physics::GpuStatus{Physics::GpuState::Ready, "PhysX GPU: fake"});
            TestSupport::FakePhysicsBackend *physics = fake.get();
            physics->UseDeformableSoftBodies();
            Application::GetInstance().Set3DPhysicsBackend(std::move(fake));
            return physics;
        }

        Rig &Make(const std::string &name, const SoftBodies::SoftBodyBackend backend)
        {
            Rig rig;
            rig.object = GameObject::Create(name);
            rig.renderer = rig.object->AddComponent<MeshRenderer>();
            rig.renderer->SetMesh(TestSupport::MakeBoxMesh());
            rig.body = rig.object->AddComponent<SoftBody>();
            rig.body->resolution = 4;
            rig.body->stiffness = 1.0e6f;
            rig.body->backend = backend;
            _rigs.push_back(std::move(rig));
            return _rigs.back();
        }

        /// One fixed step with the scene step before it: the fake device moves the volumes and the backend reads them
        /// back (the physics backend's Update), then the component skins its mesh
        static void Step(TestSupport::FakePhysicsBackend *physics, SoftBody *body, const int steps = 1)
        {
            for (int i = 0; i < steps; ++i)
            {
                physics->Update(kDt);
                body->OnFixedUpdate();
            }
        }

        static float MeanY(const Mesh &mesh)
        {
            double y = 0.0;
            for (const auto &vertex : mesh.GetVertices())
            {
                y += vertex.position[1];
            }
            return static_cast<float>(y / static_cast<double>(mesh.GetVertices().size()));
        }

        std::deque<Rig> _rigs;
    };
}

TEST_F(DeformableComponentTest, AutoAndPhysXUseThePhysXBackendAndCpuStillWins)
{
    TestSupport::FakePhysicsBackend *physics = InstallPhysics();
    SoftBodyWarningCapture warnings;
    const int gpuBefore = SoftBodies::SoftBodyUsage::GetCount(true);
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);

    Rig &autoRig = Make("Auto", SoftBodies::SoftBodyBackend::Auto);
    autoRig.body->OnAttach();
    Rig &physxRig = Make("PhysX", SoftBodies::SoftBodyBackend::PhysX);
    physxRig.body->OnAttach();
    Rig &cpuRig = Make("Cpu", SoftBodies::SoftBodyBackend::CPU);
    cpuRig.body->OnAttach();

    EXPECT_EQ(autoRig.body->GetActiveBackendName(), "PhysX");
    EXPECT_EQ(physxRig.body->GetActiveBackendName(), "PhysX");
    EXPECT_EQ(cpuRig.body->GetActiveBackendName(), "CPU");
    ASSERT_NE(physics->deformableDriver, nullptr);
    EXPECT_EQ(physics->deformableDriver->volumes.size(), 2u) << "two bodies are volumes of the PhysX backend";
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore + 2);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);
    EXPECT_TRUE(warnings.warnings.empty());
    EXPECT_EQ(physics->GetSoftBodyBackend()->GetName(), "PhysX");
    EXPECT_EQ(Physics::MakeSoftBodiesStatus(physics->GetSoftBodiesGpuStatus()).state, SubsystemState::Running);

    // It falls through the scene step and the skinning
    const std::uint64_t version = autoRig.body->GetRuntimeMesh()->GetVersion();
    const float y = MeanY(*autoRig.body->GetRuntimeMesh());
    Step(physics, autoRig.body, 10);
    EXPECT_GT(autoRig.body->GetRuntimeMesh()->GetVersion(), version);
    EXPECT_LT(MeanY(*autoRig.body->GetRuntimeMesh()), y - 0.05f);
    EXPECT_EQ(physics->deformableDriver->volumes.begin()->second.readBacks, 10) << "one read-back per scene step";

    // AddVelocity reaches the driver as a velocity read-modify-write
    autoRig.body->AddVelocity(Math::Vector3(1.0f, 0.0f, 0.0f));
    EXPECT_EQ(physics->deformableDriver->volumes.begin()->second.velocityReadBacks, 1);

    autoRig.body->OnDestroy();
    physxRig.body->OnDestroy();
    EXPECT_TRUE(physics->deformableDriver->volumes.empty());
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore);
}

TEST_F(DeformableComponentTest, TheMaterialModelAndSelfCollisionReachTheDriverAndChangesFollow)
{
    TestSupport::FakePhysicsBackend *physics = InstallPhysics();
    Rig &rig = Make("Model", SoftBodies::SoftBodyBackend::PhysX);
    rig.body->materialModel = Physics::SoftMaterialModel::NeoHookean;
    rig.body->selfCollision = true;
    rig.body->poisson = 0.4f;
    rig.body->friction = 0.8f;
    rig.body->solverIterations = 3;
    rig.body->OnAttach();
    ASSERT_EQ(rig.body->GetActiveBackendName(), "PhysX");
    const auto &volume = physics->deformableDriver->volumes.begin()->second;
    EXPECT_EQ(volume.material.materialModel, Physics::SoftMaterialModel::NeoHookean);
    EXPECT_TRUE(volume.material.selfCollision);
    EXPECT_FLOAT_EQ(volume.material.poisson, 0.4f);
    EXPECT_FLOAT_EQ(volume.material.dynamicFriction, 0.8f);
    EXPECT_FLOAT_EQ(volume.material.youngsModulus, 1.0e6f);
    EXPECT_EQ(volume.material.solverIterations, 12u);
    EXPECT_FLOAT_EQ(volume.density, rig.body->density);

    // Steps with unchanged settings send nothing; an edit is sent once
    Step(physics, rig.body, 3);
    EXPECT_EQ(physics->deformableDriver->materialChanges, 0);
    rig.body->materialModel = Physics::SoftMaterialModel::CoRotational;
    rig.body->selfCollision = false;
    Step(physics, rig.body, 3);
    EXPECT_EQ(physics->deformableDriver->materialChanges, 1);
    EXPECT_EQ(volume.material.materialModel, Physics::SoftMaterialModel::CoRotational);
    EXPECT_FALSE(volume.material.selfCollision);

    // The ground plane belongs to the CPU backend: it changes nothing here
    rig.body->collideWithGround = true;
    rig.body->groundHeight = 3.0f;
    Step(physics, rig.body, 3);
    EXPECT_EQ(physics->deformableDriver->materialChanges, 1);
    rig.body->OnDestroy();
}

TEST_F(DeformableComponentTest, ARefusedBodyRestartsOnTheCpuWithOneWarningAndTheBackendIsDropped)
{
    TestSupport::FakePhysicsBackend *physics = InstallPhysics();
    physics->deformableDriver->failCreate = true;
    const int gpuBefore = SoftBodies::SoftBodyUsage::GetCount(true);
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);

    SoftBodyWarningCapture warnings;
    Rig &rig = Make("Refused", SoftBodies::SoftBodyBackend::PhysX);
    rig.body->OnAttach();
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("PhysX backend couldn't create the body"), 1);
    EXPECT_EQ(warnings.Count("PhysX GPU soft bodies"), 1) << "the backend's own failure line";
    EXPECT_EQ(physics->GetSoftBodyBackend(), nullptr) << "the failed backend is no longer offered";
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);

    // It simulates on the CPU
    const float y = MeanY(*rig.body->GetRuntimeMesh());
    Step(physics, rig.body, 10);
    EXPECT_LT(MeanY(*rig.body->GetRuntimeMesh()), y - 0.05f);

    // The next body asking for Auto goes to the CPU quietly, a PhysX one with the selection's one warning
    const int before = static_cast<int>(warnings.warnings.size());
    Rig &autoRig = Make("Auto", SoftBodies::SoftBodyBackend::Auto);
    autoRig.body->OnAttach();
    EXPECT_EQ(autoRig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(static_cast<int>(warnings.warnings.size()), before) << "Auto is quiet";
    Rig &physxRig = Make("PhysX", SoftBodies::SoftBodyBackend::PhysX);
    physxRig.body->OnAttach();
    EXPECT_EQ(physxRig.body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(warnings.Count("backend is PhysX"), 1);
    rig.body->OnDestroy();
    autoRig.body->OnDestroy();
    physxRig.body->OnDestroy();
}

TEST_F(DeformableComponentTest, AGpuFailureMidRunRestartsTheBodyOnTheCpuWithOneWarning)
{
    TestSupport::FakePhysicsBackend *physics = InstallPhysics();
    const int gpuBefore = SoftBodies::SoftBodyUsage::GetCount(true);
    const int cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
    Rig &rig = Make("Falls", SoftBodies::SoftBodyBackend::Auto);
    rig.body->OnAttach();
    ASSERT_EQ(rig.body->GetActiveBackendName(), "PhysX");
    Step(physics, rig.body, 5);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore + 1);

    // The GPU refuses the next read-back (the driver dies with the backend, so it is not touched again)
    SoftBodyWarningCapture warnings;
    physics->deformableDriver->failReadBack = true;
    physics->Update(kDt);
    EXPECT_EQ(physics->GetSoftBodyBackend(), nullptr);
    rig.body->OnFixedUpdate();

    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU") << "restarted on the CPU";
    EXPECT_EQ(warnings.Count("physics backend changed"), 1);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore + 1);
    EXPECT_EQ(rig.renderer->GetDeformedMesh(), rig.body->GetRuntimeMesh()) << "still drawn";

    const float y = MeanY(*rig.body->GetRuntimeMesh());
    for (int i = 0; i < 10; ++i)
    {
        rig.body->OnFixedUpdate();
    }
    EXPECT_LT(MeanY(*rig.body->GetRuntimeMesh()), y) << "it simulates on the CPU";
    EXPECT_EQ(warnings.Count("physics backend changed"), 1) << "once";
    rig.body->OnDestroy();
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), cpuBefore);
}

TEST_F(DeformableComponentTest, AReplacedBackendsHandlesAreNotTheNewOnes)
{
    TestSupport::FakePhysicsBackend *physics = InstallPhysics();
    const int gpuBefore = SoftBodies::SoftBodyUsage::GetCount(true);
    Rig &rig = Make("Old", SoftBodies::SoftBodyBackend::PhysX);
    rig.body->OnAttach();
    ASSERT_EQ(rig.body->GetActiveBackendName(), "PhysX");
    Step(physics, rig.body, 3);

    // A new backend (a new generation) now answers GetSoftBodyBackend; the old body's handle (index 1 in both) must
    // not name the new backend's first body
    TestSupport::FakeDeformableDriver *fresh = physics->RecreateDeformableSoftBodies();
    Rig &other = Make("New", SoftBodies::SoftBodyBackend::PhysX);
    other.body->OnAttach();
    ASSERT_EQ(other.body->GetActiveBackendName(), "PhysX");
    ASSERT_EQ(fresh->volumes.size(), 1u);
    Step(physics, other.body, 5);
    const float otherY = MeanY(*other.body->GetRuntimeMesh());

    SoftBodyWarningCapture warnings;
    rig.body->OnFixedUpdate();
    EXPECT_EQ(rig.body->GetActiveBackendName(), "CPU") << "restarted on the CPU, not attached to the new backend";
    EXPECT_EQ(warnings.Count("physics backend changed"), 1);
    EXPECT_EQ(fresh->destroyCalls, 0) << "the old handle never reached the new backend";
    EXPECT_EQ(fresh->volumes.size(), 1u);
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(true), gpuBefore + 1);
    Step(physics, other.body, 5);
    EXPECT_LT(MeanY(*other.body->GetRuntimeMesh()), otherY) << "the other body is untouched and falls on";

    rig.body->OnDestroy();
    EXPECT_EQ(fresh->destroyCalls, 0);
    other.body->OnDestroy();
    EXPECT_EQ(fresh->destroyCalls, 1);
}
