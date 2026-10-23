#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "NetworkTestSupport.hpp"
#include "engine/Time.hpp"
#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkInput.hpp"
#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// Player input end to end on the network simulator (netcode stage B4): a NetworkServer and NetworkClients in one process
// over seeded virtual links with no sleeps, plus hostile peers that send what no honest client sends.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;

namespace
{
    constexpr std::uint32_t kProtocol = 0x4E32'4E49;
    constexpr std::uint64_t kSceneHash = 0x1AB0CE11ull;
    constexpr const char *kScenePath = "res://input.scene";

    InputControlSpec Spec(const std::string &name, const InputControlKind kind, const std::uint8_t bits, const bool isSigned)
    {
        InputControlSpec spec;
        spec.name = name;
        spec.kind = kind;
        spec.bits = bits;
        spec.range = 1.0f;
        spec.isSigned = isSigned;
        return spec;
    }

    /// jump (button), move (Vector2, 8 bits), throttle (unsigned axis, 6 bits): values jump, move.x, move.y, throttle
    InputLayout GameLayout()
    {
        return InputLayout::Create({Spec("throttle", InputControlKind::Axis, 6, false),
                                    Spec("move", InputControlKind::Vector2, 8, true),
                                    Spec("jump", InputControlKind::Button, 1, false)})
            .value();
    }

    InputLayout OtherLayout()
    {
        return InputLayout::Create({Spec("fire", InputControlKind::Button, 1, false)}).value();
    }

    std::vector<float> Values(const float jump, const float moveX, const float moveY, const float throttle)
    {
        return {jump, moveX, moveY, throttle};
    }

    /// What a client queues for stamp `s` in the streaming tests
    std::vector<float> StreamValues(const std::uint32_t s)
    {
        const float x = (static_cast<float>(s % 11) - 5.0f) / 5.0f;
        return Values(static_cast<float>(s % 2), x, -x, static_cast<float>(s % 64) / 63.0f);
    }

    /// The values a layout holds after the wire: quantised
    std::vector<float> OnTheWire(const InputLayout &layout, const std::vector<float> &values)
    {
        return layout.Unpack(layout.Pack(values).value()).value();
    }

    struct Peer
    {
        explicit Peer(const std::vector<std::uint32_t> &uuids, const std::string &name) : scene(MakeScene(name, uuids))
        {
            bound = world.BindScene(*scene);
        }

        std::unique_ptr<Scene> scene;
        NetworkWorld world;
        std::expected<std::uint32_t, std::string> bound{std::unexpected(std::string("not bound"))};
    };

    struct ClientRig
    {
        explicit ClientRig(const std::string &name) : peer({1, 2, 3}, name) {}

        Peer peer;
        N2Net::SimTransport *endpoint = nullptr;
        std::unique_ptr<NetworkClient> client;
    };

    /// A hand-made peer of the server: a Connection over a simulator endpoint that sends whatever it is told to
    class RawPeer
    {
    public:
        RawPeer(const std::uint64_t now, const N2Net::PeerId server, N2Net::SimTransport &endpoint,
                const N2Net::ConnectionConfig &config)
            : _endpoint(&endpoint), _server(server), _connection(config, now)
        {
        }

        void Pump(const std::uint64_t now)
        {
            const auto handler = [&](const N2Net::TransportEvent &event)
            {
                if (event.type != N2Net::TransportEventType::Datagram || event.peer != _server)
                {
                    return;
                }
                (void)_connection.OnPacket(now, event.data,
                                           [&](const N2Net::MessageChannel channel, const std::span<const std::uint8_t> message)
                                           {
                                               if (channel == N2Net::MessageChannel::ReliableOrdered)
                                               {
                                                   received.emplace_back(message.begin(), message.end());
                                               }
                                           });
            };
            _endpoint->Poll(handler);
            if (!_connection.IsFailed())
            {
                _connection.Update(now, [&](const std::span<const std::uint8_t> packet) { _endpoint->Send(_server, packet); });
            }
        }

        void SendReliable(const std::vector<std::uint8_t> &message) { (void)_connection.SendReliable(message); }
        void SendUnreliable(const std::vector<std::uint8_t> &message) { (void)_connection.SendUnreliable(message); }

        [[nodiscard]] std::optional<ServerHelloMessage> Hello() const
        {
            for (const std::vector<std::uint8_t> &message : received)
            {
                const auto decoded = DecodeServerMessage(message);
                if (decoded.has_value())
                {
                    if (const auto *hello = std::get_if<ServerHelloMessage>(&*decoded))
                    {
                        return *hello;
                    }
                }
            }
            return std::nullopt;
        }

        std::vector<std::vector<std::uint8_t>> received;

    private:
        N2Net::SimTransport *_endpoint;
        N2Net::PeerId _server;
        N2Net::Connection _connection;
    };

