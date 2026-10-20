#include <gtest/gtest.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/ProjectSettings.hpp"
#include "engine/network/NetworkSettings.hpp"

// The project setting "network": parsing, defaults, ranges and validation through ApplyProjectSettings.
// Application::Init reads it with ReadNetworkSettings before it creates the physics backend.

using namespace N2Engine;
using namespace N2Engine::Network;
using nlohmann::json;

namespace
{
    json Block(const json &network)
    {
        return json{{"network", network}};
    }

    std::vector<std::string> Problems(const json &settings)
    {
        std::vector<std::string> problems;
        (void)ReadNetworkSettings(settings, &problems);
        return problems;
    }
}

TEST(NetworkSettingsTest, DefaultsToTheNonePresetWithTheDesignDefaults)
{
    std::vector<std::string> problems;
    for (const json &settings : {json::object(), json::array(), json(nullptr), json(3), Block(json::object()),
                                 Block(nullptr), Block(5), Block("deterministic")})
    {
        const NetworkSettings parsed = ReadNetworkSettings(settings, &problems);
        EXPECT_EQ(parsed.preset, NetworkPreset::None) << settings.dump();
        EXPECT_EQ(parsed.tickRate, 60);
        EXPECT_TRUE(parsed.transport.udp);
        EXPECT_TRUE(parsed.transport.webSocket);
        EXPECT_EQ(parsed.transport.port, 7777);
        EXPECT_EQ(parsed.authoritative.snapshotRate, 30);
        EXPECT_EQ(parsed.authoritative.interpolationDelayMs, 100);
        EXPECT_EQ(parsed.authoritative.inputRedundancy, 4);
        EXPECT_EQ(parsed.authoritative.predictionMaxMs, 250);
        EXPECT_EQ(parsed.authoritative.smoothingMs, 100);
        EXPECT_EQ(parsed.authoritative.snapThreshold, 1.0);
        EXPECT_EQ(parsed.authoritative.lagCompMaxMs, 200);
        EXPECT_EQ(parsed.deterministic.mode, DeterministicMode::Lockstep);
        EXPECT_EQ(parsed.deterministic.inputDelay, 2);
        EXPECT_EQ(parsed.deterministic.rollbackWindow, 8);
        EXPECT_EQ(parsed.deterministic.hashInterval, 30);
        EXPECT_TRUE(parsed.overrides.is_object());
        EXPECT_TRUE(parsed.overrides.empty());
        EXPECT_FALSE(parsed.customised);
    }
    // A block that is absent, or not an object, is not a warning here (ApplyProjectSettings reports a non-object)
    EXPECT_TRUE(problems.empty());
}

TEST(NetworkSettingsTest, AnEmptyBlockEqualsNoBlock)
{
    // The none preset, and a project that never heard of the block, read the same
    const NetworkSettings none = ReadNetworkSettings(json::object());
    const NetworkSettings empty = ReadNetworkSettings(Block(json::object()));
    const NetworkSettings explicitNone = ReadNetworkSettings(Block(json{{"preset", "none"}}));
    for (const NetworkSettings *other : {&empty, &explicitNone})
    {
        EXPECT_EQ(other->preset, none.preset);
        EXPECT_EQ(other->tickRate, none.tickRate);
        EXPECT_EQ(other->transport.port, none.transport.port);
        EXPECT_EQ(other->authoritative.snapshotRate, none.authoritative.snapshotRate);
        EXPECT_EQ(other->deterministic.rollbackWindow, none.deterministic.rollbackWindow);
        EXPECT_EQ(other->overrides, none.overrides);
    }
}

TEST(NetworkSettingsTest, ReadsEveryValueFromSavedSettings)
{
    const json saved = json::parse(R"({"network": {
        "preset": "Authoritative", "tickRate": 120,
        "transport": {"udp": false, "webSocket": true, "port": 9000},
        "authoritative": {"snapshotRate": 20, "interpolationDelayMs": 150, "inputRedundancy": 3,
                          "predictionMaxMs": 200, "smoothingMs": 80, "snapThreshold": 2.5, "lagCompMaxMs": 100},
        "deterministic": {"mode": "ROLLBACK", "inputDelay": 4, "rollbackWindow": 12, "hashInterval": 60},
        "overrides": {"math.simdTier": "auto"}, "customised": true}})");
    std::vector<std::string> problems;
    const NetworkSettings parsed = ReadNetworkSettings(saved, &problems);
    EXPECT_TRUE(problems.empty()) << (problems.empty() ? "" : problems[0]);
    EXPECT_EQ(parsed.preset, NetworkPreset::Authoritative);
    EXPECT_EQ(parsed.tickRate, 120);
    EXPECT_FALSE(parsed.transport.udp);
    EXPECT_TRUE(parsed.transport.webSocket);
    EXPECT_EQ(parsed.transport.port, 9000);
    EXPECT_EQ(parsed.authoritative.snapshotRate, 20);
    EXPECT_EQ(parsed.authoritative.interpolationDelayMs, 150);
    EXPECT_EQ(parsed.authoritative.inputRedundancy, 3);
    EXPECT_EQ(parsed.authoritative.predictionMaxMs, 200);
    EXPECT_EQ(parsed.authoritative.smoothingMs, 80);
    EXPECT_EQ(parsed.authoritative.snapThreshold, 2.5);
    EXPECT_EQ(parsed.authoritative.lagCompMaxMs, 100);
    EXPECT_EQ(parsed.deterministic.mode, DeterministicMode::Rollback);
    EXPECT_EQ(parsed.deterministic.inputDelay, 4);
    EXPECT_EQ(parsed.deterministic.rollbackWindow, 12);
    EXPECT_EQ(parsed.deterministic.hashInterval, 60);
    EXPECT_EQ(parsed.overrides, (json{{"math.simdTier", "auto"}}));
    EXPECT_TRUE(parsed.customised);
}

