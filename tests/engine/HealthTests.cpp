#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <math/MathRegistrar.hpp>
#include <nlohmann/json.hpp>

#include "engine/Application.hpp"
#include "engine/Health.hpp"
#include "engine/Window.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/network/NetworkSettings.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;

// ============================================================================
// EngineHealth
// ============================================================================

TEST(EngineHealthTest, EmptyIsHealthy)
{
    EXPECT_TRUE(EngineHealth{}.IsHealthy());
}

TEST(EngineHealthTest, RunningDisabledAndNotStartedAreHealthy)
{
    const EngineHealth health{.subsystems = {
        {.name = "Window", .state = SubsystemState::Running},
        {.name = "Audio", .state = SubsystemState::Disabled, .detail = "Headless mode"},
        {.name = "Renderer", .state = SubsystemState::NotStarted},
    }};

    EXPECT_TRUE(health.IsHealthy());
}

TEST(EngineHealthTest, AnyFailureIsUnhealthy)
{
    const EngineHealth health{.subsystems = {
        {.name = "Window", .state = SubsystemState::Running},
        {.name = "Physics", .state = SubsystemState::Failed, .detail = "PhysX failed"},
    }};

    EXPECT_FALSE(health.IsHealthy());
}

TEST(EngineHealthTest, FindByName)
{
    const EngineHealth health{.subsystems = {
        {.name = "Window", .state = SubsystemState::Running},
        {.name = "Audio", .state = SubsystemState::Failed, .detail = "No device"},
    }};

    const SubsystemStatus *audio = health.Find("Audio");
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->state, SubsystemState::Failed);
    EXPECT_EQ(audio->detail, "No device");

    EXPECT_EQ(health.Find("Scripting"), nullptr);
}

TEST(EngineHealthTest, StateNames)
{
    EXPECT_EQ(ToString(SubsystemState::NotStarted), "NotStarted");
    EXPECT_EQ(ToString(SubsystemState::Running), "Running");
    EXPECT_EQ(ToString(SubsystemState::Disabled), "Disabled");
    EXPECT_EQ(ToString(SubsystemState::Failed), "Failed");
}

TEST(EngineHealthTest, ApplicationHasNoHealthBeforeInit)
{
    // Tests never call Application::Init (it opens a window), so nothing is reported yet
    const EngineHealth &health = Application::GetInstance().GetHealth();

    EXPECT_TRUE(health.subsystems.empty());
    EXPECT_TRUE(health.IsHealthy());
}

TEST(ApplicationShutdownTest, SafeWithoutInitAndWhenRepeated)
{
    auto &app = Application::GetInstance();

    // Tests never call Init; Shutdown must cope with nothing started, and with a second call
    EXPECT_NO_THROW(app.Shutdown());
    EXPECT_NO_THROW(app.Shutdown());

    EXPECT_EQ(app.Get3DPhysicsBackend(), nullptr);
    EXPECT_FALSE(app.GetWindow().IsValid());
}

TEST(ApplicationShutdownTest, ShutdownEndsANetworkSimulationModeSoTheNextProjectStartsOffline)
{
    auto &app = Application::GetInstance();

    // A project with a network preset leaves its mode behind; a later project with preset none does not touch the mode
    // in Init, so Shutdown has to put it back (the A1 follow-up: the none preset kept an earlier mode)
    for (const SimulationMode mode : {SimulationMode::Authoritative, SimulationMode::Deterministic})
    {
        app.SetSimulationMode(mode);
        ASSERT_EQ(app.GetSimulationMode(), mode);
        app.Shutdown();
        EXPECT_EQ(app.GetSimulationMode(), SimulationMode::None);
        EXPECT_FALSE(app.GetSimulationClock().IsNetworked());
    }

    // Already none stays none, and a second Shutdown is still harmless
    app.Shutdown();
    EXPECT_EQ(app.GetSimulationMode(), SimulationMode::None);
}