    struct SessionConfig
    {
        N2Net::SimLinkConfig link;
        std::uint64_t seed = 1;
        InputLayout layout = GameLayout();
        ServerInputOptions input = [] {
            ServerInputOptions options;
            options.delayTicks = 0;
            return options;
        }();
        std::uint32_t startTick = 1000;
    };

    class Session
    {
    public:
        explicit Session(SessionConfig configuration = {})
            : config(std::move(configuration)), network(config.seed), serverPeer({1, 2, 3}, "server")
        {
            serverEndpoint = &network.AddEndpoint();
            NetworkServerOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            options.inputLayout = config.layout;
            options.input = config.input;
            server = std::make_unique<NetworkServer>(*serverEndpoint, serverPeer.world, std::move(options));
            server->SetCurrentTick(config.startTick);
        }

        ClientRig &AddClient(const std::optional<InputLayout> &layout = std::nullopt, const std::uint32_t redundancy = 4)
        {
            auto rig = std::make_unique<ClientRig>("client");
            N2Net::SimTransport &endpoint = network.AddEndpoint();
            rig->endpoint = &endpoint;
            network.Link(serverEndpoint->Id(), endpoint.Id(), config.link);

            NetworkClientOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            options.inputLayout = layout.has_value() ? *layout : config.layout;
            options.inputRedundancy = redundancy;
            rig->client = std::make_unique<NetworkClient>(endpoint, rig->peer.world, std::move(options));
            clients.push_back(std::move(rig));
            return *clients.back();
        }

        RawPeer &AddRaw()
        {
            N2Net::SimTransport &endpoint = network.AddEndpoint();
            network.Link(serverEndpoint->Id(), endpoint.Id(), config.link);
            raws.push_back(std::make_unique<RawPeer>(network.NowMicros(), serverEndpoint->Id(), endpoint,
                                                     MakeWorldConnectionConfig(kProtocol, endpoint)));
            return *raws.back();
        }

        void Step(const std::uint64_t micros = 1000)
        {
            network.Advance(micros);
            const std::uint64_t now = network.NowMicros();
            server->Update(now);
            for (const auto &rig : clients)
            {
                rig->client->Update(now);
            }
            for (const auto &raw : raws)
            {
                raw->Pump(now);
            }
        }

        bool RunUntil(const std::function<bool()> &done, const std::uint64_t limitMillis = 20'000)
        {
            for (std::uint64_t elapsed = 0; elapsed < limitMillis; ++elapsed)
            {
                if (done())
                {
                    return true;
                }
                Step();
            }
            return done();
        }

        void RunFor(const std::uint64_t millis)
        {
            for (std::uint64_t elapsed = 0; elapsed < millis; ++elapsed)
            {
                Step();
            }
        }

        /// Joins the client and returns its id
        ClientId Join(ClientRig &rig)
        {
            EXPECT_TRUE(RunUntil([&] { return rig.client->IsJoined(); }));
            return rig.client->LocalClientId();
        }

        /// A raw peer that accepts the server's fingerprint
        void JoinRaw(RawPeer &raw)
        {
            ASSERT_TRUE(RunUntil([&] { return raw.Hello().has_value(); }));
            ClientAcceptMessage accept;
            accept.fingerprint = raw.Hello()->fingerprint;
            raw.SendReliable(Encode(accept));
            const std::size_t before = server->JoinedClientCount();
            ASSERT_TRUE(RunUntil([&] { return server->JoinedClientCount() == before + 1; }));
        }

        SessionConfig config;
        N2Net::SimNetwork network;
        Peer serverPeer;
        N2Net::SimTransport *serverEndpoint = nullptr;
        std::unique_ptr<NetworkServer> server;
        std::vector<std::unique_ptr<ClientRig>> clients;
        std::vector<std::unique_ptr<RawPeer>> raws;
    };

    /// What the server read for each of the stream's ticks
    struct StreamResult
    {
        std::vector<InputSource> sources;
        std::vector<std::vector<float>> values;
        std::uint64_t notFrames = 0;
        std::uint64_t wrongValues = 0;
    };

