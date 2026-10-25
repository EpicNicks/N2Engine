#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RollbackTestSupport.hpp"
#include "engine/network/LockstepInputMapping.hpp"
#include "engine/network/RollbackSession.hpp"
#include "engine/network/n2net/SimTransport.hpp"

// Netcode C5 on the simulator: rollback sessions that exchange InputBundles over SimTransport links with scripted
// latency, jitter, loss, reordering and duplication, and the design's key test: the state hash (and every replicated
// field, exactly) at the end equals what a lockstep session reaches when it is fed the very inputs the rollback
// sessions ended up using, with mispredictions and rollbacks having happened on the way.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;
using namespace N2Engine::Network::Testing::Rollback;
using namespace N2Engine::N2Net;

namespace
{
    constexpr std::uint32_t kDelay = 2;
    constexpr std::uint64_t kTickMicros = 16'000;
    /// Frames of the local input every datagram repeats: loss is covered by redundancy, as in lockstep
    constexpr std::uint32_t kRedundancy = 24;

    struct NetPeer
    {
        NetPeer(const std::uint32_t slots, const std::uint32_t slot, const std::uint32_t window, const InputLayout &inputLayout,
                SimTransport &link, const std::uint32_t totalTicks)
            : arena(slots, inputLayout), transport(&link), localSlot(slot), total(totalTicks), log(totalTicks)
        {
            RollbackConfig config;
            config.slotCount = slots;
            config.localSlot = slot;
            config.inputDelay = kDelay;
            config.rollbackWindow = window;
            config.layout = *MakeLockstepLayoutInfo(inputLayout);
            RollbackHost host;
            host.save = [this](const std::uint32_t tick, WorldSnapshot &out) { return arena.rollback.Save(tick, out); };
            host.restore = [this](const WorldSnapshot &snapshot) { return arena.rollback.Restore(snapshot); };
            host.step = [this](const TickInputs &inputs)
            {
                if (inputs.tick < log.size())
                {
                    log[inputs.tick] = inputs; // the last run of a tick is the one that counts
                }
                return arena.Step(inputs);
            };
            host.confirmed = [this](const std::uint32_t first) { arena.rollback.ReleaseConfirmed(first); };
            host.resimulated = [this] { arena.rollback.ReleasePool(); };
            session = std::make_unique<RollbackSession>(config, std::move(host));
            arena.events = &session->Events();
            session->Events().SetPlayHandler([this](const RollbackEvent &event) { played.push_back(event); });
            session->Events().SetCancelHandler([this](const RollbackEvent &event) { cancelled.push_back(event); });
        }

        [[nodiscard]] bool Done() const
        {
            return session->CurrentTick() >= total && session->ConfirmedTick() >= total;
        }

        void Receive()
        {
            transport->Poll(
                [&](const TransportEvent &event)
                {
                    if (event.type != TransportEventType::Datagram)
                    {
                        return;
                    }
                    const auto bundle = DecodeInputBundleFor(arena.layout, event.data);
                    if (!bundle.has_value())
                    {
                        ++rejected;
                        return;
                    }
                    for (std::size_t i = 0; i < bundle->frames.size(); ++i)
                    {
                        (void)session->SubmitRemote(bundle->slot, bundle->firstTick + static_cast<std::uint32_t>(i),
                                                    bundle->frames[i]);
                    }
                });
        }

        void SampleAndSend(const std::vector<PeerId> &others)
        {
            if (sampled + kDelay < total)
            {
                std::uint32_t assigned = 0;
                const LockstepInput input = ScriptedInput(arena.layout, localSlot, kDelay + sampled);
                if (session->SubmitLocal(input, assigned) == SubmitResult::Accepted)
                {
                    sent.push_back(input);
                    ++sampled;
                }
            }
            if (sent.empty())
            {
                return;
            }
            // The newest kRedundancy frames, every time: a lost datagram is covered by the next
            const std::uint32_t newest = kDelay + static_cast<std::uint32_t>(sent.size());
            const std::uint32_t firstTick = newest > kDelay + kRedundancy ? newest - kRedundancy : kDelay;
            InputBundle bundle;
            bundle.slot = static_cast<std::uint8_t>(localSlot);
            bundle.ackTick = session->ConfirmedTick();
            bundle.firstTick = firstTick;
            bundle.payloadBytes = static_cast<std::uint8_t>(session->Config().layout.payloadBytes);
            bundle.layoutHash = session->Config().layout.hash;
            for (std::uint32_t tick = firstTick; tick < newest; ++tick)
            {
                bundle.frames.push_back(sent[tick - kDelay]);
            }
            std::array<std::uint8_t, kMaxPacketSize> buffer{};
            const std::size_t size = EncodeInputBundle(bundle, buffer);
            ASSERT_GT(size, 0u);
            for (const PeerId other : others)
            {
                (void)transport->Send(other, std::span<const std::uint8_t>(buffer.data(), size));
            }
        }