// ============================================================================
// Window without a successful InitWindow (the state left behind by any init failure)
// ============================================================================

TEST(WindowHealthTest, UninitializedWindowIsInvalid)
{
    const Window window;

    EXPECT_FALSE(window.IsValid());
    EXPECT_EQ(window.GetRenderer(), nullptr);
    EXPECT_EQ(window.GetInputSystem(), nullptr);
    EXPECT_TRUE(window.GetInitError().empty());
    EXPECT_FALSE(window.RendererFailed());
}

TEST(WindowHealthTest, UninitializedWindowAccessorsAreSafe)
{
    Window window;

    // None of these may touch a null GLFW window or renderer
    EXPECT_TRUE(window.ShouldClose());
    EXPECT_EQ(window.GetWindowDimensions()[0], 0);
    EXPECT_EQ(window.GetWindowDimensions()[1], 0);
    window.Clear();
    window.SetWindowMode(WindowMode::Fullscreen);
}

// ============================================================================
// Scripting
// ============================================================================

TEST(ScriptingHealthTest, LuaRuntimeInitializeReportsSuccess)
{
    auto &lua = Scripting::LuaRuntime::Instance();

    ASSERT_TRUE(lua.Initialize());

    // Bindings and the module system are registered
    EXPECT_TRUE(lua.GetState()["require"].valid());
    EXPECT_NO_THROW(lua.GetState().script("local x = 1 + 1"));
}

// ============================================================================
// The network preset at startup (kept last in this file: it leaves a health report behind, and the test above that
// expects none before Init must run first)
// ============================================================================

namespace
{
    namespace fs = std::filesystem;

    /// A folder with a project.n2proj whose settings are given; the project is refused or opened by Application::Init
    class NetworkPresetInitTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _root = fs::temp_directory_path() / "n2-network-preset-init-test" / info->name();
            std::error_code error;
            fs::remove_all(_root, error);
            fs::create_directories(_root);
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_root.parent_path(), error);
        }

        /// Init on a project with these settings. Only a REFUSED start is run in this process: a start that goes ahead
        /// would open a window, an audio device and a physics scene, and change the process-wide SIMD tier and Lua seed
        /// that every other test relies on (docs/testing.html).
        EngineHealth InitWith(const nlohmann::json &settings,
                              const Config::ApplicationOptions::PhysicsBackend hostBackend =
                                  Config::ApplicationOptions::PhysicsBackend::PHYSX)
        {
            const nlohmann::json file = {
                {"formatVersion", 1},
                {"name", "NetworkPresetTest"},
                {"projectId", "8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10"},
                {"engineVersion", "1.0.0"},
                {"settings", settings},
            };
            const auto project = IO::ProjectFile::FromJson(file);
            EXPECT_TRUE(project) << (project ? "" : project.error());
            if (project)
            {
                EXPECT_TRUE(project->Save(_root));
            }
            Config::ApplicationOptions options;
            options.projectPath = _root.string();
            options.physicsBackend = hostBackend;
            options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
            options.isHeadless = true;
            return Application::GetInstance().Init(options);
        }

        fs::path _root;
    };

    bool Mentions(const EngineHealth &health, const std::string &text)
    {
        const SubsystemStatus *network = health.Find("Network");
        return network != nullptr && network->detail.find(text) != std::string::npos;
    }
}

