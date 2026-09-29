#include <gtest/gtest.h>

#include "engine/Application.hpp"
#include "engine/Health.hpp"
#include "engine/Window.hpp"
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
