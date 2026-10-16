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
