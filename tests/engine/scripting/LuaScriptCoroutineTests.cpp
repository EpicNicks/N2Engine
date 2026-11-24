#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>

#include "engine/Application.hpp"
#include "engine/SimulationClock.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

// Lua coroutines started by a script (LuaComponent:StartCoroutine), resumed by the object's coroutine scheduler, and the
// coroutine library under the instruction budget
class LuaScriptCoroutineTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;
    Scene *_scene = nullptr;
    GameObject::Ptr _go;
    LuaComponent *_component = nullptr;

    static void WriteAsset(const std::string &relativePath, const std::string &source)
    {
        const fs::path path = s_root / "assets" / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << source;
    }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
        s_root = fs::temp_directory_path() / "n2engine_lua_script_coroutine_tests";
        std::error_code ec;
        fs::remove_all(s_root, ec);
        // A script whose OnAttach starts whatever coroutine the test put in the global `co_body`
        WriteAsset("co/Starter.lua", R"(
            local Starter = {}
            Starter.__index = Starter
            function Starter:Start(...)
                self.handle = self.component:StartCoroutine(co_body, ...)
                return self.handle
            end
            return Starter
        )");
        // The same, with a raised InstructionBudget (issue #445): its coroutines' resumes get it too
        WriteAsset("co/RaisedStarter.lua", R"(
            local RaisedStarter = { InstructionBudget = 2000000 }
            RaisedStarter.__index = RaisedStarter
            function RaisedStarter:Start(...)
                return self.component:StartCoroutine(co_body, ...)
            end
            return RaisedStarter
        )");
        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_root);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }

    void SetUp() override
    {
        SceneManager::AddScene(Scene::Create(std::string("ScriptCoroutine_") +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
        _go = GameObject::Create("Runner");
        _scene->AddRootGameObject(_go);
        _component = _go->AddComponent<LuaComponent>();
        _component->SetScript(IO::ResourcePath("res://co/Starter.lua"));
        ASSERT_FALSE(_component->HasMissingScript());
        Lua()["co_log"] = Lua().create_table();
    }

    void TearDown() override
    {
        Lua()["co_body"] = sol::lua_nil;
        Lua()["co_log"] = sol::lua_nil;
    }

    void Define(const std::string &source) { Lua().safe_script("co_body = " + source); }
    int LogSize() { return static_cast<int>(Lua()["co_log"].get<sol::table>().size()); }
};

TEST_F(LuaScriptCoroutineTest, NothingRunsUntilTheFirstSchedulerUpdateThenOneStepPerFrame)
{
    Define("function() co_log[#co_log+1] = 'a' coroutine.yield() co_log[#co_log+1] = 'b' coroutine.yield() co_log[#co_log+1] = 'c' end");
    sol::table self = _component->GetScriptInstance();
    sol::object handle = self["Start"](self);
    ASSERT_TRUE(handle.is<ScriptCoroutineHandle>());
    EXPECT_EQ(LogSize(), 0) << "StartCoroutine runs none of the body";
    _scene->AdvanceCoroutines();
    EXPECT_EQ(LogSize(), 1);
    _scene->AdvanceCoroutines();
    EXPECT_EQ(LogSize(), 2);
    _scene->AdvanceCoroutines();
    EXPECT_EQ(LogSize(), 3);
    EXPECT_FALSE(handle.as<ScriptCoroutineHandle>().IsRunning()) << "the function returned";
}

TEST_F(LuaScriptCoroutineTest, ArgumentsReachTheFunctionAndYieldedSecondsWait)
{
    // Tests never advance the engine clock, so the frame delta stays zero: a wait of seconds stays suspended, and a
    // wait of 0 resumes on the next frame
    Define("function(a, b) co_log[#co_log+1] = a + b coroutine.yield(0) co_log[#co_log+1] = 'zero' coroutine.yield(3600) "
           "co_log[#co_log+1] = 'late' end");
    sol::table self = _component->GetScriptInstance();
    self["Start"](self, 2, 3);
    _scene->AdvanceCoroutines();
    ASSERT_EQ(LogSize(), 1);
    EXPECT_EQ(Lua()["co_log"][1].get<int>(), 5);
    _scene->AdvanceCoroutines();
    EXPECT_EQ(LogSize(), 2);
    for (int frame = 0; frame < 5; ++frame)
    {
        _scene->AdvanceCoroutines();
    }
    EXPECT_EQ(LogSize(), 2) << "an hour has not passed";
}

TEST_F(LuaScriptCoroutineTest, StopEndsItAndStopAllEndsEveryOne)
{
    Define("function() while true do co_log[#co_log+1] = 1 coroutine.yield() end end");
    sol::table self = _component->GetScriptInstance();
    sol::object first = self["Start"](self);
    self["Start"](self);
    _scene->AdvanceCoroutines();
    EXPECT_EQ(LogSize(), 2);
    first.as<ScriptCoroutineHandle>().Stop();
    EXPECT_FALSE(first.as<ScriptCoroutineHandle>().IsRunning());
    _scene->AdvanceCoroutines();
    EXPECT_EQ(LogSize(), 3) << "only the second still runs";
    _component->StopAllScriptCoroutines();
    _scene->AdvanceCoroutines();
    EXPECT_EQ(LogSize(), 3);
}

TEST_F(LuaScriptCoroutineTest, DeactivatingTheObjectStopsItsCoroutines)
{
    Define("function() while true do co_log[#co_log+1] = 1 coroutine.yield() end end");
    sol::table self = _component->GetScriptInstance();
    self["Start"](self);
    _scene->AdvanceCoroutines();
    EXPECT_EQ(LogSize(), 1);
    _go->SetActive(false);
    _scene->AdvanceCoroutines();
    EXPECT_EQ(LogSize(), 1);
}

TEST_F(LuaScriptCoroutineTest, AnErrorIsLoggedOnceAndEndsOnlyThatCoroutine)
{
    Define("function() coroutine.yield() error('boom') end");
    sol::table self = _component->GetScriptInstance();
    sol::object handle = self["Start"](self);
    int errors = 0;
    const auto subscription = Logger::logEvent += [&](const std::string_view message, const Logger::LogLevel level)
    {
        if (level == Logger::LogLevel::Error && message.find("boom") != std::string_view::npos)
        {
            ++errors;
        }
    };
    _scene->AdvanceCoroutines();
    _scene->AdvanceCoroutines();
    _scene->AdvanceCoroutines();
    Logger::logEvent -= subscription;
    EXPECT_EQ(errors, 1);
    EXPECT_FALSE(handle.as<ScriptCoroutineHandle>().IsRunning());
}

TEST_F(LuaScriptCoroutineTest, AYieldThatIsNotANumberOfSecondsIsAnError)
{
    Define("function() coroutine.yield('soon') co_log[#co_log+1] = 1 end");
    sol::table self = _component->GetScriptInstance();
    sol::object handle = self["Start"](self);
    _scene->AdvanceCoroutines();
    EXPECT_FALSE(handle.as<ScriptCoroutineHandle>().IsRunning());
    EXPECT_EQ(LogSize(), 0);
}

TEST_F(LuaScriptCoroutineTest, ALoopInsideACoroutineHitsTheBudgetAndSwitchesTheComponentOff)
{
    const LuaLimits saved = LuaRuntime::Instance().GetLimits();
    LuaRuntime::Instance().SetLimits({200'000, saved.maxBytes});
    Define("function() coroutine.yield() while true do end end");
    sol::table self = _component->GetScriptInstance();
    sol::object handle = self["Start"](self);
    _scene->AdvanceCoroutines();
    _scene->AdvanceCoroutines(); // runs into the loop
    LuaRuntime::Instance().SetLimits(saved);
    EXPECT_FALSE(handle.as<ScriptCoroutineHandle>().IsRunning());
    EXPECT_TRUE(_component->IsFaulted());
}

TEST_F(LuaScriptCoroutineTest, ThePlainCoroutineLibraryWorksAndIsBudgeted)
{
    const LuaLimits saved = LuaRuntime::Instance().GetLimits();
    LuaRuntime::Instance().SetLimits({200'000, saved.maxBytes});
    auto &lua = Lua();
    EXPECT_EQ(lua.safe_script("local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1) return b * 2 end) "
                              "return co(1) + co(10)").get<int>(),
              22);
    // A coroutine that loops without yielding is stopped by the budget of the call that resumed it
    const auto result = lua.safe_script("local co = coroutine.create(function() while true do end end) "
                                        "return coroutine.resume(co)",
                                        sol::script_pass_on_error);
    EXPECT_FALSE(result.valid());
    LuaRuntime::Instance().SetLimits(saved);
}

TEST_F(LuaScriptCoroutineTest, WaitForSecondsRealtimeChecksItsValueAndAnythingElseIsStillAnError)
{
    EXPECT_FLOAT_EQ(Lua().safe_script("return WaitForSecondsRealtime(0.25).seconds").get<float>(), 0.25f);
    for (const char *bad : {"WaitForSecondsRealtime(-1)", "WaitForSecondsRealtime(0/0)", "WaitForSecondsRealtime(math.huge)"})
    {
        EXPECT_FALSE(Lua().safe_script(std::string("return ") + bad, sol::script_pass_on_error).valid()) << bad;
    }
    // A table is not a wait
    Define("function() coroutine.yield({ seconds = 1 }) co_log[#co_log+1] = 1 end");
    sol::table self = _component->GetScriptInstance();
    sol::object handle = self["Start"](self);
    _scene->AdvanceCoroutines();
    EXPECT_FALSE(handle.as<ScriptCoroutineHandle>().IsRunning());
    EXPECT_EQ(LogSize(), 0);
}

TEST_F(LuaScriptCoroutineTest, ACoroutineOfAComponentWithARaisedBudgetGetsItOnEveryResume)
{
    const LuaLimits saved = LuaRuntime::Instance().GetLimits();
    LuaRuntime::Instance().SetLimits({200'000, saved.maxBytes});
    const auto go = GameObject::Create("RaisedRunner");
    _scene->AddRootGameObject(go);
    auto *raised = go->AddComponent<LuaComponent>();
    raised->SetScript(IO::ResourcePath("res://co/RaisedStarter.lua"));
    ASSERT_FALSE(raised->HasMissingScript());
    EXPECT_EQ(raised->GetInstructionBudget(), 2'000'000u);
    // About a million instructions after the first yield: five times the runtime's budget, half the raised one
    Define("function() coroutine.yield() local x = 0 for i = 1, 200000 do x = x + 1 x = x + 1 x = x + 1 x = x + 1 end "
           "co_log[#co_log+1] = x end");
    sol::table raisedSelf = raised->GetScriptInstance();
    sol::table plainSelf = _component->GetScriptInstance();
    raisedSelf["Start"](raisedSelf);
    plainSelf["Start"](plainSelf);
    _scene->AdvanceCoroutines();
    _scene->AdvanceCoroutines();
    LuaRuntime::Instance().SetLimits(saved);
    EXPECT_FALSE(raised->IsFaulted());
    EXPECT_EQ(LogSize(), 1) << "only the raised component's coroutine finished its loop";
    EXPECT_TRUE(_component->IsFaulted());
}

// coroutine.yield(WaitForSecondsRealtime(s)) (issue #445): frames of a known delta through Application::Tick, on a headless
// software window as ApplicationTickTest runs them
class LuaScriptCoroutineRealtimeTest : public LuaScriptCoroutineTest
{
protected:
    void SetUp() override
    {
        Config::ApplicationOptions options;
        options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
        options.isHeadless = true;
        Window &window = Application::GetInstance().GetWindow();
        ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
        Application::GetInstance().Set3DPhysicsBackend(nullptr);
        Time::SetTimeScale(1.0f);
        LuaScriptCoroutineTest::SetUp();
    }

    void TearDown() override
    {
        Time::SetTimeScale(1.0f);
        Application &app = Application::GetInstance();
        app.SetSimulationMode(SimulationMode::None);
        app.GetSimulationClock().Reset();
        // Other tests rely on the engine clock standing still: a frame of no time leaves the deltas at zero again
        Frame(0.0);
        LuaScriptCoroutineTest::TearDown();
        app.GetWindow().Shutdown();
    }

    static void Frame(const double seconds)
    {
        TickOptions options;
        options.render = false;
        options.pollEvents = false;
        options.deltaSeconds = seconds;
        Application::GetInstance().Tick(options);
    }

    /// Starts a scaled and a realtime wait of 0.1 s side by side, runs `frames` frames of 0.06 s and returns the log
    std::string RunBothWaits(const int frames)
    {
        Define("function(tag, wait) coroutine.yield(wait) co_log[#co_log+1] = tag end");
        const sol::object realtimeWait = Lua().safe_script("return WaitForSecondsRealtime(0.1)").get<sol::object>();
        sol::table self = _component->GetScriptInstance();
        self["Start"](self, "scaled", 0.1);
        self["Start"](self, "realtime", realtimeWait);
        std::string log;
        for (int frame = 0; frame < frames; ++frame)
        {
            Frame(0.06);
        }
        const sol::table entries = Lua()["co_log"];
        for (std::size_t i = 1; i <= entries.size(); ++i)
        {
            log += entries[i].get<std::string>() + ";";
        }
        return log;
    }
};

TEST_F(LuaScriptCoroutineRealtimeTest, ARealtimeWaitIgnoresTheTimeScaleAndAScaledOneFollowsIt)
{
    Time::SetTimeScale(0.0f); // hit-stop
    // Frame 1 runs both bodies to their yield; frames 2 and 3 add 0.06 s of unscaled time each: 0.12 >= 0.1
    EXPECT_EQ(RunBothWaits(2), "");
    Frame(0.06);
    std::string log;
    const sol::table entries = Lua()["co_log"];
    for (std::size_t i = 1; i <= entries.size(); ++i)
    {
        log += entries[i].get<std::string>() + ";";
    }
    EXPECT_EQ(log, "realtime;") << "the scaled wait stands still while timeScale is 0";

    Time::SetTimeScale(1.0f);
    Frame(0.06);
    Frame(0.06);
    EXPECT_EQ(Lua()["co_log"].get<sol::table>().size(), 2u) << "the scaled wait finishes once time runs again";
}

TEST_F(LuaScriptCoroutineRealtimeTest, ARealtimeWaitIsUnscaledFrameTimeInANetworkSimulationModeToo)
{
    // Coroutines run once per frame, outside the fixed ticks, in every mode: the realtime wait counts the same unscaled
    // frame time there, and the scaled one still follows timeScale (which in a network mode only scales frame time)
    Application::GetInstance().SetSimulationMode(SimulationMode::Deterministic);
    Time::SetTimeScale(0.0f);
    const std::uint64_t ticksBefore = Time::GetTick();
    EXPECT_EQ(RunBothWaits(3), "realtime;");
    EXPECT_GT(Time::GetTick(), ticksBefore) << "the ticks ran: timeScale does not pause a network mode";
}
