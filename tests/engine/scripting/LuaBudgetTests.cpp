#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <math/UUID.hpp>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp" // ActionMap.hpp only forward-declares it
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

// The Lua state's limits (audit F3a): an instruction budget per outermost entry from C++ into Lua, a cap on the
// memory of the state, and Application.Quit refused on an editor host. Every test lowers the limits through the
// API (never the defaults), so none takes long.
class LuaBudgetTest : public ::testing::Test
{
protected:
    static constexpr std::uint64_t TestBudget = 200'000;
    static constexpr std::size_t MiB = std::size_t{1024} * 1024;

    static inline fs::path s_projectRoot;
    static inline std::size_t s_baselineBytes = 0;

    static void WriteAsset(const std::string &relativePath, const std::string &source)
    {
        const fs::path path = s_projectRoot / "assets" / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << source;
    }

    // `for` body of 4 additions plus the loop's own instruction: 5 VM instructions per iteration
    static std::string SpinSource(const int iterations)
    {
        return "local x = 0 for i = 1, " + std::to_string(iterations) +
               " do x = x + 1 x = x + 1 x = x + 1 x = x + 1 end return x";
    }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
        s_baselineBytes = LuaRuntime::Instance().GetMemoryUsed();

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_budget_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        // An OnUpdate that never ends
        WriteAsset("budget/Looper.lua", R"(
            local Looper = {}
            Looper.__index = Looper
            function Looper:OnUpdate()
                budget_loop_calls = budget_loop_calls + 1
                while true do end
            end
            return Looper
        )");

        WriteAsset("budget/Counter.lua", R"(
            local Counter = {}
            Counter.__index = Counter
            function Counter:OnUpdate()
                budget_counter = budget_counter + 1
            end
            return Counter
        )");

        // A memory bomb in OnUpdate
        WriteAsset("budget/Bomber.lua", R"(
            local Bomber = {}
            Bomber.__index = Bomber
            function Bomber:OnUpdate()
                budget_bomb_calls = budget_bomb_calls + 1
                local s = string.rep('x', 64 * 1024 * 1024)
            end
            return Bomber
        )");

        // The loop sits where only a table access reaches it: the class's __index
        WriteAsset("budget/IndexLoop.lua", R"(
            local IndexLoop = {}
            IndexLoop.__index = function(self, key)
                while true do end
            end
            return IndexLoop
        )");

        // A loop at the top level of the chunk
        WriteAsset("budget/ChunkLoop.lua", "while true do end\n");

        // Two modules that each use about a million instructions when they are first required
        WriteAsset("budget/SpinA.lua", SpinSource(200000) + "\n");
        WriteAsset("budget/SpinB.lua", SpinSource(200000) + "\n");

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    static LuaRuntime &Runtime() { return LuaRuntime::Instance(); }
    static sol::state &Lua() { return Runtime().GetState(); }

    LuaLimits _saved;
    std::vector<std::pair<std::string, Logger::LogLevel>> _logs;
    std::size_t _logSubscription = 0;
    Input::InputAction _action{"BudgetTest"};

    void SetUp() override
    {
        _saved = Runtime().GetLimits();
        Runtime().SetLimits({TestBudget, Runtime().GetMemoryUsed() + 16 * MiB});
        _logSubscription = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
        {
            _logs.emplace_back(std::string(message), level);
        };
        Application::SetScriptQuitAllowed(true);
        Application::GetInstance().ResetQuitRequest();
    }

    void TearDown() override
    {
        Logger::logEvent -= _logSubscription;
        Runtime().SetLimits(_saved);
        Lua()["budget_action"] = sol::lua_nil;
        Application::SetScriptQuitAllowed(true);
        Application::GetInstance().ResetQuitRequest();
    }

    static sol::protected_function_result Run(const std::string &source)
    {
        return Runtime().RunSource(source, "budget-test");
    }

    static std::string ErrorOf(const sol::protected_function_result &result)
    {
        if (result.valid())
        {
            return {};
        }
        const sol::error error = result;
        return error.what();
    }

    int CountLogs(const std::string_view needle, const Logger::LogLevel level) const
    {
        int found = 0;
        for (const auto &[message, logLevel] : _logs)
        {
            if (logLevel == level && message.find(needle) != std::string::npos)
            {
                ++found;
            }
        }
        return found;
    }

