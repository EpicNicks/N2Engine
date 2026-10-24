#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/network/LockstepInputMapping.hpp"
#include "engine/network/n2net/HashTree.hpp"
#include "engine/network/n2net/Lockstep.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/replay/GoldenReplay.hpp"
#include "engine/replay/LockstepReplay.hpp"
#include "engine/replay/ReplayLog.hpp"

// Netcode C3 on the replay side: a lockstep session over the seeded SimTransport (lossy, reordering, duplicating links)
// records its confirmed inputs as an n2replay file, and replaying that file headless gives the hashes the live peers
// printed; and the desync report a peer writes when the root exchange disagrees (an injected one-field divergence) names
// the object and field, carries the input log and the last K checkpoint trees.

using namespace N2Engine;
using namespace N2Engine::Replay;
using namespace N2Engine::N2Net;

namespace
{
    constexpr int kHashInterval = 30;
    constexpr std::uint32_t kHistory = 4;

    // A layout with every control type, and the scene reading "move" and the buttons: the version 2 sessions run under it
    Network::InputLayout RichLayout()
    {
        using Network::InputControlKind;
        auto created = Network::InputLayout::Create({
            {"move", InputControlKind::Vector2, 8, 1.0f, true},
            {"jump", InputControlKind::Button, 1, 1.0f, false},
            {"fire", InputControlKind::Button, 1, 1.0f, false},
            {"throttle", InputControlKind::Axis, 5, 4.0f, false},
            {"look", InputControlKind::Vector3, 6, 2.0f, true},
        });
        EXPECT_TRUE(created.has_value());
        return created.has_value() ? *created : Network::InputLayout{};
    }

