#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/Application.hpp"
#include "engine/Health.hpp"
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
    backend.Shutdown();
}

TEST(GpuProbeTest, ExplicitOffBehavesTheSame)
{
    PhysXBackend backend;
    backend.SetGpuMode(GpuMode::Off);
    ASSERT_TRUE(backend.Initialize());
    EXPECT_EQ(backend.GetGpuStatus().state, GpuState::Disabled);
    backend.Shutdown();
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