    static LuaComponent *Attach(const GameObject::Ptr &go, const char *scriptPath)
    {
        auto *component = go->AddComponent<LuaComponent>();
        component->SetScript(IO::ResourcePath(scriptPath));
        return component;
    }
};

TEST_F(LuaBudgetTest, TheDefaultsAreGenerousAndTheStateStartsSmall)
{
    const LuaLimits defaults;
    EXPECT_EQ(defaults.instructionsPerEntry, 50'000'000u);
    EXPECT_EQ(defaults.maxBytes, 256 * MiB);
    // Registering every binding must fit in the cap with room for scripts
    EXPECT_LT(s_baselineBytes, defaults.maxBytes / 2) << "bindings use " << s_baselineBytes << " bytes";

    const LuaLimits set = Runtime().GetLimits();
    EXPECT_EQ(set.instructionsPerEntry, TestBudget);
    EXPECT_GT(Runtime().GetMemoryUsed(), 0u);
}

TEST_F(LuaBudgetTest, AnInfiniteLoopInOnUpdateIsAbortedLoggedOnceAndTheComponentSwitchedOff)
{
    Lua()["budget_loop_calls"] = 0;
    Lua()["budget_counter"] = 0;
    const auto loopGo = GameObject::Create("Looper");
    const auto counterGo = GameObject::Create("Counter");
    auto *looper = Attach(loopGo, "res://budget/Looper.lua");
    auto *counter = Attach(counterGo, "res://budget/Counter.lua");
    ASSERT_FALSE(looper->HasMissingScript());

    for (int tick = 0; tick < 5; ++tick)
    {
        looper->OnUpdate();
        counter->OnUpdate();
    }

    EXPECT_EQ(Lua()["budget_loop_calls"].get<int>(), 1) << "after the budget ran out it is not called again";
    EXPECT_EQ(Lua()["budget_counter"].get<int>(), 5) << "other components and the engine keep running";
    EXPECT_EQ(CountLogs("instruction budget", Logger::LogLevel::Error), 1) << "logged once, not once per frame";

    // Setting the script again clears the fault
    looper->SetScript(IO::ResourcePath("res://budget/Looper.lua"));
    looper->OnUpdate();
    EXPECT_EQ(Lua()["budget_loop_calls"].get<int>(), 2);
    EXPECT_EQ(CountLogs("instruction budget", Logger::LogLevel::Error), 2);
}

TEST_F(LuaBudgetTest, ALoopAtTheTopLevelOfAChunkReturnsAnError)
{
    const sol::protected_function_result chunk = Run("while true do end");
    EXPECT_FALSE(chunk.valid());
    EXPECT_NE(ErrorOf(chunk).find("instruction budget"), std::string::npos) << ErrorOf(chunk);

    // The same through the loader: RunFile reports failure, and require hands the script an empty module
    EXPECT_FALSE(Runtime().RunFile(IO::ResourcePath("res://budget/ChunkLoop.lua")));
    const sol::protected_function_result required = Run("local m = require('budget.ChunkLoop') return m");
    EXPECT_FALSE(required.valid()) << "the budget stays spent for the rest of the entry that required it";
    EXPECT_EQ(CountLogs("Error running res://budget/ChunkLoop.lua", Logger::LogLevel::Error), 1)
        << "the loader logs a chunk's limit error once";

    // The state is fine afterwards
    const sol::protected_function_result next = Run("return 1 + 1");
    ASSERT_TRUE(next.valid());
    EXPECT_EQ(next[0].get<int>(), 2);
}

TEST_F(LuaBudgetTest, PcallAndXpcallCannotSwallowTheBudget)
{
    Lua()["budget_pcall_after"] = false;
    const sol::protected_function_result inPcall = Run(
        "local ok = pcall(function() while true do end end) budget_pcall_after = true");
    EXPECT_FALSE(inPcall.valid());
    EXPECT_FALSE(Lua()["budget_pcall_after"].get<bool>()) << "the script went on after its pcall";

    Lua()["budget_xpcall_after"] = false;
    const sol::protected_function_result inXpcall = Run(
        "local ok = xpcall(function() while true do end end, function(message) return message end)"
        " budget_xpcall_after = true");
    EXPECT_FALSE(inXpcall.valid());
    EXPECT_FALSE(Lua()["budget_xpcall_after"].get<bool>()) << "the script went on after its xpcall";

    // A nest of them, and a handler that loops too
    Lua()["budget_nest_after"] = false;
    const sol::protected_function_result nested = Run(
        "pcall(function() pcall(function() xpcall(function() while true do end end,"
        " function(message) while true do end end) end) end) budget_nest_after = true");
    EXPECT_FALSE(nested.valid());
    EXPECT_FALSE(Lua()["budget_nest_after"].get<bool>());

    // pcall still does its job for ordinary errors
    const sol::protected_function_result ordinary = Run(
        "local ok, message = pcall(error, 'plain') return ok, message");
    ASSERT_TRUE(ordinary.valid());
    EXPECT_FALSE(ordinary[0].get<bool>());
    EXPECT_EQ(ordinary[1].get<std::string>(), "plain");
}