TEST(NetworkSettingsTest, ReadsEachPresetName)
{
    for (const auto &[name, expected] : {std::pair{"none", NetworkPreset::None},
                                         std::pair{"authoritative", NetworkPreset::Authoritative},
                                         std::pair{"deterministic", NetworkPreset::Deterministic},
                                         std::pair{"Deterministic", NetworkPreset::Deterministic},
                                         std::pair{"NONE", NetworkPreset::None}})
    {
        std::vector<std::string> problems;
        EXPECT_EQ(ReadNetworkSettings(Block(json{{"preset", name}}), &problems).preset, expected) << name;
        EXPECT_TRUE(problems.empty()) << name;
    }
    EXPECT_EQ(ToString(NetworkPreset::Authoritative), "authoritative");
    EXPECT_EQ(ToString(DeterministicMode::Rollback), "rollback");
}

TEST(NetworkSettingsTest, AnUnknownPresetIsAWarningAndMeansNone)
{
    for (const json &bad : {json("lockstep"), json(true), json(1), json::object(), json("")})
    {
        std::vector<std::string> problems;
        EXPECT_EQ(ReadNetworkSettings(Block(json{{"preset", bad}}), &problems).preset, NetworkPreset::None)
            << bad.dump();
        ASSERT_EQ(problems.size(), 1u) << bad.dump();
        EXPECT_NE(problems[0].find("network.preset"), std::string::npos);
    }
}

TEST(NetworkSettingsTest, AnUnknownModeIsAWarningAndMeansLockstep)
{
    for (const json &bad : {json("peer"), json(false), json(2)})
    {
        std::vector<std::string> problems;
        const NetworkSettings parsed =
            ReadNetworkSettings(Block(json{{"deterministic", {{"mode", bad}}}}), &problems);
        EXPECT_EQ(parsed.deterministic.mode, DeterministicMode::Lockstep) << bad.dump();
        ASSERT_EQ(problems.size(), 1u) << bad.dump();
        EXPECT_NE(problems[0].find("network.deterministic.mode"), std::string::npos);
    }
}

TEST(NetworkSettingsTest, NumbersOutsideTheirRangeAreClampedWithOneWarningEach)
{
    const json network = {
        {"tickRate", 1000},
        {"transport", {{"port", 0}}},
        {"authoritative", {{"snapshotRate", 1}, {"predictionMaxMs", 5000}, {"snapThreshold", 0.0}}},
        {"deterministic", {{"inputDelay", 99}, {"rollbackWindow", -3}, {"hashInterval", 0}}},
    };
    std::vector<std::string> problems;
    const NetworkSettings parsed = ReadNetworkSettings(Block(network), &problems);
    EXPECT_EQ(parsed.tickRate, 120);
    EXPECT_EQ(parsed.transport.port, 1);
    EXPECT_EQ(parsed.authoritative.snapshotRate, 10);
    EXPECT_EQ(parsed.authoritative.predictionMaxMs, 250);
    EXPECT_EQ(parsed.authoritative.snapThreshold, 0.01);
    EXPECT_EQ(parsed.deterministic.inputDelay, 6);
    EXPECT_EQ(parsed.deterministic.rollbackWindow, 0);
    EXPECT_EQ(parsed.deterministic.hashInterval, 1);
    EXPECT_EQ(problems.size(), 8u);
    for (const std::string &problem : problems)
    {
        EXPECT_NE(problem.find("network."), std::string::npos) << problem;
    }
}

