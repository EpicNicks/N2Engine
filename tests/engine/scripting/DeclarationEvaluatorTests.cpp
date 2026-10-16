#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/scripting/DeclarationEvaluator.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/serialization/FieldInfo.hpp"

#include "HangWatchdog.hpp"

// The declarations-only evaluator (Lua edit-host option B, PR L1): a script's field declarations read in a throwaway,
// budgeted state with no engine bindings. Not wired into anything yet; these tests are its whole use.

using namespace N2Engine;
using namespace N2Engine::Scripting;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    const DeclaredField *Find(const DeclarationResult &result, const std::string &name)
    {
        const auto found = std::ranges::find(result.fields, name, &DeclaredField::name);
        return found == result.fields.end() ? nullptr : &*found;
    }

    std::vector<std::string> Names(const DeclarationResult &result)
    {
        std::vector<std::string> names;
        for (const DeclaredField &field : result.fields)
        {
            names.push_back(field.name);
        }
        return names;
    }

    // Every evaluation closes its state: nothing the allocator counted is left, and none ran past its cap
    DeclarationResult Eval(const std::string &source, const DeclarationLimits &limits = {})
    {
        const DeclarationResult result = DeclarationEvaluator::Evaluate(source, "test.lua", limits);
        EXPECT_EQ(result.leakedBytes, 0u) << source;
        EXPECT_LE(result.peakBytes, limits.maxBytes) << source;
        EXPECT_EQ(DeclarationEvaluator::LiveStates(), 0) << source;
        return result;
    }
}

class DeclarationEvaluatorTest : public ::testing::Test
{
protected:
    std::unique_ptr<Tests::HangWatchdog> _watchdog;

    void SetUp() override
    {
        // Every test here must end by itself: a loop only the budget stops would otherwise run until the job's limit
        _watchdog = std::make_unique<Tests::HangWatchdog>(std::chrono::seconds(60));
    }

    void TearDown() override
    {
        EXPECT_EQ(DeclarationEvaluator::LiveStates(), 0);
        _watchdog.reset();
    }
};

TEST_F(DeclarationEvaluatorTest, TheDefaultsAreTheDesignsBudgets)
{
    const DeclarationLimits limits;
    EXPECT_EQ(limits.instructions, 1'000'000u);
    EXPECT_EQ(limits.maxBytes, std::size_t{16} * 1024 * 1024);
    EXPECT_EQ(limits.maxSourceBytes, std::size_t{4} * 1024 * 1024);
    EXPECT_EQ(limits.maxFields, 256u);
    EXPECT_EQ(limits.maxStringBytes, 4096u);
}

TEST_F(DeclarationEvaluatorTest, AWellFormedScriptIsRead)
{
    const DeclarationResult result = Eval(R"LUA(
        local Fields = {}
        Fields.__index = Fields
        Fields.SerializableFields = {
            count = 3,
            ratio = 0.5,
            label = { type = "string", default = "hi" },
            enabled = true,
            offset = { default = Vector3(0, 1, 0) },
            target = { type = "GameObject" },
            body = { type = "Rigidbody" },
            steps = { type = "int", default = 2 },
        }
        return Fields
    )LUA");
    EXPECT_TRUE(result.Complete()) << (result.errors.empty() ? "" : result.errors[0]);
    EXPECT_TRUE(result.declared);
    EXPECT_EQ(result.stubUses, 0u);
    EXPECT_EQ(Names(result), (std::vector<std::string>{"body", "count", "enabled", "label", "offset", "ratio", "steps", "target"}));

    EXPECT_EQ(Find(result, "count")->defaultKind, LuaDefaultKind::Integer);
    EXPECT_EQ(Find(result, "count")->defaultValue, 3);
    EXPECT_EQ(Find(result, "ratio")->defaultKind, LuaDefaultKind::Float);
    EXPECT_EQ(Find(result, "ratio")->defaultValue, 0.5);
    EXPECT_EQ(Find(result, "enabled")->defaultValue, true);
    EXPECT_EQ(Find(result, "label")->declaredType, "string");
    EXPECT_EQ(Find(result, "label")->defaultValue, "hi");
    EXPECT_TRUE(Find(result, "label")->isDefinition);
    EXPECT_FALSE(Find(result, "count")->isDefinition);
    EXPECT_EQ(Find(result, "offset")->defaultKind, LuaDefaultKind::Vector3);
    EXPECT_EQ(Find(result, "offset")->defaultValue, (json{{"x", 0.0f}, {"y", 1.0f}, {"z", 0.0f}}));
    EXPECT_TRUE(Find(result, "target")->defaultValue.is_null());
    EXPECT_EQ(Find(result, "body")->declaredType, "Rigidbody");

    const std::vector<FieldInfo> infos = result.ToFieldInfos();
    ASSERT_EQ(infos.size(), 8u);
    EXPECT_EQ(infos[0].kind, FieldKind::ComponentRef); // body
    EXPECT_EQ(infos[1].kind, FieldKind::Int);          // count
    EXPECT_EQ(infos[4].kind, FieldKind::Vector3);      // offset
    EXPECT_EQ(infos[7].kind, FieldKind::GameObjectRef); // target
    for (const FieldInfo &info : infos)
    {
        EXPECT_EQ(info.container, "scriptData");
    }
}