    /// A client queues one frame per 20 ms tick (stamped with the server's tick) over an impaired link and the server reads
    /// the input of its pawn every tick, `delay` ticks after the stamp. Counts the ticks that did not get their frame.
    StreamResult RunStream(const std::uint32_t redundancy, const N2Net::SimLinkConfig &impaired, const std::uint64_t seed)
    {
        SessionConfig config;
        config.seed = seed;
        config.input.delayTicks = kMaxInputDelayTicks;
        Session session(config);
        ClientRig &rig = session.AddClient(std::nullopt, redundancy);
        const ClientId clientId = session.Join(rig);
        const auto pawn = session.server->Spawn("InputSessionTest_Pawn", clientId);
        EXPECT_TRUE(pawn.has_value());

        session.network.SetLinkConfig(session.serverEndpoint->Id(), rig.endpoint->Id(), impaired);
        session.network.SetLinkConfig(rig.endpoint->Id(), session.serverEndpoint->Id(), impaired);

        StreamResult result;
        constexpr std::uint32_t kTicks = 300;
        const std::uint32_t delay = kMaxInputDelayTicks;
        const InputLayout layout = GameLayout();
        for (std::uint32_t i = 0; i < kTicks + delay; ++i)
        {
            const std::uint32_t tick = session.config.startTick + i;
            session.server->SetCurrentTick(tick);
            if (i >= delay && pawn.has_value())
            {
                const auto input = session.server->InputFor(*pawn, tick);
                EXPECT_TRUE(input.has_value());
                if (input.has_value())
                {
                    result.sources.push_back(input->source);
                    result.values.push_back(input->values);
                    if (input->source != InputSource::Frame)
                    {
                        ++result.notFrames;
                    }
                    else if (input->values != OnTheWire(layout, StreamValues(tick - delay)))
                    {
                        ++result.wrongValues;
                    }
                }
            }
            if (i < kTicks)
            {
                EXPECT_TRUE(rig.client->QueueInput(tick, StreamValues(tick)).has_value());
            }
            session.RunFor(20);
        }
        return result;
    }

    N2Net::SimLinkConfig Impaired()
    {
        N2Net::SimLinkConfig link;
        link.latencyMicros = 30'000;
        link.jitterMicros = 5'000;
        link.lossPpm = 100'000;
        link.reorderPpm = 100'000;
        link.reorderDelayMicros = 30'000;
        link.duplicatePpm = 50'000;
        return link;
    }
}

TEST(NetworkInputSessionTest, InputReachesOnlyTheObjectsItsClientOwns)
{
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    Session session;
    ClientRig &first = session.AddClient();
    ClientRig &second = session.AddClient();
    const ClientId a = session.Join(first);
    const ClientId b = session.Join(second);
    ASSERT_NE(a, b);

    const auto pawnA = session.server->Spawn(prefab.Name(), a);
    const auto pawnB = session.server->Spawn(prefab.Name(), b);
    ASSERT_TRUE(pawnA.has_value());
    ASSERT_TRUE(pawnB.has_value());

    const std::uint32_t tick = session.config.startTick;
    ASSERT_TRUE(first.client->QueueInput(tick, Values(1.0f, 0.5f, -0.5f, 1.0f)).has_value());
    ASSERT_TRUE(second.client->QueueInput(tick, Values(0.0f, -1.0f, 1.0f, 0.25f)).has_value());
    session.RunFor(100);

    const InputLayout layout = GameLayout();
    const auto inputA = session.server->InputFor(*pawnA, tick);
    const auto inputB = session.server->InputFor(*pawnB, tick);
    ASSERT_TRUE(inputA.has_value());
    ASSERT_TRUE(inputB.has_value());
    EXPECT_EQ(inputA->client, a);
    EXPECT_EQ(inputB->client, b);
    EXPECT_EQ(inputA->source, InputSource::Frame);
    EXPECT_EQ(inputB->source, InputSource::Frame);
    EXPECT_EQ(inputA->values, OnTheWire(layout, Values(1.0f, 0.5f, -0.5f, 1.0f)));
    EXPECT_EQ(inputB->values, OnTheWire(layout, Values(0.0f, -1.0f, 1.0f, 0.25f)));
    EXPECT_EQ(inputA->tick, tick);

    // Scene objects and the server's own objects take no client input; unknown ids neither
    EXPECT_FALSE(session.server->InputFor(NetworkId{1}, tick).has_value());
    const auto serverOwned = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(serverOwned.has_value());
    EXPECT_FALSE(session.server->InputFor(*serverOwned, tick).has_value());
    EXPECT_FALSE(session.server->InputFor(NetworkId{999}, tick).has_value());
    EXPECT_FALSE(session.server->InputFor(NetworkId{0}, tick).has_value());
    EXPECT_FALSE(session.server->InputForClient(999, tick).has_value());
}

TEST(NetworkInputSessionTest, AClientThatSendsNothingDoesNotGetTheOtherClientsInput)
{
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    Session session;
    ClientRig &quiet = session.AddClient();
    ClientRig &busy = session.AddClient();
    const ClientId quietId = session.Join(quiet);
    (void)session.Join(busy);
    const auto pawn = session.server->Spawn(prefab.Name(), quietId);
    ASSERT_TRUE(pawn.has_value());

    const std::uint32_t tick = session.config.startTick;
    ASSERT_TRUE(busy.client->QueueInput(tick, Values(1.0f, 1.0f, 1.0f, 1.0f)).has_value());
    session.RunFor(100);

    const auto input = session.server->InputFor(*pawn, tick);
    ASSERT_TRUE(input.has_value());
    EXPECT_EQ(input->source, InputSource::Neutral);
    EXPECT_EQ(input->values, GameLayout().NeutralValues());
    EXPECT_EQ(session.server->InputStatsFor(quietId)->framesAccepted, 0u);
}

TEST(NetworkInputSessionTest, TheServerKeepsOnlyTheInputOfClientsThatAreStillThere)
{
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const auto pawn = session.server->Spawn(prefab.Name(), id);
    ASSERT_TRUE(pawn.has_value());
    const std::uint32_t tick = session.config.startTick;
    ASSERT_TRUE(rig.client->QueueInput(tick, Values(1.0f, 0.0f, 0.0f, 0.0f)).has_value());
    session.RunFor(100);
    ASSERT_TRUE(session.server->InputFor(*pawn, tick).has_value());
    const std::uint64_t accepted = session.server->InputStatsFor(id)->framesAccepted;
    ASSERT_GT(accepted, 0u);

    rig.client->Leave();
    ASSERT_TRUE(session.RunUntil([&] { return session.server->ClientCount() == 0; }));
    // The object still names its owner, but the owner has no input any more; the totals keep what it did
    EXPECT_FALSE(session.server->InputFor(*pawn, tick).has_value());
    EXPECT_FALSE(session.server->InputStatsFor(id).has_value());
    EXPECT_EQ(session.server->InputStats().framesAccepted, accepted);
}

TEST(NetworkInputSessionTest, TheServerAcknowledgesAndTheClientTrimsItsRing)
{
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const std::uint32_t start = session.config.startTick;
    for (std::uint32_t i = 0; i < 10; ++i)
    {
        ASSERT_TRUE(rig.client->QueueInput(start + i, StreamValues(i)).has_value());
        session.RunFor(5);
    }
    EXPECT_GT(rig.client->InputStats().messagesBuilt, 0u);
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->UnackedInputFrames() == 0; }));
    EXPECT_EQ(rig.client->InputStats().framesAcked, 10u);
    EXPECT_EQ(rig.client->InputStats().acksInvalid, 0u);
    EXPECT_EQ(session.server->InputStatsFor(id)->framesAccepted, 10u);
    EXPECT_TRUE(rig.client->IsJoined());
}

