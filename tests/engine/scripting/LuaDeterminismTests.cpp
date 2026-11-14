#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <math/DeterministicRandom.hpp>
#include <math/Random.hpp>
#include <nlohmann/json.hpp>

#include "engine/ProjectSettings.hpp"
#include "engine/particles/ParticleRandom.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// Determinism S1 (issue #157, blockers B4 and B6): a seedable Random and a seedable Lua (hash seed and math.random).
// Every check compares two runs with each other (or with a value the algorithm fixes); none asserts anything about
// the unseeded behaviour, which is random by design and differs by platform.

using namespace N2Engine;
using namespace N2Engine::Scripting;
using nlohmann::json;

namespace
{
    // Sets the Lua seed for the lifetime of the object and puts "unset" back, so no other test sees it
    class LuaSeedScope
    {
    public:
        explicit LuaSeedScope(const std::uint64_t seed) { LuaRuntime::SetLuaSeed(seed); }
        ~LuaSeedScope() { LuaRuntime::SetLuaSeed(std::nullopt); }
        LuaSeedScope(const LuaSeedScope &) = delete;
        LuaSeedScope &operator=(const LuaSeedScope &) = delete;
    };

    // A fresh Lua state with the libraries the engine opens (the LuaRuntime singleton cannot be made twice, and the
    // seeds are read when a state is created, so two states stand in for two runs)
    std::unique_ptr<sol::state> MakeState()
    {
        auto lua = std::make_unique<sol::state>();
        lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
        return lua;
    }

    std::string Eval(sol::state &lua, const std::string &code)
    {
        const sol::protected_function_result result = lua.safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            ADD_FAILURE() << err.what();
            return {};
        }
        return result.get<std::string>();
    }

    // The order pairs() visits the string keys of one table, and the first values of math.random
    constexpr const char *PairsOrderScript = R"(
        local t = {}
        for i = 1, 80 do
            t["key_" .. ((i * 7919) % 1009)] = i
        end
        local out = {}
        for k, v in pairs(t) do
            out[#out + 1] = k
        end
        return table.concat(out, ",")
    )";

    constexpr const char *MathRandomScript = R"(
        local out = {}
        for i = 1, 20 do
            out[#out + 1] = tostring(math.random(1, 1000))
        end
        out[#out + 1] = string.format("%.17g", math.random())
        return table.concat(out, ",")
    )";
}

TEST(LuaDeterminismSeedTest, TheSeedIsOffUntilSet)
{
    EXPECT_FALSE(LuaRuntime::GetLuaSeed().has_value());
    {
        const LuaSeedScope scope(5);
        ASSERT_TRUE(LuaRuntime::GetLuaSeed().has_value());
        EXPECT_EQ(*LuaRuntime::GetLuaSeed(), 5u);
    }
    EXPECT_FALSE(LuaRuntime::GetLuaSeed().has_value());
}

TEST(LuaDeterminismSeedTest, SeededStatesVisitStringKeysInTheSameOrder)
{
    const LuaSeedScope scope(2024);
    const auto first = MakeState();
    const auto second = MakeState();
    const std::string orderOne = Eval(*first, PairsOrderScript);
    const std::string orderTwo = Eval(*second, PairsOrderScript);
    EXPECT_FALSE(orderOne.empty());
    EXPECT_EQ(orderOne, orderTwo);
}

TEST(LuaDeterminismSeedTest, SeededStatesGiveTheSameMathRandomSequence)
{
    const LuaSeedScope scope(777);
    const auto first = MakeState();
    const auto second = MakeState();
    const std::string sequenceOne = Eval(*first, MathRandomScript);
    const std::string sequenceTwo = Eval(*second, MathRandomScript);
    EXPECT_FALSE(sequenceOne.empty());
    EXPECT_EQ(sequenceOne, sequenceTwo);
}

TEST(LuaDeterminismSeedTest, DifferentSeedsGiveDifferentMathRandomSequences)
{
    std::string sequences[2];
    const std::uint64_t seeds[2] = {1, 2};
    for (int i = 0; i < 2; ++i)
    {
        const LuaSeedScope scope(seeds[i]);
        sequences[i] = Eval(*MakeState(), MathRandomScript);
    }
    EXPECT_NE(sequences[0], sequences[1]);
}

