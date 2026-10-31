#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/replay/GoldenReplay.hpp"

// Netcode C5 (the rollback test client): ReplaySimulation::Save and Restore. A snapshot of a replay scene is what a
// rollback restore and a late spectator's join put back, so the properties here are the ones rollback needs: restoring
// and rerunning reproduces the straight run, a fresh simulation restored from a blob continues alike, a blob of another
// scene or seed is refused without touching the simulation, and no damaged blob crashes anything.

using namespace N2Engine::Replay;

namespace
{
    constexpr std::uint32_t kSeed = 11;

    std::array<SlotState, 2> HeldAt(const int tick)
    {
        std::array<SlotState, 2> held{};
        held[0].moveX = (tick * 37) % 255 - 127;
        held[0].moveY = (tick * 11) % 255 - 127;
        held[0].buttons = (tick % 5 == 0) ? 1u : 0u;
        held[1].moveX = (tick * 5) % 100;
        return held;
    }

    std::unique_ptr<ReplaySimulation> Make(const std::string &scene, const std::uint32_t seed = kSeed)
    {
        std::string error;
        auto simulation = ReplaySimulation::Create(scene, seed, error);
        EXPECT_NE(simulation, nullptr) << error;
        return simulation;
    }

    void StepTicks(ReplaySimulation &simulation, const int ticks, std::vector<std::uint64_t> *hashes = nullptr)
    {
        for (int i = 0; i < ticks; ++i)
        {
            std::string error;
            const auto held = HeldAt(simulation.TicksRun());
            ASSERT_TRUE(simulation.Step(held, error)) << error;
            if (hashes != nullptr)
            {
                hashes->push_back(simulation.Hash());
            }
        }
    }

    std::vector<std::uint8_t> Save(ReplaySimulation &simulation)
    {
        std::string error;
        auto blob = simulation.Save(error);
        EXPECT_FALSE(blob.empty()) << error;
        return blob;
    }

    // The checksum of the blob format (FNV-1a 64 over everything before it), to re-seal a mutated blob so the parser behind
    // the checksum is exercised
    void Reseal(std::vector<std::uint8_t> &blob)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::size_t i = 0; i + 8 < blob.size(); ++i)
        {
            hash ^= blob[i];
            hash *= 1099511628211ULL;
        }
        for (int i = 0; i < 8; ++i)
        {
            blob[blob.size() - 8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((hash >> (8 * i)) & 0xFFu);
        }
    }
}

TEST(ReplaySnapshotTest, RestoreAndRerunReproducesTheStraightRun)
{
    for (const std::string &scene : SceneNames())
    {
        auto straight = Make(scene);
        std::vector<std::uint64_t> expected;
        StepTicks(*straight, 150, &expected);

        auto simulation = Make(scene);
        StepTicks(*simulation, 40);
        const std::vector<std::uint8_t> blob = Save(*simulation);
        // Run on to a different state, then back
        StepTicks(*simulation, 70);
        std::string error;
        ASSERT_TRUE(simulation->Restore(blob, error)) << scene << ": " << error;
        EXPECT_EQ(simulation->TicksRun(), 40) << scene;
        std::vector<std::uint64_t> rerun;
        StepTicks(*simulation, 110, &rerun);
        for (std::size_t i = 0; i < rerun.size(); ++i)
        {
            ASSERT_EQ(rerun[i], expected[40 + i]) << scene << " tick " << (41 + i);
        }
    }
}

TEST(ReplaySnapshotTest, SavingDoesNotChangeTheRun)
{
    for (const std::string &scene : SceneNames())
    {
        auto straight = Make(scene);
        std::vector<std::uint64_t> expected;
        StepTicks(*straight, 90, &expected);

        auto simulation = Make(scene);
        std::vector<std::uint64_t> hashes;
        for (int i = 0; i < 90; ++i)
        {
            StepTicks(*simulation, 1, &hashes);
            (void)Save(*simulation);
        }
        EXPECT_EQ(hashes, expected) << scene;
    }
}

TEST(ReplaySnapshotTest, AFreshSimulationRestoredFromABlobContinuesAlike)
{
    for (const std::string &scene : SceneNames())
    {
        auto straight = Make(scene);
        std::vector<std::uint64_t> expected;
        StepTicks(*straight, 120, &expected);

        auto origin = Make(scene);
        StepTicks(*origin, 55);
        const std::vector<std::uint8_t> blob = Save(*origin);

        auto fresh = Make(scene);
        std::string error;
        ASSERT_TRUE(fresh->Restore(blob, error)) << scene << ": " << error;
        EXPECT_EQ(fresh->TicksRun(), 55);
        EXPECT_EQ(fresh->Hash(), origin->Hash()) << scene;
        // Same bytes out as in: nothing about the blob depends on the process that wrote it
        EXPECT_EQ(Save(*fresh), blob) << scene;
        std::vector<std::uint64_t> rerun;
        StepTicks(*fresh, 65, &rerun);
        for (std::size_t i = 0; i < rerun.size(); ++i)
        {
            ASSERT_EQ(rerun[i], expected[55 + i]) << scene << " tick " << (56 + i);
        }
    }
}

TEST(ReplaySnapshotTest, TheSameStateIsTheSameBytes)
{
    for (const std::string &scene : SceneNames())
    {
        auto a = Make(scene);
        auto b = Make(scene);
        StepTicks(*a, 70);
        StepTicks(*b, 70);
        EXPECT_EQ(Save(*a), Save(*b)) << scene;
        StepTicks(*b, 1);
        EXPECT_NE(Save(*a), Save(*b)) << scene;
    }
}