TEST_F(DeclarationEvaluatorTest, ComputedDefaultsAreEvaluated)
{
    const DeclarationResult result = Eval(R"LUA(
        local BASE = 2
        local C = {}
        C.__index = C
        local function twice(n) return n * 2 end
        C.SerializableFields = {
            speed = BASE * 4,
            half = twice(0.25),
            name = "a" .. "b",
            flag = not false,
            up = { default = Vector3.new(0, 1, 0) },
            big = 2 ^ 10,
        }
        return C
    )LUA");
    EXPECT_TRUE(result.Complete());
    EXPECT_EQ(Find(result, "speed")->defaultValue, 8);
    EXPECT_EQ(Find(result, "half")->defaultValue, 0.5);
    EXPECT_EQ(Find(result, "name")->defaultValue, "ab");
    EXPECT_EQ(Find(result, "flag")->defaultValue, true);
    EXPECT_EQ(Find(result, "up")->defaultKind, LuaDefaultKind::Vector3);
    EXPECT_EQ(Find(result, "big")->defaultKind, LuaDefaultKind::Float); // ^ is a float in Lua
}

TEST_F(DeclarationEvaluatorTest, ATableKeyThatIsNotANameIsSkipped)
{
    const DeclarationResult result = Eval(R"LUA(
        local Odd = {}
        Odd.__index = Odd
        Odd.SerializableFields = { count = 3, [1] = "positional", [2.5] = true }
        return Odd
    )LUA");
    EXPECT_TRUE(result.Complete());
    EXPECT_EQ(Names(result), (std::vector<std::string>{"count"}));
}

TEST_F(DeclarationEvaluatorTest, SerializableFieldsIsFoundThroughTheClassesIndexLikeAnInstanceFindsIt)
{
    // The runtime reads it off an instance (a table whose metatable is the class): no __index, nothing is found; an
    // inherited one is found through the chain
    const DeclarationResult without = Eval(R"LUA(
        return { SerializableFields = { count = 3 } }
    )LUA");
    EXPECT_TRUE(without.Complete());
    EXPECT_FALSE(without.declared);
    EXPECT_TRUE(without.fields.empty());

    const DeclarationResult inherited = Eval(R"LUA(
        local Base = {}
        Base.__index = Base
        Base.SerializableFields = { speed = 4 }
        local Child = setmetatable({}, Base)
        Child.__index = Child
        return Child
    )LUA");
    EXPECT_TRUE(inherited.Complete());
    EXPECT_EQ(Names(inherited), (std::vector<std::string>{"speed"}));
}

TEST_F(DeclarationEvaluatorTest, AScriptWithoutDeclarationsHasNoFieldsAndIsNotUnknown)
{
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        function C:OnUpdate() end
        return C
    )LUA");
    EXPECT_TRUE(result.Complete());
    EXPECT_FALSE(result.declared);
    EXPECT_FALSE(result.unevaluable);
    EXPECT_TRUE(result.fields.empty());
    EXPECT_TRUE(result.ToFieldInfos().empty());
    EXPECT_TRUE(result.DefaultValues().empty());
}

// ==================== limits ====================

TEST_F(DeclarationEvaluatorTest, AnInfiniteLoopAtTopLevelEndsInTheBudget)
{
    const auto start = std::chrono::steady_clock::now();
    const DeclarationResult result = Eval("while true do end");
    EXPECT_TRUE(result.budgetExceeded);
    EXPECT_FALSE(result.memoryExhausted);
    EXPECT_FALSE(result.Complete());
    EXPECT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.fields.empty());
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(20));
}

TEST_F(DeclarationEvaluatorTest, ALoopInsidePcallOrXpcallStillEndsInTheBudget)
{
    // A script cannot catch the budget error (the engine sandbox's pcall and xpcall hand it on)
    const DeclarationResult inPcall = Eval(R"LUA(
        pcall(function() while true do end end)
        local C = {}
        C.__index = C
        C.SerializableFields = { survived = true }
        return C
    )LUA");
    EXPECT_TRUE(inPcall.budgetExceeded);
    EXPECT_TRUE(inPcall.fields.empty());

    const DeclarationResult inHandler = Eval(R"LUA(
        xpcall(function() error("first") end, function() while true do end end)
        return {}
    )LUA");
    EXPECT_TRUE(inHandler.budgetExceeded);

    const DeclarationResult nested = Eval(R"LUA(
        while true do
            pcall(function() while true do end end)
        end
    )LUA");
    EXPECT_TRUE(nested.budgetExceeded);
}

