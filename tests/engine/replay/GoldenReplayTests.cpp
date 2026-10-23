#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/replay/GoldenReplay.hpp"
#include "engine/replay/ReplayLog.hpp"

// Netcode C2: the replay format (ReplayLogTest) and the physics-free golden replays (GoldenReplayTest). What these assert
// is the properties of one binary in one process (the same log gives the same hashes, another input or seed gives
// others, a replay file is read back as it was written). They compare nothing with another platform: that is the CI job
// "golden replay" (scripts/replay-determinism.py), which reads the replay-hashes.json this file writes when
// N2_REPLAY_HASHES_OUT names one (N2_REPLAY_LEG names the leg) and compares it across legs and with
// tests/golden/replay-determinism.json.

using namespace N2Engine::Replay;

namespace
{
    std::string ReplayPath(const std::string &scene)
    {
        return std::string(N2_GOLDEN_REPLAY_DIR) + "/" + scene + ".n2replay.json";
    }

    ReplayLog Load(const std::string &scene)
    {
        std::string error;
        const auto log = LoadReplayLog(ReplayPath(scene), error);
        EXPECT_TRUE(log.has_value()) << scene << ": " << error;
        return log.value_or(ReplayLog{});
    }

    ReplayLog SmallLog()
    {
        ReplayLog log;
        log.scene = "transforms";
        log.seed = 7;
        log.ticks = 120;
        log.hashInterval = 60;
        log.inputs = {{0, 0, 10, -10, 1}, {0, 1, 0, 5, 0}, {30, 0, -127, 127, 3}};
        return log;
    }

    std::string Reject(const std::string &text)
    {
        std::string error;
        EXPECT_FALSE(ParseReplayLog(text, error).has_value()) << text;
        EXPECT_FALSE(error.empty()) << text;
        return error;
    }

    std::string WithEdit(const std::string &key, const nlohmann::json &value)
    {
        nlohmann::json root = nlohmann::json::parse(SerializeReplayLog(SmallLog()));
        root[key] = value;
        return root.dump();
    }
}

TEST(ReplayLogTest, SerializedLogParsesBackEqual)
{
    const ReplayLog log = SmallLog();
    std::string error;
    const auto parsed = ParseReplayLog(SerializeReplayLog(log), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->version, kReplayFormatVersion);
    EXPECT_EQ(parsed->scene, log.scene);
    EXPECT_EQ(parsed->seed, log.seed);
    EXPECT_EQ(parsed->ticks, log.ticks);
    EXPECT_EQ(parsed->hashInterval, log.hashInterval);
    EXPECT_EQ(parsed->inputs, log.inputs);
    EXPECT_EQ(SerializeReplayLog(*parsed), SerializeReplayLog(log));
}

