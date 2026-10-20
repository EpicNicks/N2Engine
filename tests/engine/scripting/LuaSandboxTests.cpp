#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/scripting/DeclarationEvaluator.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaSandbox.hpp"

#include "HangWatchdog.hpp"

// The shared sandbox builder (Lua editor extensions, PR E-X1): a fresh, capped, budgeted lua_State per profile. The
// Declarations profile is what DeclarationEvaluator builds on (its own tests are the oracle for it); the
// EditorExtension profile has no user yet, these tests are its whole use.

using namespace N2Engine;
using namespace N2Engine::Scripting;

namespace
{
    constexpr SandboxProfile Profiles[] = {SandboxProfile::Declarations, SandboxProfile::EditorExtension};

    const char *ProfileName(const SandboxProfile profile)
    {
        return profile == SandboxProfile::Declarations ? "Declarations" : "EditorExtension";
    }

    // Plenty for any small chunk, and still bounded
    constexpr SandboxBudget Roomy{50'000'000, 20'000};
    // A tight instruction budget with a long clock: only the instruction count can end a loop
    constexpr SandboxBudget Instructions{1'000'000, 60'000};
    // A huge instruction budget with a short clock: only the wall clock can end a loop
    constexpr SandboxBudget Clock{1'000'000'000'000ULL, 100};
    // A cap small enough to reach in a few thousand allocations
    constexpr std::size_t SmallCap = std::size_t{4} * 1024 * 1024;

    std::unique_ptr<LuaSandbox> Make(const SandboxProfile profile, const SandboxOptions &options = {})
    {
        auto sandbox = LuaSandbox::Create(profile, options);
        EXPECT_TRUE(sandbox != nullptr);
        return sandbox;
    }

    SandboxOutcome Exec(LuaSandbox &sandbox, const std::string &source, const SandboxBudget &budget = Roomy,
                       const int results = 0)
    {
        return sandbox.RunChunk(source, "test.lua", budget, results);
    }

    // The state is closed: nothing the allocator counted is left, none ran past its cap
    void CloseChecked(LuaSandbox &sandbox)
    {
        sandbox.Close();
        const SandboxCounters counters = sandbox.Counters();
        EXPECT_EQ(counters.usedBytes, 0u) << "bytes leaked";
        EXPECT_LE(counters.peakBytes, counters.capBytes);
        EXPECT_EQ(sandbox.State(), nullptr);
    }

    // The value of an expression as text: strings as they are, booleans as true/false, integers in decimal, nil
    std::string Value(LuaSandbox &sandbox, const std::string &expression)
    {
        const SandboxOutcome outcome = sandbox.RunChunk("return " + expression, "value.lua", Roomy, 1);
        if (!outcome.ok)
        {
            return "<error>";
        }
        lua_State *L = sandbox.State();
        std::string text;
        switch (lua_type(L, -1))
        {
        case LUA_TNIL:
            text = "nil";
            break;
        case LUA_TBOOLEAN:
            text = lua_toboolean(L, -1) != 0 ? "true" : "false";
            break;
        case LUA_TSTRING:
            text = lua_tostring(L, -1);
            break;
        case LUA_TNUMBER:
            text = lua_isinteger(L, -1) != 0 ? std::to_string(static_cast<long long>(lua_tointeger(L, -1))) : "<float>";
            break;
        default:
            text = lua_typename(L, lua_type(L, -1));
            break;
        }
        lua_settop(L, 0);
        return text;
    }

    // The string keys of a global table (_G when name is null), sorted
    std::vector<std::string> KeysOf(lua_State *L, const char *name = nullptr)
    {
        std::vector<std::string> keys;
        if (name == nullptr)
        {
            lua_pushglobaltable(L);
        }
        else
        {
            lua_getglobal(L, name);
        }
        if (lua_istable(L, -1))
        {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0)
            {
                if (lua_type(L, -2) == LUA_TSTRING)
                {
                    keys.emplace_back(lua_tostring(L, -2));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        std::ranges::sort(keys);
        return keys;
    }

    std::vector<std::string> Sorted(std::vector<std::string> names)
    {
        std::ranges::sort(names);
        return names;
    }

    // What the standard base library leaves, with the other libraries: the Declarations profile's globals
    const std::vector<std::string> DeclarationsGlobals = Sorted({
        "_G", "_VERSION", "assert", "collectgarbage", "dofile", "error", "getmetatable", "ipairs", "load", "loadfile",
        "math", "next", "pairs", "pcall", "print", "rawequal", "rawget", "rawlen", "rawset", "select", "setmetatable",
        "string", "table", "tonumber", "tostring", "type", "warn", "xpcall"});

    // The EditorExtension profile's: the base library without load, loadfile, dofile, collectgarbage, print and warn,
    // plus math, string, table and utf8. Nothing of the engine, no Editor namespace yet (E-X3).
    const std::vector<std::string> EditorGlobals = Sorted({
        "_G", "_VERSION", "assert", "error", "getmetatable", "ipairs", "math", "next", "pairs", "pcall", "rawequal",
        "rawget", "rawlen", "rawset", "select", "setmetatable", "string", "table", "tonumber", "tostring", "type",
        "utf8", "xpcall"});

    // What the declaration evaluator leaves after its environment script (what a declaration script sees)
    const std::vector<std::string> EvaluatorGlobals = Sorted({
        "Vector3", "_G", "_VERSION", "assert", "error", "getmetatable", "ipairs", "math", "next", "pairs", "pcall",
        "rawequal", "rawget", "rawlen", "rawset", "select", "setmetatable", "string", "table", "tonumber", "tostring",
        "type", "xpcall"});

    double ElapsedMs(const std::chrono::steady_clock::time_point since)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
    }
}

class LuaSandboxTest : public ::testing::Test
{
protected:
    std::unique_ptr<Tests::HangWatchdog> _watchdog;
    int _liveBefore = 0;

    void SetUp() override
    {
        // Every test here must end by itself: a loop only the budget stops would otherwise run until the job's limit
        _watchdog = std::make_unique<Tests::HangWatchdog>(std::chrono::seconds(60));
        _liveBefore = LuaSandbox::LiveStates();
    }

    void TearDown() override
    {
        EXPECT_EQ(LuaSandbox::LiveStates(), _liveBefore) << "a sandbox was left open";
        _watchdog.reset();
    }
};

// ==================== the defaults and the shape ====================

TEST_F(LuaSandboxTest, TheDefaultsAreTheDesignsBudgets)
{
    EXPECT_EQ(SandboxDefaults::LoadBudget.instructions, 10'000'000u);
    EXPECT_EQ(SandboxDefaults::LoadBudget.maxMillis, 1000u);
    EXPECT_EQ(SandboxDefaults::InvocationBudget.instructions, 200'000'000u);
    EXPECT_EQ(SandboxDefaults::InvocationBudget.maxMillis, 5000u);
    EXPECT_EQ(SandboxDefaults::HookBudget.instructions, 20'000'000u);
    EXPECT_EQ(SandboxDefaults::HookBudget.maxMillis, 500u);
    EXPECT_EQ(SandboxDefaults::MaxBytes, std::size_t{64} * 1024 * 1024);
    EXPECT_EQ(SandboxDefaults::MaxNestedCommands, 10'000u);
    EXPECT_TRUE(SandboxDefaults::EditorExtensionPrelude.empty());

    const SandboxOptions options;
    EXPECT_EQ(options.maxBytes, 0u);
    EXPECT_EQ(options.loadBudget.instructions, SandboxDefaults::LoadBudget.instructions);
    EXPECT_EQ(options.loadBudget.maxMillis, SandboxDefaults::LoadBudget.maxMillis);
    EXPECT_FALSE(options.prelude.has_value());
}

TEST_F(LuaSandboxTest, TheCapIsTheDefaultOrTheRequestRaisedToTheMinimum)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto byDefault = Make(profile);
        EXPECT_EQ(byDefault->Profile(), profile);
        EXPECT_EQ(byDefault->Counters().capBytes, SandboxDefaults::MaxBytes);
        CloseChecked(*byDefault);

        SandboxOptions custom;
        custom.maxBytes = SmallCap;
        auto small = Make(profile, custom);
        EXPECT_EQ(small->Counters().capBytes, SmallCap);
        CloseChecked(*small);

        custom.maxBytes = 1000;
        auto tiny = Make(profile, custom);
        EXPECT_EQ(tiny->Counters().capBytes, SandboxDefaults::MinBytes);
        EXPECT_EQ(Value(*tiny, "1 + 1"), "2") << "a state at the minimum cap works";
        CloseChecked(*tiny);
    }
}

TEST_F(LuaSandboxTest, ClosingIsIdempotentAndALaterRunIsRefusedAsData)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    EXPECT_EQ(LuaSandbox::LiveStates(), _liveBefore + 1);
    EXPECT_NE(sandbox->State(), nullptr);
    sandbox->Close();
    EXPECT_EQ(LuaSandbox::LiveStates(), _liveBefore);
    sandbox->Close();
    EXPECT_EQ(sandbox->State(), nullptr);
    const SandboxOutcome outcome = Exec(*sandbox, "return 1");
    EXPECT_FALSE(outcome.ok);
    EXPECT_FALSE(outcome.error.empty());
    EXPECT_EQ(sandbox->Counters().usedBytes, 0u);
}

TEST_F(LuaSandboxTest, TheDestructorClosesTheState)
{
    {
        auto sandbox = Make(SandboxProfile::EditorExtension);
        EXPECT_EQ(LuaSandbox::LiveStates(), _liveBefore + 1);
        EXPECT_TRUE(Exec(*sandbox, "local t = {} for i = 1, 100 do t[i] = {i} end").ok);
    }
    EXPECT_EQ(LuaSandbox::LiveStates(), _liveBefore);
}

// ==================== the globals of each state ====================

TEST_F(LuaSandboxTest, TheDeclarationsProfileHasExactlyTheStandardLibrariesGlobals)
{
    auto sandbox = Make(SandboxProfile::Declarations);
    EXPECT_EQ(KeysOf(sandbox->State()), DeclarationsGlobals);
    // The budget sandbox's helper is removed again once pcall and xpcall have captured it
    EXPECT_EQ(Value(*sandbox, "rawget(_G, '__n2_is_budget_error')"), "nil");
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, TheEditorExtensionProfileHasExactlyTheAllowlistedGlobals)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    EXPECT_EQ(KeysOf(sandbox->State()), EditorGlobals);
    EXPECT_TRUE(sandbox->PreludeOutcome().ok);
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, TheLibrariesOfEachProfileHaveExactlyTheAllowlistedMembers)
{
    const std::vector<std::string> stringFull =
        Sorted({"byte", "char", "dump", "find", "format", "gmatch", "gsub", "len", "lower", "match", "pack",
                "packsize", "rep", "reverse", "sub", "unpack", "upper"});
    // Without find, match, gmatch, gsub (design D6) and dump
    const std::vector<std::string> stringTrimmed =
        Sorted({"byte", "char", "format", "len", "lower", "pack", "packsize", "rep", "reverse", "sub", "unpack", "upper"});
    const std::vector<std::string> tableMembers =
        Sorted({"concat", "insert", "move", "pack", "remove", "sort", "unpack"});
    const std::vector<std::string> utf8Members =
        Sorted({"char", "charpattern", "codepoint", "codes", "len", "offset"});

    auto declarations = Make(SandboxProfile::Declarations);
    EXPECT_EQ(KeysOf(declarations->State(), "string"), stringFull);
    EXPECT_EQ(KeysOf(declarations->State(), "table"), tableMembers);
    EXPECT_TRUE(KeysOf(declarations->State(), "utf8").empty());
    CloseChecked(*declarations);

    auto editor = Make(SandboxProfile::EditorExtension);
    EXPECT_EQ(KeysOf(editor->State(), "string"), stringTrimmed);
    EXPECT_EQ(KeysOf(editor->State(), "table"), tableMembers);
    EXPECT_EQ(KeysOf(editor->State(), "utf8"), utf8Members);
    CloseChecked(*editor);
}

