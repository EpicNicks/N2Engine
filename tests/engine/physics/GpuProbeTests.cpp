#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "../particles/FakePbdDriver.hpp"
#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Health.hpp"
#include "engine/Logger.hpp"
#include "engine/Time.hpp"
#include "engine/physics/IParticleBackend.hpp"
#include "engine/rendering/ParticleSystem.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// The physics.gpu setting (parse, validate, round trip), the Particles health row mapping, and the disabled path of
// the GPU probe. Nothing here needs a GPU: the probe itself (physics.gpu auto/on) is never run, because CI has no
// CUDA device; with physics.gpu off no CUDA call is made at all.

using namespace N2Engine;
using namespace N2Engine::Physics;
using json = nlohmann::json;

TEST(GpuModeTest, ParsesTheThreeValuesCaseInsensitively)
{
    EXPECT_EQ(ParseGpuMode("off"), GpuMode::Off);
    EXPECT_EQ(ParseGpuMode("auto"), GpuMode::Auto);
    EXPECT_EQ(ParseGpuMode("on"), GpuMode::On);
    EXPECT_EQ(ParseGpuMode("AUTO"), GpuMode::Auto);
    EXPECT_EQ(ParseGpuMode("On"), GpuMode::On);
}

TEST(GpuModeTest, RejectsAnythingElse)
{
    EXPECT_FALSE(ParseGpuMode("").has_value());
    EXPECT_FALSE(ParseGpuMode("true").has_value());
    EXPECT_FALSE(ParseGpuMode("gpu").has_value());
    EXPECT_FALSE(ParseGpuMode("on ").has_value());
}

TEST(GpuModeTest, NamesRoundTrip)
{
    for (const GpuMode mode : {GpuMode::Off, GpuMode::Auto, GpuMode::On})
    {
        EXPECT_EQ(ParseGpuMode(ToString(mode)), mode);
    }
}

TEST(GpuSettingTest, DefaultsToOff)
{
    std::vector<std::string> problems;
    EXPECT_EQ(ReadPhysicsGpuMode(json::object(), &problems), GpuMode::Off);
    EXPECT_EQ(ReadPhysicsGpuMode(json{{"physics", json::object()}}, &problems), GpuMode::Off);
    EXPECT_EQ(ReadPhysicsGpuMode(json{{"physics", {{"gpu", nullptr}}}}, &problems), GpuMode::Off);
    EXPECT_EQ(ReadPhysicsGpuMode(json::array(), &problems), GpuMode::Off);
    EXPECT_TRUE(problems.empty());
}

TEST(GpuSettingTest, ReadsEachValueAndRoundTripsThroughJson)
{
    for (const GpuMode mode : {GpuMode::Off, GpuMode::Auto, GpuMode::On})
    {
        // Saved as the project file's settings, read back
        const json saved = {{"physics", {{"gpu", std::string(ToString(mode))}}}};
        const json reloaded = json::parse(saved.dump());
        std::vector<std::string> problems;
        EXPECT_EQ(ReadPhysicsGpuMode(reloaded, &problems), mode);
        EXPECT_TRUE(problems.empty());
    }
}

TEST(GpuSettingTest, UnknownValueIsAWarningAndMeansOff)
{
    for (const json &bad : {json("maybe"), json(true), json(1), json::object()})
    {
        std::vector<std::string> problems;
        EXPECT_EQ(ReadPhysicsGpuMode(json{{"physics", {{"gpu", bad}}}}, &problems), GpuMode::Off) << bad.dump();
        ASSERT_EQ(problems.size(), 1u) << bad.dump();
        EXPECT_NE(problems[0].find("physics.gpu"), std::string::npos);
    }
}