    // Held for a few ticks at a time, like a player's input (so the recorder has runs to collapse). Under the legacy
    // layout (layout == nullptr) this is the original (moveX, moveY, buttons) input.
    LockstepInput InputFor(const Network::InputLayout *layout, const std::uint32_t slot, const std::uint32_t tick)
    {
        const std::uint32_t phase = tick / 5;
        if (layout == nullptr)
        {
            return Network::LegacyInput(static_cast<int>((phase * 37 + slot * 11) % 255) - 127,
                                        static_cast<int>((phase * 17 + slot * 5) % 255) - 127, (phase * 2654435761u + slot) & 0xFFu);
        }
        std::vector<float> values(layout->ValueCount());
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            values[i] = static_cast<float>((phase * 37 + slot * 11 + i * 29) % 201) / 100.0f - 1.0f;
        }
        const auto packed = layout->Pack(values);
        EXPECT_TRUE(packed.has_value());
        return LockstepInput::FromBytes(*packed).value_or(LockstepInput{});
    }

    std::array<std::uint8_t, 8> Bytes(const std::uint64_t value)
    {
        std::array<std::uint8_t, 8> bytes{};
        for (int i = 0; i < 8; ++i)
        {
            bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
        }
        return bytes;
    }

    struct SessionPeer
    {
        SessionPeer(const LockstepConfig &config, const std::string &scene, const std::uint32_t seed, const std::uint32_t slot,
                    SimTransport *transport, const int totalTicks, const Network::InputLayout *layoutPtr)
            : scheduler(config), slot(slot), transport(transport), history(kHistory),
              recorder(layoutPtr != nullptr
                           ? LockstepRecorder(scene, seed, kHashInterval, static_cast<int>(config.slotCount), *layoutPtr)
                           : LockstepRecorder(scene, seed, kHashInterval, static_cast<int>(config.slotCount))),
              ticksWanted(totalTicks), layout(layoutPtr)
        {
            std::string error;
            simulation = ReplaySimulation::Create(scene, seed, error);
            EXPECT_TRUE(simulation) << error;
        }

        LockstepScheduler scheduler;
        std::uint32_t slot;
        SimTransport *transport;
        std::unique_ptr<ReplaySimulation> simulation;
        HashTreeHistory history;
        LockstepRecorder recorder;
        int ticksWanted;
        /// nullptr: the legacy layout, recorded as a version 1 file
        const Network::InputLayout *layout;
        std::uint32_t localCount = 0;
        std::array<std::uint32_t, kMaxLockstepSlots> peerAck{};
        std::vector<ReplayHash> liveHashes;
        std::map<std::uint32_t, std::map<std::uint32_t, std::uint64_t>> peerRoots; // peer slot -> tick -> root
        std::array<SlotState, kMaxSlots> held{};
        // Fault injection: from this tick on the root this peer reports and keeps has the scene object's field flipped
        int flipFromTick = -1;

        // Set when a root from another peer disagreed with ours
        bool desync = false;
        std::uint32_t desyncTick = 0;
        std::uint32_t desyncPeer = 0;
    };

    struct Session
    {
        Session(const std::uint64_t seed, const SimLinkConfig &link, const std::uint32_t peers, const std::string &scene,
                const std::uint32_t simSeed, const int ticks, const Network::InputLayout *sessionLayout = nullptr)
            : network(seed), scene(scene), seed(simSeed), ticks(ticks), layout(sessionLayout)
        {
            for (std::uint32_t i = 0; i < peers; ++i)
            {
                transports.push_back(&network.AddEndpoint());
            }
            for (std::uint32_t i = 0; i < peers; ++i)
            {
                for (std::uint32_t j = i + 1; j < peers; ++j)
                {
                    EXPECT_TRUE(network.Link(transports[i]->Id(), transports[j]->Id(), link));
                }
            }
            LockstepConfig config;
            config.slotCount = peers;
            config.inputDelay = 2;
            config.layout = *Network::MakeLockstepLayoutInfo(layout != nullptr ? *layout : Network::LegacyMoveLayout());
            for (std::uint32_t i = 0; i < peers; ++i)
            {
                this->peers.push_back(std::make_unique<SessionPeer>(config, scene, simSeed, i, transports[i], ticks, layout));
            }
        }

        // The tree of the confirmed state at a checkpoint: object 0 is the scene (one leaf: its state hash), objects
        // 1..N are the slots (one leaf: the held input)
        static HashTree TreeFor(SessionPeer &peer, const std::uint32_t tick, const std::uint64_t sceneHash, const std::uint32_t slots)
        {
            HashTreeBuilder builder(tick);
            std::uint64_t hash = sceneHash;
            if (peer.flipFromTick >= 0 && static_cast<int>(tick) >= peer.flipFromTick)
            {
                hash ^= 1ull << 17;
            }
            EXPECT_TRUE(builder.AddField(0, 0, Bytes(hash)));
            for (std::uint32_t s = 0; s < slots; ++s)
            {
                const SlotState &state = peer.held[s];
                const std::array<std::uint8_t, 6> encoded{static_cast<std::uint8_t>(state.moveX), static_cast<std::uint8_t>(state.moveY),
                                                          static_cast<std::uint8_t>(state.buttons & 0xFF), static_cast<std::uint8_t>((state.buttons >> 8) & 0xFF),
                                                          static_cast<std::uint8_t>((state.buttons >> 16) & 0xFF), static_cast<std::uint8_t>(state.buttons >> 24)};
                EXPECT_TRUE(builder.AddField(1 + s, 0, encoded));
            }
            return *builder.Build();
        }

        void StepPeer(SessionPeer &peer, const int frame)
        {
            const auto count = static_cast<std::uint32_t>(peers.size());
            peer.transport->Poll(
                [&](const TransportEvent &event)
                {
                    if (event.type != TransportEventType::Datagram || event.peer < 1 || event.peer > count)
                    {
                        return;
                    }
                    const std::uint32_t from = event.peer - 1;
                    if (event.data.empty())
                    {
                        return;
                    }
                    if (event.data[0] == 1)
                    {
                        const auto bundle = Network::DecodeInputBundleFor(peer.layout != nullptr ? *peer.layout : Network::LegacyMoveLayout(), event.data);
                        if (!bundle || bundle->slot != from)
                        {
                            return;
                        }
                        peer.peerAck[from] = bundle->ackTick;
                        std::uint32_t tick = bundle->firstTick;
                        for (const LockstepInput &input : bundle->frames)
                        {
                            (void)peer.scheduler.Submit(from, tick, input);
                            ++tick;
                        }
                    }
                    else if (const auto root = DecodeRootMessage(event.data))
                    {
                        peer.peerRoots[from][root->tick] = root->root;
                    }
                });

            std::uint32_t assigned = 0;
            const std::uint32_t wanted = 2 + peer.localCount;
            if (peer.scheduler.SubmitLocal(peer.slot, InputFor(peer.layout, peer.slot, wanted), assigned) == SubmitResult::Accepted)
            {
                ++peer.localCount;
            }

            TickInputs out;
            while (peer.recorder.TicksRecorded() < peer.ticksWanted && peer.scheduler.TryAdvance(out) == AdvanceStatus::Runnable)
            {
                for (std::uint32_t s = 0; s < count; ++s)
                {
                    ASSERT_TRUE(SceneStateFromInput(peer.layout != nullptr ? *peer.layout : Network::LegacyMoveLayout(), out.inputs[s],
                                                    peer.held[s]));
                }
                std::string error;
                ASSERT_TRUE(peer.simulation->Step(std::span<const SlotState>(peer.held.data(), count), error)) << error;
                if (peer.layout != nullptr)
                {
                    ASSERT_TRUE(peer.recorder.RecordInputs(std::span<const LockstepInput>(out.inputs.data(), count)));
                }
                else
                {
                    ASSERT_TRUE(peer.recorder.RecordTick(std::span<const SlotState>(peer.held.data(), count)));
                }
                const int done = peer.simulation->TicksRun();
                if (done % kHashInterval == 0)
                {
                    const std::uint64_t hash = peer.simulation->Hash();
                    peer.liveHashes.push_back(ReplayHash{done, hash});
                    peer.history.Push(TreeFor(peer, static_cast<std::uint32_t>(done), hash, count));
                }
            }

            // The root exchange: the latest few checkpoints go to everyone every frame (a lost one is covered by the next)
            std::array<std::uint8_t, 1100> buffer{};
            for (std::uint32_t other = 0; other < count; ++other)
            {
                if (other == peer.slot)
                {
                    continue;
                }
                const InputBundle bundle = BuildBundleFor(peer.scheduler, peer.slot, peer.peerAck[other]);
                const std::size_t size = EncodeInputBundle(bundle, buffer);
                if (size > 0)
                {
                    (void)peer.transport->Send(transports[other]->Id(), std::span<const std::uint8_t>(buffer.data(), size));
                }
                const auto &trees = peer.history.Trees();
                for (std::size_t t = trees.size() > 2 ? trees.size() - 2 : 0; t < trees.size(); ++t)
                {
                    const std::size_t rootSize = EncodeRootMessage(RootMessage{trees[t].Tick(), trees[t].Root()}, buffer);
                    (void)peer.transport->Send(transports[other]->Id(), std::span<const std::uint8_t>(buffer.data(), rootSize));
                }
            }

            // Compare the roots we hold with our own trees
            for (const HashTree &tree : peer.history.Trees())
            {
                for (auto &[from, roots] : peer.peerRoots)
                {
                    const auto found = roots.find(tree.Tick());
                    if (found != roots.end() && found->second != tree.Root() && (!peer.desync || tree.Tick() < peer.desyncTick))
                    {
                        peer.desync = true;
                        peer.desyncTick = tree.Tick();
                        peer.desyncPeer = from;
                    }
                }
            }
            (void)frame;
        }

        // Runs until every peer has run `ticks` ticks (and, to let the last roots arrive, a few frames more)
        int Run(const int maxFrames)
        {
            int extra = 0;
            int frame = 0;
            for (; frame < maxFrames; ++frame)
            {
                network.Advance(16'000);
                bool done = true;
                for (auto &peer : peers)
                {
                    StepPeer(*peer, frame);
                    done = done && peer->recorder.TicksRecorded() >= ticks;
                }
                if (done && ++extra > 40)
                {
                    break;
                }
            }
            return frame;
        }

        SimNetwork network;
        std::vector<SimTransport *> transports;
        std::vector<std::unique_ptr<SessionPeer>> peers;
        std::string scene;
        std::uint32_t seed;
        int ticks;
        const Network::InputLayout *layout;
    };

    SimLinkConfig RoughLink()
    {
        SimLinkConfig link;
        link.latencyMicros = 25'000;
        link.jitterMicros = 35'000;
        link.lossPpm = 200'000;
        link.reorderPpm = 100'000;
        link.reorderDelayMicros = 50'000;
        link.duplicatePpm = 100'000;
        return link;
    }

    std::string TempPath(const std::string &name)
    {
        return (std::filesystem::temp_directory_path() / name).string();
    }
}

