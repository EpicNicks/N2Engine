#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "RollbackTestSupport.hpp"
#include "engine/Application.hpp"
#include "engine/Time.hpp"
#include "engine/network/LockstepInputMapping.hpp"
#include "engine/network/LockstepProtocol.hpp"
#include "engine/network/LockstepRelay.hpp"
#include "engine/network/LockstepRuntime.hpp"
#include "engine/network/NetworkSettings.hpp"
#include "engine/network/RollbackRuntime.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/network/n2net/UdpTransport.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

// Netcode C5 remainder and C6: the relay-connected rollback runtime (LockstepRuntime in rollback mode) against the
// LockstepRelay on the simulator, next to lockstep peers; desync detection over confirmed ticks; spectators that start
// late from a player's serialised snapshot; the serialised snapshot itself and RestoreForJoin; the deterministic.mode
// switch; and RollbackRuntime driving Application's own fixed loop. The game is the Arena of RollbackTestSupport.hpp.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;
using namespace N2Engine::Network::Testing::Rollback;
using namespace N2Engine::N2Net;

namespace
{
    /// ScriptedInput's values, in the layout's order (fire, move x, move y), for a runtime's sampleInput
    std::vector<float> ScriptedValues(const std::uint32_t slot, const std::uint32_t tick)
    {
        const std::uint32_t phase = (tick + slot * 3) / 5;
        const float moveX = static_cast<float>((phase * 7 + slot * 3) % 9) / 4.0f - 1.0f;
        const float moveY = static_cast<float>((phase * 5 + slot) % 7) / 3.0f - 1.0f;
        const bool fire = (phase + slot) % 4 == 0 && (tick % 5) == 1;
        return {fire ? 1.0f : 0.0f, moveX, moveY};
    }

    std::string TempReport(const std::string &name)
    {
        return (std::filesystem::temp_directory_path() / ("n2_rollback_runtime_" + name + ".json")).string();
    }

    constexpr std::uint64_t kStepMicros = 2'000;
    constexpr std::uint32_t kHashInterval = 20;

    /// One peer: an Arena under a LockstepRuntime (lockstep, rollback or spectator)
    struct Peer
    {
        Peer(const std::uint32_t slots, const InputLayout &layout) : arena(slots, layout) {}

        Arena arena;
        RollbackEventQueue lockstepEvents;
        std::unique_ptr<LockstepRuntime> runtime;
        std::vector<LockstepEvent> events;
        /// The root after every checkpoint tick this peer ran on confirmed input only, as applyTick saw it (lockstep
        /// and spectators; a rollback peer's applyTick also runs guesses, so its confirmed roots are in History())
        std::map<std::uint32_t, std::uint64_t> confirmedRoots;
        bool frozen = false;
        std::int64_t divergeFrom = -1;
        std::string report;
    };

    struct RelaySession
    {
        RelaySession(const std::uint64_t seed, const std::uint32_t players, const SimLinkConfig &link,
                const std::function<void(LockstepRelayOptions &)> &tune = {})
            : network(seed), layout(MakeLayout()), slots(players), link(link)
        {
            relayTransport = &network.AddEndpoint();
            LockstepRelayOptions options;
            options.protocolId = 0x5151;
            options.slotCount = players;
            options.layout = layout;
            options.tickRate = 500;
            options.hashInterval = kHashInterval;
            options.stallTimeoutMicros = 2'000'000;
            options.desyncLingerMicros = 3'000'000;
            if (tune)
            {
                tune(options);
            }
            relay = std::make_unique<LockstepRelay>(*relayTransport, options);
        }

        std::uint64_t Now() const { return network.NowMicros(); }

        /// window: 0 lockstep, otherwise rollback; spectate: a spectator
        Peer &AddPeer(const std::uint32_t index, const std::uint32_t window, const bool spectate = false)
        {
            SimTransport &transport = network.AddEndpoint();
            EXPECT_TRUE(network.Link(relayTransport->Id(), transport.Id(), link));
            return AttachPeer(transport, index, window, spectate);
        }

        /// The same peer over any transport (a real UDP socket in the loopback test)
        Peer &AttachPeer(ITransport &transport, const std::uint32_t index, const std::uint32_t window, const bool spectate = false)
        {
            auto created = std::make_unique<Peer>(slots, layout);
            Peer &peer = *created;
            LockstepRuntimeOptions options;
            options.protocolId = 0x5151;
            options.layout = layout;
            options.peerName = "peer" + std::to_string(index);
            options.historyDepth = 64;
            options.desyncLingerMicros = 300'000;
            options.reportPath = TempReport(std::to_string(index) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
            peer.report = options.reportPath;
            options.observer = [&peer](const LockstepEvent &event) { peer.events.push_back(event); };
            options.sampleInput = [index](const std::uint32_t tick) { return ScriptedValues(index, tick); };
            options.rollbackWindow = window;
            options.spectate = spectate;
            Peer *const raw = &peer;
            options.applyTick = [raw, window](const TickInputs &inputs, std::uint32_t)
            {
                raw->arena.events = raw->runtime->Session() != nullptr ? &raw->runtime->Session()->Events() : &raw->lockstepEvents;
                if (!raw->arena.Step(inputs))
                {
                    return false;
                }
                if (window == 0 && (inputs.tick + 1) % kHashInterval == 0)
                {
                    raw->confirmedRoots[inputs.tick + 1] = raw->arena.Root(inputs.tick + 1);
                }
                return true;
            };
            options.buildTree = [raw](const std::uint32_t tick) -> std::optional<HashTree>
            {
                auto tree = BuildStateHashTree(raw->arena.world, raw->arena.rollback.Layouts(), tick);
                if (raw->divergeFrom >= 0 && static_cast<std::int64_t>(tick) >= raw->divergeFrom)
                {
                    // A perturbed copy: the same objects and one more field
                    HashTreeBuilder builder(tick);
                    EXPECT_TRUE(builder.AddObject(9999));
                    const std::array<std::uint8_t, 1> byte{1};
                    EXPECT_TRUE(builder.AddField(9999, 0, byte));
                    return builder.Build();
                }
                return tree;
            };
            options.save = [raw](const std::uint32_t tick, WorldSnapshot &out) { return raw->arena.rollback.Save(tick, out); };
            options.restore = [raw](const WorldSnapshot &snapshot) { return raw->arena.rollback.Restore(snapshot); };
            options.confirmed = [raw](const std::uint32_t first) { (void)raw->arena.rollback.ReleaseConfirmed(first); };
            options.resimulated = [raw] { (void)raw->arena.rollback.ReleasePool(); };
            options.join = [raw](const WorldSnapshot &snapshot) -> std::expected<void, std::string>
            {
                const auto joined = raw->arena.rollback.RestoreForJoin(snapshot);
                if (!joined)
                {
                    return std::unexpected(joined.error());
                }
                return {};
            };
            peer.runtime = std::make_unique<LockstepRuntime>(transport, std::move(options));
            peers.push_back(std::move(created));
            return peer;
        }

        void Step()
        {
            network.Advance(kStepMicros);
            relay->Update(Now());
            for (auto &peer : peers)
            {
                if (!peer->frozen)
                {
                    peer->runtime->Update(Now(), static_cast<double>(kStepMicros) / 1e6);
                }
            }
        }

        bool RunUntil(const std::function<bool()> &done, const int maxSteps = 60000)
        {
            for (int i = 0; i < maxSteps; ++i)
            {
                if (done())
                {
                    return true;
                }
                Step();
            }
            return done();
        }

        SimNetwork network;
        InputLayout layout;
        std::uint32_t slots;
        SimLinkConfig link;
        SimTransport *relayTransport = nullptr;
        std::unique_ptr<LockstepRelay> relay;
        std::vector<std::unique_ptr<Peer>> peers;
    };

