#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/network/LockstepInputMapping.hpp"
#include "engine/network/LockstepProtocol.hpp"
#include "engine/network/LockstepRelay.hpp"
#include "engine/network/LockstepRuntime.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/replay/GoldenReplay.hpp"
#include "engine/replay/LockstepReplay.hpp"

// Netcode C3 wiring on the simulator: the relay (LockstepRelay), the client runtime (LockstepRuntime), the desync flow
// (root exchange through the relay, bisection through the relay's forward envelopes, reports on both peers), the stall
// policy and the protocol codecs. Everything runs on the SimNetwork's virtual clock.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::N2Net;

namespace
{
    InputLayout TestLayout()
    {
        auto created = InputLayout::Create({
            {"move", InputControlKind::Vector2, 8, 1.0f, true},
            {"jump", InputControlKind::Button, 1, 1.0f, false},
        });
        EXPECT_TRUE(created.has_value());
        return created.has_value() ? *created : InputLayout{};
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

    std::string TempPath(const std::string &name)
    {
        return (std::filesystem::temp_directory_path() / name).string();
    }

    constexpr std::uint64_t kStepMicros = 2'000;

    /// One client: a toy simulation (a hash of every slot's accumulated input) under a LockstepRuntime
    struct Client
    {
        std::unique_ptr<LockstepRuntime> runtime;
        std::vector<std::uint64_t> slotState = std::vector<std::uint64_t>(kMaxRelaySlots, 14695981039346656037ull);
        std::vector<std::uint64_t> tickDigest; // per tick: a hash of all inputs
        std::vector<LockstepEvent> events;
        /// From this tick on, the state of slot 0 is perturbed (the injected divergence)
        std::int64_t divergeFromTick = -1;
        std::uint32_t sampled = 0;
        bool frozen = false; // not updated any more (a stalled client)
        std::string report;
    };

    struct World
    {
        World(const std::uint64_t seed, const std::uint32_t players, const SimLinkConfig &link,
              const std::function<void(LockstepRelayOptions &)> &tune = {})
            : network(seed), layout(TestLayout())
        {
            relayTransport = &network.AddEndpoint();
            LockstepRelayOptions options;
            options.protocolId = 0x1234;
            options.slotCount = players;
            options.layout = layout;
            options.tickRate = 500;
            options.hashInterval = 20;
            options.stallTimeoutMicros = 400'000;
            options.pausedGraceMicros = 3'000'000;
            options.desyncLingerMicros = 3'000'000;
            if (tune)
            {
                tune(options);
            }
            relay = std::make_unique<LockstepRelay>(*relayTransport, options);
            for (std::uint32_t i = 0; i < players; ++i)
            {
                clientTransports.push_back(&network.AddEndpoint());
                EXPECT_TRUE(network.Link(relayTransport->Id(), clientTransports.back()->Id(), link));
            }
        }

        std::uint64_t Now() const { return network.NowMicros(); }

        Client &AddClient(const std::size_t index, const InputLayout *layoutOverride = nullptr)
        {
            auto client = std::make_unique<Client>();
            Client &ref = *client;
            LockstepRuntimeOptions options;
            options.protocolId = 0x1234;
            options.layout = layoutOverride != nullptr ? *layoutOverride : layout;
            options.peerName = "peer" + std::to_string(index);
            options.reportPath = TempPath("n2_lockstep_relay_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                                          "_" + std::to_string(index) + ".json");
            ref.report = options.reportPath;
            options.historyDepth = 16;
            options.desyncLingerMicros = 300'000;
            options.observer = [&ref](const LockstepEvent &event) { ref.events.push_back(event); };
            options.sampleInput = [&ref, index](const std::uint32_t tick)
            {
                ++ref.sampled;
                const float x = static_cast<float>(((tick / 7) * 13 + index * 5) % 21) / 10.0f - 1.0f;
                return std::vector<float>{x, -x, static_cast<float>((tick / 11 + index) % 2)};
            };
            options.applyTick = [&ref](const TickInputs &inputs, std::uint32_t slots)
            {
                std::uint64_t digest = 1469598103934665603ull;
                for (std::uint32_t s = 0; s < slots; ++s)
                {
                    for (const std::uint8_t byte : inputs.inputs[s].Bytes())
                    {
                        ref.slotState[s] = (ref.slotState[s] ^ byte) * 1099511628211ull;
                        digest = (digest ^ byte) * 1099511628211ull;
                    }
                }
                ref.tickDigest.push_back(digest);
                return true;
            };
            options.buildTree = [&ref](const std::uint32_t tick)
            {
                HashTreeBuilder builder(tick);
                for (std::uint32_t s = 0; s < 2; ++s)
                {
                    std::uint64_t value = ref.slotState[s];
                    if (ref.divergeFromTick >= 0 && s == 1 && static_cast<std::int64_t>(tick) >= ref.divergeFromTick)
                    {
                        value ^= 1ull << 9;
                    }
                    EXPECT_TRUE(builder.AddField(s + 1, 0, Bytes(value)));
                }
                return builder.Build();
            };
            ref.runtime = std::make_unique<LockstepRuntime>(*clientTransports[index], std::move(options));
            clients.push_back(std::move(client));
            return ref;
        }

        void Step()
        {
            network.Advance(kStepMicros);
            relay->Update(Now());
            for (auto &client : clients)
            {
                if (!client->frozen)
                {
                    client->runtime->Update(Now(), static_cast<double>(kStepMicros) / 1e6);
                }
            }
        }

        bool RunUntil(const std::function<bool()> &done, const int maxSteps = 20000)
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

        bool AllRan(const std::uint32_t ticks)
        {
            for (auto &client : clients)
            {
                if (!client->frozen && client->runtime->TicksRun() < ticks)
                {
                    return false;
                }
            }
            return true;
        }

        SimNetwork network;
        InputLayout layout;
        SimTransport *relayTransport = nullptr;
        std::vector<SimTransport *> clientTransports;
        std::unique_ptr<LockstepRelay> relay;
        std::vector<std::unique_ptr<Client>> clients;
    };

    SimLinkConfig RoughLink()
    {
        SimLinkConfig link;
        link.latencyMicros = 6'000;
        link.jitterMicros = 6'000;
        link.lossPpm = 100'000;
        link.reorderPpm = 100'000;
        link.reorderDelayMicros = 10'000;
        link.duplicatePpm = 50'000;
        return link;
    }
}

TEST(LockstepRelayTest, TwoClientsRunTheSameTicksOnAPerfectLink)
{
    World world(1, 2, SimLinkConfig{});
    world.AddClient(0);
    world.AddClient(1);
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(300); }));
    EXPECT_EQ(world.relay->State(), RelayState::Running);
    EXPECT_EQ(world.clients[0]->runtime->Slot(), 0u);
    EXPECT_EQ(world.clients[1]->runtime->Slot(), 1u);
    const std::size_t common = std::min(world.clients[0]->tickDigest.size(), world.clients[1]->tickDigest.size());
    ASSERT_GE(common, 300u);
    for (std::size_t i = 0; i < common; ++i)
    {
        ASSERT_EQ(world.clients[0]->tickDigest[i], world.clients[1]->tickDigest[i]) << "tick " << i;
    }
    // Neither the relay nor a client counted a violation, and the relay confirmed what the clients ran
    EXPECT_EQ(world.relay->Stats().clientsDropped, 0u);
    EXPECT_EQ(world.relay->Stats().desyncs, 0u);
    EXPECT_GE(world.relay->ConfirmedTick(), 300u);
    EXPECT_GE(world.relay->Stats().rootsReceived, 2u * (300u / 20u) - 4u);
}