TEST_F(LuaSandboxTest, TheEvaluatorsEnvironmentHasExactlyTheAllowlistedGlobals)
{
    // The declaration evaluator runs its environment script in the Declarations profile's state; a script sees what
    // is left. The script lists its own globals as the fields it declares.
    const DeclarationResult result = DeclarationEvaluator::Evaluate(R"LUA(
        local names = {}
        for key in pairs(_G) do
            names[key] = true
        end
        local C = {}
        C.__index = C
        C.SerializableFields = names
        return C
    )LUA", "globals.lua");
    ASSERT_TRUE(result.errors.empty()) << result.errors[0];
    std::vector<std::string> names;
    for (const DeclaredField &field : result.fields)
    {
        names.push_back(field.name);
    }
    std::ranges::sort(names);
    EXPECT_EQ(names, EvaluatorGlobals);
    EXPECT_EQ(result.leakedBytes, 0u);
}

TEST_F(LuaSandboxTest, NothingOfTheEngineOrTheHostIsInEitherProfile)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto sandbox = Make(profile);
        for (const char *name : {"io", "os", "debug", "package", "require", "coroutine", "Editor", "Application",
                                 "GameObject", "Debug", "Input", "Vector3", "Quaternion", "Time", "Scene",
                                 "__n2_is_budget_error"})
        {
            EXPECT_EQ(Value(*sandbox, std::string("rawget(_G, '") + name + "')"), "nil") << name;
        }
        CloseChecked(*sandbox);
    }
}