// The documented meaning of the seed: math.random starts as math.randomseed(seed) would start it
TEST(LuaDeterminismSeedTest, MathRandomStartsAsMathRandomseedOfTheSeed)
{
    std::string configured;
    {
        const LuaSeedScope scope(31337);
        configured = Eval(*MakeState(), MathRandomScript);
    }
    // Unset here: this state seeds itself from the clock, then the script reseeds it explicitly, so the result does
    // not depend on the unset behaviour
    const auto reseeded = MakeState();
    const std::string explicitSeed = Eval(*reseeded, std::string("math.randomseed(31337)\n") + MathRandomScript);
    EXPECT_EQ(configured, explicitSeed);
}

// math.randomseed() with no argument asks for a random seed; with a configured seed it is that seed again
TEST(LuaDeterminismSeedTest, RandomseedWithoutArgumentsKeepsTheSequenceReproducible)
{
    const LuaSeedScope scope(4242);
    const std::string script = std::string("math.randomseed()\n") + MathRandomScript;
    const std::string sequenceOne = Eval(*MakeState(), script);
    const std::string sequenceTwo = Eval(*MakeState(), script);
    EXPECT_EQ(sequenceOne, sequenceTwo);
}

// The engine's own runtime reads the setting the same way: a state created while it is set has it recorded
TEST(LuaDeterminismSeedTest, TheEngineRuntimeRecordsTheSeedItWasCreatedWith)
{
    // The singleton may have been created by an earlier test, with nothing set: its state is then unseeded, which is
    // what a project without the setting gets
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    EXPECT_FALSE(LuaRuntime::Instance().StateLuaSeed().has_value());
}

