#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>

#include "engine/Application.hpp"
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