// ==================== the EditorExtension profile has no way to code, files or patterns ====================

TEST_F(LuaSandboxTest, EditorExtensionCannotLoadCodeReadFilesOrSteerTheCollector)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    for (const char *name : {"load", "loadstring", "loadfile", "dofile", "collectgarbage", "print", "warn", "require",
                             "io", "os", "debug", "package", "coroutine"})
    {
        EXPECT_EQ(Value(*sandbox, std::string("rawget(_G, '") + name + "')"), "nil") << name;
        EXPECT_EQ(Value(*sandbox, std::string("_G.") + name), "nil") << name;
        EXPECT_FALSE(Exec(*sandbox, std::string("local f = ") + name + " f('return 1')").ok) << name << " is callable";
    }
    // No bytecode either: string.dump is absent and the text-only loader of the host refuses a binary chunk
    EXPECT_EQ(Value(*sandbox, "string.dump"), "nil");
    EXPECT_FALSE(Exec(*sandbox, std::string("\x1b") + "Lua").ok);
    // Reaching for a library through the strings' metatable or _G finds nothing either
    EXPECT_EQ(Value(*sandbox, "_G._G.io"), "nil");
    EXPECT_EQ(Value(*sandbox, "getmetatable('')"), "protected");
    EXPECT_EQ(Value(*sandbox, "pcall(function() getmetatable('').__index = {} end)"), "false");
    EXPECT_EQ(Value(*sandbox, "(''):len()"), "0");
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, EditorExtensionHasNoStringPatterns)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    for (const char *name : {"find", "match", "gmatch", "gsub"})
    {
        EXPECT_EQ(Value(*sandbox, std::string("string.") + name), "nil") << name;
        // The method form goes through the strings' metatable, which reaches the same table
        EXPECT_EQ(Value(*sandbox, std::string("('abc').") + name), "nil") << name;
        const SandboxOutcome outcome = Exec(*sandbox, std::string("return ('abc'):") + name + "('b')");
        EXPECT_FALSE(outcome.ok) << name;
        EXPECT_FALSE(outcome.budgetExceeded);
        EXPECT_FALSE(outcome.memoryExhausted);
    }
    // The pattern string utf8 offers is data; no function in the state takes it
    EXPECT_EQ(Value(*sandbox, "type(utf8.charpattern)"), "string");
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, TheDeclarationsProfileKeepsTheFullStringLibrary)
{
    // The evaluator replaces the pattern functions with its placeholder itself; the builder leaves the library alone
    auto sandbox = Make(SandboxProfile::Declarations);
    EXPECT_EQ(Value(*sandbox, "('hello world'):find('wor', 1, true)"), "7");
    EXPECT_EQ(Value(*sandbox, "('a.b'):gsub('%.', '+')"), "a+b");
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, TheTrimmedStringLibraryAndTheRestStillBehave)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    EXPECT_EQ(Value(*sandbox, "string.format('%5.1f|%d|%s|%x', 3.14159, 42, 'x', 255)"), "  3.1|42|x|ff");
    EXPECT_EQ(Value(*sandbox, "string.rep('ab', 3, '-')"), "ab-ab-ab");
    EXPECT_EQ(Value(*sandbox, "('x'):rep(4)"), "xxxx");
    EXPECT_EQ(Value(*sandbox, "#string.rep('x', 1000)"), "1000");
    EXPECT_EQ(Value(*sandbox, "string.rep('x', 0)"), "");
    EXPECT_EQ(Value(*sandbox, "string.sub('hello', 2, 4)"), "ell");
    EXPECT_EQ(Value(*sandbox, "('hello'):sub(-3)"), "llo");
    EXPECT_EQ(Value(*sandbox, "string.len('hello')"), "5");
    EXPECT_EQ(Value(*sandbox, "('abc'):upper() .. ('ABC'):lower()"), "ABCabc");
    EXPECT_EQ(Value(*sandbox, "('abc'):reverse()"), "cba");
    EXPECT_EQ(Value(*sandbox, "string.byte('A')"), "65");
    EXPECT_EQ(Value(*sandbox, "string.char(72, 105)"), "Hi");
    EXPECT_EQ(Value(*sandbox, "tostring(12) .. tostring(true)"), "12true");
    EXPECT_EQ(Value(*sandbox, "tonumber('0x10')"), "16");
    EXPECT_EQ(Value(*sandbox, "utf8.len(utf8.char(228, 8364))"), "2");
    EXPECT_EQ(Value(*sandbox, "#utf8.char(8364)"), "3");
    EXPECT_EQ(Value(*sandbox, "math.floor(2.5)"), "2");
    EXPECT_EQ(Value(*sandbox, "math.max(3, 9, 4)"), "9");
    EXPECT_EQ(Value(*sandbox, "math.tointeger(3.0)"), "3");
    EXPECT_EQ(Value(*sandbox, "select('#', 1, 2, 3)"), "3");
    EXPECT_EQ(Value(*sandbox, "(pcall(error, 'x'))"), "false");
    EXPECT_EQ(Value(*sandbox, "(function() local t = {3, 1, 2} table.sort(t) return table.concat(t, ',') end)()"),
              "1,2,3");
    EXPECT_EQ(Value(*sandbox, "(function() local t = {} table.insert(t, 'a') table.insert(t, 1, 'b') "
                              "return table.concat(t, '') .. #t end)()"),
              "ba2");
    EXPECT_EQ(Value(*sandbox, "table.unpack({7, 8, 9}, 2)"), "8");
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, RepeatingAnEmptyStringDoesNotLoopInC)
{
    // One C call with no allocation: neither the cap nor the hook could stop 2^60 repetitions
    auto sandbox = Make(SandboxProfile::EditorExtension);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(Value(*sandbox, "string.rep('', 1152921504606846976)"), "");
    EXPECT_EQ(Value(*sandbox, "(''):rep(1152921504606846976, '')"), "");
    EXPECT_LT(ElapsedMs(started), 20000.0);
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, AScriptCannotGiveItselfAFinalizer)
{
    // The VM runs a __gc finalizer with its hooks off, so no budget could see into it
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto sandbox = Make(profile);
        const SandboxOutcome outcome = Exec(*sandbox, "setmetatable({}, { __gc = function() end })");
        EXPECT_FALSE(outcome.ok);
        EXPECT_FALSE(outcome.budgetExceeded);
        CloseChecked(*sandbox);
    }
}