TEST(NetworkInputSessionTest, RedundancyKeepsEveryInputThroughLossReorderingAndDuplication)
{
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    const StreamResult redundant = RunStream(4, Impaired(), 7);
    ASSERT_EQ(redundant.sources.size(), 300u);
    // Every tick got its own frame, with the values that were sent
    EXPECT_EQ(redundant.notFrames, 0u);
    EXPECT_EQ(redundant.wrongValues, 0u);

    // The control: one frame per message under the same impairments does lose ticks
    const StreamResult single = RunStream(1, Impaired(), 7);
    ASSERT_EQ(single.sources.size(), 300u);
    EXPECT_GT(single.notFrames, 10u);
    EXPECT_EQ(single.wrongValues, 0u);
}

TEST(NetworkInputSessionTest, TheSameSeedGivesTheSameInputToTheLastBit)
{
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    const StreamResult first = RunStream(2, Impaired(), 99);
    const StreamResult second = RunStream(2, Impaired(), 99);
    EXPECT_EQ(first.sources, second.sources);
    EXPECT_EQ(first.values, second.values);
    // With only two frames per message some ticks are missing, so the run holds the policy's output too
    EXPECT_GT(std::count(first.sources.begin(), first.sources.end(), InputSource::Repeated) +
                  std::count(first.sources.begin(), first.sources.end(), InputSource::Neutral),
              0);
}

TEST(NetworkInputSessionTest, AMissingInputRepeatsTheLastOneThenGoesNeutralAndCountsIt)
{
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    SessionConfig config;
    config.input.repeatLastTicks = 3;
    Session session(config);
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const auto pawn = session.server->Spawn(prefab.Name(), id);
    ASSERT_TRUE(pawn.has_value());

    const std::uint32_t start = config.startTick;
    for (std::uint32_t i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(rig.client->QueueInput(start + i, Values(1.0f, 0.5f, 0.5f, 0.5f)).has_value());
        session.RunFor(5);
    }
    session.RunFor(100);

    std::vector<InputSource> sources;
    for (std::uint32_t i = 0; i < 10; ++i)
    {
        sources.push_back(session.server->InputFor(*pawn, start + i)->source);
    }
    const std::vector<InputSource> expected = {InputSource::Frame,    InputSource::Frame,   InputSource::Frame,
                                               InputSource::Repeated, InputSource::Repeated, InputSource::Repeated,
                                               InputSource::Neutral,  InputSource::Neutral, InputSource::Neutral,
                                               InputSource::Neutral};
    EXPECT_EQ(sources, expected);
    EXPECT_EQ(session.server->InputFor(*pawn, start + 6)->values, GameLayout().NeutralValues());
    EXPECT_EQ(session.server->InputFor(*pawn, start + 4)->values, OnTheWire(GameLayout(), Values(1.0f, 0.5f, 0.5f, 0.5f)));

    const ServerInputStats stats = *session.server->InputStatsFor(id);
    EXPECT_EQ(stats.ticksFromFrames, 3u);
    EXPECT_EQ(stats.ticksRepeated, 3u);
    EXPECT_EQ(stats.ticksNeutral, 4u);
}