TEST_F(DeclarationEvaluatorTest, TheBudgetIsTheEvaluatorsOwnAndItsLimitsAreTheCallers)
{
    const std::string source = R"LUA(
        local n = 0
        for i = 1, 100000 do n = n + 1 end
        local C = {}
        C.__index = C
        C.SerializableFields = { n = n }
        return C
    )LUA";

    // The runtime's own budget (a different state) is tiny: it makes no difference to the evaluator
    auto &runtime = LuaRuntime::Instance();
    const LuaLimits saved = runtime.GetLimits();
    runtime.SetLimits({1000, saved.maxBytes});
    const DeclarationResult generous = Eval(source);
    EXPECT_EQ(runtime.GetLimits().instructionsPerEntry, 1000u) << "the evaluator changed the runtime's limits";
    runtime.SetLimits(saved);
    EXPECT_TRUE(generous.Complete());
    EXPECT_EQ(Find(generous, "n")->defaultValue, 100000);

    // And the evaluator's own is what the caller gives it
    DeclarationLimits small;
    small.instructions = 5000;
    const DeclarationResult starved = Eval(source, small);
    EXPECT_TRUE(starved.budgetExceeded);
    EXPECT_EQ(runtime.GetLimits().instructionsPerEntry, saved.instructionsPerEntry);
}

TEST_F(DeclarationEvaluatorTest, AMemoryBombIsRefusedAtTheCapNotFollowed)
{
    const DeclarationResult oneBlock = Eval(R"LUA(
        local s = string.rep("x", 1024 * 1024 * 1024)
        return {}
    )LUA");
    EXPECT_TRUE(oneBlock.memoryExhausted);
    EXPECT_FALSE(oneBlock.budgetExceeded);
    EXPECT_FALSE(oneBlock.errors.empty());

    const DeclarationResult growing = Eval(R"LUA(
        local t = {}
        while true do t[#t + 1] = string.rep("x", 1024) end
    )LUA");
    EXPECT_TRUE(growing.memoryExhausted);
    EXPECT_GT(growing.peakBytes, std::size_t{8} * 1024 * 1024);
    EXPECT_LE(growing.peakBytes, std::size_t{16} * 1024 * 1024);

    DeclarationLimits small;
    small.maxBytes = std::size_t{2} * 1024 * 1024;
    const DeclarationResult capped = Eval("local s = string.rep('x', 4 * 1024 * 1024) return {}", small);
    EXPECT_TRUE(capped.memoryExhausted);

    // pcall does not hide it from the evaluation: a script that catches the error and goes on gets what it declared
    const DeclarationResult caught = Eval(R"LUA(
        local ok = pcall(string.rep, "x", 1024 * 1024 * 1024)
        local C = {}
        C.__index = C
        C.SerializableFields = { caught = not ok }
        return C
    )LUA");
    EXPECT_FALSE(caught.memoryExhausted);
    EXPECT_EQ(Find(caught, "caught")->defaultValue, true);
}

TEST_F(DeclarationEvaluatorTest, DeepRecursionIsAnErrorNotACrash)
{
    const DeclarationResult result = Eval(R"LUA(
        local function f(n) return 1 + f(n + 1) end
        f(1)
    )LUA");
    EXPECT_FALSE(result.Complete());
    EXPECT_FALSE(result.errors.empty());
}

TEST_F(DeclarationEvaluatorTest, ATooLargeSourceIsRefusedBeforeAnyStateIsMade)
{
    DeclarationLimits limits;
    limits.maxSourceBytes = 16;
    const DeclarationResult result = Eval("local C = {} return C -- longer than sixteen bytes", limits);
    EXPECT_FALSE(result.errors.empty());
    EXPECT_EQ(result.peakBytes, 0u);
    EXPECT_FALSE(result.Complete());
}

TEST_F(DeclarationEvaluatorTest, MoreFieldsThanTheLimitAreIgnoredAndSaid)
{
    DeclarationLimits limits;
    limits.maxFields = 3;
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = { a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7 }
        return C
    )LUA", limits);
    EXPECT_EQ(result.fields.size(), 3u);
    EXPECT_FALSE(result.errors.empty());
    EXPECT_FALSE(result.Complete());
}

TEST_F(DeclarationEvaluatorTest, ALongStringDefaultIsUnknownNotCopied)
{
    DeclarationLimits limits;
    limits.maxStringBytes = 8;
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = { short = "abc", long = string.rep("x", 100) }
        return C
    )LUA", limits);
    EXPECT_EQ(Find(result, "short")->defaultValue, "abc");
    EXPECT_TRUE(Find(result, "long")->unevaluable);
    EXPECT_TRUE(Find(result, "long")->defaultValue.is_null());
}

// ==================== errors as data ====================