TEST(LockstepRecorder, CollapsesHeldInputsAndRoundTripsThroughTheFile)
{
    LockstepRecorder recorder("transforms", 77, 10, 2);
    const std::array<SlotState, 2> a{SlotState{1, 2, 3}, SlotState{}};
    const std::array<SlotState, 2> b{SlotState{1, 2, 3}, SlotState{-4, 0, 0}};
    ASSERT_TRUE(recorder.RecordTick(a));
    ASSERT_TRUE(recorder.RecordTick(a));
    ASSERT_TRUE(recorder.RecordTick(b));
    ASSERT_TRUE(recorder.RecordTick(b));
    const ReplayLog &log = recorder.Log();
    EXPECT_EQ(log.ticks, 4);
    ASSERT_EQ(log.inputs.size(), 2u) << "a row only where a slot's state changes";
    EXPECT_EQ(log.inputs[0], (ReplayInput{0, 0, 1, 2, 3}));
    EXPECT_EQ(log.inputs[1], (ReplayInput{2, 1, -4, 0, 0}));
    EXPECT_EQ(ValidateReplayLog(log), "");

    std::string error;
    const auto parsed = ParseReplayLog(SerializeReplayLog(log), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->inputs, log.inputs);
    EXPECT_EQ(parsed->ticks, 4);
    EXPECT_EQ(parsed->version, kReplayFormatVersion) << "the format did not change";
}