TEST_F(LuaSandboxTest, OrdinaryErrorsAreCaughtByPcallAndTheStateKeepsWorking)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto sandbox = Make(profile);
        EXPECT_EQ(Value(*sandbox, "(pcall(function() error('boom') end))"), "false");
        EXPECT_EQ(Value(*sandbox, "(select(2, xpcall(function() error('boom') end, function(m) return 'handled' end)))"),
                  "handled");
        EXPECT_EQ(Value(*sandbox, "40 + 2"), "42");
        CloseChecked(*sandbox);
    }
}

// ==================== errors are data ====================

TEST_F(LuaSandboxTest, ScriptErrorsAreReportedAsData)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto sandbox = Make(profile);
        for (const char *source : {"error('boom')", "error({})", "error(nil)", "local x = nil + 1", "x = = 1",
                                   "return (", "local t = nil; t.x = 1", "error(404)"})
        {
            const SandboxOutcome outcome = Exec(*sandbox, source);
            EXPECT_FALSE(outcome.ok) << source;
            EXPECT_FALSE(outcome.budgetExceeded) << source;
            EXPECT_FALSE(outcome.timedOut) << source;
            EXPECT_FALSE(outcome.memoryExhausted) << source;
            EXPECT_FALSE(outcome.error.empty()) << source;
            EXPECT_EQ(lua_gettop(sandbox->State()), 0) << "a failed run leaves nothing on the stack: " << source;
        }
        EXPECT_EQ(Value(*sandbox, "1 + 1"), "2");
        CloseChecked(*sandbox);
    }
}