TEST(GpuSettingTest, ApplyProjectSettingsValidatesIt)
{
    EXPECT_TRUE(ApplyProjectSettings(json{{"physics", {{"gpu", "auto"}}}}, std::set<std::string>{"physics"}).empty());
    EXPECT_TRUE(ApplyProjectSettings(json{{"physics", {{"gpu", "off"}}}}, std::set<std::string>{"physics"}).empty());

    const auto problems = ApplyProjectSettings(json{{"physics", {{"gpu", "banana"}}}}, std::set<std::string>{"physics"});
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("physics.gpu"), std::string::npos);
}

TEST(ParticlesStatusTest, MapsEachGpuStateToAHealthRow)
{
    const SubsystemStatus off = MakeParticlesStatus({GpuState::Disabled, "physics.gpu is off"});
    EXPECT_EQ(off.name, "Particles");
    EXPECT_EQ(off.state, SubsystemState::Disabled);
    EXPECT_EQ(off.detail, "physics.gpu is off");

    const SubsystemStatus unavailable = MakeParticlesStatus({GpuState::Unavailable, "no GPU"});
    EXPECT_EQ(unavailable.state, SubsystemState::Disabled);
    EXPECT_EQ(unavailable.detail, "no GPU");

    const SubsystemStatus ready = MakeParticlesStatus({GpuState::Ready, "device 0"});
    EXPECT_EQ(ready.state, SubsystemState::Running);
    EXPECT_EQ(ready.detail, "device 0");

    const SubsystemStatus failed = MakeParticlesStatus({GpuState::Failed, "probe failed"});
    EXPECT_EQ(failed.state, SubsystemState::Failed);
    EXPECT_EQ(failed.detail, "probe failed");
}

TEST(ParticlesStatusTest, OnlyAFailedProbeMakesTheEngineUnhealthy)
{
    for (const GpuState state : {GpuState::Disabled, GpuState::Unavailable, GpuState::Ready})
    {
        const EngineHealth health{.subsystems = {MakeParticlesStatus({state, "x"})}};
        EXPECT_TRUE(health.IsHealthy()) << ToString(state);
    }
    const EngineHealth failed{.subsystems = {MakeParticlesStatus({GpuState::Failed, "x"})}};
    EXPECT_FALSE(failed.IsHealthy());
}

#ifdef N2ENGINE_PHYSX_ENABLED

TEST(GpuProbeTest, BackendDefaultsToDisabledAndKeepsTheCpuScene)
{
    PhysXBackend backend;
    // The default is off: no probe, no CUDA call, and the status says why
    EXPECT_EQ(backend.GetGpuStatus().state, GpuState::Disabled);
    ASSERT_TRUE(backend.Initialize());
    const GpuStatus status = backend.GetGpuStatus();
    EXPECT_EQ(status.state, GpuState::Disabled);
    EXPECT_FALSE(status.detail.empty());
    EXPECT_EQ(MakeParticlesStatus(status).state, SubsystemState::Disabled);
    // physics.gpu off is today's CPU scene: no CUDA context, and no PhysX particle backend to offer
    EXPECT_EQ(backend.GetParticleBackend(), nullptr);
    backend.Shutdown();
    EXPECT_EQ(backend.GetParticleBackend(), nullptr);
}

TEST(GpuProbeTest, ExplicitOffBehavesTheSame)
{
    PhysXBackend backend;
    backend.SetGpuMode(GpuMode::Off);
    ASSERT_TRUE(backend.Initialize());
    EXPECT_EQ(backend.GetGpuStatus().state, GpuState::Disabled);
    EXPECT_EQ(backend.GetParticleBackend(), nullptr);
    backend.Shutdown();
}

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

        [[nodiscard]] int Count(const std::string &text) const
        {
            int n = 0;
            for (const std::string &w : warnings)
            {
                n += w.find(text) != std::string::npos ? 1 : 0;
            }
            return n;
        }

        std::vector<std::string> warnings;

    private:
        std::size_t _id = 0;
    };

    /// A physics backend that is initialised with physics.gpu off (no CUDA call on any machine), then given a
    /// particle backend over a fake driver and told which GPU mode the project asked for: the state a real GPU scene
    /// would be in, reached without a GPU through the test seam. The fake is owned by the particle backend, so the
    /// pointer to it is only good while that backend lives.
    struct InjectedBackend
    {
        explicit InjectedBackend(const GpuMode mode)
        {
            backend = std::make_unique<PhysXBackend>();
            backend->SetGpuMode(GpuMode::Off);
            if (!backend->Initialize())
            {
                return;
            }
            backend->SetGpuMode(mode); // only read again when a failure is recorded
            auto fake = std::make_unique<TestSupport::FakePbdDriver>();
            driver = fake.get();
            ready = backend->InstallParticleDriverForTesting(std::move(fake));
        }

        std::unique_ptr<PhysXBackend> backend;
        TestSupport::FakePbdDriver *driver = nullptr;
        bool ready = false;
    };
}