TEST(LockstepRelayTest, ThreeClientsAgreeOverALossyReorderingLink)
{
    World world(7, 3, RoughLink());
    for (std::size_t i = 0; i < 3; ++i)
    {
        world.AddClient(i);
    }
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(200); }, 60000));
    const std::size_t common = std::min({world.clients[0]->tickDigest.size(), world.clients[1]->tickDigest.size(),
                                         world.clients[2]->tickDigest.size()});
    ASSERT_GE(common, 200u);
    for (std::size_t i = 0; i < common; ++i)
    {
        ASSERT_EQ(world.clients[0]->tickDigest[i], world.clients[1]->tickDigest[i]) << "tick " << i;
        ASSERT_EQ(world.clients[0]->tickDigest[i], world.clients[2]->tickDigest[i]) << "tick " << i;
    }
    EXPECT_EQ(world.relay->Stats().desyncs, 0u);
}

TEST(LockstepRelayTest, ASessionIsRecordedAndTheC2RunnerReplaysItToTheLiveHashes)
{
    // The sim here is the C2 scene stepper: the live checkpoint hashes are the ones RunReplay must print for the log
    const std::string scene = Replay::SceneNames().front();
    World world(3, 2, RoughLink());
    struct Sim
    {
        std::unique_ptr<Replay::ReplaySimulation> simulation;
        std::vector<Replay::ReplayHash> hashes;
    };
    std::array<Sim, 2> sims;
    for (std::size_t i = 0; i < 2; ++i)
    {
        Client &client = world.AddClient(i);
        std::string error;
        sims[i].simulation = Replay::ReplaySimulation::Create(scene, 99, error);
        ASSERT_TRUE(sims[i].simulation) << error;
        LockstepRuntimeOptions dummy; // options are built in AddClient; rebuild the runtime with the scene sim
        (void)dummy;
        const InputLayout layout = world.layout;
        LockstepRuntimeOptions options;
        options.protocolId = 0x1234;
        options.layout = layout;
        options.scene = scene;
        options.seed = 99;
        options.sampleInput = [i](const std::uint32_t tick)
        {
            const float x = static_cast<float>(((tick / 7) * 13 + i * 5) % 21) / 10.0f - 1.0f;
            return std::vector<float>{x, -x, static_cast<float>((tick / 11 + i) % 2)};
        };
        Sim *sim = &sims[i];
        options.applyTick = [sim, layout](const TickInputs &inputs, std::uint32_t slots)
        {
            std::array<Replay::SlotState, Replay::kMaxSlots> held{};
            for (std::uint32_t s = 0; s < slots; ++s)
            {
                if (!Replay::SceneStateFromInput(layout, inputs.inputs[s], held[s]))
                {
                    return false;
                }
            }
            std::string error;
            return sim->simulation->Step(std::span<const Replay::SlotState>(held.data(), slots), error);
        };
        options.buildTree = [sim](const std::uint32_t tick)
        {
            const std::uint64_t hash = sim->simulation->Hash();
            sim->hashes.push_back(Replay::ReplayHash{static_cast<int>(tick), hash});
            HashTreeBuilder builder(tick);
            EXPECT_TRUE(builder.AddField(1, 0, Bytes(hash)));
            return builder.Build();
        };
        client.runtime = std::make_unique<LockstepRuntime>(*world.clientTransports[i], std::move(options));
    }
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(120); }, 60000));
    EXPECT_EQ(world.relay->Stats().desyncs, 0u);

    const Replay::ReplayLog *log = world.clients[0]->runtime->Log();
    ASSERT_NE(log, nullptr);
    Replay::ReplayLog copy = *log;
    ASSERT_GE(copy.ticks, 120);
    copy.ticks = 120;
    // The file round trips through its text form
    std::string error;
    const auto parsed = Replay::ParseReplayLog(Replay::SerializeReplayLog(copy), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    const Replay::ReplayResult replayed = Replay::RunReplay(*parsed);
    ASSERT_TRUE(replayed.ok) << replayed.error;
    for (const Replay::ReplayHash &replay : replayed.hashes)
    {
        bool found = false;
        for (const Replay::ReplayHash &live : sims[0].hashes)
        {
            if (live.tick == replay.tick)
            {
                found = true;
                EXPECT_EQ(live.hash, replay.hash) << "tick " << replay.tick;
            }
        }
        EXPECT_TRUE(found) << "no live hash at tick " << replay.tick;
    }
    EXPECT_GE(replayed.hashes.size(), 4u);
}