TEST_F(DeclarationEvaluatorTest, ErrorsAreReportedAsData)
{
    for (const char *source : {
             "error('boom')",
             "error({ code = 1 })",
             "error(404)",
             "local = 1",                                // a syntax error
             "return 1",                                 // not a table
             "return nil",
             "return 'text'",
             "local t = nil; t.x = 1",                   // indexing nil
             "setmetatable({}, { __gc = function() end })", // the sandbox refuses finalizers
             "\x1b" "Lua binary chunk bytes",            // a precompiled chunk is refused
             "",                                         // empty: returns nothing
         })
    {
        const DeclarationResult result = Eval(source);
        EXPECT_FALSE(result.Complete()) << source;
        EXPECT_FALSE(result.errors.empty()) << source;
        EXPECT_FALSE(result.budgetExceeded) << source;
        EXPECT_FALSE(result.memoryExhausted) << source;
        EXPECT_TRUE(result.fields.empty()) << source;
        EXPECT_FALSE(result.declared) << source;
    }
}

TEST_F(DeclarationEvaluatorTest, AFailingMetamethodLeavesThatFieldToItsDefault)
{
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        local broken = setmetatable({}, { __index = function() error("no") end })
        C.SerializableFields = { fine = 1, broken = broken, other = { default = 2 } }
        return C
    )LUA");
    EXPECT_TRUE(result.Complete());
    EXPECT_EQ(Names(result), (std::vector<std::string>{"broken", "fine", "other"}));
    EXPECT_TRUE(Find(result, "broken")->isDefinition);
    EXPECT_TRUE(Find(result, "broken")->defaultValue.is_null());
    EXPECT_EQ(Find(result, "fine")->defaultValue, 1);
    EXPECT_EQ(Find(result, "other")->defaultValue, 2);

    const DeclarationResult unreadable = Eval(R"LUA(
        local C = {}
        C.__index = function(_, key) if key == "SerializableFields" then error("no") end end
        return C
    )LUA");
    EXPECT_FALSE(unreadable.Complete());
    EXPECT_FALSE(unreadable.errors.empty());
    EXPECT_TRUE(unreadable.fields.empty());
}

TEST_F(DeclarationEvaluatorTest, AMetamethodLoopWhileReadingIsBudgetedToo)
{
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = function(_, key) while true do end end
        return C
    )LUA");
    EXPECT_TRUE(result.budgetExceeded);
}

TEST_F(DeclarationEvaluatorTest, AClassWithAFinalizerIsRefused)
{
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.__gc = function() while true do end end
        C.SerializableFields = { a = 1 }
        return C
    )LUA");
    EXPECT_FALSE(result.Complete());
    EXPECT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.fields.empty());
}

// ==================== the sandbox ====================

TEST_F(DeclarationEvaluatorTest, NoEngineBindingAndNoUnsafeLibraryIsReachable)
{
    // Each name is read from the globals table itself (rawget: the placeholder is not consulted)
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = {}
        local names = {
            "Application", "GameObject", "Input", "SceneManager", "Debug", "Time", "Math", "Vector2", "Quaternion",
            "Color", "Physics", "Window", "Camera", "Event", "Scene", "Logger",
            "io", "os", "debug", "package", "coroutine", "utf8", "require", "dofile", "loadfile", "load", "print",
            "warn", "collectgarbage", "_loaded_modules",
        }
        for _, name in ipairs(names) do
            C.SerializableFields[name] = rawget(_G, name) == nil and "absent" or "present"
        end
        return C
    )LUA");
    EXPECT_TRUE(result.Complete());
    ASSERT_EQ(result.fields.size(), 30u);
    for (const DeclaredField &field : result.fields)
    {
        EXPECT_EQ(field.defaultValue, "absent") << field.name;
    }
}

TEST_F(DeclarationEvaluatorTest, WhatIsLeftOfTheLibrariesIsTheSafeCore)
{
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = {}
        local function has(t, name) return rawget(t, name) ~= nil and "yes" or "no" end
        local F = C.SerializableFields
        F.mathFloor = has(math, "floor")
        F.stringRep = has(string, "rep")
        F.stringFormat = has(string, "format")
        F.tableConcat = has(table, "concat")
        F.pcall = has(_G, "pcall")
        F.setmetatable = has(_G, "setmetatable")
        -- the pattern functions and math.random are placeholders, so their results are unknown, not wrong
        F.findIsStub = tostring(string.find)
        F.randomIsStub = tostring(math.random)
        return C
    )LUA");
    EXPECT_TRUE(result.Complete());
    EXPECT_EQ(Find(result, "mathFloor")->defaultValue, "yes");
    EXPECT_EQ(Find(result, "stringRep")->defaultValue, "yes");
    EXPECT_EQ(Find(result, "stringFormat")->defaultValue, "yes");
    EXPECT_EQ(Find(result, "tableConcat")->defaultValue, "yes");
    EXPECT_EQ(Find(result, "pcall")->defaultValue, "yes");
    EXPECT_EQ(Find(result, "setmetatable")->defaultValue, "yes");
    EXPECT_EQ(Find(result, "findIsStub")->defaultValue, "stub");
    EXPECT_EQ(Find(result, "randomIsStub")->defaultValue, "stub");
}

