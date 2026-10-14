#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../particles/FakePhysicsBackend.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/serialization/FieldInfo.hpp"
#include "engine/softbody/SoftBodyBackendSelection.hpp"

// SelectSoftBodyBackend has the semantics of SelectParticleBackend: Auto uses the physics backend's GPU soft-body
// backend when it has one and is silent otherwise, PhysX falls back to the CPU backend with a reason, CPU never asks.
// A fake physics backend (which can offer a fake GPU soft-body backend) stands in for PhysX.

using namespace N2Engine;
using namespace N2Engine::SoftBodies;

namespace
{
    Physics::GpuStatus Status(const Physics::GpuState state, const std::string &detail)
    {
        return Physics::GpuStatus{state, detail};
    }
}

TEST(SoftBodySelectionTest, TheBackendEnumSerialisesAsAutoPhysXCpu)
{
    EXPECT_EQ(N2EnumOptions(static_cast<const SoftBodyBackend *>(nullptr)),
              (std::vector<std::string>{"Auto", "PhysX", "CPU"}));
    EXPECT_EQ(static_cast<int>(SoftBodyBackend::Auto), 0) << "Auto is the default";
}

TEST(SoftBodySelectionTest, CpuNeverAsksThePhysicsBackend)
{
    TestSupport::FakePhysicsBackend physics(false, Status(Physics::GpuState::Ready, "fake"));
    physics.withGpuSoftBodies = true;
    const SoftBodyBackendSelection selection = SelectSoftBodyBackend(SoftBodyBackend::CPU, &physics);
    EXPECT_EQ(selection.gpu, nullptr);
    EXPECT_FALSE(selection.fellBack);
    EXPECT_TRUE(selection.reason.empty());

    // Also with no physics backend at all
    const SoftBodyBackendSelection none = SelectSoftBodyBackend(SoftBodyBackend::CPU, nullptr);
    EXPECT_EQ(none.gpu, nullptr);
    EXPECT_FALSE(none.fellBack);
}

TEST(SoftBodySelectionTest, AutoUsesTheGpuBackendWhenThePhysicsBackendHasOne)
{
    TestSupport::FakePhysicsBackend physics(false, Status(Physics::GpuState::Ready, "fake"));
    physics.withGpuSoftBodies = true;
    const SoftBodyBackendSelection selection = SelectSoftBodyBackend(SoftBodyBackend::Auto, &physics);
    EXPECT_EQ(selection.gpu, &physics.softGpu);
    EXPECT_EQ(selection.gpu->GetName(), "PhysX");
    EXPECT_FALSE(selection.fellBack);
    EXPECT_TRUE(selection.reason.empty());
}

TEST(SoftBodySelectionTest, PhysXUsesTheGpuBackendWhenThePhysicsBackendHasOne)
{
    TestSupport::FakePhysicsBackend physics(false, Status(Physics::GpuState::Ready, "fake"));
    physics.withGpuSoftBodies = true;
    const SoftBodyBackendSelection selection = SelectSoftBodyBackend(SoftBodyBackend::PhysX, &physics);
    EXPECT_EQ(selection.gpu, &physics.softGpu);
    EXPECT_FALSE(selection.fellBack);
}

TEST(SoftBodySelectionTest, AutoIsSilentAboutHavingNoGpuBackend)
{
    TestSupport::FakePhysicsBackend off(false, Status(Physics::GpuState::Disabled, "physics.gpu is off"));
    const SoftBodyBackendSelection withPhysics = SelectSoftBodyBackend(SoftBodyBackend::Auto, &off);
    EXPECT_EQ(withPhysics.gpu, nullptr);
    EXPECT_FALSE(withPhysics.fellBack);
    EXPECT_TRUE(withPhysics.reason.empty());

    const SoftBodyBackendSelection noPhysics = SelectSoftBodyBackend(SoftBodyBackend::Auto, nullptr);
    EXPECT_EQ(noPhysics.gpu, nullptr);
    EXPECT_FALSE(noPhysics.fellBack);
}

TEST(SoftBodySelectionTest, PhysXWithoutAGpuBackendFallsBackWithTheProbesReason)
{
    TestSupport::FakePhysicsBackend unavailable(false, Status(Physics::GpuState::Unavailable, "no CUDA device"));
    const SoftBodyBackendSelection selection = SelectSoftBodyBackend(SoftBodyBackend::PhysX, &unavailable);
    EXPECT_EQ(selection.gpu, nullptr);
    EXPECT_TRUE(selection.fellBack);
    EXPECT_NE(selection.reason.find("backend is PhysX"), std::string::npos) << selection.reason;
    EXPECT_NE(selection.reason.find("no CUDA device"), std::string::npos) << selection.reason;
    EXPECT_NE(selection.reason.find("CPU backend"), std::string::npos) << selection.reason;
}

TEST(SoftBodySelectionTest, ASceneThatIsReadyButHasNoSoftBodyBackendStillFallsBack)
{
    // What every real backend is until the PhysX soft-body backend exists: the GPU scene is Ready (particles may
    // run on it), but there is no soft-body backend
    TestSupport::FakePhysicsBackend ready(true, Status(Physics::GpuState::Ready, "PhysX GPU: fake"));
    const SoftBodyBackendSelection selection = SelectSoftBodyBackend(SoftBodyBackend::PhysX, &ready);
    EXPECT_EQ(selection.gpu, nullptr);
    EXPECT_TRUE(selection.fellBack);
    EXPECT_NE(selection.reason.find("no PhysX soft-body backend"), std::string::npos) << selection.reason;
    EXPECT_EQ(SelectSoftBodyBackend(SoftBodyBackend::Auto, &ready).gpu, nullptr);
}