TEST(NetworkSettingsTest, WrongTypesAreWarningsAndMeanTheDefault)
{
    const json network = {
        {"tickRate", "fast"},
        {"transport", {{"udp", "yes"}, {"webSocket", 1}, {"port", 77.5}}},
        {"authoritative", {{"snapshotRate", true}, {"snapThreshold", "big"}}},
        {"deterministic", {{"inputDelay", json::array()}, {"hashInterval", 2.5}}},
        {"customised", "maybe"},
    };
    std::vector<std::string> problems;
    const NetworkSettings parsed = ReadNetworkSettings(Block(network), &problems);
    EXPECT_EQ(parsed.tickRate, 60);
    EXPECT_TRUE(parsed.transport.udp);
    EXPECT_TRUE(parsed.transport.webSocket);
    EXPECT_EQ(parsed.transport.port, 7777);
    EXPECT_EQ(parsed.authoritative.snapshotRate, 30);
    EXPECT_EQ(parsed.authoritative.snapThreshold, 1.0);
    EXPECT_EQ(parsed.deterministic.inputDelay, 2);
    EXPECT_EQ(parsed.deterministic.hashInterval, 30);
    EXPECT_FALSE(parsed.customised);
    EXPECT_EQ(problems.size(), 9u);
}

TEST(NetworkSettingsTest, ANestedValueThatIsNotAnObjectIsAWarning)
{
    std::vector<std::string> problems;
    const NetworkSettings parsed = ReadNetworkSettings(
        Block(json{{"transport", 5}, {"authoritative", "x"}, {"deterministic", json::array()}}), &problems);
    EXPECT_EQ(parsed.transport.port, 7777);
    EXPECT_EQ(parsed.authoritative.snapshotRate, 30);
    EXPECT_EQ(parsed.deterministic.inputDelay, 2);
    ASSERT_EQ(problems.size(), 3u);
    EXPECT_NE(problems[0].find("network.transport"), std::string::npos);
    EXPECT_NE(problems[1].find("network.authoritative"), std::string::npos);
    EXPECT_NE(problems[2].find("network.deterministic"), std::string::npos);
}

TEST(NetworkSettingsTest, AValueAtTheEdgeOfItsRangeIsAccepted)
{
    const json network = {
        {"tickRate", 30},
        {"transport", {{"port", 65535}}},
        {"deterministic", {{"inputDelay", 0}, {"rollbackWindow", 15}}},
    };
    EXPECT_TRUE(Problems(Block(network)).empty());
}

TEST(NetworkSettingsTest, OverridesKeepOnlyGovernedKeys)
{
    const json network = {{"overrides", {{"math.simdTier", "avx"}, {"physics.gpu", "on"}, {"nonsense", 1}}}};
    std::vector<std::string> problems;
    const NetworkSettings parsed = ReadNetworkSettings(Block(network), &problems);
    EXPECT_EQ(parsed.overrides, (json{{"math.simdTier", "avx"}, {"physics.gpu", "on"}}));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("network.overrides.nonsense"), std::string::npos);

    problems.clear();
    EXPECT_TRUE(ReadNetworkSettings(Block(json{{"overrides", 4}}), &problems).overrides.empty());
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("network.overrides"), std::string::npos);
}

TEST(NetworkSettingsTest, ASpecExistsForEveryNumericSettingAndHoldsTheDefaultInsideItsRange)
{
    EXPECT_NE(FindNumericSpec("tickRate"), nullptr);
    EXPECT_NE(FindNumericSpec("deterministic.rollbackWindow"), nullptr);
    EXPECT_EQ(FindNumericSpec("nonsense"), nullptr);
    for (const NumericSettingSpec &spec : NumericSettingSpecs())
    {
        EXPECT_LE(spec.min, spec.defaultValue) << spec.key;
        EXPECT_GE(spec.max, spec.defaultValue) << spec.key;
    }
}

TEST(NetworkSettingsTest, ApplyProjectSettingsValidatesTheBlockAndChangesNothingLive)
{
    const std::set<std::string> only{"network"};
    EXPECT_TRUE(ApplyProjectSettings(Block(json{{"preset", "authoritative"}}), only).empty());
    EXPECT_TRUE(ApplyProjectSettings(Block(json::object()), only).empty());
    // A block that is absent or null changes nothing and says nothing
    EXPECT_TRUE(ApplyProjectSettings(json::object(), only).empty());
    EXPECT_TRUE(ApplyProjectSettings(Block(nullptr), only).empty());

    const auto bad = ApplyProjectSettings(Block(json{{"preset", "banana"}, {"tickRate", 9999}}), only);
    ASSERT_EQ(bad.size(), 2u);
    EXPECT_NE(bad[0].find("network.preset"), std::string::npos);
    EXPECT_NE(bad[1].find("network.tickRate"), std::string::npos);

    const auto notAnObject = ApplyProjectSettings(Block(5), only);
    ASSERT_EQ(notAnObject.size(), 1u);
    EXPECT_NE(notAnObject[0].find("network"), std::string::npos);
}

TEST(NetworkSettingsTest, ABadValueInOneBlockDoesNotStopTheOthers)
{
    // The network warnings come next to those of the other blocks, and every block is still processed
    const json settings = {
        {"network", {{"preset", "banana"}}},
        {"math", {{"simdTier", "avx512"}}},
        {"determinism", {{"luaSeed", -1}}},
    };
    const auto problems = ApplyProjectSettings(settings, std::set<std::string>{"network", "math", "determinism"});
    EXPECT_EQ(problems.size(), 3u);
}