TEST_F(NetworkPresetInitTest, ADeterministicProjectIsRefusedAndNothingStarts)
{
    auto &app = Application::GetInstance();
    const nlohmann::json settings = {{"network", {{"preset", "deterministic"}}},
                                     {"physics", {{"fixedTimestep", 1.0 / 60.0}}}};
    const EngineHealth health = InitWith(settings);

    // The refusal is a Failed Network row that names every requirement still unmet
    const SubsystemStatus *network = health.Find("Network");
    ASSERT_NE(network, nullptr);
    EXPECT_EQ(network->state, SubsystemState::Failed);
    EXPECT_FALSE(health.IsHealthy());
    EXPECT_TRUE(Mentions(health, "refused to start"));
    EXPECT_TRUE(Mentions(health, "the physics backend declares CrossPlatform determinism"));
    EXPECT_TRUE(Mentions(health, "math.simdTier is scalar"));
    EXPECT_TRUE(Mentions(health, "determinism.luaSeed is set"));
    // C1 landed: the engine-owned Sin/Cos/Atan2/... exist in every build, so that requirement is met
    EXPECT_FALSE(Mentions(health, "engine-owned trig (C1)"));
    EXPECT_TRUE(Mentions(health, "physics results do not depend on the thread count"));
    // The preset selects the tick loop (SimulationMode::Deterministic), so that requirement is met
    EXPECT_FALSE(Mentions(health, "the netcode tick loop"));
    // The strict profile is the one requirement that depends on the build (there is a strict-FP CI leg)
    EXPECT_EQ(Mentions(health, "strict floating-point profile"), !Math::IsStrictFp());
    // Met on this project: physics.gpu is off by default and the fixed timestep is 1 / tickRate
    EXPECT_FALSE(Mentions(health, "physics.gpu is off"));
    EXPECT_FALSE(Mentions(health, "the fixed timestep is on"));
    EXPECT_FALSE(Mentions(health, "physics.fixedTimestep is"));

    // GetHealth (what GetEngineHealth reads) keeps the refusal on the rows it recomposes
    for (const char *name : {"Particles", "SoftBodies"})
    {
        const SubsystemStatus *row = app.GetHealth().Find(name);
        ASSERT_NE(row, nullptr) << name;
        EXPECT_NE(row->detail.find("the network preset was refused"), std::string::npos) << name;
    }

    // Nothing was started: no window, no renderer, no camera, no physics, no scene
    EXPECT_FALSE(app.GetWindow().IsValid());
    EXPECT_EQ(app.GetWindow().GetRenderer(), nullptr);
    EXPECT_EQ(app.GetMainCamera(), nullptr);
    EXPECT_EQ(app.Get3DPhysicsBackend(), nullptr);
    // ...and the simulation mode is untouched: the preset's mode is applied only to a start that goes ahead
    EXPECT_EQ(app.GetSimulationMode(), SimulationMode::None);
    for (const char *name : {"Window", "Renderer", "Physics", "Particles", "SoftBodies", "Audio", "Scripting"})
    {
        const SubsystemStatus *row = health.Find(name);
        ASSERT_NE(row, nullptr) << name;
        EXPECT_EQ(row->state, SubsystemState::NotStarted) << name;
    }

    // GetHealth reports the same, and Init is not left half-done: asking again refuses again (it does not take
    // the "already initialized" path)
    EXPECT_EQ(app.GetHealth().Find("Network")->state, SubsystemState::Failed);
    const EngineHealth again = InitWith(settings);
    ASSERT_NE(again.Find("Network"), nullptr);
    EXPECT_EQ(again.Find("Network")->detail, network->detail);
}

TEST_F(NetworkPresetInitTest, TheRefusalListsEveryRequirementTheProjectCouldNotMeet)
{
    // Every project setting a Deterministic project can make is made: the refusal still stands, for what no setting
    // can provide (a cross-platform backend and a strict-FP build; engine-owned trig is met since C1)
    const EngineHealth health = InitWith({
        {"network", {{"preset", "deterministic"}}},
        {"physics", {{"gpu", "off"}, {"deterministic", true}}},
        {"math", {{"simdTier", "scalar"}}},
        {"determinism", {{"luaSeed", 12345}}},
    });
    const SubsystemStatus *network = health.Find("Network");
    ASSERT_NE(network, nullptr);
    EXPECT_EQ(network->state, SubsystemState::Failed);
    EXPECT_TRUE(Mentions(health, "the physics backend declares CrossPlatform determinism"));
    EXPECT_FALSE(Mentions(health, "engine-owned trig (C1)"));
    EXPECT_EQ(Mentions(health, "strict floating-point profile"), !Math::IsStrictFp());
    // Met by those settings (and the tick loop by the preset), so not named
    EXPECT_FALSE(Mentions(health, "the netcode tick loop"));
    EXPECT_FALSE(Mentions(health, "math.simdTier is scalar"));
    EXPECT_FALSE(Mentions(health, "determinism.luaSeed is set"));
    EXPECT_FALSE(Mentions(health, "physics.gpu is off"));
#ifdef N2ENGINE_PHYSX_ENABLED
    // PhysX with physics.deterministic runs with 0 worker threads: SameBinary, so thread count is not the problem
    EXPECT_FALSE(Mentions(health, "physics results do not depend on the thread count"));
#endif
}