TEST(SoftBodySelectionTest, PhysXWithNoPhysicsBackendAtAllFallsBackToo)
{
    const SoftBodyBackendSelection selection = SelectSoftBodyBackend(SoftBodyBackend::PhysX, nullptr);
    EXPECT_EQ(selection.gpu, nullptr);
    EXPECT_TRUE(selection.fellBack);
    EXPECT_NE(selection.reason.find("there is no physics backend"), std::string::npos) << selection.reason;
}

TEST(SoftBodySelectionTest, TheFakeBackendsDefaultsMatchTheRealOnes)
{
    // No soft-body backend unless asked for, and a Ready scene is then not Ready for soft bodies
    TestSupport::FakePhysicsBackend fake(false, Status(Physics::GpuState::Ready, "PhysX GPU: fake"));
    EXPECT_EQ(fake.GetSoftBodyBackend(), nullptr);
    EXPECT_EQ(fake.GetSoftBodiesGpuStatus().state, Physics::GpuState::Unavailable);
    fake.withGpuSoftBodies = true;
    EXPECT_NE(fake.GetSoftBodyBackend(), nullptr);
    EXPECT_EQ(fake.GetSoftBodiesGpuStatus().state, Physics::GpuState::Ready);
}

// ---------------------------------------------------------------- the SoftBodies health row's usage text

TEST(SoftBodyUsageTest, DescribesTheLiveBodiesPerBackend)
{
    // The counts are process-wide, so everything is relative to what was there
    const int cpu = SoftBodyUsage::GetCount(false);
    const int gpu = SoftBodyUsage::GetCount(true);
    SoftBodyUsage::Add(false);
    SoftBodyUsage::Add(false);
    SoftBodyUsage::Add(true);
    EXPECT_EQ(SoftBodyUsage::GetCount(false), cpu + 2);
    EXPECT_EQ(SoftBodyUsage::GetCount(true), gpu + 1);
    const std::string text = SoftBodyUsage::Describe();
    EXPECT_EQ(text.rfind("SoftBodies:", 0), 0u) << text;
    EXPECT_NE(text.find("CPU"), std::string::npos) << text;
    EXPECT_NE(text.find("PhysX"), std::string::npos) << text;
    SoftBodyUsage::Remove(false);
    SoftBodyUsage::Remove(false);
    SoftBodyUsage::Remove(true);
    EXPECT_EQ(SoftBodyUsage::GetCount(false), cpu);
    EXPECT_EQ(SoftBodyUsage::GetCount(true), gpu);
    if (cpu == 0 && gpu == 0)
    {
        EXPECT_TRUE(SoftBodyUsage::Describe().empty());
    }
}

TEST(SoftBodyUsageTest, TheTextIsExactlyTheDocumentedForm)
{
    // Only when nothing else is counted (the other tests balance their Add and Remove)
    if (SoftBodyUsage::GetCount(false) != 0 || SoftBodyUsage::GetCount(true) != 0)
    {
        GTEST_SKIP() << "another test left soft bodies alive";
    }
    EXPECT_EQ(SoftBodyUsage::Describe(), "");
    SoftBodyUsage::Add(false);
    EXPECT_EQ(SoftBodyUsage::Describe(), "SoftBodies: 1 CPU");
    SoftBodyUsage::Add(false);
    SoftBodyUsage::Add(true);
    EXPECT_EQ(SoftBodyUsage::Describe(), "SoftBodies: 2 CPU, 1 PhysX");
    SoftBodyUsage::Remove(false);
    SoftBodyUsage::Remove(false);
    EXPECT_EQ(SoftBodyUsage::Describe(), "SoftBodies: 1 PhysX");
    SoftBodyUsage::Remove(true);
    EXPECT_EQ(SoftBodyUsage::Describe(), "");
}

TEST(SoftBodyUsageTest, ComposeLeavesTheGpuProbesDetailAloneWithoutBodiesAndAppendsTheBackendsWithThem)
{
    if (SoftBodyUsage::GetCount(false) != 0 || SoftBodyUsage::GetCount(true) != 0)
    {
        GTEST_SKIP() << "another test left soft bodies alive";
    }
    // No bodies: the row is exactly what the probe made, also with an empty detail (no physics backend case included)
    EXPECT_EQ(SoftBodyUsage::Compose("physics.gpu is off"), "physics.gpu is off");
    EXPECT_EQ(SoftBodyUsage::Compose(""), "");

    SoftBodyUsage::Add(false);
    SoftBodyUsage::Add(false);
    SoftBodyUsage::Add(true);
    EXPECT_EQ(SoftBodyUsage::Compose("physics.gpu is off"), "physics.gpu is off; SoftBodies: 2 CPU, 1 PhysX");
    EXPECT_EQ(SoftBodyUsage::Compose(""), "SoftBodies: 2 CPU, 1 PhysX");
    SoftBodyUsage::Remove(false);
    SoftBodyUsage::Remove(false);
    SoftBodyUsage::Remove(true);
    EXPECT_EQ(SoftBodyUsage::Compose("x"), "x");
}