TEST_F(DeclarationEvaluatorTest, PatternFunctionsAndRandomAreUnknownNotRun)
{
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = {
            found = ("abc"):find("b"),
            replaced = ("abc"):gsub("b", "x"),
            roll = math.random(1, 6),
            fine = ("abc"):upper(),
        }
        return C
    )LUA");
    EXPECT_FALSE(result.Complete());
    EXPECT_TRUE(Find(result, "found")->unevaluable);
    EXPECT_TRUE(Find(result, "replaced")->unevaluable);
    EXPECT_TRUE(Find(result, "roll")->unevaluable);
    EXPECT_FALSE(Find(result, "fine")->unevaluable);
    EXPECT_EQ(Find(result, "fine")->defaultValue, "ABC");
}

TEST_F(DeclarationEvaluatorTest, ThePlaceholdersMetatableCannotBeReachedOrReplaced)
{
    const DeclarationResult changed = Eval("setmetatable(Anything, { __index = function() return 1 end })");
    EXPECT_FALSE(changed.errors.empty());

    const DeclarationResult globals = Eval("setmetatable(_G, nil)");
    EXPECT_FALSE(globals.errors.empty());

    const DeclarationResult read = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = { kind = type(getmetatable(Anything)), same = rawequal(Anything, Other.Thing) }
        return C
    )LUA");
    EXPECT_EQ(Find(read, "kind")->defaultValue, "string");
    EXPECT_EQ(Find(read, "same")->defaultValue, true);
}

// ==================== inert placeholders ====================

TEST_F(DeclarationEvaluatorTest, TopLevelEngineCallsAreTolerated)
{
    const DeclarationResult result = Eval(R"LUA(
        Debug.Log("loading")
        local go = GameObject.Find("Player")
        local pos = go:GetComponent("Rigidbody").Position + Vector3(1, 2, 3)
        Application.Quit()
        SceneManager.LoadScene("other")
        local Stations = require("scripts.SmokeStations")
        local limit = 89.0 * Math.DEG_TO_RAD
        local q = Quaternion.FromEulerAngles(0.3, 0.0, 0.1)
        Input.Subscribe(function() error("never called") end)

        local C = {}
        C.__index = C
        C.SerializableFields = { speed = 5, label = { type = "string", default = "x" } }
        function C:OnAttach() self.x = Stations[1].camera end
        return C
    )LUA");
    EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? "" : result.errors[0]);
    EXPECT_TRUE(result.Complete());
    EXPECT_GT(result.stubUses, 0u) << "the script used engine names, which were placeholders";
    EXPECT_EQ(Names(result), (std::vector<std::string>{"label", "speed"}));
    EXPECT_EQ(Find(result, "speed")->defaultValue, 5);
}

TEST_F(DeclarationEvaluatorTest, ADeclarationThatComesFromTheEngineIsUnknownNotEmptyOrWrong)
{
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = {
            fromEngine = Config.Speed,
            computed = 2 * Math.PI,
            typed = { type = "number", default = Config.Speed },
            typedFromEngine = { type = Types.Number, default = 3 },
            plain = 7,
            vec = Vector3(Config.X, 0, 0),
            nothing = { default = nil },
        }
        return C
    )LUA");
    EXPECT_FALSE(result.Complete());
    EXPECT_TRUE(result.AnyFieldUnevaluable());
    EXPECT_FALSE(result.unevaluable) << "the list itself is known";
    EXPECT_TRUE(result.errors.empty());
    ASSERT_EQ(result.fields.size(), 7u);

    // listed, with the kind the rest of the declaration gives, and a reason
    EXPECT_TRUE(Find(result, "fromEngine")->unevaluable);
    EXPECT_FALSE(Find(result, "fromEngine")->unevaluableReason.empty());
    EXPECT_EQ(Find(result, "fromEngine")->defaultKind, LuaDefaultKind::Other);
    EXPECT_TRUE(Find(result, "computed")->unevaluable);
    EXPECT_TRUE(Find(result, "typed")->unevaluable);
    EXPECT_EQ(Find(result, "typed")->declaredType, "number");
    EXPECT_EQ(result.ToFieldInfos()[Find(result, "typed") - &result.fields[0]].kind, FieldKind::Float);
    EXPECT_TRUE(Find(result, "typedFromEngine")->unevaluable);
    EXPECT_EQ(Find(result, "typedFromEngine")->defaultValue, 3);
    EXPECT_TRUE(Find(result, "vec")->unevaluable);
    EXPECT_FALSE(Find(result, "plain")->unevaluable);
    EXPECT_FALSE(Find(result, "nothing")->unevaluable);
}