    SimLinkConfig LateLink()
    {
        SimLinkConfig link;
        link.latencyMicros = 7'000;
        link.jitterMicros = 6'000;
        link.lossPpm = 60'000;
        link.reorderPpm = 60'000;
        link.reorderDelayMicros = 8'000;
        link.duplicatePpm = 30'000;
        return link;
    }

    /// Every checkpoint of a's history is in b's with the same root; the number compared
    std::size_t CompareHistories(const LockstepRuntime &a, const LockstepRuntime &b)
    {
        std::size_t compared = 0;
        for (const HashTree &tree : a.History().Trees())
        {
            if (const HashTree *const other = b.History().Find(tree.Tick()); other != nullptr)
            {
                EXPECT_EQ(tree.Root(), other->Root()) << "tick " << tree.Tick();
                ++compared;
            }
        }
        return compared;
    }
}

// ===== The codecs of wire version 2 =====

TEST(RollbackRuntimeProtocolTest, SpectateSnapshotRequestAndSnapshotPartsRoundTripAndRefuseWhatIsWrong)
{
    EXPECT_EQ(kLockstepWireVersion, 2u);
    const JoinMessage join{kLockstepWireVersion, 3, 0x1122334455667788ull};
    const auto spectate = EncodeSpectate(join);
    ASSERT_EQ(spectate.size(), 12u);
    EXPECT_EQ(spectate[0], static_cast<std::uint8_t>(RelayMessage::Spectate));
    const auto decoded = DecodeSpectate(spectate);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->payloadBytes, 3u);
    EXPECT_EQ(decoded->layoutHash, join.layoutHash);
    EXPECT_FALSE(DecodeSpectate(Encode(join)).has_value()) << "a Join is not a Spectate";
    EXPECT_FALSE(DecodeJoin(spectate).has_value());

    const auto request = EncodeSnapshotRequest(77);
    ASSERT_TRUE(DecodeSnapshotRequest(request).has_value());
    EXPECT_EQ(*DecodeSnapshotRequest(request), 77u);
    EXPECT_FALSE(DecodeSnapshotRequest(std::span<const std::uint8_t>(request.data(), 4)).has_value());

    const std::vector<std::uint8_t> data(100, 0xAB);
    SnapshotPartMessage part;
    part.requestId = 9;
    part.tick = 1234;
    part.totalBytes = 250;
    part.offset = 150;
    part.data = data;
    const auto encoded = Encode(RelayMessage::SnapshotData, part);
    ASSERT_EQ(encoded.size(), 117u);
    const auto back = DecodeSnapshotPart(RelayMessage::SnapshotData, encoded);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->requestId, 9u);
    EXPECT_EQ(back->tick, 1234u);
    EXPECT_EQ(back->totalBytes, 250u);
    EXPECT_EQ(back->offset, 150u);
    EXPECT_EQ(back->data.size(), 100u);
    EXPECT_FALSE(DecodeSnapshotPart(RelayMessage::SnapshotPart, encoded).has_value()) << "the type is checked";
    // Past the total, empty, or too large: refused both ways
    part.offset = 151;
    EXPECT_TRUE(Encode(RelayMessage::SnapshotData, part).empty());
    std::vector<std::uint8_t> pastTotal = encoded;
    pastTotal[13] = 151; // offset low byte
    EXPECT_FALSE(DecodeSnapshotPart(RelayMessage::SnapshotData, pastTotal).has_value());
    part.offset = 0;
    part.data = {};
    EXPECT_TRUE(Encode(RelayMessage::SnapshotData, part).empty());
    const std::vector<std::uint8_t> huge(kMaxSnapshotPartBytes + 1, 1);
    part.data = huge;
    part.totalBytes = static_cast<std::uint32_t>(huge.size());
    EXPECT_TRUE(Encode(RelayMessage::SnapshotData, part).empty());
    EXPECT_FALSE(DecodeSnapshotPart(RelayMessage::SnapshotData, std::span<const std::uint8_t>(encoded.data(), 17)).has_value());

    // A spectator's welcome carries kSpectatorSlot; any other slot past the count is still refused
    WelcomeMessage welcome;
    welcome.slot = kSpectatorSlot;
    welcome.slotCount = 2;
    welcome.payloadBytes = 1;
    welcome.neutral = {0};
    ASSERT_TRUE(DecodeWelcome(Encode(welcome)).has_value());
    EXPECT_EQ(DecodeWelcome(Encode(welcome))->slot, kSpectatorSlot);
    welcome.slot = 2;
    EXPECT_FALSE(DecodeWelcome(Encode(welcome)).has_value());
    EXPECT_EQ(DecodeRefuse(EncodeRefuse(RefuseReason::NoSnapshot)), RefuseReason::NoSnapshot);
    EXPECT_FALSE(DecodeRefuse(std::vector<std::uint8_t>{0x44, 7}).has_value());
}