TEST_F(LuaSandboxTest, AControlCharacterInAnErrorIsReplaced)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    const SandboxOutcome outcome = Exec(*sandbox, "error('a\\1b\\27c', 0)");
    EXPECT_FALSE(outcome.ok);
    for (const char c : outcome.error)
    {
        EXPECT_GE(static_cast<unsigned char>(c), 0x20u);
    }
    EXPECT_LE(outcome.error.size(), std::size_t{300});
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, ARunLeavesTheRequestedNumberOfResults)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    EXPECT_TRUE(Exec(*sandbox, "return 1, 2, 3", Roomy, 2).ok);
    EXPECT_EQ(lua_gettop(sandbox->State()), 2);
    EXPECT_EQ(lua_tointeger(sandbox->State(), 1), 1);
    EXPECT_EQ(lua_tointeger(sandbox->State(), 2), 2);
    lua_settop(sandbox->State(), 0);
    EXPECT_TRUE(Exec(*sandbox, "return 1, 2, 3").ok);
    EXPECT_EQ(lua_gettop(sandbox->State()), 0);
    EXPECT_TRUE(Exec(*sandbox, "return 1", Roomy, -1).ok);
    EXPECT_EQ(lua_gettop(sandbox->State()), 0) << "a negative count is treated as none";
    CloseChecked(*sandbox);
}

// ==================== budgets ====================

TEST_F(LuaSandboxTest, AnInfiniteLoopEndsAsABudgetTripInBoundedTime)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto sandbox = Make(profile);
        const auto started = std::chrono::steady_clock::now();
        const SandboxOutcome outcome = Exec(*sandbox, "while true do end", Instructions);
        EXPECT_LT(ElapsedMs(started), 30000.0);
        EXPECT_FALSE(outcome.ok);
        EXPECT_TRUE(outcome.budgetExceeded);
        EXPECT_FALSE(outcome.timedOut) << "the instruction count ended it, not the clock";
        EXPECT_FALSE(outcome.memoryExhausted);
        EXPECT_FALSE(outcome.error.empty());
        const SandboxCounters counters = sandbox->Counters();
        EXPECT_TRUE(counters.budgetExhausted);
        EXPECT_EQ(counters.instructionLimit, Instructions.instructions);
        CloseChecked(*sandbox);
    }
}

TEST_F(LuaSandboxTest, ALoopInsidePcallXpcallOrAMetamethodIsStillATrip)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto sandbox = Make(profile);
        for (const char *source : {
                 "pcall(function() while true do end end) return 1",
                 "pcall(pcall, function() while true do end end) return 1",
                 "xpcall(function() while true do end end, function(m) return m end) return 1",
                 "local t = setmetatable({}, { __index = function() while true do end end }) local x = t.field return 1",
                 "local ok = pcall(function() while true do end end) while true do end"})
        {
            const SandboxOutcome outcome = Exec(*sandbox, source, Instructions);
            EXPECT_FALSE(outcome.ok) << source;
            EXPECT_TRUE(outcome.budgetExceeded) << source;
        }
        // And the state is whole afterwards: a new entry has a new budget
        EXPECT_EQ(Value(*sandbox, "6 * 7"), "42");
        CloseChecked(*sandbox);
    }
}

