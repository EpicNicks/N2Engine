#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
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
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

// engine/src/scripting/LuaPanicRepair.cpp: the thread's nCcalls (C calls in the low 16 bits, non-yieldable calls in
// the high 16), for the panic test
namespace N2Engine::Scripting::Detail
{
    std::uint32_t ThreadCallCounts(lua_State *L);
}

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

        // A collision handler (its argument is a userdata the engine builds before the call)
        WriteAsset("budget/Collider.lua", R"(
            local Collider = {}
            Collider.__index = Collider
            function Collider:OnCollisionEnter(collision)
                budget_collisions = budget_collisions + 1
            end
            return Collider
        )");

        // A module that returns many values
        WriteAsset("budget/Many.lua",
                   "return {}, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20\n");

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
    // Every test here runs a loop that only the budget stops: if the budget is broken, the program ends naming the
    // test instead of spinning until the CI job's limit
    std::unique_ptr<Tests::HangWatchdog> _watchdog;

    void SetUp() override
    {
        _watchdog = std::make_unique<Tests::HangWatchdog>();
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
        _watchdog.reset();
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

    // Every line the test captured, with its level: printed when a log expectation fails, so CI shows what was logged
    std::string DumpLogs() const
    {
        std::string out = "\ncaptured log lines (" + std::to_string(_logs.size()) + "):";
        for (const auto &[message, logLevel] : _logs)
        {
            const char *level = logLevel == Logger::LogLevel::Error ? "Error"
                                : logLevel == Logger::LogLevel::Warn ? "Warn"
                                                                     : "Info";
            out += "\n  [" + std::string(level) + "] " + message;
        }
        return out;
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
    EXPECT_EQ(CountLogs("instruction budget", Logger::LogLevel::Error), 2) << DumpLogs();
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

    // ...and xpcall still hands ordinary errors to the script's handler (it is kept only from the budget error, which
    // Lua would hand it inside the count hook, where nothing counts its instructions)
    const sol::protected_function_result handled = Run(
        "return xpcall(error, function(message) return 'handled: ' .. message end, 'plain')");
    ASSERT_TRUE(handled.valid()) << ErrorOf(handled);
    EXPECT_FALSE(handled[0].get<bool>());
    EXPECT_EQ(handled[1].get<std::string>(), "handled: plain");

    // A handler whose own instructions run out of budget (after an ordinary error) is stopped too
    Lua()["budget_handler_after"] = false;
    const sol::protected_function_result handlerLoop = Run(
        "xpcall(error, function(message) while true do end end, 'plain') budget_handler_after = true");
    EXPECT_FALSE(handlerLoop.valid());
    EXPECT_NE(ErrorOf(handlerLoop).find("instruction budget"), std::string::npos) << ErrorOf(handlerLoop);
    EXPECT_FALSE(Lua()["budget_handler_after"].get<bool>());
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
    EXPECT_EQ(CountLogs("not enough memory", Logger::LogLevel::Error), 1) << DumpLogs();
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
    EXPECT_EQ(CountLogs("callback aborted", Logger::LogLevel::Error), 1) << DumpLogs();
}

TEST_F(LuaBudgetTest, AnArgumentThatCannotBeAllocatedAtTheCapSwitchesTheCallbackOffAndNothingThrows)
{
    // An input callback and a collision handler each get a userdata argument, which is allocated before the protected
    // call (sol pushes a callback's arguments first; the engine builds the collision first). With the cap below what
    // the state holds, that allocation fails outside every protected call, down the panic path: the C++ exception sol
    // throws must stay in the engine, the callback and the component are switched off, and the error is logged once.
    // Nothing here depends on free registry slots: the copy of the callback EventHandler makes for each dispatch
    // copies a shared pointer (MakeScriptCallback), not the sol::protected_function and its registry references.
    Lua()["budget_action"] = InputActionRef(_action);
    Lua()["budget_hits"] = 0;
    Lua()["budget_collisions"] = 0;
    Lua().script("budget_action:Subscribe(function(action) budget_hits = budget_hits + 1 end)");
    const auto go = GameObject::Create("Collider");
    auto *collider = Attach(go, "res://budget/Collider.lua");
    ASSERT_FALSE(collider->HasMissingScript());
    const auto other = GameObject::Create("Other");
    Physics::Collision collision;
    collision.gameObject = go.get();
    collision.otherGameObject = other.get();

    Lua().collect_garbage();
    // Half of what the state holds: no allocation that grows it can succeed, even after Lua's emergency collection
    Runtime().SetLimits({TestBudget, Runtime().GetMemoryUsed() / 2});
    for (int event = 0; event < 3; ++event)
    {
        EXPECT_NO_THROW(_action.GetOnStateChanged()(_action));
        EXPECT_NO_THROW(collider->OnCollisionEnter(collision));
    }
    Runtime().SetLimits({TestBudget, Runtime().GetMemoryUsed() + 16 * MiB});

    EXPECT_EQ(Lua()["budget_hits"].get<int>(), 0);
    EXPECT_EQ(Lua()["budget_collisions"].get<int>(), 0);
    EXPECT_EQ(CountLogs("callback aborted", Logger::LogLevel::Error), 1) << DumpLogs();
    EXPECT_EQ(CountLogs("res://budget/Collider.lua aborted", Logger::LogLevel::Error), 1) << DumpLogs();
    EXPECT_EQ(CountLogs("not enough memory", Logger::LogLevel::Error), 2) << DumpLogs();

    // The stack is back at its base and the state works
    EXPECT_EQ(lua_gettop(Lua().lua_state()), 0);
    const sol::protected_function_result normal = Run("local t = {} for i = 1, 100 do t[i] = i end return #t");
    ASSERT_TRUE(normal.valid()) << ErrorOf(normal);
    EXPECT_EQ(normal[0].get<int>(), 100);
}

TEST_F(LuaBudgetTest, TheEnginesLoadingKeepsOneValueOfAChunkOnTheStack)
{
    // A chunk can return any number of values. The engine's loading (RunChunk: RunFile, require and hot reload,
    // LuaComponent) keeps only the first, so if an allocation after it fails down the panic path, unwinding the
    // result pops one value, within the panic function's padding, instead of as many as the script returned.
    lua_State *L = Lua().lua_state();
    const int before = lua_gettop(L);
    {
        const sol::protected_function_result many = Runtime().RunChunk(
            "return {}, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20", "budget-many");
        ASSERT_TRUE(many.valid()) << ErrorOf(many);
        EXPECT_EQ(many.return_count(), 1);
        EXPECT_EQ(lua_gettop(L), before + 1);
        EXPECT_TRUE(many[0].is<sol::table>());
    }
    EXPECT_EQ(lua_gettop(L), before);

    // require of such a module still hands the script its table
    const sol::protected_function_result required = Run("return type(require('budget.Many'))");
    ASSERT_TRUE(required.valid()) << ErrorOf(required);
    EXPECT_EQ(required[0].get<std::string>(), "table");
}

TEST_F(LuaBudgetTest, ANumberErrorObjectIsReportedWithoutConvertingIt)
{
    // Converting a number error object to text makes a new string: at the memory cap, outside a protected call, that
    // allocation would go down the panic path (out of a destructor, std::terminate). The engine reads a string as it is
    // and formats a number from its value into a C++ buffer, never converting the Lua object.
    // (A chunk or a method call goes through sol's default message handler, a traceback, which already hands back a
    // string; ProtectedCall has no handler, so its error object is the script's value itself.)
    const auto readError = [](const char *errorValue)
    {
        sol::table raising;
        {
            const sol::protected_function_result made = Run(
                std::string("return setmetatable({}, {__index = function(self, key) error(") + errorValue + ") end})");
            EXPECT_TRUE(made.valid()) << ErrorOf(made);
            if (!made.valid())
            {
                return std::string();
            }
            raising = made[0].get<sol::table>();
        }
        std::string accessError;
        EXPECT_FALSE(Runtime().ProtectedCall([&] { (void)raising["anything"].valid(); }, accessError));
        return accessError;
    };
    const std::string fraction = readError("0.123");
    const std::string integer = readError("404");
#if (defined(_MSC_VER) && (defined(_DEBUG) || defined(N2ENGINE_DEBUG))) || defined(N2ENGINE_PLATFORM_WEB)
    // MSVC Debug and the web build: the longjmp out of the body runs C++ cleanups that pop the stack (MSVC by design;
    // on the web build longjmp is a WebAssembly exception), so the slot Lua takes as the error object can be another
    // value (seen on MSVC Debug: the __index function, "(a function error value)"; a string slot, such as the key,
    // would be reported as it is). The known limitation in docs/scripting-lua.html (resource limits). Only a text is
    // required there.
    EXPECT_FALSE(fraction.empty());
    EXPECT_FALSE(integer.empty());
#else
    EXPECT_EQ(fraction, "(a number error value: 0.123)");
    EXPECT_EQ(integer, "(a number error value: 404)");
#endif
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

    EXPECT_GE(CountLogs("instruction budget", Logger::LogLevel::Error), 1) << DumpLogs();

    // The same read straight through ProtectedCall (what GuardedAccess uses), so a failure above shows which error
    // text the access path produced
    sol::table looping;
    {
        const sol::protected_function_result made =
            Run("return setmetatable({}, {__index = function(self, key) while true do end end})");
        ASSERT_TRUE(made.valid()) << ErrorOf(made);
        looping = made[0].get<sol::table>();
    }
    std::string accessError;
    EXPECT_FALSE(Runtime().ProtectedCall([&] { (void)looping["anything"].valid(); }, accessError));
    EXPECT_NE(accessError.find("instruction budget"), std::string::npos) << "ProtectedCall error: '" << accessError << "'";

    const sol::protected_function_result normal = Run("local t = {} for i = 1, 100 do t[i] = i end return #t");
    ASSERT_TRUE(normal.valid()) << ErrorOf(normal);
    EXPECT_EQ(normal[0].get<int>(), 100);

    // The count hook is still alive: a loop is still aborted (an error that went down the panic path used to leave
    // the hook disabled for good)
    const sol::protected_function_result spin = Run("while true do end");
    ASSERT_FALSE(spin.valid());
    EXPECT_NE(ErrorOf(spin).find("instruction budget"), std::string::npos);
}

TEST_F(LuaBudgetTest, TheBudgetErrorOnThePanicPathLeavesTheHookAndTheStateWorking)
{
    // An unprotected call (lua_call, no pcall anywhere): the count hook's error reaches the panic function, which sol
    // turns into a C++ exception. Lua leaves the thread's hook flag off after an error raised inside a hook unless a
    // protected call restores it, so without the repair in the panic function no later loop would ever be stopped.
    // Hundreds of them must not wear out the C-call counter either ("C stack overflow").
    // At rest (no call running) the main thread counts no C call and the one non-yieldable call lua_newstate gives it
    constexpr std::uint32_t FreshMainThread = 0x10000;
    lua_State *L = Lua().lua_state();
    EXPECT_EQ(N2Engine::Scripting::Detail::ThreadCallCounts(L), FreshMainThread);

    Lua().script("budget_unprotected = function() while true do end end");
    sol::unsafe_function unprotected = Lua()["budget_unprotected"];
    const std::uint64_t panicsBefore = Runtime().PanicCount();
    for (int attempt = 0; attempt < 250; ++attempt)
    {
        EXPECT_ANY_THROW(unprotected());
    }
    EXPECT_EQ(Runtime().PanicCount() - panicsBefore, 250u);
    Runtime().RecoverAfterPanic(panicsBefore); // the panic function leaves padding on the stack for its catcher
    EXPECT_EQ(lua_gettop(L), 0);
    Lua()["budget_unprotected"] = sol::lua_nil;

    // Each call the errors unwound added to both halves; the repair puts them back to the fresh values
    const std::uint32_t calls = N2Engine::Scripting::Detail::ThreadCallCounts(L);
    EXPECT_EQ(calls & 0xffffu, 0u) << "C calls left counted after the panics";
    EXPECT_EQ(calls & 0xffff0000u, FreshMainThread) << "non-yieldable calls left counted after the panics";

    const sol::protected_function_result normal = Run("local t = {} for i = 1, 100 do t[i] = i end return #t");
    ASSERT_TRUE(normal.valid()) << ErrorOf(normal);
    EXPECT_EQ(normal[0].get<int>(), 100);

    // The result above is still held: the next entry must not touch the stack under it
    const sol::protected_function_result spin = Run("while true do end");
    ASSERT_FALSE(spin.valid());
    EXPECT_NE(ErrorOf(spin).find("instruction budget"), std::string::npos) << ErrorOf(spin);
    EXPECT_EQ(normal[0].get<int>(), 100);
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
    EXPECT_EQ(CountLogs("Application.Quit is ignored on an editor host", Logger::LogLevel::Warn), 1) << DumpLogs();
}

TEST_F(LuaBudgetTest, QuitFromLuaStillQuitsAGame)
{
    ASSERT_TRUE(Application::IsScriptQuitAllowed());

    const sol::protected_function_result quit = Run("Application.Quit()");
    ASSERT_TRUE(quit.valid()) << ErrorOf(quit);

    EXPECT_TRUE(Application::GetInstance().IsQuitRequested());
    EXPECT_EQ(CountLogs("ignored on an editor host", Logger::LogLevel::Warn), 0) << DumpLogs();

    // C++ callers are not affected by the flag
    Application::GetInstance().ResetQuitRequest();
    Application::SetScriptQuitAllowed(false);
    Application::Quit();
    EXPECT_TRUE(Application::GetInstance().IsQuitRequested());
}