// ===== The deterministic.mode switch =====

TEST(RollbackRuntimeSettingsTest, OnlyTheDeterministicPresetInRollbackModeGetsAWindow)
{
    NetworkSettings settings;
    EXPECT_EQ(RollbackWindowFor(settings), 0u) << "preset none";
    settings.preset = NetworkPreset::Deterministic;
    EXPECT_EQ(RollbackWindowFor(settings), 0u) << "lockstep mode is window 0";
    settings.deterministic.mode = DeterministicMode::Rollback;
    EXPECT_EQ(RollbackWindowFor(settings), 8u);
    settings.deterministic.rollbackWindow = 3;
    EXPECT_EQ(RollbackWindowFor(settings), 3u);
    settings.deterministic.rollbackWindow = 40;
    EXPECT_EQ(RollbackWindowFor(settings), 15u) << "clamped to the session's bound";
    settings.deterministic.rollbackWindow = 0;
    EXPECT_EQ(RollbackWindowFor(settings), 0u) << "rollback with window 0 is lockstep";
    settings.deterministic.rollbackWindow = 8;
    settings.preset = NetworkPreset::Authoritative;
    EXPECT_EQ(RollbackWindowFor(settings), 0u);
}

// ===== The serialised snapshot and RestoreForJoin =====

TEST(RollbackRuntimeSnapshotTest, ASerialisedSnapshotRoundTripsExactlyAndCorruptionIsRefused)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    Arena arena(2, layout);
    RollbackEventQueue events;
    arena.events = &events;
    // Run until some shots are alive (at least 30 ticks)
    std::uint32_t at = 0;
    for (; at < 30 || (arena.world.Count() <= 2 && at < 400); ++at)
    {
        TickInputs inputs;
        inputs.tick = at;
        inputs.inputs[0] = ScriptedInput(layout, 0, at);
        inputs.inputs[1] = ScriptedInput(layout, 1, at);
        ASSERT_TRUE(arena.Step(inputs));
    }
    WorldSnapshot snapshot;
    ASSERT_TRUE(arena.rollback.Save(at, snapshot));
    ASSERT_GT(snapshot.objects.size(), 2u) << "some shots are alive";
    const std::vector<std::uint8_t> bytes = SerializeWorldSnapshot(snapshot);
    const auto back = DeserializeWorldSnapshot(bytes);
    ASSERT_TRUE(back.has_value()) << back.error();
    EXPECT_EQ(back->tick, at);
    EXPECT_EQ(back->nextId, snapshot.nextId);
    EXPECT_EQ(back->rng, snapshot.rng);
    EXPECT_EQ(back->streams, snapshot.streams);
    ASSERT_EQ(back->objects.size(), snapshot.objects.size());
    for (std::size_t i = 0; i < snapshot.objects.size(); ++i)
    {
        EXPECT_EQ(back->objects[i].id, snapshot.objects[i].id);
        EXPECT_EQ(back->objects[i].prefab, snapshot.objects[i].prefab);
        EXPECT_EQ(back->objects[i].owner, snapshot.objects[i].owner);
        EXPECT_EQ(back->objects[i].spawned, snapshot.objects[i].spawned);
        EXPECT_EQ(back->objects[i].values.layoutHash, snapshot.objects[i].values.layoutHash);
        ASSERT_EQ(back->objects[i].values.fields.size(), snapshot.objects[i].values.fields.size());
        for (std::size_t f = 0; f < snapshot.objects[i].values.fields.size(); ++f)
        {
            EXPECT_EQ(back->objects[i].values.fields[f], snapshot.objects[i].values.fields[f]);
        }
    }
    EXPECT_EQ(SerializeWorldSnapshot(*back), bytes) << "the form is canonical";

    // Restoring the decoded copy is the same as restoring the original
    const std::string atSave = arena.Dump();
    const std::uint64_t rootSave = arena.Root(at);
    for (std::uint32_t tick = at; tick < at + 10; ++tick)
    {
        TickInputs inputs;
        inputs.tick = tick;
        inputs.inputs[0] = ScriptedInput(layout, 0, tick);
        inputs.inputs[1] = ScriptedInput(layout, 1, tick);
        ASSERT_TRUE(arena.Step(inputs));
    }
    ASSERT_TRUE(arena.rollback.Restore(*back).has_value());
    EXPECT_EQ(arena.Dump(), atSave);
    EXPECT_EQ(arena.Root(at), rootSave);

    std::vector<std::uint8_t> flipped = bytes;
    flipped[flipped.size() / 2] ^= 0x10;
    EXPECT_FALSE(DeserializeWorldSnapshot(flipped).has_value()) << "the checksum";
    EXPECT_FALSE(DeserializeWorldSnapshot(std::span<const std::uint8_t>(bytes.data(), bytes.size() - 1)).has_value());
    EXPECT_FALSE(DeserializeWorldSnapshot(std::span<const std::uint8_t>(bytes.data(), 10)).has_value());
}