TEST(NetworkInputSessionTest, FramesFarAheadAreRefusedAndTheBufferStaysBounded)
{
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    SessionConfig config;
    config.input.maxFutureTicks = 8;
    Session session(config);
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);

    // 400 messages in 400 ms (under the rate limit), each a frame one tick further ahead of the server's tick, which
    // does not move
    const std::uint32_t start = config.startTick;
    for (std::uint32_t i = 0; i < 400; ++i)
    {
        ASSERT_TRUE(rig.client->QueueInput(start + i, StreamValues(i)).has_value());
        session.Step();
    }
    session.RunFor(50);
    EXPECT_TRUE(rig.client->IsJoined());
    EXPECT_TRUE(session.server->IsClientJoined(id));
    const ServerInputStats stats = *session.server->InputStatsFor(id);
    EXPECT_EQ(stats.framesAccepted, 9u); // ticks start to start + 8
    EXPECT_GT(stats.framesTooFar, 100u);
    // A frame that is too far ahead is not acknowledged, so the client keeps sending it; the ring is bounded anyway
    EXPECT_LE(rig.client->UnackedInputFrames(), kMaxUnackedInputFrames);
}

TEST(NetworkInputSessionTest, MoreInputMessagesThanAllowedDropTheClient)
{
    SessionConfig config;
    config.input.maxMessagesPerSecond = 20;
    Session session(config);
    ClientRig &rig = session.AddClient();
    (void)session.Join(rig);

    const std::uint32_t start = config.startTick;
    for (std::uint32_t i = 0; i < 200 && session.server->JoinedClientCount() == 1; ++i)
    {
        (void)rig.client->QueueInput(start + i, StreamValues(i));
        session.Step();
    }
    EXPECT_EQ(session.server->JoinedClientCount(), 0u);
    EXPECT_EQ(session.server->Stats().droppedRateLimited, 1u);
    EXPECT_EQ(session.server->Stats().clientsDropped, 1u);
}

TEST(NetworkInputSessionTest, InputAtTheLimitIsNotAnOffence)
{
    SessionConfig config;
    config.input.maxMessagesPerSecond = 60;
    Session session(config);
    ClientRig &rig = session.AddClient();
    (void)session.Join(rig);

    // One message every 20 ms for two seconds: 50 a second
    const std::uint32_t start = config.startTick;
    for (std::uint32_t i = 0; i < 100; ++i)
    {
        session.server->SetCurrentTick(start + i);
        ASSERT_TRUE(rig.client->QueueInput(start + i, StreamValues(i)).has_value());
        session.RunFor(20);
    }
    EXPECT_EQ(session.server->JoinedClientCount(), 1u);
    EXPECT_EQ(session.server->Stats().droppedRateLimited, 0u);
}

TEST(NetworkInputSessionTest, HostilePeersAreDroppedAndTheirInputNeverReachesTheGame)
{
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    const InputLayout layout = GameLayout();
    const std::uint32_t tick = SessionConfig().startTick;

    const auto valid = [&](const std::uint32_t sequence)
    {
        InputFrame frame;
        frame.tick = tick;
        frame.sequence = sequence;
        frame.payload = layout.Pack(Values(1.0f, 1.0f, 1.0f, 1.0f)).value();
        return EncodeInputFrames(std::vector<InputFrame>{frame}, layout.PayloadBytes());
    };

    struct Attack
    {
        const char *name;
        std::function<std::vector<std::uint8_t>()> message;
        std::uint64_t NetworkServerStats::*counter;
    };
    const std::vector<Attack> attacks = {
        {"payload with padding bits set",
         [&]
         {
             std::vector<std::uint8_t> bytes = valid(1);
             bytes.back() = 0xFF;
             return bytes;
         },
         &NetworkServerStats::droppedMalformed},
        {"truncated",
         [&]
         {
             std::vector<std::uint8_t> bytes = valid(1);
             bytes.pop_back();
             return bytes;
         },
         &NetworkServerStats::droppedMalformed},
        {"trailing bytes",
         [&]
         {
             std::vector<std::uint8_t> bytes = valid(1);
             bytes.push_back(0);
             return bytes;
         },
         &NetworkServerStats::droppedMalformed},
        {"a count of 255",
         [&]
         {
             std::vector<std::uint8_t> bytes = valid(1);
             bytes[1] = 255;
             return bytes;
         },
         &NetworkServerStats::droppedMalformed},
        {"sequence 0", [&] { return valid(0); }, &NetworkServerStats::droppedMalformed},
        {"a server-only input ack", [&] { return Encode(InputAckMessage{1, 0}); }, &NetworkServerStats::droppedProtocol},
        {"a state update", [&] { return std::vector<std::uint8_t>{kStateUpdateType, 1, 0, 0, 0, 1, 0}; },
         &NetworkServerStats::droppedProtocol},
    };

    for (const Attack &attack : attacks)
    {
        SCOPED_TRACE(attack.name);
        Session session;
        const auto pawn = session.server->Spawn(prefab.Name());
        ASSERT_TRUE(pawn.has_value());
        RawPeer &raw = session.AddRaw();
        session.JoinRaw(raw);
        const ClientId id = session.server->JoinedClients().front();
        raw.SendUnreliable(attack.message());
        ASSERT_TRUE(session.RunUntil([&] { return session.server->JoinedClientCount() == 0; }));
        EXPECT_EQ(session.server->Stats().*attack.counter, 1u);
        EXPECT_EQ(session.server->Stats().clientsDropped, 1u);
        EXPECT_FALSE(session.server->InputForClient(id, tick).has_value());
        EXPECT_EQ(session.server->InputStats().framesAccepted, 0u);
    }
}