#ifdef N2ENGINE_JOLT_ENABLED
TEST_F(NetworkPresetInitTest, JoltMeetsTheFeatureRequirementWhetherTheProjectOrTheHostAskedForIt)
{
    // A Deterministic project on Jolt: the project's physics.backend, or no setting and the host's option. Jolt has
    // every feature group (layers, triggers and events from J4, scene queries from J5), so the feature requirement is
    // met either way. determinism.luaSeed is left unset on purpose so that the start is still refused: on the strict-FP
    // Jolt leg everything else is met, and a start that goes ahead must not run in this process (see InitWith).
    auto &app = Application::GetInstance();
    const nlohmann::json common = {
        {"network", {{"preset", "deterministic"}}},
        {"math", {{"simdTier", "scalar"}}},
    };
    nlohmann::json byProject = common;
    byProject["physics"] = {{"gpu", "off"}, {"deterministic", true}, {"backend", "jolt"}};
    nlohmann::json byHost = common;
    byHost["physics"] = {{"gpu", "off"}, {"deterministic", true}};
    const std::pair<nlohmann::json, Config::ApplicationOptions::PhysicsBackend> cases[] = {
        {byProject, Config::ApplicationOptions::PhysicsBackend::PHYSX},
        {byProject, Config::ApplicationOptions::PhysicsBackend::AUTO},
        {byHost, Config::ApplicationOptions::PhysicsBackend::JOLT},
    };
    for (const auto &[settings, host] : cases)
    {
        const EngineHealth health = InitWith(settings, host);
        const SubsystemStatus *network = health.Find("Network");
        ASSERT_NE(network, nullptr);
        EXPECT_EQ(network->state, SubsystemState::Failed);
        EXPECT_TRUE(Mentions(health, "determinism.luaSeed is set")) << network->detail;
        EXPECT_FALSE(Mentions(health, "the physics backend implements layers and the collision matrix"))
            << network->detail;
        EXPECT_FALSE(Mentions(health, "does not implement")) << network->detail;
        // Jolt was selected (the project's or the host's request was honoured, not refused as unavailable)
        EXPECT_FALSE(Mentions(health, "no such backend")) << network->detail;
        EXPECT_EQ(app.Get3DPhysicsBackend(), nullptr);
        EXPECT_FALSE(app.GetWindow().IsValid());
    }
}
#endif

TEST_F(NetworkPresetInitTest, TheRollbackModeAlsoNamesTheSnapshotRequirement)
{
    const EngineHealth health =
        InitWith({{"network", {{"preset", "deterministic"}, {"deterministic", {{"mode", "rollback"}}}}}});
    EXPECT_TRUE(Mentions(health, "supports snapshot and restore (rollback)"));
}

TEST_F(NetworkPresetInitTest, ABadNetworkValueDoesNotMakeAProjectRefused)
{
    // An unknown preset means none, so the project is not refused (the warning comes from ApplyProjectSettings).
    // Init cannot go ahead here (see InitWith), so check the decision it makes: the preset read from this file.
    const nlohmann::json settings = {{"network", {{"preset", "deterministic-ish"}}}};
    EXPECT_EQ(ReadNetworkSettings(settings).preset, Network::NetworkPreset::None);
}