TEST(LockstepRecorder, RefusesTooManySlotsAndTooManyTicks)
{
    LockstepRecorder none("transforms", 1, 10, 0);
    EXPECT_FALSE(none.RecordTick({}));
    LockstepRecorder many("transforms", 1, 10, kMaxSlots + 1);
    EXPECT_FALSE(many.RecordTick({}));
    LockstepRecorder full("transforms", 1, 10, 1);
    const std::array<SlotState, 1> idle{};
    for (int i = 0; i < kMaxTicks; ++i)
    {
        ASSERT_TRUE(full.RecordTick(idle));
    }
    EXPECT_FALSE(full.RecordTick(idle));
    EXPECT_EQ(full.TicksRecorded(), kMaxTicks);
}

TEST(ReplaySimulation, SteppingByHandEqualsRunReplay)
{
    ReplayLog log;
    log.scene = "transforms";
    log.seed = 5;
    log.ticks = 90;
    log.hashInterval = 30;
    log.inputs = {ReplayInput{0, 0, 10, -3, 1}, ReplayInput{40, 0, 0, 0, 0}, ReplayInput{41, 1, 5, 5, 6}};
    const ReplayResult reference = RunReplay(log);
    ASSERT_TRUE(reference.ok) << reference.error;
    ASSERT_EQ(reference.hashes.size(), 3u);

    std::string error;
    auto simulation = ReplaySimulation::Create(log.scene, log.seed, error);
    ASSERT_TRUE(simulation) << error;
    std::array<SlotState, 2> held{};
    std::size_t hashes = 0;
    for (int tick = 0; tick < log.ticks; ++tick)
    {
        for (const ReplayInput &row : log.inputs)
        {
            if (row.tick == tick)
            {
                held[static_cast<std::size_t>(row.slot)] = SlotState{row.moveX, row.moveY, row.buttons};
            }
        }
        ASSERT_TRUE(simulation->Step(held, error)) << error;
        if ((tick + 1) % 30 == 0)
        {
            ASSERT_LT(hashes, reference.hashes.size());
            EXPECT_EQ(simulation->Hash(), reference.hashes[hashes].hash) << "tick " << tick + 1;
            ++hashes;
        }
    }
    EXPECT_EQ(simulation->TicksRun(), 90);
    std::string unknown;
    EXPECT_EQ(ReplaySimulation::Create("no such scene", 1, unknown), nullptr);
    EXPECT_FALSE(unknown.empty());
}