// Random.Seed(n) in Lua: the engine Random draws from the deterministic generator from then on
class LuaRandomSeedTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { ASSERT_TRUE(LuaRuntime::Instance().Initialize()); }
    void SetUp() override { Math::Random::Unseed(); }
    void TearDown() override { Math::Random::Unseed(); }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static constexpr const char *DrawScript = R"(
        local out = {}
        local f = Random.Float()
        out[#out + 1] = string.format("%.9g", f)
        local i = Random.Int(1, 100)
        out[#out + 1] = tostring(i)
        local r = Random.Range(0.5, 1.5)
        out[#out + 1] = string.format("%.9g", r)
        local inside = Random.InUnitCircle()
        out[#out + 1] = string.format("%.9g,%.9g", inside.x, inside.y)
        local on = Random.OnUnitCircle()
        out[#out + 1] = string.format("%.9g,%.9g", on.x, on.y)
        return table.concat(out, ";")
    )";
};

TEST_F(LuaRandomSeedTest, SeedingAgainReproducesTheRandomSequence)
{
    std::string draws[2];
    for (int run = 0; run < 2; ++run)
    {
        Eval(Lua(), "Random.Seed(42) return ''");
        // Several draws, each in its own chunk run, so the order is the order of the runs
        for (int i = 0; i < 5; ++i)
        {
            draws[run] += Eval(Lua(), DrawScript) + "|";
        }
    }
    EXPECT_FALSE(draws[0].empty());
    EXPECT_EQ(draws[0], draws[1]);
    EXPECT_TRUE(Math::Random::IsSeeded());
}

TEST_F(LuaRandomSeedTest, TheLuaSeedIsTheCppSeed)
{
    Eval(Lua(), "Random.Seed(9001) return ''");
    const std::string fromLua = Eval(Lua(), "return string.format('%.9g', Random.Float())");

    Math::DeterministicRandom reference(9001);
    const float expected = reference.NextFloat();
    char text[64];
    std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(expected));
    EXPECT_EQ(fromLua, text);
}

TEST_F(LuaRandomSeedTest, DifferentSeedsGiveDifferentSequences)
{
    Eval(Lua(), "Random.Seed(1) return ''");
    const std::string one = Eval(Lua(), DrawScript);
    Eval(Lua(), "Random.Seed(2) return ''");
    const std::string two = Eval(Lua(), DrawScript);
    EXPECT_NE(one, two);
}

// The engine's Random and the one of the particle systems use one algorithm: a change to either shows here
TEST(DeterministicRandomParityTest, SameU32SequenceAsParticleRandom)
{
    for (const std::uint32_t seed : {1u, 42u, 0xDEADBEEFu})
    {
        Math::DeterministicRandom engineStream(seed);
        Particles::ParticleRandom particleStream(seed);
        for (int i = 0; i < 100; ++i)
        {
            const std::uint32_t fromEngine = engineStream.NextU32();
            const std::uint32_t fromParticles = particleStream.NextU32();
            ASSERT_EQ(fromEngine, fromParticles) << "seed " << seed << " draw " << i;
        }
    }
}

// The project setting determinism.luaSeed
TEST(DeterminismSettingTest, DefaultsToUnset)
{
    std::vector<std::string> problems;
    EXPECT_FALSE(ReadDeterminismLuaSeed(json::object(), &problems).has_value());
    EXPECT_FALSE(ReadDeterminismLuaSeed(json{{"determinism", json::object()}}, &problems).has_value());
    EXPECT_FALSE(ReadDeterminismLuaSeed(json{{"determinism", {{"luaSeed", nullptr}}}}, &problems).has_value());
    EXPECT_FALSE(ReadDeterminismLuaSeed(json::array(), &problems).has_value());
    EXPECT_FALSE(ReadDeterminismLuaSeed(json{{"determinism", 3}}, &problems).has_value());
    EXPECT_TRUE(problems.empty());
}

TEST(DeterminismSettingTest, ReadsANonNegativeInteger)
{
    for (const std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{123456789},
                                     std::uint64_t{18446744073709551615ull}})
    {
        // Saved as the project file's settings, read back
        const json reloaded = json::parse(json{{"determinism", {{"luaSeed", seed}}}}.dump());
        std::vector<std::string> problems;
        const auto read = ReadDeterminismLuaSeed(reloaded, &problems);
        ASSERT_TRUE(read.has_value()) << seed;
        EXPECT_EQ(*read, seed);
        EXPECT_TRUE(problems.empty());
    }
}

TEST(DeterminismSettingTest, ARefusedValueIsAWarningAndMeansUnset)
{
    for (const json &bad : {json(-1), json(1.5), json(5.0), json("7"), json(true), json::object(), json::array()})
    {
        std::vector<std::string> problems;
        EXPECT_FALSE(ReadDeterminismLuaSeed(json{{"determinism", {{"luaSeed", bad}}}}, &problems).has_value())
            << bad.dump();
        ASSERT_EQ(problems.size(), 1u) << bad.dump();
        EXPECT_NE(problems[0].find("determinism.luaSeed"), std::string::npos);
    }
}

TEST(DeterminismSettingTest, ApplyProjectSettingsValidatesItAndChangesNothingLive)
{
    EXPECT_TRUE(ApplyProjectSettings(json{{"determinism", {{"luaSeed", 5}}}}, std::set<std::string>{"determinism"})
                    .empty());
    EXPECT_TRUE(ApplyProjectSettings(json{{"determinism", json::object()}}, std::set<std::string>{"determinism"})
                    .empty());

    const auto bad = ApplyProjectSettings(json{{"determinism", {{"luaSeed", "banana"}}}},
                                          std::set<std::string>{"determinism"});
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_NE(bad[0].find("determinism.luaSeed"), std::string::npos);

    const auto notAnObject = ApplyProjectSettings(json{{"determinism", 5}}, std::set<std::string>{"determinism"});
    ASSERT_EQ(notAnObject.size(), 1u);
    EXPECT_NE(notAnObject[0].find("determinism"), std::string::npos);

    // Validation only: the configured seed is still unset
    EXPECT_FALSE(LuaRuntime::GetLuaSeed().has_value());
}

// Random.NewStream(seed) / RandomStream(seed): streams of their own, the algorithm of the seeded global Random
class LuaRandomStreamTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { ASSERT_TRUE(LuaRuntime::Instance().Initialize()); }
    void SetUp() override { Math::Random::Unseed(); }
    void TearDown() override { Math::Random::Unseed(); }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    // The error text of a chunk that must fail, empty if it succeeded
    static std::string ErrorOf(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid())
        {
            return {};
        }
        const sol::error err = result;
        return err.what();
    }

    // Every function of a stream once, as text (floats to 9 digits, which round-trips a float)
    static constexpr const char *StreamDrawScript = R"(
        local out = {}
        out[#out + 1] = string.format("%.9g", s:Float())
        out[#out + 1] = tostring(s:Int(1, 100))
        out[#out + 1] = string.format("%.9g", s:Range(0.5, 1.5))
        local inside = s:InUnitCircle()
        out[#out + 1] = string.format("%.9g,%.9g", inside.x, inside.y)
        local on = s:OnUnitCircle()
        out[#out + 1] = string.format("%.9g,%.9g", on.x, on.y)
        return table.concat(out, ";")
    )";
};

// Golden values: the integers the algorithm gives for these seeds (computed from the algorithm's definition, and the
// first u32 of seed 1 is DeterministicRandomTest.KnownSequence's), the same on every platform
TEST_F(LuaRandomStreamTest, KnownSequences)
{
    EXPECT_EQ(Eval(Lua(), R"(
        local s = Random.NewStream(1)
        local out = {}
        for i = 1, 6 do out[#out + 1] = tostring(s:Int(1, 100)) end
        out[#out + 1] = string.format("%.9g", s:Float())
        out[#out + 1] = tostring(s:GetState())
        return table.concat(out, ",")
    )"), "12,37,65,40,45,78,0.972367644,1401181200");
    EXPECT_EQ(Eval(Lua(), R"(
        local s = RandomStream(42)
        local out = {}
        for i = 1, 6 do out[#out + 1] = tostring(s:Int(1, 100)) end
        out[#out + 1] = string.format("%.9g", s:Float())
        return table.concat(out, ",")
    )"), "25,57,32,18,60,86,0.131385505");
    EXPECT_EQ(Eval(Lua(), R"(
        local s = RandomStream.new(123456789)
        local out = {}
        for i = 1, 6 do out[#out + 1] = tostring(s:Int(1, 100)) end
        out[#out + 1] = string.format("%.9g", s:Float())
        out[#out + 1] = tostring(s:GetState())
        return table.concat(out, ",")
    )"), "22,100,75,94,7,27,0.851841033,1524637988");
}

// A stream seeded n draws what C++ Math::DeterministicRandom(n) draws
TEST_F(LuaRandomStreamTest, MatchesADeterministicRandomWithTheSameSeed)
{
    const std::string fromLua = Eval(Lua(), R"(
        local s = Random.NewStream(9001)
        local out = {}
        for i = 1, 20 do out[#out + 1] = tostring(s:Int(-1000, 1000)) end
        out[#out + 1] = string.format("%.9g", s:Float())
        return table.concat(out, ",")
    )");
    Math::DeterministicRandom reference(9001);
    std::string expected;
    for (int i = 0; i < 20; ++i)
    {
        expected += std::to_string(reference.NextInt(-1000, 1000)) + ",";
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(reference.NextFloat()));
    expected += text;
    EXPECT_EQ(fromLua, expected);
}

// Function for function, a stream seeded n is the global Random after Random.Seed(n)
TEST_F(LuaRandomStreamTest, DrawsWhatTheSeededGlobalRandomDraws)
{
    const std::string global = Eval(Lua(), std::string(R"(
        Random.Seed(77)
        local s = {
            Float = function() return Random.Float() end,
            Int = function(_, a, b) return Random.Int(a, b) end,
            Range = function(_, a, b) return Random.Range(a, b) end,
            InUnitCircle = function() return Random.InUnitCircle() end,
            OnUnitCircle = function() return Random.OnUnitCircle() end,
        }
        local function run()
    )") + StreamDrawScript + R"(
        end
        return run() .. "|" .. run()
    )");
    Math::Random::Unseed();
    const std::string stream = Eval(Lua(), std::string(R"(
        local s = Random.NewStream(77)
        local function run()
    )") + StreamDrawScript + R"(
        end
        return run() .. "|" .. run()
    )");
    EXPECT_FALSE(stream.empty());
    EXPECT_EQ(global, stream);
}

// Two streams, and a stream and the global Random, do not disturb each other
TEST_F(LuaRandomStreamTest, StreamsAreIndependent)
{
    // Alone: the first five draws of a stream seeded 5
    const std::string alone = Eval(Lua(), R"(
        local a = Random.NewStream(5)
        local out = {}
        for i = 1, 5 do out[#out + 1] = tostring(a:Int(0, 1000000)) end
        return table.concat(out, ",")
    )");
    // The same, interleaved with draws from another stream with the same seed, a different seed and the global Random
    // (seeded and unseeded)
    const std::string interleaved = Eval(Lua(), R"(
        local a = Random.NewStream(5)
        local twin = Random.NewStream(5)
        local other = Random.NewStream(6)
        local out = {}
        for i = 1, 5 do
            twin:Int(0, 1000000)
            other:Float()
            other:OnUnitCircle()
            Random.Float()
            if i == 3 then Random.Seed(5) end
            Random.Int(0, 10)
            out[#out + 1] = tostring(a:Int(0, 1000000))
        end
        return table.concat(out, ",")
    )");
    EXPECT_FALSE(alone.empty());
    EXPECT_EQ(alone, interleaved);

    // Seeding the global Random does not reseed or restart a stream either
    const std::string afterGlobalSeed = Eval(Lua(), R"(
        local a = Random.NewStream(5)
        local first = a:Int(0, 1000000)
        Random.Seed(5)
        local second = a:Int(0, 1000000)
        return tostring(first) .. "," .. tostring(second)
    )");
    EXPECT_EQ(afterGlobalSeed, alone.substr(0, alone.find(',', alone.find(',') + 1)));
}

// GetState/SetState put a stream back where it was; Seed restarts it; Clone is a second stream at the same point
TEST_F(LuaRandomStreamTest, StateSaveAndRestore)
{
    EXPECT_EQ(Eval(Lua(), R"(
        local s = Random.NewStream(31337)
        for i = 1, 10 do s:Float() end
        local saved = s:GetState()
        local function draws()
            local out = {}
            for i = 1, 8 do out[#out + 1] = tostring(s:Int(1, 6)) .. string.format(":%.9g", s:Range(-1, 1)) end
            local p = s:InUnitCircle()
            out[#out + 1] = string.format("%.9g,%.9g", p.x, p.y)
            return table.concat(out, ",")
        end
        local first = draws()
        s:SetState(saved)
        local again = draws()
        local copy
        s:SetState(saved)
        copy = s:Clone()
        local fromClone = (function() local keep = s; s = copy; local r = draws(); s = keep; return r end)()
        local fromOriginal = draws()
        -- A state is an integer in [0, 2^32), so it fits any field and JSON
        local inRange = math.type(saved) == "integer" and saved >= 0 and saved < 4294967296
        return tostring(first == again) .. tostring(first == fromClone) .. tostring(first == fromOriginal) ..
            tostring(inRange)
    )"), "truetruetruetrue");

    // Seed(n) restarts the sequence of RandomStream(n); a seed is taken modulo 2^32, as Random.Seed takes it
    EXPECT_EQ(Eval(Lua(), R"(
        local s = RandomStream(8)
        local first = s:Int(1, 1000000)
        s:Int(1, 1000000)
        s:Seed(8)
        local restarted = s:Int(1, 1000000)
        local wrapped = RandomStream(8 + 4294967296):Int(1, 1000000)
        local negative = RandomStream(-1):GetState()
        return tostring(first == restarted) .. tostring(first == wrapped) .. tostring(negative)
    )"), "truetrue4294967295");
}

// The state matches C++: a state saved from Lua seeds a C++ DeterministicRandom to the same point, and back
TEST_F(LuaRandomStreamTest, TheStateIsTheCppState)
{
    Math::DeterministicRandom reference(2024);
    for (int i = 0; i < 7; ++i)
    {
        (void) reference.NextU32();
    }
    const std::string fromLua = Eval(Lua(), R"(
        local s = RandomStream(2024)
        for i = 1, 7 do s:Int(0, 4294967295 // 2) end
        return tostring(s:GetState())
    )");
    // Int(0, 2^31 - 1) has a span of 2^31, which divides 2^32: no rejection, so one u32 per draw
    EXPECT_EQ(fromLua, std::to_string(reference.State()));
}

// Bad arguments are Lua errors, not truncations
TEST_F(LuaRandomStreamTest, BadArgumentsAreErrors)
{
    EXPECT_FALSE(ErrorOf("Random.NewStream()").empty());
    EXPECT_FALSE(ErrorOf("RandomStream()").empty());
    EXPECT_NE(ErrorOf("Random.NewStream(1.5)").find("must be an integer"), std::string::npos);
    EXPECT_NE(ErrorOf("Random.NewStream('5')").find("must be an integer"), std::string::npos);
    EXPECT_NE(ErrorOf("RandomStream(1):SetState({})").find("must be an integer"), std::string::npos);
    EXPECT_NE(ErrorOf("RandomStream(1):Int(1, 2.5)").find("must be an integer"), std::string::npos);
    EXPECT_NE(ErrorOf("RandomStream(1):Int(0, 4294967296)").find("outside the 32-bit integer range"),
              std::string::npos);
    // An integral float is an integer
    EXPECT_EQ(Eval(Lua(), "return tostring(RandomStream(3.0):GetState())"), "3");
    // An empty or reversed Int range gives min, as Random.Int does when seeded
    EXPECT_EQ(Eval(Lua(), "local s = RandomStream(3) return tostring(s:Int(7, 7)) .. tostring(s:Int(9, 2))"), "79");
}