TEST_F(DeclarationEvaluatorTest, AListThatComesFromTheEngineIsUnevaluableWithItsReason)
{
    const DeclarationResult table = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = Config.Fields
        return C
    )LUA");
    EXPECT_TRUE(table.unevaluable);
    EXPECT_FALSE(table.unevaluableReason.empty());
    EXPECT_FALSE(table.Complete());
    EXPECT_TRUE(table.fields.empty());

    const DeclarationResult cls = Eval("return Framework.Class('Spin')");
    EXPECT_TRUE(cls.unevaluable);
    EXPECT_FALSE(cls.unevaluableReason.empty());
    EXPECT_FALSE(cls.Complete());
    EXPECT_TRUE(cls.errors.empty()) << "not a script error: the evaluation cannot know";
}

TEST_F(DeclarationEvaluatorTest, APlaceholderCannotMakeAnEvaluationHang)
{
    // It never ends the loop on its own (it is always truthy), so the budget does
    const DeclarationResult polling = Eval("while Input.GetKey('w') do end");
    EXPECT_TRUE(polling.budgetExceeded);

    // But an integer index answers nil, so the ways to walk it end
    const DeclarationResult walking = Eval(R"LUA(
        local n = 0
        for _ in ipairs(Scene.Objects) do n = n + 1 end
        for _ in pairs(Scene.Objects) do n = n + 1 end
        for i = 1, #Scene.Objects do n = n + 1 end
        local C = {}
        C.__index = C
        C.SerializableFields = { walked = n }
        return C
    )LUA");
    EXPECT_TRUE(walking.Complete());
    EXPECT_EQ(Find(walking, "walked")->defaultValue, 0);

    // A loop through calls of it is counted like any other
    const DeclarationResult calls = Eval("local x = Engine while true do x = x.Next() end");
    EXPECT_TRUE(calls.budgetExceeded);
}

TEST_F(DeclarationEvaluatorTest, APlaceholderHoldsNothingAndStaysThePlaceholder)
{
    const DeclarationResult result = Eval(R"LUA(
        Config.speed = 99
        local C = {}
        C.__index = C
        C.SerializableFields = { a = Config.speed }
        return C
    )LUA");
    // A write to it goes nowhere (its __newindex does nothing): what is read back is unknown, never the 99
    EXPECT_TRUE(Find(result, "a")->unevaluable);
}

// ==================== the runtime is not touched ====================

TEST_F(DeclarationEvaluatorTest, NoEngineSingletonOrLuaStateOfTheRuntimeIsUsed)
{
    auto &runtime = LuaRuntime::Instance();
    sol::state &runtimeState = runtime.GetState();
    runtimeState["EvaluatorLeak"] = sol::lua_nil;
    const std::size_t memoryBefore = runtime.GetMemoryUsed();
    const std::uint64_t panicsBefore = runtime.PanicCount();
    const LuaLimits limitsBefore = runtime.GetLimits();

    const DeclarationResult result = Eval(R"LUA(
        EvaluatorLeak = "set by the script"
        Application = { Quit = function() error("not the real one") end }
        local C = {}
        C.__index = C
        C.SerializableFields = { a = 1 }
        return C
    )LUA");
    EXPECT_TRUE(result.Complete());

    // Read the counters first: looking at the global makes a reference in the runtime's state
    EXPECT_EQ(runtime.GetMemoryUsed(), memoryBefore) << "the runtime's state was used";
    EXPECT_EQ(runtime.PanicCount(), panicsBefore);
    const sol::object leaked = runtimeState["EvaluatorLeak"];
    EXPECT_EQ(leaked.get_type(), sol::type::lua_nil) << "a global the script set reached the runtime's state";
    EXPECT_EQ(runtime.GetLimits().instructionsPerEntry, limitsBefore.instructionsPerEntry);
    EXPECT_EQ(runtime.GetLimits().maxBytes, limitsBefore.maxBytes);
}

TEST_F(DeclarationEvaluatorTest, ASecondEvaluationDoesNotSeeTheFirstOnesGlobals)
{
    (void)Eval("Shared = 1 return nil");
    const DeclarationResult second = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = { seen = rawget(_G, "Shared") == nil and "no" or "yes" }
        return C
    )LUA");
    EXPECT_EQ(Find(second, "seen")->defaultValue, "no");
}