TEST_F(LuaSandboxTest, TheWallClockEndsWhatTheInstructionCountWouldNotYet)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto sandbox = Make(profile);
        const auto started = std::chrono::steady_clock::now();
        const SandboxOutcome outcome = Exec(*sandbox, "while true do end", Clock);
        EXPECT_LT(ElapsedMs(started), 30000.0);
        EXPECT_FALSE(outcome.ok);
        EXPECT_TRUE(outcome.budgetExceeded);
        EXPECT_TRUE(outcome.timedOut);
        EXPECT_TRUE(sandbox->Counters().timedOut);
        // The next entry starts with the clock and the count refilled
        EXPECT_EQ(Value(*sandbox, "1 + 2"), "3");
        EXPECT_FALSE(sandbox->Counters().timedOut);
        EXPECT_FALSE(sandbox->Counters().budgetExhausted);
        CloseChecked(*sandbox);
    }
}

TEST_F(LuaSandboxTest, AnEntryUnderItsBudgetRunsToTheEnd)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    const SandboxOutcome outcome =
        Exec(*sandbox, "local n = 0 for i = 1, 100000 do n = n + i end result = n", Instructions);
    EXPECT_TRUE(outcome.ok) << outcome.error;
    EXPECT_FALSE(outcome.budgetExceeded);
    EXPECT_EQ(Value(*sandbox, "result"), "5000050000");
    EXPECT_FALSE(sandbox->Counters().budgetExhausted);
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, AZeroBudgetIsTheLoadDefaultNeverUnlimited)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    const auto started = std::chrono::steady_clock::now();
    const SandboxOutcome outcome = Exec(*sandbox, "while true do end", SandboxBudget{});
    EXPECT_LT(ElapsedMs(started), 30000.0);
    EXPECT_FALSE(outcome.ok);
    EXPECT_TRUE(outcome.budgetExceeded);
    EXPECT_EQ(sandbox->Counters().instructionLimit, SandboxDefaults::LoadBudget.instructions);
    CloseChecked(*sandbox);
}

TEST_F(LuaSandboxTest, DeepRecursionEndsAsAnErrorNotACrash)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto sandbox = Make(profile);
        const SandboxOutcome outcome =
            Exec(*sandbox, "local function f(n) return 1 + f(n + 1) end return f(1)", SandboxDefaults::LoadBudget);
        EXPECT_FALSE(outcome.ok);
        EXPECT_FALSE(outcome.error.empty());
        EXPECT_EQ(Value(*sandbox, "1 + 1"), "2");
        CloseChecked(*sandbox);
    }
}

// ==================== memory ====================

TEST_F(LuaSandboxTest, AMemoryBombIsRefusedAtTheCapNotFollowed)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        SandboxOptions options;
        options.maxBytes = SmallCap;

        {
            auto sandbox = Make(profile, options);
            const SandboxOutcome outcome = Exec(*sandbox, "local s = string.rep('x', 1e9) return #s");
            EXPECT_FALSE(outcome.ok);
            EXPECT_TRUE(outcome.memoryExhausted);
            EXPECT_FALSE(outcome.budgetExceeded);
            EXPECT_FALSE(outcome.error.empty());
            EXPECT_EQ(Value(*sandbox, "40 + 2"), "42") << "usable after a memory error";
            CloseChecked(*sandbox);
        }
        {
            auto sandbox = Make(profile, options);
            const SandboxOutcome outcome = Exec(*sandbox, "local t = {} local i = 0 while true do i = i + 1 t[i] = {i} end");
            EXPECT_FALSE(outcome.ok);
            EXPECT_TRUE(outcome.memoryExhausted);
            EXPECT_FALSE(outcome.budgetExceeded);
            EXPECT_GT(sandbox->Counters().peakBytes, SmallCap / 2);
            EXPECT_LE(sandbox->Counters().peakBytes, SmallCap);
            CloseChecked(*sandbox);
        }
        {
            auto sandbox = Make(profile, options);
            const SandboxOutcome outcome = Exec(*sandbox, "local s = 'x' while true do s = s .. s end");
            EXPECT_FALSE(outcome.ok);
            EXPECT_TRUE(outcome.memoryExhausted);
            CloseChecked(*sandbox);
        }
        {
            // The string library's growth is in C, past what the hook sees: the allocator is what stops it
            auto sandbox = Make(profile, options);
            const SandboxOutcome outcome =
                Exec(*sandbox, "local t = {} while true do t[#t + 1] = string.rep('y', 1024) end");
            EXPECT_FALSE(outcome.ok);
            EXPECT_TRUE(outcome.memoryExhausted);
            CloseChecked(*sandbox);
        }
    }
}