TEST(RollbackRuntimeSnapshotTest, ALateJoinerRestoresAnotherPeersSnapshotAndRunsOnInStep)
{
    ShotPrefab prefab;
    const InputLayout layout = MakeLayout();
    const auto inputsAt = [&](const std::uint32_t tick)
    {
        TickInputs inputs;
        inputs.tick = tick;
        inputs.inputs[0] = ScriptedInput(layout, 0, tick);
        inputs.inputs[1] = ScriptedInput(layout, 1, tick);
        return inputs;
    };
    Arena first(2, layout);
    RollbackEventQueue firstEvents;
    first.events = &firstEvents;
    for (std::uint32_t tick = 0; tick < 47; ++tick)
    {
        ASSERT_TRUE(first.Step(inputsAt(tick)));
    }
    WorldSnapshot snapshot;
    ASSERT_TRUE(first.rollback.Save(47, snapshot));
    const std::vector<std::uint8_t> bytes = SerializeWorldSnapshot(snapshot);

    // The joiner has the scene as loaded: the avatars, no shots, a fresh id counter and random stream
    Arena joiner(2, layout);
    RollbackEventQueue joinerEvents;
    joiner.events = &joinerEvents;
    const auto decoded = DeserializeWorldSnapshot(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error();
    const auto joined = joiner.rollback.RestoreForJoin(*decoded);
    ASSERT_TRUE(joined.has_value()) << joined.error();
    EXPECT_GT(joined->spawned, 0u) << "the live shots were instantiated under their ids";
    EXPECT_EQ(joiner.Dump(), first.Dump());
    EXPECT_EQ(joiner.Root(47), first.Root(47));

    for (std::uint32_t tick = 47; tick < 120; ++tick)
    {
        ASSERT_TRUE(first.Step(inputsAt(tick)));
        ASSERT_TRUE(joiner.Step(inputsAt(tick)));
        ASSERT_EQ(joiner.Root(tick + 1), first.Root(tick + 1)) << "tick " << tick;
    }
    EXPECT_EQ(joiner.Dump(), first.Dump());

    // A snapshot of a scene object the joiner does not have is refused
    WorldSnapshot alien = snapshot;
    WorldSnapshot::Object ghost;
    ghost.id = 999;
    ghost.spawned = false;
    alien.objects.push_back(ghost);
    Arena other(2, layout);
    EXPECT_FALSE(other.rollback.RestoreForJoin(alien).has_value());
}

// ===== The relay-connected rollback runtime =====

TEST(RollbackRuntimeRelayTest, RollbackAndLockstepPeersShareOneRelaySessionAndAgreeOnEveryConfirmedCheckpoint)
{
    ShotPrefab prefab;
    RelaySession session(31, 3, LateLink());
    Peer &rollback8 = session.AddPeer(0, 8);
    Peer &rollback4 = session.AddPeer(1, 4);
    Peer &lockstep = session.AddPeer(2, 0);
    ASSERT_TRUE(session.RunUntil(
        [&]
        {
            return rollback8.runtime->ConfirmedTick() >= 400 && rollback4.runtime->ConfirmedTick() >= 400 &&
                   lockstep.runtime->ConfirmedTick() >= 400;
        }));
    ASSERT_EQ(rollback8.runtime->State(), LockstepState::Running) << rollback8.runtime->Error();
    ASSERT_EQ(rollback4.runtime->State(), LockstepState::Running) << rollback4.runtime->Error();
    ASSERT_EQ(lockstep.runtime->State(), LockstepState::Running) << lockstep.runtime->Error();
    ASSERT_NE(rollback8.runtime->Session(), nullptr);
    ASSERT_NE(rollback4.runtime->Session(), nullptr);
    EXPECT_EQ(lockstep.runtime->Session(), nullptr);

    // Rollbacks happened: the guesses were wrong some of the time, and the peers ran ahead of the confirmed tick
    EXPECT_GT(rollback8.runtime->Session()->Stats().rollbacks, 0u);
    EXPECT_GT(rollback8.runtime->Session()->Stats().ticksPredicted, 0u);
    EXPECT_GT(rollback4.runtime->Session()->Stats().rollbacks, 0u);

    // The relay compared every peer's roots and found no difference; each rollback peer reported confirmed ticks only
    EXPECT_EQ(session.relay->Stats().desyncs, 0u);
    EXPECT_GE(session.relay->Stats().rootsReceived, 3u * (400u / kHashInterval) - 6u);
    EXPECT_GE(CompareHistories(*rollback8.runtime, *lockstep.runtime), 10u);
    EXPECT_GE(CompareHistories(*rollback4.runtime, *lockstep.runtime), 10u);
    // And the lockstep peer's own record of its confirmed roots matches what the rollback peers published
    std::size_t matched = 0;
    for (const HashTree &tree : rollback8.runtime->History().Trees())
    {
        if (const auto found = lockstep.confirmedRoots.find(tree.Tick()); found != lockstep.confirmedRoots.end())
        {
            EXPECT_EQ(found->second, tree.Root()) << "tick " << tree.Tick();
            ++matched;
        }
    }
    EXPECT_GE(matched, 10u);
    // The replay of a rollback peer is the confirmed input stream, as a lockstep peer's
    ASSERT_NE(rollback8.runtime->Log(), nullptr);
    EXPECT_GE(rollback8.runtime->Log()->ticks, 400);
}

TEST(RollbackRuntimeRelayTest, ADivergenceUnderRollbackIsCaughtAtTheFirstConfirmedCheckpointAndStopsTheSession)
{
    ShotPrefab prefab;
    RelaySession session(32, 2, LateLink());
    Peer &first = session.AddPeer(0, 8);
    Peer &second = session.AddPeer(1, 8);
    second.divergeFrom = 60;
    ASSERT_TRUE(session.RunUntil([&] { return first.runtime->Finished() && second.runtime->Finished(); }));
    EXPECT_EQ(session.relay->Stats().desyncs, 1u);
    for (Peer *peer : {&first, &second})
    {
        const auto &stop = peer->runtime->StopInfo();
        ASSERT_TRUE(stop.has_value());
        EXPECT_EQ(stop->reason, StopReason::Desync);
        EXPECT_EQ(stop->tick, 60u) << "the first checkpoint at or after the divergence, once confirmed";
        EXPECT_TRUE(stop->reportWritten) << stop->reportError;
        std::error_code ignored;
        std::filesystem::remove(peer->report, ignored);
    }
}

TEST(RollbackRuntimeRelayTest, ASpectatorJoinsLateFromAPlayersSnapshotAndRunsTheConfirmedTicksInStep)
{
    ShotPrefab prefab;
    RelaySession session(33, 2, LateLink());
    Peer &player = session.AddPeer(0, 8); // the lowest slot: the relay asks it, a rollback peer, for the snapshot
    // Seated first, so it is slot 0 (slots go by the order of the joins)
    ASSERT_TRUE(session.RunUntil([&] { return player.runtime->State() == LockstepState::Lobby; }, 20000));
    ASSERT_EQ(player.runtime->Slot(), 0u);
    Peer &lockstep = session.AddPeer(1, 0);
    ASSERT_TRUE(session.RunUntil([&] { return lockstep.runtime->ConfirmedTick() >= 150; }));

    Peer &spectator = session.AddPeer(2, 0, true);
    ASSERT_TRUE(session.RunUntil(
        [&] { return spectator.runtime->TicksRun() >= 500 && lockstep.runtime->TicksRun() >= 500; }));
    ASSERT_EQ(spectator.runtime->State(), LockstepState::Running) << spectator.runtime->Error();
    EXPECT_TRUE(spectator.runtime->IsSpectator());
    EXPECT_EQ(spectator.runtime->Slot(), kSpectatorSlot);
    EXPECT_EQ(session.relay->Stats().spectatorsJoined, 1u);
    EXPECT_EQ(session.relay->Stats().snapshotsRelayed, 1u);
    EXPECT_EQ(player.runtime->Stats().snapshotsSent, 1u);
    EXPECT_EQ(session.relay->JoinedCount(), 2u) << "a spectator takes no slot";

    std::uint32_t startTick = 0;
    for (const LockstepEvent &event : spectator.events)
    {
        if (event.type == LockstepEvent::Type::Started)
        {
            startTick = event.tick;
        }
    }
    EXPECT_GE(startTick, 150u) << "it started from the snapshot, not from tick 0";

    // Every checkpoint the spectator ran is the lockstep player's
    std::size_t compared = 0;
    for (const auto &[tick, root] : spectator.confirmedRoots)
    {
        EXPECT_GT(tick, startTick);
        const auto found = lockstep.confirmedRoots.find(tick);
        if (found != lockstep.confirmedRoots.end())
        {
            EXPECT_EQ(found->second, root) << "tick " << tick;
            ++compared;
        }
    }
    EXPECT_GE(compared, 10u);
    EXPECT_EQ(session.relay->Stats().desyncs, 0u);
}

TEST(RollbackRuntimeRelayTest, ASpectatorIsRefusedWhenTheRelayTakesNone)
{
    ShotPrefab prefab;
    RelaySession session(34, 1, SimLinkConfig{}, [](LockstepRelayOptions &options) { options.maxSpectators = 0; });
    session.AddPeer(0, 0);
    Peer &spectator = session.AddPeer(1, 0, true);
    ASSERT_TRUE(session.RunUntil([&] { return spectator.runtime->Finished(); }, 20000));
    EXPECT_EQ(spectator.runtime->State(), LockstepState::Refused);
    EXPECT_EQ(spectator.runtime->Refusal(), RefuseReason::SessionFull);
}

// ===== C5 follow-up coverage: more peers, a leaving snapshot source, long sessions, real sockets =====

TEST(RollbackRuntimeFollowUpTest, FourPlayersOfMixedModesAndTwoSpectatorsAllAgreeOnEveryConfirmedCheckpoint)
{
    ShotPrefab prefab;
    RelaySession session(51, 4, LateLink());
    Peer &rollback8 = session.AddPeer(0, 8);
    Peer &rollback3 = session.AddPeer(1, 3);
    Peer &rollback15 = session.AddPeer(2, 15);
    Peer &lockstep = session.AddPeer(3, 0);
    Peer &early = session.AddPeer(4, 0, true); // a spectator that asks before the session has started
    ASSERT_TRUE(session.RunUntil(
        [&]
        {
            return rollback8.runtime->ConfirmedTick() >= 300 && rollback3.runtime->ConfirmedTick() >= 300 &&
                   rollback15.runtime->ConfirmedTick() >= 300 && lockstep.runtime->ConfirmedTick() >= 300;
        }));
    Peer &late = session.AddPeer(5, 0, true);
    ASSERT_TRUE(session.RunUntil(
        [&]
        {
            return early.runtime->TicksRun() >= 450 && late.runtime->TicksRun() >= 450 && lockstep.runtime->TicksRun() >= 450;
        }));
    for (Peer *peer : {&rollback8, &rollback3, &rollback15, &lockstep, &early, &late})
    {
        ASSERT_EQ(peer->runtime->State(), LockstepState::Running) << peer->runtime->Error();
    }
    EXPECT_EQ(session.relay->JoinedCount(), 4u);
    EXPECT_EQ(session.relay->SpectatorCount(), 2u);
    EXPECT_EQ(session.relay->Stats().desyncs, 0u);
    EXPECT_GE(session.relay->Stats().rootsReceived, 4u * (300u / kHashInterval) - 8u);
    for (Peer *peer : {&rollback8, &rollback3, &rollback15})
    {
        EXPECT_GT(peer->runtime->Session()->Stats().rollbacks, 0u);
        EXPECT_GE(CompareHistories(*peer->runtime, *lockstep.runtime), 8u);
    }
    for (Peer *spectator : {&early, &late})
    {
        std::size_t compared = 0;
        for (const auto &[tick, root] : spectator->confirmedRoots)
        {
            if (const auto found = lockstep.confirmedRoots.find(tick); found != lockstep.confirmedRoots.end())
            {
                EXPECT_EQ(found->second, root) << "tick " << tick;
                ++compared;
            }
        }
        EXPECT_GE(compared, 5u);
    }
}

TEST(RollbackRuntimeFollowUpTest, ThePlayerAskedForASnapshotLeavingAtAnyPointStillGivesTheSpectatorItsView)
{
    // The relay asks the lowest live slot; it leaves between the request and the end of the transfer (the delay in
    // simulator steps after the request is swept). The request must go to another player and the spectator must run on.
    for (const std::uint32_t delay : {0u, 1u, 2u, 4u, 9u, 20u, 60u})
    {
        SCOPED_TRACE("delay " + std::to_string(delay));
        ShotPrefab prefab;
        RelaySession session(60 + delay, 3, LateLink());
        Peer &source = session.AddPeer(0, 8);
        ASSERT_TRUE(session.RunUntil([&] { return source.runtime->State() == LockstepState::Lobby; }, 20000));
        ASSERT_EQ(source.runtime->Slot(), 0u);
        Peer &lockstep = session.AddPeer(1, 0);
        Peer &other = session.AddPeer(2, 6);
        ASSERT_TRUE(session.RunUntil([&] { return lockstep.runtime->ConfirmedTick() >= 120; }));
        Peer &spectator = session.AddPeer(3, 0, true);
        ASSERT_TRUE(session.RunUntil([&] { return session.relay->Stats().snapshotRequests >= 1; }, 20000));
        for (std::uint32_t i = 0; i < delay; ++i)
        {
            session.Step();
        }
        source.runtime->Leave();
        source.frozen = true;
        ASSERT_TRUE(session.RunUntil([&] { return spectator.runtime->TicksRun() >= 400 && lockstep.runtime->TicksRun() >= 400; }))
            << "state " << ToString(spectator.runtime->State()) << " " << spectator.runtime->Error();
        EXPECT_EQ(spectator.runtime->State(), LockstepState::Running);
        EXPECT_EQ(lockstep.runtime->State(), LockstepState::Running);
        EXPECT_EQ(other.runtime->State(), LockstepState::Running);
        EXPECT_EQ(session.relay->Stats().desyncs, 0u);
        EXPECT_EQ(session.relay->SpectatorCount(), 1u);
        std::size_t compared = 0;
        for (const auto &[tick, root] : spectator.confirmedRoots)
        {
            if (const auto found = lockstep.confirmedRoots.find(tick); found != lockstep.confirmedRoots.end())
            {
                EXPECT_EQ(found->second, root) << "tick " << tick;
                ++compared;
            }
        }
        EXPECT_GE(compared, 5u);
    }
}

TEST(RollbackRuntimeFollowUpTest, ALongSessionDoesNotDriftAndItsRingsAndPoolsStayBounded)
{
    ShotPrefab prefab;
    SimLinkConfig link;
    link.latencyMicros = 4'000;
    link.jitterMicros = 3'000;
    link.lossPpm = 20'000;
    link.reorderPpm = 20'000;
    link.reorderDelayMicros = 4'000;
    RelaySession session(71, 3, link);
    Peer &rollback8 = session.AddPeer(0, 8);
    Peer &rollback4 = session.AddPeer(1, 4);
    Peer &lockstep = session.AddPeer(2, 0);
    constexpr std::uint32_t kTicks = 5000;
    std::size_t maxParked = 0;
    std::size_t maxObjects = 0;
    std::uint32_t maxAhead = 0;
    std::size_t maxTrees = 0;
    for (int step = 0; step < 120000 && lockstep.runtime->ConfirmedTick() < kTicks; ++step)
    {
        session.Step();
        for (Peer *peer : {&rollback8, &rollback4})
        {
            maxParked = std::max(maxParked, peer->arena.world.Parked().size());
            maxObjects = std::max(maxObjects, peer->arena.world.Count());
            maxAhead = std::max(maxAhead, peer->runtime->TicksRun() - peer->runtime->ConfirmedTick());
            maxTrees = std::max(maxTrees, peer->runtime->History().Trees().size());
        }
    }
    ASSERT_GE(lockstep.runtime->ConfirmedTick(), kTicks);
    ASSERT_EQ(rollback8.runtime->State(), LockstepState::Running) << rollback8.runtime->Error();
    ASSERT_EQ(rollback4.runtime->State(), LockstepState::Running) << rollback4.runtime->Error();
    EXPECT_EQ(session.relay->Stats().desyncs, 0u) << "no drift in 5000 ticks";
    EXPECT_GE(session.relay->Stats().rootsReceived, 3u * (kTicks / kHashInterval) - 12u);
    EXPECT_GE(CompareHistories(*rollback8.runtime, *lockstep.runtime), 30u);
    EXPECT_GE(CompareHistories(*rollback4.runtime, *lockstep.runtime), 30u);
    EXPECT_GT(rollback8.runtime->Session()->Stats().rollbacks, 100u) << "the run really rolled back, many times";
    // Bounded: how far past the confirmed tick a peer runs (the window), the live and the parked objects, the history
    EXPECT_LE(maxAhead, 9u + 1u);
    EXPECT_LE(maxObjects, 40u);
    EXPECT_LE(maxParked, 64u) << "released as the snapshots that need them leave the ring";
    EXPECT_LE(maxTrees, 64u) << "historyDepth";
    for (Peer *peer : {&rollback8, &rollback4})
    {
        // Nothing from before the ring can be restored to
        EXPECT_EQ(peer->runtime->Session()->SnapshotAt(0), nullptr);
        EXPECT_LE(peer->arena.world.Parked().size(), 64u);
    }
}

TEST(RollbackRuntimeFollowUpTest, TwoRollbackPeersAgreeOverRealLoopbackUdpSockets)
{
    ShotPrefab prefab;
    RelaySession session(81, 2, SimLinkConfig{}); // only its peer factory is used; the relay below is the real one
    UdpTransportOptions relayOpen;
    relayOpen.acceptUnknownPeers = true;
    UdpOpenResult relayResult = UdpTransport::Open(relayOpen, false);
    if (relayResult.error != UdpError::None)
    {
        GTEST_SKIP() << "no loopback UDP socket here: " << relayResult.message;
    }
    UdpTransportOptions clientOpen;
    clientOpen.acceptUnknownPeers = false;
    UdpOpenResult aResult = UdpTransport::Open(clientOpen, false);
    UdpOpenResult bResult = UdpTransport::Open(clientOpen, false);
    ASSERT_EQ(aResult.error, UdpError::None) << aResult.message;
    ASSERT_EQ(bResult.error, UdpError::None) << bResult.message;
    LockstepRelayOptions relayOptions;
    relayOptions.protocolId = 0x5151;
    relayOptions.slotCount = 2;
    relayOptions.layout = session.layout;
    relayOptions.tickRate = 500;
    relayOptions.hashInterval = kHashInterval;
    LockstepRelay relay(*relayResult.transport, relayOptions);
    ASSERT_TRUE(relay.Valid());
    Peer &a = session.AttachPeer(*aResult.transport, 0, 8);
    Peer &b = session.AttachPeer(*bResult.transport, 1, 5);
    ASSERT_EQ(aResult.transport->Connect("127.0.0.1", relayResult.transport->LocalPort()).error, UdpError::None);
    ASSERT_EQ(bResult.transport->Connect("127.0.0.1", relayResult.transport->LocalPort()).error, UdpError::None);

    const auto start = std::chrono::steady_clock::now();
    auto last = start;
    const auto deadline = start + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline && (a.runtime->ConfirmedTick() < 400 || b.runtime->ConfirmedTick() < 400))
    {
        const auto now = std::chrono::steady_clock::now();
        const auto micros = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - start).count());
        const double delta = std::chrono::duration<double>(now - last).count();
        last = now;
        relay.Update(micros);
        a.runtime->Update(micros, delta);
        b.runtime->Update(micros, delta);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(a.runtime->State(), LockstepState::Running) << a.runtime->Error();
    ASSERT_EQ(b.runtime->State(), LockstepState::Running) << b.runtime->Error();
    ASSERT_GE(a.runtime->ConfirmedTick(), 400u);
    ASSERT_GE(b.runtime->ConfirmedTick(), 400u);
    EXPECT_EQ(relay.Stats().desyncs, 0u);
    EXPECT_GE(CompareHistories(*a.runtime, *b.runtime), 15u);
}