TEST_F(DeclarationEvaluatorTest, SeveralThreadsEvaluateIndependently)
{
#if defined(N2ENGINE_PLATFORM_WEB) && !defined(__EMSCRIPTEN_PTHREADS__)
    GTEST_SKIP() << "The single-threaded web build cannot start threads";
#endif
    constexpr int threadCount = 8;
    constexpr int rounds = 12;
    std::atomic<int> failures{0};

    const auto work = [&failures](const int index)
    {
        for (int round = 0; round < rounds; ++round)
        {
            DeclarationLimits limits;
            limits.instructions = 200'000; // the loops below end sooner in a test
            const std::string own = "t" + std::to_string(index);
            const DeclarationResult fine = DeclarationEvaluator::Evaluate(
                "local C = {} C.__index = C C.SerializableFields = { " + own + " = " + std::to_string(index) +
                    ", shared = 'x' } Debug.Log('hello') return C",
                "thread.lua", limits);
            if (!fine.Complete() || fine.fields.size() != 2 || fine.fields[1].name != own ||
                fine.fields[1].defaultValue != index || fine.leakedBytes != 0)
            {
                ++failures;
            }
            const DeclarationResult loop = DeclarationEvaluator::Evaluate("while true do end", "loop.lua", limits);
            if (!loop.budgetExceeded || loop.leakedBytes != 0)
            {
                ++failures;
            }
            const DeclarationResult bomb =
                DeclarationEvaluator::Evaluate("local s = string.rep('x', 1024 * 1024 * 1024)", "bomb.lua", limits);
            if (!bomb.memoryExhausted || bomb.leakedBytes != 0)
            {
                ++failures;
            }
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < threadCount; ++i)
    {
        threads.emplace_back(work, i);
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(DeclarationEvaluator::LiveStates(), 0);
}

// ==================== the corpus: the evaluator against the running runtime ====================

namespace
{
    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    // The fields and defaults a LuaComponent on the real runtime (every binding, the shared state) gets from the
    // script, and what the evaluator reads from the same source: field by field the same
    void ExpectSameAsTheRuntime(const std::string &resourcePath, const std::string &source)
    {
        const auto go = GameObject::Create("Corpus");
        auto *component = go->AddComponent<LuaComponent>();
        component->SetScript(IO::ResourcePath(resourcePath));
        ASSERT_FALSE(component->HasMissingScript()) << resourcePath;

        std::vector<FieldInfo> real = component->DescribeFields();
        ASSERT_FALSE(real.empty()) << resourcePath;
        EXPECT_EQ(real.front().name, "scriptUUID");
        real.erase(real.begin());

        const DeclarationResult result = Eval(source);
        EXPECT_TRUE(result.Complete()) << resourcePath << ": " << (result.errors.empty() ? "" : result.errors[0]);
        const std::vector<FieldInfo> evaluated = result.ToFieldInfos();
        ASSERT_EQ(evaluated.size(), real.size()) << resourcePath;
        for (std::size_t i = 0; i < real.size(); ++i)
        {
            EXPECT_EQ(evaluated[i].ToJson(), real[i].ToJson()) << resourcePath << " field " << real[i].name;
            EXPECT_EQ(evaluated[i].limits, real[i].limits) << resourcePath << " field " << real[i].name;
            EXPECT_EQ(evaluated[i].container, real[i].container) << resourcePath << " field " << real[i].name;
            EXPECT_EQ(evaluated[i].enumOptions, real[i].enumOptions) << resourcePath << " field " << real[i].name;
        }

        // The defaults the runtime stored in the component's data
        const json &stored = component->GetScriptData();
        const json defaults = result.DefaultValues();
        if (stored.is_object())
        {
            EXPECT_EQ(defaults, stored) << resourcePath;
        }
        else
        {
            EXPECT_TRUE(defaults.empty()) << resourcePath;
        }
    }
}

class DeclarationCorpusTest : public ::testing::Test
{
protected:
    static inline fs::path s_projectRoot;
    static inline std::vector<std::pair<std::string, std::string>> s_scripts; // resource path, source

    static void WriteAsset(const std::string &relativePath, const std::string &source)
    {
        const fs::path path = s_projectRoot / "assets" / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << source;
        s_scripts.emplace_back("res://" + relativePath, source);
    }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());

        s_projectRoot = fs::temp_directory_path() / "n2engine_declaration_corpus_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
        s_scripts.clear();

        // The forms LuaReflectionTests covers (Fields, Other, Odd), and what a real project writes around them
        WriteAsset("corpus/Fields.lua", R"LUA(
            local Fields = {}
            Fields.__index = Fields
            Fields.SerializableFields = {
                count = 3,
                ratio = 0.5,
                label = { type = "string", default = "hi" },
                enabled = true,
                offset = { default = Vector3(0, 1, 0) },
                target = { type = "GameObject" },
                body = { type = "Rigidbody" },
                steps = { type = "int", default = 2 },
            }
            return Fields
        )LUA");
        WriteAsset("corpus/Other.lua", R"LUA(
            local Other = {}
            Other.__index = Other
            Other.SerializableFields = {
                count = 9,
                brand = "new",
            }
            return Other
        )LUA");
        WriteAsset("corpus/Odd.lua", R"LUA(
            local Odd = {}
            Odd.__index = Odd
            Odd.SerializableFields = { count = 3, [1] = "positional" }
            return Odd
        )LUA");
        WriteAsset("corpus/Types.lua", R"LUA(
            local Types = {}
            Types.__index = Types
            Types.SerializableFields = {
                a = { type = "number" },
                b = { type = "float", default = 1.5 },
                c = { type = "integer" },
                d = { type = "boolean", default = false },
                e = { type = "Vector3" },
                f = { type = "SomethingUnknown", default = "kept" },
                g = { type = "SpriteComponent" },
                h = { type = 5, default = 4 },
                i = { default = nil },
                j = -2,
                k = 1e300,
                l = {},
            }
            return Types
        )LUA");
        WriteAsset("corpus/Computed.lua", R"LUA(
            local BASE = 2
            local function scaled(n) return n * BASE end
            local Computed = {}
            Computed.__index = Computed
            Computed.SerializableFields = {
                speed = scaled(4),
                name = "id" .. 7,
                up = { default = Vector3.new(0, 1, 0) },
                lookup = { default = ({ 10, 20 })[2] },
            }
            return Computed
        )LUA");
        WriteAsset("corpus/Inherited.lua", R"LUA(
            local Base = {}
            Base.__index = Base
            Base.SerializableFields = { speed = 4, tag = "base" }
            local Child = setmetatable({}, Base)
            Child.__index = Child
            function Child:OnUpdate() end
            return Child
        )LUA");
        WriteAsset("corpus/NoIndex.lua", R"LUA(
            return { SerializableFields = { hidden = 1 } }
        )LUA");
        WriteAsset("corpus/ShorthandVector.lua", R"LUA(
            local C = {}
            C.__index = C
            C.SerializableFields = { origin = Vector3(1, 2, 3), n = 2 }
            return C
        )LUA");
        WriteAsset("corpus/TopLevelEngine.lua", R"LUA(
            Debug.Warn("declaration corpus: a script that calls the engine at its top level")
            local limit = 89.0 * Math.DEG_TO_RAD
            local C = {}
            C.__index = C
            C.SerializableFields = { speed = 5, label = { type = "string", default = "x" } }
            return C
        )LUA");

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }
};