        Arena arena;
        SimTransport *transport;
        std::uint32_t localSlot;
        std::uint32_t total;
        std::unique_ptr<RollbackSession> session;
        std::vector<TickInputs> log;
        std::vector<LockstepInput> sent;
        std::vector<RollbackEvent> played;
        std::vector<RollbackEvent> cancelled;
        std::uint32_t sampled = 0;
        std::uint32_t rejected = 0;
    };

    struct MeshOptions
    {
        std::uint32_t players = 2;
        std::uint32_t window = 8;
        std::uint32_t ticks = 240;
        SimLinkConfig link;
        /// Called every frame with the frame number and the network: scripted changes of the links
        std::function<void(std::uint32_t, SimNetwork &, const std::vector<SimTransport *> &)> script;
        std::uint32_t maxFrames = 6000;
    };

    struct MeshResult
    {
        std::vector<std::unique_ptr<NetPeer>> peers;
        std::unique_ptr<SimNetwork> network;
        std::uint32_t frames = 0;
        bool finished = false;
    };

    MeshResult RunMesh(const std::uint64_t seed, const MeshOptions &options, const InputLayout &layout)
    {
        MeshResult result;
        result.network = std::make_unique<SimNetwork>(seed);
        std::vector<SimTransport *> transports;
        for (std::uint32_t i = 0; i < options.players; ++i)
        {
            transports.push_back(&result.network->AddEndpoint());
        }
        for (std::uint32_t i = 0; i < options.players; ++i)
        {
            for (std::uint32_t j = i + 1; j < options.players; ++j)
            {
                EXPECT_TRUE(result.network->Link(transports[i]->Id(), transports[j]->Id(), options.link));
            }
        }
        for (std::uint32_t i = 0; i < options.players; ++i)
        {
            result.peers.push_back(
                std::make_unique<NetPeer>(options.players, i, options.window, layout, *transports[i], options.ticks));
        }
        for (; result.frames < options.maxFrames; ++result.frames)
        {
            result.network->Advance(kTickMicros);
            if (options.script)
            {
                options.script(result.frames, *result.network, transports);
            }
            bool allDone = true;
            for (std::uint32_t i = 0; i < options.players; ++i)
            {
                NetPeer &peer = *result.peers[i];
                peer.Receive();
                std::vector<PeerId> others;
                for (std::uint32_t j = 0; j < options.players; ++j)
                {
                    if (j != i)
                    {
                        others.push_back(transports[j]->Id());
                    }
                }
                peer.SampleAndSend(others);
                // One new tick per frame (a rollback is extra work inside that frame)
                const AdvanceReport report = peer.session->Advance();
                EXPECT_NE(report.result, AdvanceResult::Failed) << peer.session->Error();
                if (report.result == AdvanceResult::Failed)
                {
                    return result;
                }
                allDone = allDone && peer.Done();
            }
            if (allDone)
            {
                result.finished = true;
                break;
            }
        }
        return result;
    }

    /// A lockstep session fed `inputs` (the final inputs of every tick): the reference state
    struct LockstepRun
    {
        LockstepRun(const std::uint32_t slots, const InputLayout &layout) : arena(slots, layout)
        {
            arena.events = &queue;
            queue.SetPlayHandler([this](const RollbackEvent &event) { played.push_back(event); });
        }