TEST(LockstepRelayTest, AnInjectedDivergenceIsFoundBisectedAndReportedByBothPeersAndTheSessionStops)
{
    World world(11, 2, SimLinkConfig{.latencyMicros = 2'000});
    world.AddClient(0);
    Client &second = world.AddClient(1);
    second.divergeFromTick = 60;
    ASSERT_TRUE(world.RunUntil([&] { return world.clients[0]->runtime->Finished() && world.clients[1]->runtime->Finished(); },
                               40000));
    EXPECT_EQ(world.relay->Stats().desyncs, 1u);
    for (auto &client : world.clients)
    {
        const auto &stop = client->runtime->StopInfo();
        ASSERT_TRUE(stop.has_value());
        EXPECT_EQ(client->runtime->State(), LockstepState::Stopped);
        EXPECT_EQ(stop->reason, StopReason::Desync);
        EXPECT_EQ(stop->tick, 60u) << "the first checkpoint at or after the injected tick";
#if defined(__EMSCRIPTEN__)
        // No threads on the web: the bisection is skipped, the report is still written
        EXPECT_EQ(stop->divergence.kind, Divergence::Kind::None);
        if (!stop->reportWritten) { ADD_FAILURE() << stop->reportError; }
        continue;
#endif
        EXPECT_EQ(stop->divergence.kind, Divergence::Kind::FieldDiffers);
        EXPECT_EQ(stop->divergence.objectId, 2u) << "rounds " << stop->divergence.rounds << " slot " << client->runtime->Slot() << " kind " << int(stop->divergence.kind) << " failure " << int(stop->bisectFailure);
        EXPECT_EQ(stop->divergence.fieldId, 0u);
        EXPECT_NE(stop->rootA, stop->rootB);
        ASSERT_TRUE(stop->reportWritten) << stop->reportError;

        std::ifstream file(client->report, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        const nlohmann::json report = nlohmann::json::parse(text.str(), nullptr, false);
        ASSERT_FALSE(report.is_discarded());
        EXPECT_EQ(report.at("format"), "n2desync");
        EXPECT_EQ(report.at("tick"), 60);
        // The input log from tick 0 is inside, in the n2replay form
        ASSERT_TRUE(report.contains("inputLog"));
        EXPECT_EQ(report.at("inputLog").at("format"), "n2replay");
        std::error_code ignored;
        std::filesystem::remove(client->report, ignored);
    }
    // Neither client kept simulating past the stop
    EXPECT_LT(world.clients[0]->runtime->TicksRun(), 400u);
}

TEST(LockstepRelayTest, ThePeerOutsideTheDesyncPairStopsAndWritesAReportWithoutBisecting)
{
    World world(12, 3, SimLinkConfig{.latencyMicros = 2'000});
    world.AddClient(0);
    world.AddClient(1).divergeFromTick = 40;
    world.AddClient(2);
    ASSERT_TRUE(world.RunUntil([&] { return world.clients[0]->runtime->Finished() && world.clients[1]->runtime->Finished() &&
                                            world.clients[2]->runtime->Finished(); },
                               60000));
    // Slot 1 differs from slot 0 (the first to report); slot 2 reported later and is the third party only if it matched
    // neither: either way every client stopped and wrote its report
    for (auto &client : world.clients)
    {
        ASSERT_TRUE(client->runtime->StopInfo().has_value());
        EXPECT_EQ(client->runtime->StopInfo()->reason, StopReason::Desync);
        EXPECT_TRUE(client->runtime->StopInfo()->reportWritten);
        std::error_code ignored;
        std::filesystem::remove(client->report, ignored);
    }
}

TEST(LockstepRelayTest, AStalledClientIsDroppedAtOneTickByEveryPeerAndTheSessionGoesOn)
{
    World world(21, 3, SimLinkConfig{.latencyMicros = 2'000});
    for (std::size_t i = 0; i < 3; ++i)
    {
        world.AddClient(i);
    }
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(80); }));
    world.clients[2]->frozen = true; // it stops sending and answering
    ASSERT_TRUE(world.RunUntil([&] { return world.relay->IsSlotDropped(2); }, 20000));
    ASSERT_TRUE(world.RunUntil([&] { return world.clients[0]->runtime->TicksRun() > 250 && world.clients[1]->runtime->TicksRun() > 250; },
                               40000));
    EXPECT_EQ(world.relay->State(), RelayState::Running);
    EXPECT_EQ(world.relay->Stats().slotsDropped, 1u);
    EXPECT_GE(world.relay->Stats().stalls, 1u);
    // Both survivors applied the same drop tick and ran the same ticks
    std::uint32_t dropTick[2] = {0, 0};
    for (std::size_t i = 0; i < 2; ++i)
    {
        int drops = 0;
        for (const LockstepEvent &event : world.clients[i]->events)
        {
            if (event.type == LockstepEvent::Type::SlotDropped)
            {
                ++drops;
                EXPECT_EQ(event.slot, 2u);
                dropTick[i] = event.tick;
            }
        }
        EXPECT_EQ(drops, 1);
    }
    EXPECT_EQ(dropTick[0], dropTick[1]);
    const std::size_t common = std::min(world.clients[0]->tickDigest.size(), world.clients[1]->tickDigest.size());
    for (std::size_t i = 0; i < common; ++i)
    {
        ASSERT_EQ(world.clients[0]->tickDigest[i], world.clients[1]->tickDigest[i]) << "tick " << i;
    }
}

TEST(LockstepRelayTest, TheStopPolicyEndsTheSessionForEveryoneWhenASlotStalls)
{
    World world(22, 2, SimLinkConfig{.latencyMicros = 2'000},
                [](LockstepRelayOptions &options) { options.stallPolicy = StallPolicy::Stop; });
    world.AddClient(0);
    world.AddClient(1);
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(60); }));
    world.clients[1]->frozen = true;
    ASSERT_TRUE(world.RunUntil([&] { return world.clients[0]->runtime->Finished(); }, 20000));
    ASSERT_TRUE(world.clients[0]->runtime->StopInfo().has_value());
    EXPECT_EQ(world.clients[0]->runtime->StopInfo()->reason, StopReason::Stalled);
    EXPECT_EQ(world.clients[0]->runtime->StopInfo()->slotA, 1u);
    EXPECT_EQ(world.relay->Stats().slotsDropped, 0u);
}

TEST(LockstepRelayTest, AClientThatLeavesIsDropped)
{
    World world(23, 2, SimLinkConfig{.latencyMicros = 2'000});
    world.AddClient(0);
    world.AddClient(1);
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(60); }));
    world.clients[1]->runtime->Leave();
    world.clients[1]->frozen = true;
    ASSERT_TRUE(world.RunUntil([&] { return world.relay->IsSlotDropped(1); }, 20000));
    ASSERT_TRUE(world.RunUntil([&] { return world.clients[0]->runtime->TicksRun() > 200; }, 20000));
    EXPECT_EQ(world.clients[0]->runtime->State(), LockstepState::Running);
}

TEST(LockstepRelayTest, APausedClientGetsTheGraceBeforeItIsDropped)
{
    World world(24, 2, SimLinkConfig{.latencyMicros = 2'000});
    world.AddClient(0);
    world.AddClient(1);
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(60); }));
    world.clients[1]->runtime->SetPaused(true);
    // Let the pause reach the relay, then the client goes quiet
    for (int i = 0; i < 20; ++i)
    {
        world.Step();
    }
    world.clients[1]->frozen = true;
    // Past the stall timeout (400 ms) but inside the 3 s grace: still seated
    for (int i = 0; i < 500; ++i) // one second
    {
        world.Step();
    }
    EXPECT_FALSE(world.relay->IsSlotDropped(1));
    ASSERT_TRUE(world.RunUntil([&] { return world.relay->IsSlotDropped(1); }, 20000));
}