TEST(ReplayLogTest, ALogWithNoInputsIsValid)
{
    ReplayLog log = SmallLog();
    log.inputs.clear();
    std::string error;
    const auto parsed = ParseReplayLog(SerializeReplayLog(log), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_TRUE(parsed->inputs.empty());
}

TEST(ReplayLogTest, AnUnknownVersionIsRefusedByName)
{
    const std::string error = Reject(WithEdit("version", 2));
    EXPECT_NE(error.find("version 2"), std::string::npos) << error;
    Reject(WithEdit("version", "1"));
}

TEST(ReplayLogTest, WrongFormatAndBrokenJsonAreRefused)
{
    Reject("");
    Reject("[1, 2]");
    Reject("{\"format\": \"n2replay\"");
    Reject(WithEdit("format", "other"));
}

TEST(ReplayLogTest, BadFieldsAreRefused)
{
    Reject(WithEdit("scene", 5));
    Reject(WithEdit("scene", ""));
    Reject(WithEdit("seed", -1));
    Reject(WithEdit("seed", 4294967296LL));
    Reject(WithEdit("ticks", -1));
    Reject(WithEdit("ticks", 1.5));
    Reject(WithEdit("hashInterval", 0));
    Reject(WithEdit("inputs", "none"));
}

TEST(ReplayLogTest, BadInputRowsAreRefused)
{
    Reject(WithEdit("inputs", nlohmann::json::array({nlohmann::json::array({0, 0, 0, 0})})));        // too short
    Reject(WithEdit("inputs", nlohmann::json::array({nlohmann::json::array({0, 0, 128, 0, 0})})));   // move out of range
    Reject(WithEdit("inputs", nlohmann::json::array({nlohmann::json::array({0, 4, 0, 0, 0})})));     // slot out of range
    Reject(WithEdit("inputs", nlohmann::json::array({nlohmann::json::array({120, 0, 0, 0, 0})})));   // tick == ticks
    Reject(WithEdit("inputs", nlohmann::json::array({nlohmann::json::array({0, 0, 0, 0, -1})})));    // buttons negative
    Reject(WithEdit("inputs", nlohmann::json::array({nlohmann::json::array({0, 0, 0, 0, 1.5})})));   // not an integer
    // not sorted, and the same (tick, slot) twice
    Reject(WithEdit("inputs", nlohmann::json::array({nlohmann::json::array({5, 0, 0, 0, 0}), nlohmann::json::array({4, 0, 0, 0, 0})})));
    Reject(WithEdit("inputs", nlohmann::json::array({nlohmann::json::array({5, 1, 0, 0, 0}), nlohmann::json::array({5, 1, 0, 0, 0})})));
    Reject(WithEdit("inputs", nlohmann::json::array({nlohmann::json::array({5, 1, 0, 0, 0}), nlohmann::json::array({5, 0, 0, 0, 0})})));
}

TEST(ReplayLogTest, AMissingFileIsAnError)
{
    std::string error;
    EXPECT_FALSE(LoadReplayLog(std::string(N2_GOLDEN_REPLAY_DIR) + "/no-such.n2replay.json", error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST(GoldenReplayTest, TheCommittedReplaysCoverEverySceneAndAreCanonical)
{
    std::set<std::string> scenes;
    for (const std::string &name : SceneNames())
    {
        const ReplayLog log = Load(name);
        EXPECT_EQ(log.scene, name);
        EXPECT_EQ(log.ticks, 600);
        EXPECT_EQ(log.hashInterval, 60);
        EXPECT_FALSE(log.inputs.empty());
        scenes.insert(log.scene);

        // The committed text is what the serializer writes: a regenerated file is a no-op diff
        std::ifstream in(ReplayPath(name), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // A Windows checkout may have turned the line ends into CRLF (git autocrlf); the content is what counts
        std::erase(text, '\r');
        EXPECT_EQ(text, SerializeReplayLog(log)) << name;
    }
    EXPECT_EQ(scenes.size(), SceneNames().size());
}

TEST(GoldenReplayTest, EveryReplayRunsTwiceToTheSameHashes)
{
    for (const std::string &name : SceneNames())
    {
        const ReplayLog log = Load(name);
        const ReplayResult first = RunReplay(log);
        const ReplayResult second = RunReplay(log);
        ASSERT_TRUE(first.ok) << name << ": " << first.error;
        ASSERT_TRUE(second.ok) << name << ": " << second.error;
        ASSERT_EQ(first.hashes.size(), 10u) << name;
        for (std::size_t i = 0; i < first.hashes.size(); ++i)
        {
            EXPECT_EQ(first.hashes[i].tick, static_cast<int>(60 * (i + 1)));
            EXPECT_EQ(first.hashes[i].hash, second.hashes[i].hash) << name << " tick " << first.hashes[i].tick;
        }
        // The state moves: no two checkpoints are equal
        std::set<std::uint64_t> distinct;
        for (const ReplayHash &entry : first.hashes)
        {
            distinct.insert(entry.hash);
        }
        EXPECT_EQ(distinct.size(), first.hashes.size()) << name;
    }
}

TEST(GoldenReplayTest, AnotherInputOrAnotherSeedGivesOtherHashes)
{
    for (const std::string &name : SceneNames())
    {
        const ReplayLog log = Load(name);
        const ReplayResult base = RunReplay(log);
        ASSERT_TRUE(base.ok) << name << ": " << base.error;

        // One input changed at tick 300: the hashes before it are unchanged, those after it differ
        ReplayLog changed = log;
        changed.inputs.push_back({299, 0, 1, 2, 3});
        // keep the rows sorted: the new row goes before the first row at a later tick
        std::stable_sort(changed.inputs.begin(), changed.inputs.end(), [](const ReplayInput &a, const ReplayInput &b) {
            return a.tick != b.tick ? a.tick < b.tick : a.slot < b.slot;
        });
        for (std::size_t i = 1; i < changed.inputs.size(); ++i)
        {
            ASSERT_TRUE(changed.inputs[i - 1].tick != changed.inputs[i].tick || changed.inputs[i - 1].slot != changed.inputs[i].slot)
                << "the extra row collided with a committed one";
        }
        const ReplayResult other = RunReplay(changed);
        ASSERT_TRUE(other.ok) << other.error;
        for (std::size_t i = 0; i < base.hashes.size(); ++i)
        {
            if (base.hashes[i].tick <= 299)
            {
                EXPECT_EQ(base.hashes[i].hash, other.hashes[i].hash) << name << " tick " << base.hashes[i].tick;
            }
            else
            {
                EXPECT_NE(base.hashes[i].hash, other.hashes[i].hash) << name << " tick " << base.hashes[i].tick;
            }
        }

        ReplayLog reseeded = log;
        reseeded.seed += 1;
        const ReplayResult seeded = RunReplay(reseeded);
        ASSERT_TRUE(seeded.ok) << seeded.error;
        EXPECT_NE(base.hashes.back().hash, seeded.hashes.back().hash) << name;
    }
}

TEST(GoldenReplayTest, AnInvalidLogOrUnknownSceneIsAnErrorNotAHash)
{
    ReplayLog log = SmallLog();
    log.scene = "no_such_scene";
    ReplayResult result = RunReplay(log);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("no_such_scene"), std::string::npos);
    EXPECT_TRUE(result.hashes.empty());

    log = SmallLog();
    log.inputs.push_back({1, 0, 0, 0, 0}); // out of order after (30, 0)
    result = RunReplay(log);
    EXPECT_FALSE(result.ok);
}

TEST(GoldenReplayTest, ALogOfZeroTicksHasNoCheckpoints)
{
    ReplayLog log = SmallLog();
    log.ticks = 0;
    log.inputs.clear();
    const ReplayResult result = RunReplay(log);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.hashes.empty());
}

TEST(GoldenReplayTest, InputsHeldBetweenRowsAreEquivalentToRepeatedRows)
{
    // "Held until the next row" means a row repeated every tick changes nothing
    ReplayLog sparse = SmallLog();
    sparse.scene = "lua_driven";
    sparse.inputs = {{0, 0, 40, -40, 1}};
    ReplayLog dense = sparse;
    dense.inputs.clear();
    for (int tick = 0; tick < dense.ticks; ++tick)
    {
        dense.inputs.push_back({tick, 0, 40, -40, 1});
    }
    const ReplayResult a = RunReplay(sparse);
    const ReplayResult b = RunReplay(dense);
    ASSERT_TRUE(a.ok && b.ok) << a.error << b.error;
    ASSERT_EQ(a.hashes.size(), b.hashes.size());
    for (std::size_t i = 0; i < a.hashes.size(); ++i)
    {
        EXPECT_EQ(a.hashes[i].hash, b.hashes[i].hash);
    }
}

// Runs every committed replay, prints the checkpoint lines and writes the file the CI leg uploads (when asked to)
TEST(GoldenReplayCorpusTest, RunsEveryCommittedReplayAndWritesTheHashes)
{
    std::vector<NamedResult> results;
    int ticks = 0;
    int interval = 0;
    for (const std::string &name : SceneNames())
    {
        const ReplayLog log = Load(name);
        ticks = log.ticks;
        interval = log.hashInterval;
        NamedResult named;
        named.name = name;
        named.result = RunReplay(log);
        EXPECT_TRUE(named.result.ok) << name << ": " << named.result.error;
        for (const ReplayHash &entry : named.result.hashes)
        {
            std::printf("[REPLAY] %s tick %d %s\n", name.c_str(), entry.tick, FormatHash(entry.hash).c_str());
        }
        results.push_back(std::move(named));
    }
    std::fflush(stdout);

    const char *path = std::getenv("N2_REPLAY_HASHES_OUT");
    if (path == nullptr || *path == '\0')
    {
        return;
    }
    const char *leg = std::getenv("N2_REPLAY_LEG");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << HashesToJson((leg != nullptr && *leg != '\0') ? std::string(leg) : std::string("local"), results, ticks, interval);
    out.flush();
    EXPECT_TRUE(static_cast<bool>(out)) << "could not write " << path;
}