TEST(PhysicsBackendParticleFailureTest, TheSeamMakesAReadyGpuWithAParticleBackend)
{
    InjectedBackend rig(GpuMode::Auto);
    ASSERT_TRUE(rig.ready);
    EXPECT_EQ(rig.backend->GetGpuStatus().state, GpuState::Ready);
    EXPECT_EQ(MakeParticlesStatus(rig.backend->GetGpuStatus()).state, SubsystemState::Running);
    EXPECT_NE(rig.backend->GetParticleBackend(), nullptr);
    // One particle backend per scene
    EXPECT_FALSE(rig.backend->InstallParticleDriverForTesting(std::make_unique<TestSupport::FakePbdDriver>()));
    rig.backend->Shutdown();
}

TEST(PhysicsBackendParticleFailureTest, ADriverFailureDropsTheBackendAndChangesTheStatusOnce)
{
    struct Case
    {
        GpuMode mode;
        GpuState state;
        SubsystemState row;
        bool healthy;
    };
    // auto: no usable GPU particles is not an error; on: the project asked for the GPU, so it is
    for (const Case c : {Case{GpuMode::Auto, GpuState::Unavailable, SubsystemState::Disabled, true},
                         Case{GpuMode::On, GpuState::Failed, SubsystemState::Failed, false}})
    {
        InjectedBackend rig(c.mode);
        ASSERT_TRUE(rig.ready);
        IParticleBackend *particles = rig.backend->GetParticleBackend();
        ASSERT_NE(particles, nullptr);

        ParticleSystemDesc desc;
        desc.maxParticles = 16;
        const ParticleSystemHandle handle = particles->CreateSystem(desc);
        ASSERT_NE(handle, kInvalidParticleSystem);
        EXPECT_EQ(rig.backend->GetGpuStatus().state, GpuState::Ready);

        WarningCapture warnings;
        rig.driver->failUpload = true;
        const Math::Vector3 at(0.0f, 1.0f, 0.0f);
        const Math::Vector3 velocity(0.0f, 0.0f, 0.0f);
        particles->Spawn(handle, std::span<const Math::Vector3>(&at, 1), std::span<const Math::Vector3>(&velocity, 1));

        // The next look at the backend notices the failure: it is dropped, the status changes, one warning
        EXPECT_EQ(rig.backend->GetParticleBackend(), nullptr) << ToString(c.mode);
        EXPECT_EQ(rig.backend->GetParticleBackend(), nullptr);
        const GpuStatus status = rig.backend->GetGpuStatus();
        EXPECT_EQ(status.state, c.state) << ToString(c.mode);
        EXPECT_NE(status.detail.find("GPU particles unavailable"), std::string::npos) << status.detail;
        // The health row Application::GetHealth re-reads is this mapping of the status
        const SubsystemStatus row = MakeParticlesStatus(status);
        EXPECT_EQ(row.state, c.row);
        EXPECT_EQ((EngineHealth{.subsystems = {row}}).IsHealthy(), c.healthy);
        EXPECT_EQ(warnings.Count("GPU particles unavailable"), 1) << "logged once, not on every look";
        rig.backend->Shutdown();
    }
}