TEST_F(DeclarationCorpusTest, EveryScriptGivesTheFieldsTheRuntimeGives)
{
    ASSERT_GE(s_scripts.size(), 8u);
    for (const auto &[resourcePath, source] : s_scripts)
    {
        ExpectSameAsTheRuntime(resourcePath, source);
    }
}

TEST_F(DeclarationCorpusTest, AScriptTheRuntimeCannotInstantiateIsNotMistakenForOne)
{
    // A class whose field only an engine value can give: the runtime lists the field with the engine's value, the
    // evaluator lists it as unknown instead of leaving it out
    const DeclarationResult result = Eval(R"LUA(
        local C = {}
        C.__index = C
        C.SerializableFields = { speed = Math.PI }
        return C
    )LUA");
    ASSERT_EQ(result.fields.size(), 1u);
    EXPECT_TRUE(result.fields[0].unevaluable);
}

TEST_F(DeclarationCorpusTest, ANamedScriptAssetIsEvaluatedThroughTheResourceLoader)
{
    const DeclarationResult found = DeclarationEvaluator::EvaluateScript(IO::ResourcePath("res://corpus/Fields.lua"));
    EXPECT_TRUE(found.Complete());
    EXPECT_EQ(found.fields.size(), 8u);
    EXPECT_EQ(found.leakedBytes, 0u);

    const DeclarationResult missing = DeclarationEvaluator::EvaluateScript(IO::ResourcePath("res://corpus/Nope.lua"));
    EXPECT_FALSE(missing.errors.empty());
    EXPECT_FALSE(missing.Complete());
    EXPECT_EQ(DeclarationEvaluator::LiveStates(), 0);
}

// lua_project's own scripts: none declares SerializableFields, but all do top-level work a declaration pass must
// survive (require, Math.DEG_TO_RAD, Quaternion.FromEulerAngles, Vector3 literals)
class DeclarationProjectCorpusTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(N2_LUA_PROJECT_DIR);
    }
};

TEST_F(DeclarationProjectCorpusTest, EveryProjectScriptIsReadLikeTheRuntimeReadsIt)
{
    const fs::path scripts = fs::path(N2_LUA_PROJECT_DIR) / "assets" / "scripts";
    int checked = 0;
    for (const auto &entry : fs::directory_iterator(scripts))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".lua")
        {
            continue;
        }
        ExpectSameAsTheRuntime("res://scripts/" + entry.path().filename().string(), ReadFile(entry.path()));
        ++checked;
    }
    EXPECT_GE(checked, 8) << "the project's scripts were not found";
}