TEST_F(LuaSandboxTest, TheDefaultCapIs64MiBForTheEditorExtensionProfile)
{
    auto sandbox = Make(SandboxProfile::EditorExtension);
    // 100 MiB in one block: past the cap with room to spare, so no allocation of this size is made
    const SandboxOutcome outcome = Exec(*sandbox, "return string.rep('x', 100 * 1024 * 1024)");
    EXPECT_FALSE(outcome.ok);
    EXPECT_TRUE(outcome.memoryExhausted);
    EXPECT_LE(sandbox->Counters().peakBytes, SandboxDefaults::MaxBytes);
    // 8 MiB is well within it
    EXPECT_EQ(Value(*sandbox, "#string.rep('x', 8 * 1024 * 1024)"), "8388608");
    CloseChecked(*sandbox);
}

// ==================== independence ====================

TEST_F(LuaSandboxTest, TwoStatesAliveAtOnceAreIndependent)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto first = Make(profile);
        auto second = Make(profile);
        EXPECT_EQ(LuaSandbox::LiveStates(), _liveBefore + 2);
        EXPECT_NE(first->State(), second->State());
        void *firstAllocator = nullptr;
        void *secondAllocator = nullptr;
        lua_getallocf(first->State(), &firstAllocator);
        lua_getallocf(second->State(), &secondAllocator);
        EXPECT_NE(firstAllocator, secondAllocator) << "each state has its own allocator";

        EXPECT_TRUE(Exec(*first, "Shared = 1; string.rep = nil; math.floor = nil").ok);
        EXPECT_EQ(Value(*second, "rawget(_G, 'Shared')"), "nil");
        EXPECT_EQ(Value(*second, "type(string.rep)"), "function");
        EXPECT_EQ(Value(*second, "math.floor(2.5)"), "2");
        EXPECT_EQ(Value(*first, "Shared"), "1");

        // One tripping does not touch the other, and neither does closing it
        const SandboxOutcome tripped = Exec(*first, "while true do end", Instructions);
        EXPECT_TRUE(tripped.budgetExceeded);
        EXPECT_FALSE(second->Counters().budgetExhausted);
        EXPECT_TRUE(Exec(*second, "local n = 0 for i = 1, 1000 do n = n + 1 end").ok);
        CloseChecked(*first);
        EXPECT_EQ(LuaSandbox::LiveStates(), _liveBefore + 1);
        EXPECT_EQ(Value(*second, "1 + 1"), "2");
        CloseChecked(*second);
    }
}

TEST_F(LuaSandboxTest, TheGameStateAndItsLimitsAreNotTouched)
{
    auto &runtime = LuaRuntime::Instance();
    sol::state &runtimeState = runtime.GetState();
    const std::size_t memoryBefore = runtime.GetMemoryUsed();
    const std::uint64_t panicsBefore = runtime.PanicCount();
    const LuaLimits saved = runtime.GetLimits();

    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        SandboxOptions options;
        options.maxBytes = SmallCap;
        auto sandbox = Make(profile, options);
        EXPECT_NE(sandbox->State(), runtimeState.lua_state());
        void *sandboxAllocator = nullptr;
        void *runtimeAllocator = nullptr;
        lua_getallocf(sandbox->State(), &sandboxAllocator);
        lua_getallocf(runtimeState.lua_state(), &runtimeAllocator);
        EXPECT_NE(sandboxAllocator, runtimeAllocator);

        // The runtime's own budget is tiny: it makes no difference to a sandbox
        runtime.SetLimits({1000, saved.maxBytes});
        EXPECT_TRUE(Exec(*sandbox, "local n = 0 for i = 1, 100000 do n = n + 1 end").ok);
        runtime.SetLimits(saved);

        // And what trips here trips only here
        EXPECT_TRUE(Exec(*sandbox, "while true do end", Instructions).budgetExceeded);
        EXPECT_TRUE(Exec(*sandbox, "local s = 'x' while true do s = s .. s end").memoryExhausted);
        EXPECT_TRUE(Exec(*sandbox, "Leak = 'set by the script'").ok);
        CloseChecked(*sandbox);
    }

    EXPECT_EQ(runtime.GetMemoryUsed(), memoryBefore) << "the runtime's state was used";
    EXPECT_EQ(runtime.PanicCount(), panicsBefore);
    EXPECT_EQ(runtime.GetLimits().instructionsPerEntry, saved.instructionsPerEntry);
    EXPECT_EQ(runtime.GetLimits().maxBytes, saved.maxBytes);
    lua_getglobal(runtimeState.lua_state(), "Leak");
    EXPECT_TRUE(lua_isnil(runtimeState.lua_state(), -1)) << "a global the script set reached the runtime's state";
    lua_pop(runtimeState.lua_state(), 1);
}

// ==================== the prelude hook ====================

TEST_F(LuaSandboxTest, ThePreludeIsEmptyAndRunsNothingByDefault)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        auto sandbox = Make(profile);
        EXPECT_TRUE(sandbox->PreludeOutcome().ok);
        EXPECT_TRUE(sandbox->PreludeOutcome().error.empty());
        EXPECT_EQ(lua_gettop(sandbox->State()), 0);
        CloseChecked(*sandbox);
    }
}