TEST_F(LuaBudgetTest, AMemoryBombIsAbortedAndTheStateStaysUsable)
{
    const sol::protected_function_result big = Run("local s = string.rep('x', 64 * 1024 * 1024) return #s");
    EXPECT_FALSE(big.valid());
    EXPECT_NE(ErrorOf(big).find("not enough memory"), std::string::npos) << ErrorOf(big);

    const sol::protected_function_result growing = Run(
        "local t = {} for i = 1, 1000 do t[#t + 1] = string.rep('x', 1000000) end return #t");
    EXPECT_FALSE(growing.valid());
    EXPECT_NE(ErrorOf(growing).find("not enough memory"), std::string::npos) << ErrorOf(growing);
    EXPECT_LE(Runtime().GetMemoryUsed(), Runtime().GetLimits().maxBytes);

    Lua().collect_garbage();
    const sol::protected_function_result normal = Run("return 40 + 2");
    ASSERT_TRUE(normal.valid()) << ErrorOf(normal);
    EXPECT_EQ(normal[0].get<int>(), 42);
}

TEST_F(LuaBudgetTest, AComponentThatRunsOutOfMemoryIsLoggedOnceAndSwitchedOff)
{
    Lua()["budget_bomb_calls"] = 0;
    const auto go = GameObject::Create("Bomber");
    auto *bomber = Attach(go, "res://budget/Bomber.lua");

    for (int tick = 0; tick < 3; ++tick)
    {
        bomber->OnUpdate();
    }

    EXPECT_EQ(Lua()["budget_bomb_calls"].get<int>(), 1);
    EXPECT_EQ(CountLogs("not enough memory", Logger::LogLevel::Error), 1);
}

TEST_F(LuaBudgetTest, NormalScriptsAreUnaffectedByTheDefaults)
{
    Runtime().SetLimits(LuaLimits{});

    const sol::protected_function_result sum = Run("local s = 0 for i = 1, 1000000 do s = s + i end return s");
    ASSERT_TRUE(sum.valid()) << ErrorOf(sum);
    EXPECT_EQ(sum[0].get<long long>(), 500000500000LL);

    const sol::protected_function_result strings = Run(
        "local t = {} for i = 1, 20000 do t[i] = tostring(i) .. 'x' end return #table.concat(t)");
    ASSERT_TRUE(strings.valid()) << ErrorOf(strings);
}

TEST_F(LuaBudgetTest, ZeroMeansUnlimited)
{
    Runtime().SetLimits({0, 0});

    const sol::protected_function_result spin = Run(SpinSource(500000));
    ASSERT_TRUE(spin.valid()) << ErrorOf(spin); // 2.5 million instructions, over the test budget
    EXPECT_EQ(spin[0].get<int>(), 2000000);

    const sol::protected_function_result big = Run("local s = string.rep('x', 40 * 1024 * 1024) return #s");
    ASSERT_TRUE(big.valid()) << ErrorOf(big);
}


TEST_F(LuaBudgetTest, AnEntryNestedInsideARunningOneDoesNotRefillTheBudget)
{
    Runtime().SetLimits({1'500'000, 64 * MiB});

    // Each part alone fits the budget of 1.5 million (about 1 million instructions each)...
    const sol::protected_function_result alone = Run(SpinSource(200000));
    ASSERT_TRUE(alone.valid()) << ErrorOf(alone);
    const sol::protected_function_result moduleAlone = Run("require('budget.SpinA') return true");
    ASSERT_TRUE(moduleAlone.valid()) << ErrorOf(moduleAlone);

    // ...but one inside the other shares it: require's chunk is an entry nested in the running chunk
    const sol::protected_function_result nested = Run(
        "local x = 0 for i = 1, 200000 do x = x + 1 x = x + 1 x = x + 1 x = x + 1 end"
        " require('budget.SpinB') return x");
    EXPECT_FALSE(nested.valid()) << "the nested chunk got a fresh budget";
    EXPECT_NE(ErrorOf(nested).find("instruction budget"), std::string::npos) << ErrorOf(nested);
}