TEST(NetworkInputSessionTest, AnHonestRawPeerIsAccepted)
{
    // The control for the hostile test: the same raw peer sending a good frame is neither dropped nor ignored
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    Session session;
    RawPeer &raw = session.AddRaw();
    session.JoinRaw(raw);
    const ClientId id = session.server->JoinedClients().front();
    const InputLayout layout = GameLayout();
    InputFrame frame;
    frame.tick = session.config.startTick;
    frame.sequence = 1;
    frame.payload = layout.Pack(Values(1.0f, 1.0f, 0.0f, 1.0f)).value();
    raw.SendUnreliable(EncodeInputFrames(std::vector<InputFrame>{frame}, layout.PayloadBytes()));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->InputStatsFor(id)->framesAccepted == 1; }));
    EXPECT_EQ(session.server->JoinedClientCount(), 1u);
    const auto input = session.server->InputForClient(id, session.config.startTick);
    ASSERT_TRUE(input.has_value());
    EXPECT_EQ(input->source, InputSource::Frame);
}

TEST(NetworkInputSessionTest, InputBeforeJoiningDropsTheClient)
{
    Session session;
    RawPeer &raw = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
    const InputLayout layout = GameLayout();
    InputFrame frame;
    frame.tick = 1;
    frame.sequence = 1;
    frame.payload = layout.NeutralPayload();
    raw.SendUnreliable(EncodeInputFrames(std::vector<InputFrame>{frame}, layout.PayloadBytes()));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().clientsDropped == 1; }));
    EXPECT_EQ(session.server->Stats().droppedProtocol, 1u);
    EXPECT_EQ(session.server->InputStats().framesReceived, 0u);
}

TEST(NetworkInputSessionTest, AServerWithNoLayoutTakesNoInput)
{
    SessionConfig config;
    config.layout = InputLayout();
    Session session(config);
    ClientRig &rig = session.AddClient();
    (void)session.Join(rig);
    const auto queued = rig.client->QueueInput(1, std::vector<float>{});
    EXPECT_FALSE(queued.has_value());
    EXPECT_FALSE(session.server->InputForClient(rig.client->LocalClientId(), 1).has_value());

    // A peer that sends input anyway is dropped
    RawPeer &raw = session.AddRaw();
    session.JoinRaw(raw);
    InputFrame frame;
    frame.tick = 1;
    frame.sequence = 1;
    raw.SendUnreliable(EncodeInputFrames(std::vector<InputFrame>{frame}, 0));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().clientsDropped == 1; }));
    EXPECT_EQ(session.server->Stats().droppedProtocol, 1u);
    EXPECT_TRUE(rig.client->IsJoined());
}

TEST(NetworkInputSessionTest, ADifferentInputLayoutRefusesToJoin)
{
    Session session;
    ClientRig &rig = session.AddClient(OtherLayout());
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsFinished(); }));
    EXPECT_EQ(rig.client->State(), NetworkClientState::Refused);
    EXPECT_EQ(rig.client->Refusal(), JoinRefusal::Registry);
    EXPECT_NE(rig.client->Error().find("input layout"), std::string::npos);
    EXPECT_EQ(session.server->JoinedClientCount(), 0u);

    // Same controls, another quantisation: also refused
    Session session2;
    InputLayout coarser = InputLayout::Create({Spec("throttle", InputControlKind::Axis, 5, false),
                                               Spec("move", InputControlKind::Vector2, 8, true),
                                               Spec("jump", InputControlKind::Button, 1, false)})
                              .value();
    ClientRig &rig2 = session2.AddClient(coarser);
    ASSERT_TRUE(session2.RunUntil([&] { return rig2.client->IsFinished(); }));
    EXPECT_EQ(rig2.client->Refusal(), JoinRefusal::Registry);
}