TEST_F(LuaSandboxTest, AGivenPreludeRunsInTheFreshStateBeforeCreateReturns)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        SandboxOptions options;
        options.prelude = std::string("PreludeValue = 41 + 1\nfunction PreludeAdd(a, b) return a + b end");
        auto sandbox = Make(profile, options);
        EXPECT_TRUE(sandbox->PreludeOutcome().ok) << sandbox->PreludeOutcome().error;
        EXPECT_EQ(Value(*sandbox, "PreludeValue"), "42");
        EXPECT_EQ(Value(*sandbox, "PreludeAdd(2, 3)"), "5");
        EXPECT_EQ(lua_gettop(sandbox->State()), 0);
        // The only change is what the prelude defined
        std::vector<std::string> expected = profile == SandboxProfile::Declarations ? DeclarationsGlobals : EditorGlobals;
        expected.push_back("PreludeAdd");
        expected.push_back("PreludeValue");
        std::ranges::sort(expected);
        EXPECT_EQ(KeysOf(sandbox->State()), expected);
        CloseChecked(*sandbox);
    }
}

TEST_F(LuaSandboxTest, AnErroringPreludeIsReportedAsData)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        for (const char *prelude : {"error('prelude failed')", "x = = 1", "local t = nil; t.x = 1", "error({})"})
        {
            SandboxOptions options;
            options.prelude = std::string(prelude);
            auto sandbox = Make(profile, options);
            const SandboxOutcome &outcome = sandbox->PreludeOutcome();
            EXPECT_FALSE(outcome.ok) << prelude;
            EXPECT_FALSE(outcome.budgetExceeded) << prelude;
            EXPECT_FALSE(outcome.memoryExhausted) << prelude;
            EXPECT_FALSE(outcome.error.empty()) << prelude;
            CloseChecked(*sandbox);
        }
    }
}

TEST_F(LuaSandboxTest, ALoopingPreludeIsReportedAsABudgetTripUnderTheLoadBudget)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        {
            // The default load budget (10 million instructions, one second): whichever ends it first
            SandboxOptions options;
            options.prelude = std::string("while true do end");
            const auto started = std::chrono::steady_clock::now();
            auto sandbox = Make(profile, options);
            EXPECT_LT(ElapsedMs(started), 30000.0);
            EXPECT_FALSE(sandbox->PreludeOutcome().ok);
            EXPECT_TRUE(sandbox->PreludeOutcome().budgetExceeded);
            CloseChecked(*sandbox);
        }
        {
            SandboxOptions options;
            options.prelude = std::string("pcall(function() while true do end end)");
            options.loadBudget = Instructions;
            auto sandbox = Make(profile, options);
            EXPECT_TRUE(sandbox->PreludeOutcome().budgetExceeded);
            EXPECT_FALSE(sandbox->PreludeOutcome().timedOut);
            EXPECT_EQ(sandbox->Counters().instructionLimit, Instructions.instructions);
            CloseChecked(*sandbox);
        }
        {
            SandboxOptions options;
            options.prelude = std::string("while true do end");
            options.loadBudget = Clock;
            auto sandbox = Make(profile, options);
            EXPECT_TRUE(sandbox->PreludeOutcome().budgetExceeded);
            EXPECT_TRUE(sandbox->PreludeOutcome().timedOut);
            CloseChecked(*sandbox);
        }
    }
}

TEST_F(LuaSandboxTest, APreludeThatExhaustsMemoryIsReportedAsData)
{
    for (const SandboxProfile profile : Profiles)
    {
        SCOPED_TRACE(ProfileName(profile));
        SandboxOptions options;
        options.maxBytes = SmallCap;
        options.prelude = std::string("local s = string.rep('x', 1e9)");
        auto sandbox = Make(profile, options);
        EXPECT_FALSE(sandbox->PreludeOutcome().ok);
        EXPECT_TRUE(sandbox->PreludeOutcome().memoryExhausted);
        EXPECT_FALSE(sandbox->PreludeOutcome().budgetExceeded);
        CloseChecked(*sandbox);
    }
}

// ==================== the evaluator through the builder ====================

TEST_F(LuaSandboxTest, TheEvaluatorClosesItsStateThroughTheBuilder)
{
    const DeclarationResult result = DeclarationEvaluator::Evaluate(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = { speed = 5, label = { type = "string", default = "hi" } }
        return C
    )LUA", "builder.lua");
    EXPECT_TRUE(result.Complete());
    EXPECT_EQ(result.fields.size(), 2u);
    EXPECT_EQ(result.leakedBytes, 0u);
    EXPECT_GT(result.peakBytes, 0u);
    EXPECT_EQ(LuaSandbox::LiveStates(), _liveBefore);
    EXPECT_EQ(DeclarationEvaluator::LiveStates(), 0);

    const DeclarationResult looping = DeclarationEvaluator::Evaluate("while true do end", "loop.lua");
    EXPECT_TRUE(looping.budgetExceeded);
    EXPECT_EQ(looping.leakedBytes, 0u);
    EXPECT_EQ(LuaSandbox::LiveStates(), _liveBefore);
}