TEST(LockstepRelayTest, AClientWithAnotherInputLayoutIsRefusedAndALateJoinerTooLate)
{
    World world(31, 2, SimLinkConfig{.latencyMicros = 2'000});
    auto other = InputLayout::Create({{"move", InputControlKind::Vector2, 8, 1.0f, true}});
    ASSERT_TRUE(other.has_value());
    Client &bad = world.AddClient(0, &*other);
    ASSERT_TRUE(world.RunUntil([&] { return bad.runtime->Finished(); }, 4000));
    EXPECT_EQ(bad.runtime->State(), LockstepState::Refused);
    EXPECT_EQ(bad.runtime->Refusal(), RefuseReason::LayoutMismatch);
    EXPECT_EQ(world.relay->Stats().refused, 1u);
    EXPECT_EQ(world.relay->JoinedCount(), 0u);
}

TEST(LockstepRelayTest, ASessionThatHasStartedRefusesALateJoiner)
{
    World world(32, 1, SimLinkConfig{.latencyMicros = 2'000});
    world.AddClient(0);
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(20); }));
    // A second endpoint appears after the start
    SimTransport &late = world.network.AddEndpoint();
    ASSERT_TRUE(world.network.Link(world.relayTransport->Id(), late.Id(), SimLinkConfig{}));
    LockstepRuntimeOptions options;
    options.protocolId = 0x1234;
    options.layout = world.layout;
    LockstepRuntime runtime(late, std::move(options));
    for (int i = 0; i < 2000 && !runtime.Finished(); ++i)
    {
        world.Step();
        runtime.Update(world.Now(), 0.002);
    }
    EXPECT_EQ(runtime.State(), LockstepState::Refused);
    EXPECT_EQ(runtime.Refusal(), RefuseReason::SessionRunning);
}

TEST(LockstepRelayTest, AnEmptyLayoutOrTooManyPlayersMakesAnInvalidRelay)
{
    SimNetwork network(1);
    SimTransport &transport = network.AddEndpoint();
    LockstepRelayOptions options;
    options.layout = InputLayout{};
    EXPECT_FALSE(LockstepRelay(transport, options).Valid());
    options.layout = TestLayout();
    options.slotCount = 5;
    EXPECT_FALSE(LockstepRelay(transport, options).Valid());
    options.slotCount = 4;
    EXPECT_TRUE(LockstepRelay(transport, options).Valid());
}

TEST(LockstepRelayTest, RandomGarbageFromARawPeerNeverBreaksTheRelayOrTheSession)
{
    World world(41, 2, SimLinkConfig{.latencyMicros = 2'000});
    world.AddClient(0);
    world.AddClient(1);
    SimTransport &raw = world.network.AddEndpoint();
    ASSERT_TRUE(world.network.Link(world.relayTransport->Id(), raw.Id(), SimLinkConfig{}));
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto next = [&state]
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(60); }));
    for (int round = 0; round < 400; ++round)
    {
        std::vector<std::uint8_t> junk(1 + next() % 300);
        for (auto &byte : junk)
        {
            byte = static_cast<std::uint8_t>(next());
        }
        raw.Poll([](const TransportEvent &) {});
        (void)raw.Send(world.relayTransport->Id(), junk);
        world.Step();
    }
    ASSERT_TRUE(world.RunUntil([&] { return world.AllRan(250); }));
    EXPECT_EQ(world.relay->State(), RelayState::Running);
    EXPECT_LE(world.relay->ConnectionCount(), 3u);
    EXPECT_EQ(world.relay->Stats().desyncs, 0u);
}