TEST(LockstepSession, RecordedSessionReplaysToIdenticalHashes)
{
    for (const std::string scene : {"transforms", "lua_sim", "lua_driven"})
    {
        constexpr int kTicks = 240;
        Session session(0xA11CE, RoughLink(), 3, scene, 4242, kTicks);
        session.Run(8000);
        for (auto &peer : session.peers)
        {
            ASSERT_EQ(peer->recorder.TicksRecorded(), kTicks) << scene << ": a peer did not finish";
            EXPECT_FALSE(peer->desync) << scene;
        }
        // Every peer saw the same hashes and recorded the same input log...
        const SessionPeer &first = *session.peers[0];
        ASSERT_EQ(first.liveHashes.size(), static_cast<std::size_t>(kTicks / kHashInterval));
        for (auto &peer : session.peers)
        {
            ASSERT_EQ(peer->liveHashes.size(), first.liveHashes.size());
            for (std::size_t i = 0; i < first.liveHashes.size(); ++i)
            {
                EXPECT_EQ(peer->liveHashes[i].hash, first.liveHashes[i].hash) << scene << " checkpoint " << i;
            }
            EXPECT_EQ(peer->recorder.Log().inputs, first.recorder.Log().inputs) << scene;
        }
        EXPECT_GT(first.recorder.Log().inputs.size(), 3u);

        // ...and replaying the file headless (through the text form, as a file would be) gives them again
        std::string error;
        const auto parsed = ParseReplayLog(SerializeReplayLog(first.recorder.Log()), error);
        ASSERT_TRUE(parsed.has_value()) << error;
        const ReplayResult replay = RunReplay(*parsed);
        ASSERT_TRUE(replay.ok) << scene << ": " << replay.error;
        ASSERT_EQ(replay.hashes.size(), first.liveHashes.size());
        for (std::size_t i = 0; i < replay.hashes.size(); ++i)
        {
            EXPECT_EQ(replay.hashes[i].tick, first.liveHashes[i].tick);
            EXPECT_EQ(replay.hashes[i].hash, first.liveHashes[i].hash) << scene << " replay vs live at tick " << replay.hashes[i].tick;
        }
    }
}

TEST(LockstepSession, AnInjectedDivergenceIsFoundAndReported)
{
    constexpr int kTicks = 210;
    Session session(0xBADC0DE, RoughLink(), 3, "transforms", 99, kTicks);
    session.peers[1]->flipFromTick = 120; // peer 1's reported state differs from the checkpoint at tick 120 on
    session.Run(8000);

    // Peers 0 and 2 disagree with peer 1 first at tick 120; peer 1 disagrees with them there too
    for (const auto &peer : session.peers)
    {
        ASSERT_TRUE(peer->desync) << "peer " << peer->slot;
        EXPECT_EQ(peer->desyncTick, 120u);
    }
    EXPECT_EQ(session.peers[0]->desyncPeer, 1u);
    EXPECT_EQ(session.peers[2]->desyncPeer, 1u);

    // Peer 0 bisects against peer 1's tree of that tick (here by direct access: the stand-in for the request/response)
    SessionPeer &reporter = *session.peers[0];
    SessionPeer &culprit = *session.peers[1];
    const HashTree *localTree = reporter.history.Find(120);
    const HashTree *remoteTree = culprit.history.Find(120);
    ASSERT_NE(localTree, nullptr);
    ASSERT_NE(remoteTree, nullptr);
    HashTree local = *localTree;
    HashTree remote = *remoteTree;
    const Divergence divergence = Bisect(local, remote);
    ASSERT_EQ(divergence.kind, Divergence::Kind::FieldDiffers);
    EXPECT_EQ(divergence.objectId, 0u) << "the scene object";
    EXPECT_EQ(divergence.fieldId, 0);

    DesyncReport report;
    report.peerName = "peer-0";
    report.tick = 120;
    report.localRoot = local.Root();
    report.remoteRoot = remote.Root();
    report.buildFingerprintJson = BuildFingerprintJson();
    report.determinismFingerprintJson = R"({"engineVersion":"test","sceneHash":"0x1"})";
    report.inputLog = reporter.recorder.Log();
    report.trees = &reporter.history.Trees();
    report.divergence = divergence;

    const std::string path = TempPath("n2_c3_desync_report_test.json");
    std::string error;
    ASSERT_TRUE(WriteDesyncReport(report, path, error)) << error;
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(path);

    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    ASSERT_FALSE(json.is_discarded());
    EXPECT_EQ(json["format"], "n2desync");
    EXPECT_EQ(json["version"], 1);
    EXPECT_EQ(json["tick"], 120);
    EXPECT_EQ(json["peer"], "peer-0");
    EXPECT_EQ(json["firstDivergence"]["kind"], "field-differs");
    EXPECT_EQ(json["firstDivergence"]["objectId"], 0);
    EXPECT_EQ(json["firstDivergence"]["fieldId"], 0);
    EXPECT_EQ(json["localRoot"], FormatHash(local.Root()));
    EXPECT_NE(json["localRoot"], json["remoteRoot"]);
    EXPECT_TRUE(json["build"].contains("compiler"));
    EXPECT_EQ(json["determinism"]["engineVersion"], "test");
    // The last K checkpoint trees, oldest first, ending at the newest
    ASSERT_EQ(json["trees"].size(), kHistory);
    EXPECT_LT(json["trees"][0]["tick"].get<int>(), json["trees"][kHistory - 1]["tick"].get<int>());
    EXPECT_EQ(json["trees"][kHistory - 1]["tick"], kTicks);
    EXPECT_EQ(json["trees"][0]["objects"][0]["id"], 0);

    // The input log in the report is a replay file from tick 0 that replays to the hashes the reporter saw
    const auto replay = ParseReplayLog(json["inputLog"].dump(), error);
    ASSERT_TRUE(replay.has_value()) << error;
    EXPECT_EQ(replay->ticks, kTicks);
    const ReplayResult result = RunReplay(*replay);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.hashes.size(), reporter.liveHashes.size());
    EXPECT_EQ(result.hashes.back().hash, reporter.liveHashes.back().hash);
}