        void Run(const std::uint32_t slots, const InputLayout &layout, const std::vector<TickInputs> &inputs)
        {
            LockstepConfig config;
            config.slotCount = slots;
            config.inputDelay = kDelay;
            config.layout = *MakeLockstepLayoutInfo(layout);
            LockstepScheduler scheduler(config);
            ASSERT_TRUE(scheduler.Valid());
            for (std::uint32_t tick = 0; tick < inputs.size(); ++tick)
            {
                // The input sampled while the simulation is at `tick` is for tick + delay
                const std::uint32_t target = tick + kDelay;
                for (std::uint32_t slot = 0; slot < slots; ++slot)
                {
                    const LockstepInput input = target < inputs.size() ? inputs[target].inputs[slot] : config.layout.neutral;
                    std::uint32_t assigned = 0;
                    ASSERT_EQ(scheduler.SubmitLocal(slot, input, assigned), SubmitResult::Accepted);
                    ASSERT_EQ(assigned, target);
                }
                TickInputs out;
                ASSERT_EQ(scheduler.TryAdvance(out), AdvanceStatus::Runnable);
                queue.BeginTick(tick);
                ASSERT_TRUE(arena.Step(out));
                arena.rollback.ReleaseConfirmed(tick + 1);
                queue.Confirm(tick + 1);
                ++ran;
            }
        }

        Arena arena;
        RollbackEventQueue queue{RollbackEventPolicy::OnConfirm};
        std::vector<RollbackEvent> played;
        std::uint32_t ran = 0;
    };