TEST(LockstepProtocolTest, MessagesRoundTripAndEveryTruncationIsRefused)
{
    WelcomeMessage welcome;
    welcome.slot = 1;
    welcome.slotCount = 3;
    welcome.inputDelay = 2;
    welcome.tickRate = 120;
    welcome.hashInterval = 30;
    welcome.dropFill = 1;
    welcome.payloadBytes = 3;
    welcome.layoutHash = 0x1122334455667788ull;
    welcome.neutral = {1, 2, 3};
    const auto encoded = Encode(welcome);
    const auto decoded = DecodeWelcome(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->layoutHash, welcome.layoutHash);
    EXPECT_EQ(decoded->neutral, welcome.neutral);
    EXPECT_EQ(decoded->tickRate, 120);
    for (std::size_t length = 0; length < encoded.size(); ++length)
    {
        EXPECT_FALSE(DecodeWelcome(std::span<const std::uint8_t>(encoded.data(), length)).has_value()) << length;
    }
    auto longer = encoded;
    longer.push_back(0);
    EXPECT_FALSE(DecodeWelcome(longer).has_value());

    const auto join = Encode(JoinMessage{kLockstepWireVersion, 4, 77});
    ASSERT_TRUE(DecodeJoin(join).has_value());
    for (std::size_t length = 0; length < join.size(); ++length)
    {
        EXPECT_FALSE(DecodeJoin(std::span<const std::uint8_t>(join.data(), length)).has_value());
    }
    EXPECT_FALSE(DecodeJoin(Encode(JoinMessage{kLockstepWireVersion, 200, 1})).has_value());

    const auto drop = Encode(DropSlotMessage{2, 12345, DropReason::Stalled});
    const auto dropped = DecodeDropSlot(drop);
    ASSERT_TRUE(dropped.has_value());
    EXPECT_EQ(dropped->fromTick, 12345u);
    EXPECT_FALSE(DecodeDropSlot(Encode(DropSlotMessage{9, 1, DropReason::Stalled})).has_value());

    const auto stop = Encode(StopMessage{StopReason::Desync, 60, 0, 1, 5, 6});
    const auto stopped = DecodeStop(stop);
    ASSERT_TRUE(stopped.has_value());
    EXPECT_EQ(stopped->rootB, 6u);
    for (std::size_t length = 0; length < stop.size(); ++length)
    {
        EXPECT_FALSE(DecodeStop(std::span<const std::uint8_t>(stop.data(), length)).has_value());
    }

    EXPECT_EQ(DecodeRefuse(EncodeRefuse(RefuseReason::SessionFull)), RefuseReason::SessionFull);
    EXPECT_FALSE(DecodeRefuse(std::vector<std::uint8_t>{0x44, 0}).has_value());
    EXPECT_EQ(DecodePause(EncodePause(true)), true);

    const std::array<std::uint8_t, 18> request{3};
    const auto forward = EncodeForward(RelayMessage::To, 1, request);
    const auto decodedForward = DecodeForward(RelayMessage::To, forward);
    ASSERT_TRUE(decodedForward.has_value());
    EXPECT_EQ(decodedForward->slot, 1);
    EXPECT_EQ(decodedForward->inner.size(), request.size());
    // Only bisect kinds may be forwarded, and the direction byte must match
    const std::array<std::uint8_t, 4> notBisect{1, 2, 3, 4};
    EXPECT_FALSE(DecodeForward(RelayMessage::To, EncodeForward(RelayMessage::To, 0, notBisect)).has_value());
    EXPECT_FALSE(DecodeForward(RelayMessage::From, forward).has_value());
    EXPECT_TRUE(EncodeForward(RelayMessage::To, 0, std::vector<std::uint8_t>(kLockstepMaxMessageBytes, 3)).empty());
}