namespace
{
    class ParticleFallbackTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));
        }

        void TearDown() override
        {
            _object.reset();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
        }

        Rendering::ParticleSystem *Make()
        {
            _object = GameObject::Create("FailChain");
            auto *system = _object->AddComponent<Rendering::ParticleSystem>();
            system->emissionRate = 50.0f;
            system->lifetimeMin = 10.0f;
            system->lifetimeMax = 10.0f;
            system->shapeRadius = 0.0f;
            return system;
        }

        GameObject::Ptr _object;
    };
}

TEST_F(ParticleFallbackTest, AGpuFailureRestartsTheSystemOnTheCpuWithOneWarning)
{
    InjectedBackend rig(GpuMode::Auto);
    ASSERT_TRUE(rig.ready);
    PhysXBackend *physics = rig.backend.get();
    TestSupport::FakePbdDriver *driver = rig.driver;
    Application::GetInstance().Set3DPhysicsBackend(std::move(rig.backend));

    Rendering::ParticleSystem *system = Make();
    system->OnAttach();
    ASSERT_EQ(system->GetActiveBackendName(), "PhysX");
    for (int i = 0; i < 5; ++i)
    {
        system->OnFixedUpdate();
    }
    ASSERT_GT(system->GetAliveCount(), 0u);
    ASSERT_EQ(driver->systems.size(), 1u);

    // The GPU refuses the next transfer, which the next emission makes (the driver dies with the particle backend,
    // so it is not touched again)
    WarningCapture warnings;
    driver->failUpload = true;
    for (int i = 0; i < 5; ++i)
    {
        system->OnFixedUpdate();
    }

    EXPECT_EQ(system->GetActiveBackendName(), "CPU") << "restarted on the CPU";
    EXPECT_EQ(physics->GetParticleBackend(), nullptr);
    EXPECT_EQ(physics->GetGpuStatus().state, GpuState::Unavailable);
    EXPECT_EQ(warnings.Count("GPU particles unavailable"), 1);
    EXPECT_EQ(warnings.Count("physics backend changed"), 1);

    for (int i = 0; i < 10; ++i)
    {
        system->OnFixedUpdate();
    }
    EXPECT_GT(system->GetAliveCount(), 0u) << "it simulates on the CPU";
    EXPECT_EQ(warnings.Count("GPU particles unavailable"), 1);
    system->OnDestroy();
}

#ifndef _WIN32
// The Linux static libraries have no GPU library: auto and on report Unavailable (never Failed) and never touch CUDA,
// so this is safe to run without a GPU. (On Windows the probe would load the GPU library, so it is not run in CI.)
TEST(GpuProbeTest, CpuOnlyBuildsReportUnavailableNotFailed)
{
    for (const GpuMode mode : {GpuMode::Auto, GpuMode::On})
    {
        PhysXBackend backend;
        backend.SetGpuMode(mode);
        ASSERT_TRUE(backend.Initialize());
        EXPECT_EQ(backend.GetGpuStatus().state, GpuState::Unavailable) << ToString(mode);
        EXPECT_EQ(MakeParticlesStatus(backend.GetGpuStatus()).state, SubsystemState::Disabled);
        EXPECT_EQ(backend.GetParticleBackend(), nullptr) << "no GPU library: no PhysX particle backend";
        backend.Shutdown();
    }
}
#endif

#endif

namespace
{
    class LuaGpuTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite() { ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize()); }
    };
}

TEST_F(LuaGpuTest, IsGpuAvailableIsFalseWithoutAGpuScene)
{
    Application::GetInstance().Set3DPhysicsBackend(nullptr);
    sol::state &lua = Scripting::LuaRuntime::Instance().GetState();
    const sol::protected_function_result result = lua.safe_script("return Physics.IsGpuAvailable()", sol::script_pass_on_error);
    ASSERT_TRUE(result.valid());
    EXPECT_FALSE(result.get<bool>());
}