TEST(NetworkInputSessionTest, InputCanBeQueuedOnlyOnceJoined)
{
    Session session;
    ClientRig &rig = session.AddClient();
    EXPECT_FALSE(rig.client->QueueInput(1, Values(0, 0, 0, 0)).has_value());
    (void)session.Join(rig);
    EXPECT_TRUE(rig.client->QueueInput(1, Values(0, 0, 0, 0)).has_value());
    EXPECT_FALSE(rig.client->QueueInput(1, Values(0, 0, 0, 0)).has_value()); // the tick must rise
    EXPECT_FALSE(rig.client->QueueInput(2, std::vector<float>{1.0f}).has_value());
}

TEST(NetworkInputSessionTest, TheActiveServerIsTheLastOneMadeAndAliveOnly)
{
    EXPECT_EQ(NetworkServer::Active(), nullptr);
    {
        Session session;
        EXPECT_EQ(NetworkServer::Active(), session.server.get());
    }
    EXPECT_EQ(NetworkServer::Active(), nullptr);
}

// ---- Lua -------------------------------------------------------------------------------------------------------------

namespace
{
    sol::state &Lua()
    {
        return Scripting::LuaRuntime::Instance().GetState();
    }

    /// Runs a chunk; true when it ran without a Lua error
    bool RunLua(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error error = result;
            (void)error;
            return false;
        }
        return true;
    }

    /// Evaluates a Lua expression; true when it is true
    bool LuaIsTrue(const std::string &expression)
    {
        const sol::protected_function_result result = Lua().safe_script("return " + expression, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error error = result;
            (void)error;
            return false;
        }
        return result.get<bool>();
    }

    class NetworkInputLuaTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite() { ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize()); }
        void SetUp() override { SetDeclaredInputLayout(InputLayout()); }
        void TearDown() override { SetDeclaredInputLayout(InputLayout()); }
    };
}

TEST_F(NetworkInputLuaTest, DeclareInputLayoutBuildsTheSameLayoutAsCpp)
{
    ASSERT_TRUE(RunLua(R"(
        Network.DeclareInputLayout({
            { name = "jump", kind = "button" },
            { name = "move", kind = "vector2", bits = 8, range = 1 },
            { name = "throttle", kind = "axis", bits = 6, range = 1, signed = false },
        })
    )"));
    const InputLayout &declared = DeclaredInputLayout();
    EXPECT_EQ(declared.ControlCount(), 3u);
    EXPECT_EQ(declared.Hash(), GameLayout().Hash());
    EXPECT_EQ(declared.PayloadBytes(), GameLayout().PayloadBytes());

    // Declaration order does not matter, and the last declaration wins
    ASSERT_TRUE(RunLua(R"(Network.DeclareInputLayout({ { name = "z", kind = "button" }, { name = "a", kind = "vector3", bits = 4, range = 2.5 } }))"));
    EXPECT_EQ(DeclaredInputLayout().ControlCount(), 2u);
    EXPECT_EQ(DeclaredInputLayout().Control(0).name, "a");
    EXPECT_EQ(DeclaredInputLayout().Control(0).kind, InputControlKind::Vector3);
    EXPECT_EQ(DeclaredInputLayout().Control(0).range, 2.5f);

    // The defaults of a field left out: 8 bits, range 1, signed
    ASSERT_TRUE(RunLua(R"(Network.DeclareInputLayout({ { name = "aim", kind = "axis" } }))"));
    EXPECT_EQ(DeclaredInputLayout().Control(0).bits, 8);
    EXPECT_EQ(DeclaredInputLayout().Control(0).range, 1.0f);
    EXPECT_TRUE(DeclaredInputLayout().Control(0).isSigned);
}

TEST_F(NetworkInputLuaTest, DeclareInputLayoutRefusesEverythingMalformedAndChangesNothing)
{
    SetDeclaredInputLayout(GameLayout());
    const std::vector<std::string> bad = {
        "Network.DeclareInputLayout()",
        "Network.DeclareInputLayout(nil)",
        "Network.DeclareInputLayout(false)",
        "Network.DeclareInputLayout(5)",
        "Network.DeclareInputLayout('jump')",
        "Network.DeclareInputLayout({ 5 })",
        "Network.DeclareInputLayout({ { } })",
        "Network.DeclareInputLayout({ { name = 'a' } })",
        "Network.DeclareInputLayout({ { kind = 'button' } })",
        "Network.DeclareInputLayout({ { name = 5, kind = 'button' } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'nope' } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'Button' } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 5 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', bits = 99 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', bits = 0 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', bits = 17 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', bits = 1 } })", // a signed axis needs 2
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', bits = 2.5 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', bits = '8' } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', range = 0 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', range = -1 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', range = 0/0 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', range = math.huge } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', range = 1e30 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', range = 'big' } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'axis', signed = 1 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'button', extra = 1 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'button', [1] = 1 } })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'button' }, { name = 'a', kind = 'button' } })",
        "Network.DeclareInputLayout({ { name = 'bad name', kind = 'button' } })",
        "Network.DeclareInputLayout({ { name = '', kind = 'button' } })",
        "Network.DeclareInputLayout({ { name = '1a', kind = 'button' } })",
        "Network.DeclareInputLayout({ { name = string.rep('a', 33), kind = 'button' } })",
        "Network.DeclareInputLayout({ name = 'a', kind = 'button' })",
        "Network.DeclareInputLayout({ { name = 'a', kind = 'button' }, extra = true })",
        // 33 controls, and a layout with too many bits
        "local t = {} for i = 1, 33 do t[i] = { name = 'c' .. i, kind = 'button' } end Network.DeclareInputLayout(t)",
        "local t = {} for i = 1, 11 do t[i] = { name = 'v' .. i, kind = 'vector3', bits = 8 } end Network.DeclareInputLayout(t)",
    };
    for (const std::string &code : bad)
    {
        SCOPED_TRACE(code);
        EXPECT_FALSE(RunLua(code));
        EXPECT_EQ(DeclaredInputLayout().Hash(), GameLayout().Hash());
    }

    // A plain empty list is a layout with no controls
    EXPECT_TRUE(RunLua("Network.DeclareInputLayout({})"));
    EXPECT_TRUE(DeclaredInputLayout().Empty());
}