TEST_F(LuaBudgetTest, ACallbackThatRunsOutOfBudgetIsLoggedOnceAndSwitchedOff)
{
    Lua()["budget_action"] = InputActionRef(_action);
    Lua()["budget_hits"] = 0;
    Lua().script("budget_action:Subscribe(function(action) budget_hits = budget_hits + 1 while true do end end)");

    for (int event = 0; event < 3; ++event)
    {
        _action.GetOnStateChanged()(_action);
    }

    EXPECT_EQ(Lua()["budget_hits"].get<int>(), 1);
    EXPECT_EQ(CountLogs("callback aborted", Logger::LogLevel::Error), 1);
}

TEST_F(LuaBudgetTest, ALoopInAMetamethodTheEngineReadsIsAbortedWithoutLeavingTheStateBroken)
{
    Runtime().SetLimits({20'000, 64 * MiB});

    // Each SetScript reads the script's methods through its __index, outside any protected call: the budget error
    // has to come out as an error the engine handles. Hundreds of them must not wear the state out either.
    const auto go = GameObject::Create("IndexLoop");
    auto *component = go->AddComponent<LuaComponent>();
    for (int attempt = 0; attempt < 60; ++attempt)
    {
        component->SetScript(IO::ResourcePath("res://budget/IndexLoop.lua"));
        component->OnUpdate();
    }

    EXPECT_GE(CountLogs("instruction budget", Logger::LogLevel::Error), 1);
    const sol::protected_function_result normal = Run("local t = {} for i = 1, 100 do t[i] = i end return #t");
    ASSERT_TRUE(normal.valid()) << ErrorOf(normal);
    EXPECT_EQ(normal[0].get<int>(), 100);

    // The count hook is still alive: a loop is still aborted (an error that went down the panic path used to leave
    // the hook disabled for good)
    const sol::protected_function_result spin = Run("while true do end");
    ASSERT_FALSE(spin.valid());
    EXPECT_NE(ErrorOf(spin).find("instruction budget"), std::string::npos);
}

TEST_F(LuaBudgetTest, ScriptsCannotSetAFinalizerBecauseTheVmRunsItWithTheHookOff)
{
    const sol::protected_function_result gc = Run("setmetatable({}, {__gc = function() while true do end end})");
    ASSERT_FALSE(gc.valid());
    EXPECT_NE(ErrorOf(gc).find("__gc"), std::string::npos);

    const sol::protected_function_result plain = Run("return getmetatable(setmetatable({}, {__index = {a = 1}})).__index.a");
    ASSERT_TRUE(plain.valid()) << ErrorOf(plain);
    EXPECT_EQ(plain[0].get<int>(), 1);
}

TEST_F(LuaBudgetTest, QuitFromLuaIsRefusedOnAnEditorHostAndLoggedOnce)
{
    Application::SetScriptQuitAllowed(false);

    const sol::protected_function_result first = Run("Application.Quit() Application.Quit()");
    ASSERT_TRUE(first.valid()) << ErrorOf(first);

    EXPECT_FALSE(Application::GetInstance().IsQuitRequested());
    EXPECT_EQ(CountLogs("Application.Quit is ignored on an editor host", Logger::LogLevel::Warn), 1);
}

TEST_F(LuaBudgetTest, QuitFromLuaStillQuitsAGame)
{
    ASSERT_TRUE(Application::IsScriptQuitAllowed());

    const sol::protected_function_result quit = Run("Application.Quit()");
    ASSERT_TRUE(quit.valid()) << ErrorOf(quit);

    EXPECT_TRUE(Application::GetInstance().IsQuitRequested());
    EXPECT_EQ(CountLogs("ignored on an editor host", Logger::LogLevel::Warn), 0);

    // C++ callers are not affected by the flag
    Application::GetInstance().ResetQuitRequest();
    Application::SetScriptQuitAllowed(false);
    Application::Quit();
    EXPECT_TRUE(Application::GetInstance().IsQuitRequested());
}