TEST(LockstepSession, VersionTwoSessionUnderARichLayoutReplaysToIdenticalHashes)
{
    const Network::InputLayout layout = RichLayout();
    for (const std::string scene : {"transforms", "lua_driven"})
    {
        constexpr int kTicks = 240;
        Session session(0xC0DE5, RoughLink(), 3, scene, 777, kTicks, &layout);
        session.Run(8000);
        const SessionPeer &first = *session.peers[0];
        for (auto &peer : session.peers)
        {
            ASSERT_EQ(peer->recorder.TicksRecorded(), kTicks) << scene << ": a peer did not finish";
            EXPECT_FALSE(peer->desync) << scene;
            EXPECT_EQ(peer->recorder.Log().inputs, first.recorder.Log().inputs) << scene;
            ASSERT_EQ(peer->liveHashes.size(), first.liveHashes.size());
            for (std::size_t i = 0; i < first.liveHashes.size(); ++i)
            {
                EXPECT_EQ(peer->liveHashes[i].hash, first.liveHashes[i].hash) << scene << " checkpoint " << i;
            }
        }
        const ReplayLog &log = first.recorder.Log();
        EXPECT_EQ(log.version, kReplayFormatVersionLayout);
        ASSERT_GT(log.inputs.size(), 3u);

        std::string error;
        const auto parsed = ParseReplayLog(SerializeReplayLog(log), error);
        ASSERT_TRUE(parsed.has_value()) << error;
        EXPECT_EQ(parsed->inputs, log.inputs);
        const ReplayResult replay = RunReplay(*parsed);
        ASSERT_TRUE(replay.ok) << scene << ": " << replay.error;
        ASSERT_EQ(replay.hashes.size(), first.liveHashes.size());
        for (std::size_t i = 0; i < replay.hashes.size(); ++i)
        {
            EXPECT_EQ(replay.hashes[i].hash, first.liveHashes[i].hash) << scene << " replay vs live at tick " << replay.hashes[i].tick;
        }
    }
}

TEST(DesyncReport, UnwritablePathFailsWithAnError)
{
    DesyncReport report;
    std::string error;
    EXPECT_FALSE(WriteDesyncReport(report, TempPath("no-such-dir-n2/c3/report.json"), error));
    EXPECT_FALSE(error.empty());
}

TEST(DesyncReport, TruncatesHugeTreesAndAcceptsNoFingerprints)
{
    HashTreeBuilder builder(60);
    const std::array<std::uint8_t, 1> one{1};
    for (std::uint32_t id = 0; id < kMaxReportObjects + 10; ++id)
    {
        ASSERT_TRUE(builder.AddField(id, 0, one));
    }
    HashTreeHistory history(2);
    history.Push(*builder.Build());
    DesyncReport report;
    report.trees = &history.Trees();
    report.inputLog.scene = "transforms";
    report.inputLog.ticks = 1;
    const nlohmann::json json = nlohmann::json::parse(DesyncReportToJson(report));
    EXPECT_EQ(json["trees"][0]["objectCount"], kMaxReportObjects + 10);
    EXPECT_EQ(json["trees"][0]["truncated"], true);
    EXPECT_EQ(json["trees"][0]["objects"].size(), kMaxReportObjects);
    EXPECT_TRUE(json["build"].is_object());
    EXPECT_EQ(json["firstDivergence"]["kind"], "none");
}