    std::vector<RollbackEventKey> Survivors(const NetPeer &peer)
    {
        std::vector<RollbackEventKey> keys;
        for (const RollbackEvent &event : peer.played)
        {
            keys.push_back(event.key);
        }
        for (const RollbackEvent &event : peer.cancelled)
        {
            const auto found = std::find(keys.begin(), keys.end(), event.key);
            if (found != keys.end())
            {
                keys.erase(found);
            }
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    std::vector<RollbackEventKey> Keys(const std::vector<RollbackEvent> &events)
    {
        std::vector<RollbackEventKey> keys;
        for (const RollbackEvent &event : events)
        {
            keys.push_back(event.key);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    /// Runs the mesh and checks every peer against the lockstep reference. Returns the result for further checks.
    MeshResult RunAndCompare(const std::uint64_t seed, const MeshOptions &options)
    {
        const ShotPrefab prefab;
        const InputLayout layout = MakeLayout();
        MeshResult result = RunMesh(seed, options, layout);
        EXPECT_TRUE(result.finished) << "the sessions did not finish in " << options.maxFrames << " frames";
        if (!result.finished)
        {
            return result;
        }

        // The peers agree on the inputs they ran, tick by tick
        for (std::uint32_t tick = 0; tick < options.ticks; ++tick)
        {
            for (std::uint32_t p = 1; p < options.players; ++p)
            {
                for (std::uint32_t slot = 0; slot < options.players; ++slot)
                {
                    EXPECT_EQ(result.peers[p]->log[tick].inputs[slot], result.peers[0]->log[tick].inputs[slot])
                        << "tick " << tick << " slot " << slot << " peer " << p;
                }
            }
        }

        LockstepRun reference(options.players, layout);
        reference.Run(options.players, layout, result.peers[0]->log);
        EXPECT_EQ(reference.ran, options.ticks);
        const std::uint64_t expectedRoot = reference.arena.Root(options.ticks);
        const std::string expectedDump = reference.arena.Dump();
        for (std::uint32_t p = 0; p < options.players; ++p)
        {
            NetPeer &peer = *result.peers[p];
            EXPECT_EQ(peer.session->CurrentTick(), options.ticks);
            EXPECT_EQ(peer.session->ConfirmedTick(), options.ticks);
            EXPECT_EQ(peer.rejected, 0u);
            EXPECT_EQ(peer.arena.Root(options.ticks), expectedRoot) << "peer " << p;
            EXPECT_EQ(peer.arena.Dump(), expectedDump) << "peer " << p;
            EXPECT_LE(peer.session->Stats().deepestRollback, options.window);
            EXPECT_EQ(Survivors(peer), Keys(reference.played)) << "peer " << p << ": the effects that survived are lockstep's";
            EXPECT_EQ(peer.session->Events().Pending(), 0u) << "everything is confirmed";
        }
        return result;
    }
}

TEST(RollbackSessionNetworkTest, TwoPeersOverALaggyLinkEndWithTheLockstepState)
{
    MeshOptions options;
    options.link.latencyMicros = 70'000; // about 4 ticks one way
    options.link.jitterMicros = 25'000;
    MeshResult result = RunAndCompare(11, options);
    ASSERT_TRUE(result.finished);
    for (const auto &peer : result.peers)
    {
        EXPECT_GT(peer->session->Stats().mispredictions, 0u) << "the input changes faster than the link: guesses are wrong";
        EXPECT_GT(peer->session->Stats().rollbacks, 0u);
        EXPECT_GT(peer->session->Stats().ticksResimulated, 0u);
        EXPECT_GT(peer->session->Stats().ticksRun, 0u);
    }
}

TEST(RollbackSessionNetworkTest, LossReorderingAndDuplicationDoNotChangeTheResult)
{
    MeshOptions options;
    options.link.latencyMicros = 50'000;
    options.link.jitterMicros = 30'000;
    options.link.lossPpm = 80'000;
    options.link.reorderPpm = 150'000;
    options.link.reorderDelayMicros = 60'000;
    options.link.duplicatePpm = 100'000;
    MeshResult result = RunAndCompare(23, options);
    ASSERT_TRUE(result.finished);
    EXPECT_GT(result.peers[0]->session->Stats().rollbacks, 0u);
}

TEST(RollbackSessionNetworkTest, ALatencySpikePastTheWindowStallsAndStillConverges)
{
    MeshOptions options;
    options.window = 4;
    options.link.latencyMicros = 20'000;
    options.script = [](const std::uint32_t frame, SimNetwork &network, const std::vector<SimTransport *> &transports)
    {
        SimLinkConfig config;
        config.latencyMicros = (frame >= 60 && frame < 110) ? 400'000 : 20'000; // 25 ticks, far more than the window
        config.jitterMicros = 10'000;
        network.SetLinkConfig(transports[0]->Id(), transports[1]->Id(), config);
        network.SetLinkConfig(transports[1]->Id(), transports[0]->Id(), config);
    };
    MeshResult result = RunAndCompare(31, options);
    ASSERT_TRUE(result.finished);
    for (const auto &peer : result.peers)
    {
        EXPECT_GT(peer->session->Stats().windowFullWaits, 0u) << "the window held the sessions back";
    }
}

TEST(RollbackSessionNetworkTest, ThreePeersFullyMeshed)
{
    MeshOptions options;
    options.players = 3;
    options.window = 10;
    options.ticks = 200;
    options.link.latencyMicros = 60'000;
    options.link.jitterMicros = 40'000;
    options.link.lossPpm = 30'000;
    MeshResult result = RunAndCompare(47, options);
    ASSERT_TRUE(result.finished);
    for (const auto &peer : result.peers)
    {
        EXPECT_GT(peer->session->Stats().mispredictions, 0u);
    }
}

TEST(RollbackSessionNetworkTest, ALargerWindowRollsBackDeeperButEndsTheSame)
{
    MeshOptions shallow;
    shallow.window = 3;
    shallow.link.latencyMicros = 90'000;
    MeshResult a = RunAndCompare(5, shallow);
    ASSERT_TRUE(a.finished);

    MeshOptions deep = shallow;
    deep.window = 12;
    MeshResult b = RunAndCompare(5, deep);
    ASSERT_TRUE(b.finished);

    EXPECT_LE(a.peers[0]->session->Stats().deepestRollback, 3u);
    EXPECT_GT(b.peers[0]->session->Stats().deepestRollback, a.peers[0]->session->Stats().deepestRollback);
    EXPECT_EQ(a.peers[0]->arena.Dump(), b.peers[0]->arena.Dump()) << "the window changes how, not what";
}

TEST(RollbackSessionNetworkTest, WindowZeroOverTheSameLinkIsLockstepAndNeverRollsBack)
{
    MeshOptions options;
    options.window = 0;
    options.link.latencyMicros = 70'000;
    options.link.jitterMicros = 20'000;
    options.link.lossPpm = 40'000;
    MeshResult result = RunAndCompare(61, options);
    ASSERT_TRUE(result.finished);
    for (const auto &peer : result.peers)
    {
        EXPECT_EQ(peer->session->Stats().rollbacks, 0u);
        EXPECT_EQ(peer->session->Stats().ticksPredicted, 0u);
        EXPECT_EQ(peer->session->Stats().snapshotsSaved, 0u);
    }
}