TEST(LockstepProtocolTest, RandomBytesNeverDecodeToSomethingOutOfRange)
{
    std::uint64_t state = 88172645463325252ull;
    const auto next = [&state]
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (int i = 0; i < 20000; ++i)
    {
        std::vector<std::uint8_t> bytes(next() % 40);
        for (auto &byte : bytes)
        {
            byte = static_cast<std::uint8_t>(next());
        }
        if (!bytes.empty() && (next() & 1))
        {
            bytes[0] = static_cast<std::uint8_t>(0x40 + next() % 0x30);
        }
        if (const auto welcome = DecodeWelcome(bytes))
        {
            EXPECT_TRUE(welcome->slot < welcome->slotCount || welcome->slot == kSpectatorSlot);
            EXPECT_LE(welcome->slotCount, kMaxRelaySlots);
            EXPECT_EQ(welcome->neutral.size(), welcome->payloadBytes);
        }
        if (const auto stop = DecodeStop(bytes))
        {
            EXPECT_LT(stop->slotA, kMaxRelaySlots);
            EXPECT_LT(stop->slotB, kMaxRelaySlots);
        }
        if (const auto drop = DecodeDropSlot(bytes))
        {
            EXPECT_LT(drop->slot, kMaxRelaySlots);
        }
        (void)DecodeJoin(bytes);
        (void)DecodeRefuse(bytes);
        (void)DecodePause(bytes);
        (void)DecodeForward(RelayMessage::To, bytes);
        (void)DecodeForward(RelayMessage::From, bytes);
        (void)DecodeSpectate(bytes);
        (void)DecodeSnapshotRequest(bytes);
        for (const RelayMessage type : {RelayMessage::SnapshotData, RelayMessage::SnapshotPart})
        {
            if (const auto part = DecodeSnapshotPart(type, bytes))
            {
                EXPECT_LE(part->offset, part->totalBytes);
                EXPECT_LE(part->data.size(), part->totalBytes - part->offset);
                EXPECT_LE(part->data.size(), kMaxSnapshotPartBytes);
            }
        }
    }
}

TEST(LockstepProtocolTest, ALayoutFileParsesToTheSameLayoutAsC)
{
    const auto parsed = ParseInputLayoutJson(
        R"([{"name":"move","kind":"vector2","bits":8,"range":1,"signed":true},{"name":"jump","kind":"button"}])");
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->Hash(), TestLayout().Hash());
    EXPECT_FALSE(ParseInputLayoutJson("{}").has_value());
    EXPECT_FALSE(ParseInputLayoutJson(R"([{"name":"a"}])").has_value());
    EXPECT_FALSE(ParseInputLayoutJson(R"([{"name":"a","kind":"nope"}])").has_value());
    EXPECT_FALSE(ParseInputLayoutJson(R"([{"name":"a","kind":"button","extra":1}])").has_value());
    EXPECT_FALSE(ParseInputLayoutJson("not json").has_value());
}