TEST(ReplaySnapshotTest, AForeignBlobIsRefusedAndTheSimulationIsUnchanged)
{
    const std::vector<std::string> scenes = SceneNames();
    for (const std::string &scene : scenes)
    {
        auto simulation = Make(scene);
        StepTicks(*simulation, 30);
        const std::uint64_t before = simulation->Hash();
        const std::vector<std::uint8_t> own = Save(*simulation);

        for (const std::string &other : scenes)
        {
            if (other == scene)
            {
                continue;
            }
            auto foreign = Make(other);
            StepTicks(*foreign, 30);
            std::string error;
            EXPECT_FALSE(simulation->Restore(Save(*foreign), error)) << scene << " <- " << other;
            EXPECT_NE(error.find("scene"), std::string::npos) << error;
        }
        auto otherSeed = Make(scene, kSeed + 1);
        StepTicks(*otherSeed, 30);
        std::string error;
        EXPECT_FALSE(simulation->Restore(Save(*otherSeed), error)) << scene;
        EXPECT_NE(error.find("seed"), std::string::npos) << error;

        EXPECT_EQ(simulation->TicksRun(), 30);
        EXPECT_EQ(simulation->Hash(), before) << scene;
        // And it still runs on as the straight run does
        auto straight = Make(scene);
        StepTicks(*straight, 30);
        std::vector<std::uint64_t> a;
        std::vector<std::uint64_t> b;
        StepTicks(*simulation, 10, &a);
        StepTicks(*straight, 10, &b);
        EXPECT_EQ(a, b) << scene;
        EXPECT_FALSE(own.empty());
    }
}

TEST(ReplaySnapshotTest, TruncatedAndFlippedBlobsAreRefusedWithoutACrash)
{
    for (const std::string &scene : SceneNames())
    {
        auto simulation = Make(scene);
        StepTicks(*simulation, 45);
        const std::vector<std::uint8_t> blob = Save(*simulation);
        const std::uint64_t before = simulation->Hash();

        std::string error;
        EXPECT_FALSE(simulation->Restore({}, error));
        for (std::size_t length = 0; length < blob.size(); ++length)
        {
            error.clear();
            EXPECT_FALSE(simulation->Restore(std::span<const std::uint8_t>(blob.data(), length), error)) << scene << " " << length;
            EXPECT_FALSE(error.empty());
        }
        // Every bit of the blob, flipped: the checksum catches it
        std::vector<std::uint8_t> flipped = blob;
        for (std::size_t i = 0; i < flipped.size(); ++i)
        {
            for (int bit = 0; bit < 8; bit += 3)
            {
                flipped[i] ^= static_cast<std::uint8_t>(1u << bit);
                error.clear();
                EXPECT_FALSE(simulation->Restore(flipped, error)) << scene << " byte " << i << " bit " << bit;
                flipped[i] ^= static_cast<std::uint8_t>(1u << bit);
            }
        }
        EXPECT_EQ(simulation->Hash(), before) << scene;
        EXPECT_EQ(simulation->TicksRun(), 45);
    }
}

TEST(ReplaySnapshotTest, AResealedMutatedBlobNeverCrashesAndNeverHalfRestores)
{
    for (const std::string &scene : SceneNames())
    {
        auto simulation = Make(scene);
        StepTicks(*simulation, 45);
        const std::vector<std::uint8_t> blob = Save(*simulation);
        const std::uint64_t before = simulation->Hash();

        std::uint32_t state = 12345;
        for (int round = 0; round < 600; ++round)
        {
            std::vector<std::uint8_t> mutated = blob;
            // Mutate a byte (never the checksum, which is re-sealed), sometimes truncate the body too
            state = state * 1664525u + 1013904223u;
            const std::size_t at = (state >> 8) % (mutated.size() - 8);
            state = state * 1664525u + 1013904223u;
            mutated[at] = static_cast<std::uint8_t>(state >> 16);
            if (round % 7 == 0)
            {
                state = state * 1664525u + 1013904223u;
                const std::size_t cut = 8 + (state >> 8) % (mutated.size() - 8);
                mutated.resize(cut);
                if (mutated.size() < 12)
                {
                    continue;
                }
            }
            Reseal(mutated);
            std::string error;
            auto probe = Make(scene);
            StepTicks(*probe, 45);
            if (!probe->Restore(mutated, error))
            {
                // Refused: the simulation is exactly as it was
                EXPECT_EQ(probe->Hash(), before) << scene << " round " << round;
                EXPECT_EQ(probe->TicksRun(), 45);
            }
            else
            {
                // Accepted (the byte changed nothing that matters, or it was a harmless copy): the state hashes to what
                // the blob said, so it is a state some Save could have written
                EXPECT_EQ(probe->Hash(), before) << scene << " round " << round;
            }
        }
    }
}

TEST(ReplaySnapshotTest, ASnapshotOfAFreshSceneRoundTrips)
{
    for (const std::string &scene : SceneNames())
    {
        auto fresh = Make(scene);
        const auto blob = Save(*fresh);
        auto other = Make(scene);
        StepTicks(*other, 20);
        std::string error;
        ASSERT_TRUE(other->Restore(blob, error)) << error;
        EXPECT_EQ(other->TicksRun(), 0);
        EXPECT_EQ(other->Hash(), fresh->Hash());
    }
}