// ===== RollbackRuntime in Application's fixed loop =====

namespace
{
    /// Records, in every FixedUpdate, whether Time::GetTick() is the session tick + 1
    class TickProbe final : public Component
    {
    public:
        explicit TickProbe(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "RbRuntimeTickProbe"; }
        void OnFixedUpdate() override
        {
            ++fixedUpdates;
            if (Time::GetTick() != static_cast<std::uint64_t>(sessionTick) + 1)
            {
                ++mismatches;
            }
        }
        static inline std::uint32_t sessionTick = 0;
        static inline int fixedUpdates = 0;
        static inline int mismatches = 0;
    };

    /// A scene in SceneManager (the current one) with two avatars and the probe, bound to a NetworkWorld
    struct CurrentSceneGame
    {
        CurrentSceneGame()
        {
            RegisterTestComponents();
            SceneManager::AddScene(Scene::Create("RbRuntimeGlue_" + std::to_string(++counter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Scene &scene = SceneManager::GetCurSceneRef();
            for (std::uint32_t slot = 0; slot < 2; ++slot)
            {
                const GameObject::Ptr avatar = GameObject::Create("Avatar" + std::to_string(slot));
                avatar->SetUUID(TestUuid(10 + slot * 10));
                avatar->AddComponent<NetworkObject>();
                avatar->AddComponent<Avatar>();
                scene.AddRootGameObject(avatar);
            }
            const GameObject::Ptr probe = GameObject::Create("Probe");
            probe->AddComponent<TickProbe>();
            scene.AddRootGameObject(probe);
            EXPECT_TRUE(world.BindScene(scene).has_value());
            scene.ProcessAttachQueue();
        }

        ~CurrentSceneGame()
        {
            SceneManager::AddScene(Scene::Create("RbRuntimeGlue_Empty_" + std::to_string(++counter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
        }

        /// The game logic of one tick (the host's hook): avatars move, fire spawns a shot, shots fly and run out
        static bool Logic(NetworkWorld &world, RollbackWorld &rollback, const InputLayout &layout, const TickInputs &inputs)
        {
            TickProbe::sessionTick = inputs.tick;
            for (std::uint32_t slot = 0; slot < 2; ++slot)
            {
                NetworkObject *const object = world.Find(NetworkId{slot + 1});
                const auto values = layout.Unpack(inputs.inputs[slot].Bytes());
                if (object == nullptr || !values)
                {
                    return false;
                }
                Avatar &avatar = *object->GetGameObject().GetComponent<Avatar>();
                avatar.x += (*values)[1] * 0.25f;
                avatar.y += (*values)[2] * 0.5f;
                if ((*values)[0] > 0.5f && static_cast<int>(inputs.tick) - avatar.lastFire >= 3)
                {
                    avatar.lastFire = static_cast<int>(inputs.tick);
                    auto spawned = rollback.Spawn(kShotPrefab, slot + 1, inputs.tick);
                    if (!spawned)
                    {
                        return false;
                    }
                    Shot &shot = *(*spawned)->GetGameObject().GetComponent<Shot>();
                    shot.x = avatar.x;
                    shot.vx = slot == 0 ? 0.5f : -0.5f;
                    shot.life = 3 + static_cast<int>(inputs.tick % 3);
                    shot.shooter = static_cast<int>(slot);
                }
            }
            std::vector<std::uint32_t> shots;
            for (const auto &entry : world.Objects())
            {
                if (entry.second->IsSpawned())
                {
                    shots.push_back(entry.first);
                }
            }
            for (const std::uint32_t id : shots)
            {
                Shot &shot = *world.Find(NetworkId{id})->GetGameObject().GetComponent<Shot>();
                shot.x += shot.vx;
                if (--shot.life <= 0)
                {
                    world.Find(NetworkId{static_cast<std::uint32_t>(shot.shooter) + 1})->GetGameObject().GetComponent<Avatar>()->score++;
                    (void)rollback.Despawn(NetworkId{id}, inputs.tick);
                }
            }
            return true;
        }

        NetworkWorld world;
        static inline int counter = 0;
    };

    void RestFrame()
    {
        TickOptions rest;
        rest.pollEvents = false;
        rest.render = false;
        rest.deltaSeconds = 0.0;
        Application::GetInstance().Tick(rest);
    }
}

TEST(RollbackRuntimeApplicationTest, TheRuntimeDrivesApplicationsFixedLoopAndItsConfirmedRootsAreAStraightRunsOfTheSameInputs)
{
    ShotPrefab prefab;
    Application &app = Application::GetInstance();
    app.Set3DPhysicsBackend(nullptr);
    TickProbe::fixedUpdates = 0;
    TickProbe::mismatches = 0;
    const InputLayout layout = MakeLayout();
    constexpr std::uint32_t kInterval = 10;

    SimNetwork network(41);
    SimTransport &relayTransport = network.AddEndpoint();
    SimTransport &transportA = network.AddEndpoint();
    SimTransport &transportB = network.AddEndpoint();
    SimLinkConfig link;
    link.latencyMicros = 25'000;
    link.jitterMicros = 15'000;
    ASSERT_TRUE(network.Link(relayTransport.Id(), transportA.Id(), link));
    ASSERT_TRUE(network.Link(relayTransport.Id(), transportB.Id(), link));
    LockstepRelayOptions relayOptions;
    relayOptions.protocolId = 0x6262;
    relayOptions.slotCount = 2;
    relayOptions.layout = layout;
    relayOptions.tickRate = 60;
    relayOptions.hashInterval = kInterval;
    LockstepRelay relay(relayTransport, relayOptions);

    std::vector<TickInputs> confirmed; // what peer B ran: every confirmed tick's inputs
    std::vector<std::pair<std::uint32_t, std::uint64_t>> published;
    std::uint64_t rollbacks = 0;
    std::uint64_t stepped = 0;
    std::uint32_t confirmedTick = 0;
    {
        CurrentSceneGame game;
        NetworkSettings settings;
        settings.preset = NetworkPreset::Deterministic;
        settings.deterministic.mode = DeterministicMode::Rollback;
        settings.deterministic.rollbackWindow = 8;

        RollbackWorld *rollbackWorld = nullptr;
        RollbackRuntimeOptions options;
        options.session.protocolId = 0x6262;
        options.session.layout = layout;
        options.session.historyDepth = 64;
        options.session.sampleInput = [](const std::uint32_t tick) { return ScriptedValues(0, tick); };
        options.session.applyTick = [&](const TickInputs &inputs, std::uint32_t)
        { return CurrentSceneGame::Logic(game.world, *rollbackWorld, layout, inputs); };
        options.clock = [&network] { return network.NowMicros(); };
        RollbackRuntime runtime(transportA, game.world, nullptr, settings, std::move(options));
        rollbackWorld = &runtime.World();
        EXPECT_EQ(runtime.RollbackWindow(), 8u);
        EXPECT_TRUE(app.HasFixedTickDriver());
        EXPECT_TRUE(app.GetSimulationClock().IsNetworked());

        LockstepRuntimeOptions other;
        other.protocolId = 0x6262;
        other.layout = layout;
        // Peer B's input changes every tick: a repeated guess is usually wrong
        other.sampleInput = [](const std::uint32_t tick)
        {
            return std::vector<float>{(tick % 7) == 3 ? 1.0f : 0.0f, static_cast<float>(tick % 5) / 2.0f - 1.0f,
                                      static_cast<float>((tick * 3) % 4) / 1.5f - 1.0f};
        };
        other.applyTick = [&confirmed](const TickInputs &inputs, std::uint32_t)
        {
            confirmed.push_back(inputs);
            return true;
        };
        LockstepRuntime peerB(transportB, std::move(other));

        constexpr double kFrame = 1.0 / 60.0;
        for (int frame = 0; frame < 4000 && runtime.Session().ConfirmedTick() < 240; ++frame)
        {
            network.Advance(16'667);
            relay.Update(network.NowMicros());
            TickOptions tick;
            tick.pollEvents = false;
            tick.render = false;
            tick.deltaSeconds = kFrame;
            app.Tick(tick); // the runtime runs inside, as the fixed-step loop
            peerB.Update(network.NowMicros(), kFrame);
        }
        ASSERT_EQ(runtime.Session().State(), LockstepState::Running) << runtime.Session().Error();
        ASSERT_GE(runtime.Session().ConfirmedTick(), 240u);
        ASSERT_NE(runtime.Session().Session(), nullptr);
        rollbacks = runtime.Session().Session()->Stats().rollbacks;
        stepped = runtime.TicksStepped();
        confirmedTick = runtime.Session().ConfirmedTick();
        for (const HashTree &tree : runtime.Session().History().Trees())
        {
            published.emplace_back(tree.Tick(), tree.Root());
        }
        EXPECT_EQ(relay.Stats().desyncs, 0u);
    }
    EXPECT_FALSE(app.HasFixedTickDriver()) << "the runtime gave the loop back";
    RestFrame();

    EXPECT_GT(rollbacks, 0u);
    EXPECT_GT(stepped, static_cast<std::uint64_t>(confirmedTick)) << "resimulated ticks ran through StepTicks too";
    EXPECT_GT(TickProbe::fixedUpdates, 0);
    EXPECT_EQ(TickProbe::mismatches, 0) << "Time::GetTick() inside a (re)simulated tick is the tick's own";
    ASSERT_GE(published.size(), 10u);

    // The straight run: the same scene and logic, ticked in order with the confirmed inputs, no session
    std::map<std::uint32_t, std::uint64_t> straight;
    {
        CurrentSceneGame game;
        RollbackWorld rollbackWorld(game.world, nullptr);
        Scene &scene = SceneManager::GetCurSceneRef();
        for (const TickInputs &inputs : confirmed)
        {
            app.GetSimulationClock().SetTick(inputs.tick);
            ASSERT_TRUE(CurrentSceneGame::Logic(game.world, rollbackWorld, layout, inputs));
            ASSERT_EQ(app.StepTicks(1), 1u);
            scene.ProcessAttachQueue();
            rollbackWorld.CommitSpawns();
            scene.ProcessDestroyed();
            if ((inputs.tick + 1) % kInterval == 0)
            {
                straight[inputs.tick + 1] = BuildStateHashTree(game.world, rollbackWorld.Layouts(), inputs.tick + 1, nullptr)->Root();
            }
        }
    }
    std::size_t compared = 0;
    for (const auto &[tick, root] : published)
    {
        const auto found = straight.find(tick);
        if (found != straight.end())
        {
            EXPECT_EQ(found->second, root) << "tick " << tick;
            ++compared;
        }
    }
    EXPECT_GE(compared, 10u);
    app.SetSimulationMode(SimulationMode::None);
    RestFrame();
}