TEST_F(NetworkInputLuaTest, GetInputNeedsAGameObjectAndAServer)
{
    EXPECT_FALSE(RunLua("Network.GetInput()"));
    EXPECT_FALSE(RunLua("Network.GetInput(nil)"));
    EXPECT_FALSE(RunLua("Network.GetInput(false)"));
    EXPECT_FALSE(RunLua("Network.GetInput(5)"));
    // No server in this process: nil, not an error
    ASSERT_EQ(NetworkServer::Active(), nullptr);
    EXPECT_TRUE(RunLua("local go = GameObject.Create('lonely') noServerInput = Network.GetInput(go)"));
    EXPECT_TRUE(LuaIsTrue("noServerInput == nil"));
}

TEST_F(NetworkInputLuaTest, GetInputReturnsTheOwnersInputForTheCurrentTick)
{
    const ScopedPrefab prefab("InputSessionTest_Pawn");
    SessionConfig config;
    config.startTick = static_cast<std::uint32_t>(Time::GetTick());
    Session session(config);
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const auto pawn = session.server->Spawn(prefab.Name(), id);
    const auto serverPawn = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(pawn.has_value());
    ASSERT_TRUE(serverPawn.has_value());

    ASSERT_TRUE(rig.client->QueueInput(config.startTick, Values(1.0f, 1.0f, -1.0f, 0.0f)).has_value());
    session.RunFor(100);

    Lua()["inputPawn"] = Scripting::GameObjectRef(session.serverPeer.world.Find(*pawn)->GetGameObject());
    Lua()["serverPawn"] = Scripting::GameObjectRef(session.serverPeer.world.Find(*serverPawn)->GetGameObject());

    ASSERT_TRUE(RunLua("gotInput, gotSource = Network.GetInput(inputPawn)"));
    ASSERT_TRUE(Lua()["gotInput"].get_type() == sol::type::table);
    EXPECT_EQ(Lua()["gotSource"].get<std::string>(), "frame");
    ASSERT_TRUE(RunLua(R"(
        gotJump = gotInput.jump
        gotMoveX = gotInput.move.x
        gotMoveY = gotInput.move.y
        gotThrottle = gotInput.throttle
    )"));
    EXPECT_EQ(Lua()["gotJump"].get<bool>(), true);
    EXPECT_EQ(Lua()["gotMoveX"].get<float>(), 1.0f);
    EXPECT_EQ(Lua()["gotMoveY"].get<float>(), -1.0f);
    EXPECT_EQ(Lua()["gotThrottle"].get<float>(), 0.0f);

    // The server's own object takes no client input
    ASSERT_TRUE(RunLua("serverInput = Network.GetInput(serverPawn)"));
    EXPECT_TRUE(LuaIsTrue("serverInput == nil"));

    Lua()["inputPawn"] = sol::lua_nil;
    Lua()["serverPawn"] = sol::lua_nil;
}

TEST_F(NetworkInputLuaTest, AFailedCallLeavesTheLuaCallStackAsItWas)
{
    // A call that raises an error must unwind completely: nothing left on the stack and no frame left open (other suites
    // run in the same Lua state and assert an empty stack)
    const auto depth = [] {
        lua_Debug frame;
        int levels = 0;
        while (lua_getstack(Lua().lua_state(), levels, &frame))
        {
            ++levels;
        }
        return levels;
    };
    ASSERT_EQ(depth(), 0);
    const int top = lua_gettop(Lua().lua_state());
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(RunLua("Network.DeclareInputLayout(nil)"));
        EXPECT_FALSE(RunLua("Network.DeclareInputLayout({ { name = 'a' } })"));
        EXPECT_FALSE(RunLua("Network.GetInput(nil)"));
        EXPECT_FALSE(RunLua("Network.GetInput(5)"));
    }
    EXPECT_EQ(depth(), 0);
    EXPECT_EQ(lua_gettop(Lua().lua_state()), top);
}
