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
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/SimTransport.hpp"

// The world protocol end to end on the network simulator (netcode stage B1): a NetworkServer and NetworkClients in one
// process, each with a scene and a world of its own, over seeded virtual links with no sleeps. Includes the hostile
// peers: raw Connections that say what no honest client says.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;

namespace
{
    constexpr std::uint32_t kProtocol = 0x4E32'4E54;
    constexpr std::uint64_t kSceneHash = 0x5CE11E5CE11Eull;
    constexpr const char *kScenePath = "res://test.scene";

    /// A scene and the world bound to it. The world is declared after the scene, so it is destroyed first.
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

    std::vector<std::uint32_t> IdsOf(const NetworkWorld &world)
    {
        std::vector<std::uint32_t> ids;
        for (const auto &entry : world.Objects())
        {
            ids.push_back(entry.first);
        }
        return ids;
    }

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

    struct ClientRig
    {
        ClientRig(const std::vector<std::uint32_t> &uuids, const std::string &name) : peer(uuids, name) {}

        Peer peer;
        N2Net::SimTransport *endpoint = nullptr;
        std::unique_ptr<NetworkClient> client;
        std::vector<NetworkSessionEvent> events;

        [[nodiscard]] std::size_t Count(const NetworkSessionEvent::Type type) const
        {
            return static_cast<std::size_t>(
                std::count_if(events.begin(), events.end(), [type](const NetworkSessionEvent &e) { return e.type == type; }));
        }
    };

    struct SessionConfig
    {
        N2Net::SimLinkConfig link;
        std::uint64_t seed = 1;
        std::vector<std::uint32_t> sceneObjects = {1, 2, 3};
        std::uint32_t maxClients = 16;
        std::uint64_t joinTimeoutMicros = 10'000'000;
        std::uint32_t maxClientMessagesPerSecond = 32;
    };

    /// A NetworkServer with its scene and world, and the clients and raw peers linked to it, on one SimNetwork. Prefabs
    /// the test wants in the server's table are registered (ScopedPrefab) before the Session is made.
    class Session
    {
    public:
        explicit Session(SessionConfig configuration = {})
            : config(std::move(configuration)), network(config.seed), serverPeer(config.sceneObjects, "server")
        {
            serverEndpoint = &network.AddEndpoint();
            NetworkServerOptions options;
            options.protocolId = kProtocol;
            options.maxClients = config.maxClients;
            options.joinTimeoutMicros = config.joinTimeoutMicros;
            options.maxClientMessagesPerSecond = config.maxClientMessagesPerSecond;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            server = std::make_unique<NetworkServer>(*serverEndpoint, serverPeer.world, std::move(options));
            server->SetClientObserver([this](const NetworkClientEvent &event) { serverEvents.push_back(event); });
        }

        ClientRig &AddClient(const std::vector<std::uint32_t> &sceneObjects = {1, 2, 3},
                             const std::uint64_t sceneHash = kSceneHash,
                             std::optional<PrefabTable> prefabs = std::nullopt)
        {
            auto rig = std::make_unique<ClientRig>(sceneObjects, "client");
            N2Net::SimTransport &endpoint = network.AddEndpoint();
            rig->endpoint = &endpoint;
            network.Link(serverEndpoint->Id(), endpoint.Id(), config.link);

            NetworkClientOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = sceneHash;
            options.prefabs = prefabs.has_value() ? std::move(*prefabs) : PrefabTable::Capture();
            rig->client = std::make_unique<NetworkClient>(endpoint, rig->peer.world, std::move(options));
            ClientRig *const raw = rig.get();
            rig->client->SetEventObserver([raw](const NetworkSessionEvent &event) { raw->events.push_back(event); });
            clients.push_back(std::move(rig));
            return *clients.back();
        }

        RawPeer &AddRaw(const std::uint32_t maxMessageSize = static_cast<std::uint32_t>(kMaxMessageBytes))
        {
            N2Net::SimTransport &endpoint = network.AddEndpoint();
            network.Link(serverEndpoint->Id(), endpoint.Id(), config.link);
            N2Net::ConnectionConfig connection = MakeWorldConnectionConfig(kProtocol, endpoint);
            connection.maxMessageSize = maxMessageSize;
            raws.push_back(std::make_unique<RawPeer>(network.NowMicros(), serverEndpoint->Id(), endpoint, connection));
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

        bool RunUntil(const std::function<bool()> &done, const std::uint64_t limitMicros = 30'000'000)
        {
            for (std::uint64_t elapsed = 0; elapsed < limitMicros; elapsed += 1000)
            {
                if (done())
                {
                    return true;
                }
                Step();
            }
            return done();
        }

        void RunFor(const std::uint64_t micros)
        {
            for (std::uint64_t elapsed = 0; elapsed < micros; elapsed += 1000)
            {
                Step();
            }
        }

        SessionConfig config;
        N2Net::SimNetwork network;
        Peer serverPeer;
        N2Net::SimTransport *serverEndpoint = nullptr;
        std::unique_ptr<NetworkServer> server;
        std::vector<NetworkClientEvent> serverEvents;
        std::vector<std::unique_ptr<ClientRig>> clients;
        std::vector<std::unique_ptr<RawPeer>> raws;
    };

    /// A raw peer that has joined: it accepted the server's fingerprint
    void JoinRaw(Session &session, RawPeer &raw)
    {
        ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
        ClientAcceptMessage accept;
        accept.fingerprint = raw.Hello()->fingerprint;
        raw.SendReliable(Encode(accept));
        const std::size_t before = session.server->JoinedClientCount();
        ASSERT_TRUE(session.RunUntil([&] { return session.server->JoinedClientCount() == before + 1; }));
    }

    /// The server dropped everything a hostile peer sent and kept its world as it was
    void ExpectWorldUntouched(Session &session, const std::size_t objects, const std::size_t roots)
    {
        EXPECT_EQ(session.serverPeer.world.Count(), objects);
        EXPECT_EQ(session.serverPeer.scene->GetRootGameObjectCount(), roots);
        EXPECT_EQ(session.server->Stats().spawnsSent, 0u);
        EXPECT_EQ(session.server->Stats().despawnsSent, 0u);
    }
}

TEST(NetworkSessionTest, AClientJoinsAndTheWorldsMatch)
{
    Session session;
    ASSERT_TRUE(session.serverPeer.bound.has_value());
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(rig.peer.bound.has_value());
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));

    EXPECT_EQ(rig.client->State(), NetworkClientState::Joined);
    EXPECT_EQ(rig.client->LocalClientId(), 1u);
    EXPECT_TRUE(rig.client->Error().empty());
    ASSERT_TRUE(rig.client->ServerFingerprint().has_value());
    EXPECT_EQ(rig.client->ServerFingerprint()->sceneHash, kSceneHash);
    EXPECT_EQ(rig.client->ServerFingerprint()->sceneObjectCount, 3u);
    EXPECT_EQ(rig.client->ServerFingerprint()->scenePath, kScenePath);
    EXPECT_EQ(CompareFingerprints(*rig.client->ServerFingerprint(), rig.client->LocalFingerprint()), JoinRefusal::None);

    EXPECT_EQ(session.server->JoinedClientCount(), 1u);
    EXPECT_TRUE(session.server->IsClientJoined(1));
    EXPECT_FALSE(session.server->IsClientJoined(2));
    EXPECT_EQ(session.server->JoinedClients(), std::vector<ClientId>{1});
    EXPECT_EQ(session.server->Stats().clientsJoined, 1u);
    ASSERT_EQ(rig.events.size(), 1u);
    EXPECT_EQ(rig.events[0].type, NetworkSessionEvent::Type::Joined);
    ASSERT_EQ(session.serverEvents.size(), 1u);
    EXPECT_EQ(session.serverEvents[0].type, NetworkClientEvent::Type::Joined);
    EXPECT_EQ(session.serverEvents[0].client, 1u);
    // Both sides gave every scene object the same id
    EXPECT_EQ(IdsOf(rig.peer.world), IdsOf(session.serverPeer.world));
}

TEST(NetworkSessionTest, ClientsGetIdsThatAreNeverReused)
{
    Session session;
    ClientRig &first = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return first.client->IsJoined(); }));
    EXPECT_EQ(first.client->LocalClientId(), 1u);

    first.client->Leave();
    ASSERT_TRUE(session.RunUntil([&] { return session.server->ClientCount() == 0; }));

    ClientRig &second = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return second.client->IsJoined(); }));
    EXPECT_EQ(second.client->LocalClientId(), 2u);
}

TEST(NetworkSessionTest, ASpawnReachesTheClientsThatJoined)
{
    const ScopedPrefab prefab("SessionTest_Spawn");
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    const std::size_t clientRoots = rig.peer.scene->GetRootGameObjectCount();

    const auto id = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->value, 4u); // the three scene objects have 1 to 3
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->Stats().spawnsApplied == 1; }));

    ASSERT_EQ(rig.events.size(), 2u);
    EXPECT_EQ(rig.events[1].type, NetworkSessionEvent::Type::Spawned);
    EXPECT_EQ(rig.events[1].id.value, 4u);
    EXPECT_EQ(rig.events[1].prefab, prefab.Name());
    EXPECT_EQ(rig.events[1].owner, kServerClientId);

    NetworkObject *const onServer = session.serverPeer.world.Find(*id);
    NetworkObject *const onClient = rig.peer.world.Find(*id);
    ASSERT_NE(onServer, nullptr);
    ASSERT_NE(onClient, nullptr);
    EXPECT_TRUE(onClient->IsSpawned());
    EXPECT_EQ(onClient->GetPrefabName(), prefab.Name());
    EXPECT_EQ(onClient->GetOwner(), kServerClientId);
    EXPECT_EQ(rig.peer.scene->GetRootGameObjectCount(), clientRoots + 1);
    EXPECT_EQ(IdsOf(rig.peer.world), IdsOf(session.serverPeer.world));
    EXPECT_EQ(session.server->Stats().spawnsSent, 1u);
}

TEST(NetworkSessionTest, ASpawnNamesItsOwnerAndOnlyAJoinedClientCanOwn)
{
    const ScopedPrefab prefab("SessionTest_Owner");
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));

    const auto owned = session.server->Spawn(prefab.Name(), rig.client->LocalClientId());
    ASSERT_TRUE(owned.has_value());
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->Stats().spawnsApplied == 1; }));
    EXPECT_EQ(rig.events.back().owner, rig.client->LocalClientId());
    EXPECT_EQ(rig.peer.world.Find(*owned)->GetOwner(), rig.client->LocalClientId());
    EXPECT_EQ(session.serverPeer.world.Find(*owned)->GetOwner(), rig.client->LocalClientId());

    const auto stranger = session.server->Spawn(prefab.Name(), 99);
    ASSERT_FALSE(stranger.has_value());
    EXPECT_NE(stranger.error().find("has not joined"), std::string::npos);
    EXPECT_EQ(session.serverPeer.world.Count(), 4u);
}

TEST(NetworkSessionTest, OnlyPrefabsOfTheSessionsTableCanBeSpawned)
{
    const ScopedPrefab inTable("SessionTest_InTable");
    Session session;
    const ScopedPrefab late("SessionTest_Late"); // registered after the table was frozen

    const auto missing = session.server->Spawn("SessionTest_NoSuchPrefab");
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().find("prefab table"), std::string::npos);

    const auto notFrozen = session.server->Spawn(late.Name());
    ASSERT_FALSE(notFrozen.has_value());
    EXPECT_NE(notFrozen.error().find("prefab table"), std::string::npos);

    // Nothing was spawned by the refusals
    EXPECT_EQ(session.serverPeer.world.Count(), 3u);
    EXPECT_TRUE(session.server->Spawn(inTable.Name()).has_value());
}

TEST(NetworkSessionTest, ADespawnReachesTheClientsAndIdsAreNeverReused)
{
    const ScopedPrefab prefab("SessionTest_Despawn");
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));

    const auto first = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(session.RunUntil([&] { return rig.peer.world.Find(*first) != nullptr; }));

    EXPECT_TRUE(session.server->Despawn(*first));
    EXPECT_FALSE(session.server->Despawn(*first));
    EXPECT_FALSE(session.server->Despawn(NetworkId{999}));
    ASSERT_TRUE(session.RunUntil([&] { return rig.peer.world.Find(*first) == nullptr; }));
    EXPECT_EQ(rig.client->Stats().despawnsApplied, 1u);
    EXPECT_EQ(rig.Count(NetworkSessionEvent::Type::Despawned), 1u);
    EXPECT_EQ(session.server->Stats().despawnsSent, 1u);

    const auto second = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->value, first->value + 1);
    ASSERT_TRUE(session.RunUntil([&] { return rig.peer.world.Find(*second) != nullptr; }));
    EXPECT_EQ(IdsOf(rig.peer.world), IdsOf(session.serverPeer.world));
}

TEST(NetworkSessionTest, DestroyingAnObjectOnTheServerDespawnsItEverywhere)
{
    const ScopedPrefab prefab("SessionTest_Destroy");
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    const auto spawned = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(spawned.has_value());
    ASSERT_TRUE(session.RunUntil([&] { return rig.peer.world.Find(*spawned) != nullptr; }));

    // The game's own code destroys a scene object and a spawned one: the server hears of it when the scene's destroy
    // pass runs, and replicates it
    FindByNumber(*session.serverPeer.scene, 2)->Destroy();
    session.serverPeer.world.Find(*spawned)->GetGameObject().Destroy();
    session.serverPeer.scene->ProcessDestroyed();
    ASSERT_TRUE(session.RunUntil([&] { return rig.peer.world.Find(NetworkId{2}) == nullptr &&
                                              rig.peer.world.Find(*spawned) == nullptr; }));
    EXPECT_EQ(rig.client->Stats().despawnsApplied, 2u);
    EXPECT_EQ(IdsOf(rig.peer.world), IdsOf(session.serverPeer.world));
    EXPECT_EQ(session.server->Stats().despawnsSent, 2u);
}

TEST(NetworkSessionTest, ALateJoinerIsToldWhatHappenedBeforeIt)
{
    const ScopedPrefab prefab("SessionTest_Late");
    Session session;
    ClientRig &early = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return early.client->IsJoined(); }));

    const auto a = session.server->Spawn(prefab.Name());
    const auto b = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(a.has_value() && b.has_value());
    EXPECT_TRUE(session.server->Despawn(NetworkId{2})); // a scene object
    EXPECT_TRUE(session.server->Despawn(*a));           // a spawned one
    session.RunFor(200'000);

    ClientRig &late = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return late.client->IsJoined(); }));
    // The destroyed scene object first, then the live spawned object, then the end of the join state
    ASSERT_EQ(late.events.size(), 3u);
    EXPECT_EQ(late.events[0].type, NetworkSessionEvent::Type::Despawned);
    EXPECT_EQ(late.events[0].id.value, 2u);
    EXPECT_EQ(late.events[1].type, NetworkSessionEvent::Type::Spawned);
    EXPECT_EQ(late.events[1].id, *b);
    EXPECT_EQ(late.events[2].type, NetworkSessionEvent::Type::Joined);
    EXPECT_EQ(IdsOf(late.peer.world), (std::vector<std::uint32_t>{1, 3, b->value}));
    EXPECT_EQ(IdsOf(late.peer.world), IdsOf(session.serverPeer.world));
    EXPECT_EQ(IdsOf(early.peer.world), IdsOf(session.serverPeer.world));
}

TEST(NetworkSessionTest, ASpawnBeforeAnyoneJoinedIsPartOfTheJoinState)
{
    const ScopedPrefab prefab("SessionTest_Before");
    Session session;
    const auto id = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(id.has_value());
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    EXPECT_NE(rig.peer.world.Find(*id), nullptr);
    EXPECT_EQ(rig.Count(NetworkSessionEvent::Type::Spawned), 1u);
}

TEST(NetworkSessionTest, TheWorldsConvergeOverALossyReorderingDuplicatingLink)
{
    const ScopedPrefab prefab("SessionTest_Lossy");
    SessionConfig config;
    config.seed = 20261021;
    config.link.latencyMicros = 25'000;
    config.link.jitterMicros = 10'000;
    config.link.lossPpm = 100'000;
    config.link.reorderPpm = 50'000;
    config.link.reorderDelayMicros = 30'000;
    config.link.duplicatePpm = 20'000;
    Session session(config);
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }, 60'000'000));

    std::vector<NetworkId> spawned;
    for (int i = 0; i < 12; ++i)
    {
        const auto id = session.server->Spawn(prefab.Name());
        ASSERT_TRUE(id.has_value());
        spawned.push_back(*id);
        session.RunFor(40'000);
    }
    EXPECT_TRUE(session.server->Despawn(spawned[3]));
    EXPECT_TRUE(session.server->Despawn(NetworkId{2}));
    ASSERT_TRUE(session.RunUntil([&] { return IdsOf(rig.peer.world) == IdsOf(session.serverPeer.world); }, 60'000'000));

    // Spawns arrived in order, exactly once each
    std::vector<std::uint32_t> spawnOrder;
    for (const NetworkSessionEvent &event : rig.events)
    {
        if (event.type == NetworkSessionEvent::Type::Spawned)
        {
            spawnOrder.push_back(event.id.value);
        }
    }
    ASSERT_EQ(spawnOrder.size(), 12u);
    EXPECT_TRUE(std::is_sorted(spawnOrder.begin(), spawnOrder.end()));
    EXPECT_EQ(rig.client->State(), NetworkClientState::Joined);
}

TEST(NetworkSessionTest, AClientThatLeavesIsForgotten)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    rig.client->Leave();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsFinished() && session.server->ClientCount() == 0; }));
    EXPECT_EQ(rig.client->State(), NetworkClientState::Closed);
    EXPECT_FALSE(rig.client->Error().empty());
    ASSERT_EQ(session.serverEvents.size(), 2u);
    EXPECT_EQ(session.serverEvents[1].type, NetworkClientEvent::Type::Left);
    EXPECT_TRUE(session.serverEvents[1].wasJoined);
    EXPECT_EQ(session.serverEvents[1].reason, "disconnected");
}

TEST(NetworkSessionTest, TheHostCanDropAPeer)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    session.server->DropPeer(rig.endpoint->Id(), "no identity");
    EXPECT_EQ(session.server->ClientCount(), 0u);
    ASSERT_EQ(session.serverEvents.size(), 2u);
    EXPECT_EQ(session.serverEvents[1].reason, "no identity");
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsFinished(); }));
    EXPECT_EQ(rig.client->State(), NetworkClientState::Closed);
    // A peer that is not a client is only forgotten
    session.server->DropPeer(12345, "unknown");
}

TEST(NetworkSessionTest, ALostSceneEndsEverySession)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    session.serverPeer.scene.reset();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsFinished(); }));
    EXPECT_TRUE(session.server->WorldLost());
    EXPECT_EQ(session.server->ClientCount(), 0u);
    EXPECT_EQ(rig.client->State(), NetworkClientState::Closed);

    // ...and nobody new is taken
    ClientRig &another = session.AddClient();
    session.RunFor(500'000);
    EXPECT_FALSE(another.client->IsJoined());
    EXPECT_EQ(session.server->ClientCount(), 0u);
    EXPECT_GE(session.server->Stats().droppedOverCapacity, 1u);
    EXPECT_FALSE(session.server->Spawn("anything").has_value());
}

TEST(NetworkSessionTest, AClientCanNeverSpawn)
{
    const ScopedPrefab prefab("SessionTest_Hostile");
    Session session;
    RawPeer &raw = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
    const std::size_t roots = session.serverPeer.scene->GetRootGameObjectCount();

    // Before it has joined: a Spawn naming a real prefab of the table
    SpawnMessage forged;
    forged.id = NetworkId{4};
    forged.prefabIndex = PrefabTable::Capture().IndexOf(prefab.Name()).value();
    forged.owner = 1;
    raw.SendReliable(Encode(forged));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedUnauthorized == 1; }));
    session.RunFor(50'000);
    EXPECT_EQ(session.server->ClientCount(), 0u);
    ExpectWorldUntouched(session, 3, roots);
    EXPECT_EQ(session.serverPeer.world.Find(NetworkId{4}), nullptr);
}

TEST(NetworkSessionTest, AClientThatJoinedCanNeverSpawnOrDespawnEither)
{
    const ScopedPrefab prefab("SessionTest_HostileJoined");
    Session session;
    ClientRig &honest = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return honest.client->IsJoined(); }));
    const std::size_t roots = session.serverPeer.scene->GetRootGameObjectCount();

    RawPeer &spawner = session.AddRaw();
    JoinRaw(session, spawner);
    EXPECT_EQ(session.server->JoinedClientCount(), 2u);
    SpawnMessage forgedSpawn;
    forgedSpawn.id = NetworkId{4};
    forgedSpawn.prefabIndex = PrefabTable::Capture().IndexOf(prefab.Name()).value();
    spawner.SendReliable(Encode(forgedSpawn));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedUnauthorized == 1; }));

    RawPeer &despawner = session.AddRaw();
    JoinRaw(session, despawner);
    despawner.SendReliable(Encode(DespawnMessage{NetworkId{1}}));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedUnauthorized == 2; }));
    session.RunFor(100'000);

    // Both were dropped, the world is as it was, and the honest client never heard of any of it
    EXPECT_EQ(session.server->JoinedClientCount(), 1u);
    EXPECT_EQ(session.server->Stats().clientsDropped, 2u);
    ExpectWorldUntouched(session, 3, roots);
    ASSERT_NE(session.serverPeer.world.Find(NetworkId{1}), nullptr);
    EXPECT_FALSE(session.serverPeer.world.Find(NetworkId{1})->GetGameObject().IsDestroyed());
    session.serverPeer.scene->ProcessDestroyed();
    EXPECT_NE(session.serverPeer.world.Find(NetworkId{1}), nullptr);
    EXPECT_EQ(honest.client->Stats().spawnsApplied, 0u);
    EXPECT_EQ(honest.client->Stats().despawnsApplied, 0u);
    EXPECT_EQ(honest.client->State(), NetworkClientState::Joined);
}

TEST(NetworkSessionTest, EveryOtherServerMessageFromAClientIsRefusedToo)
{
    Session session;
    const std::vector<std::vector<std::uint8_t>> serverWords = {
        Encode(DespawnMessage{NetworkId{2}}), Encode(WorldReadyMessage{0, 0}),
        Encode(ServerHelloMessage{kWireVersion, Fingerprint{"1", 0, 0, "", 0, 0}, 5})};
    for (const std::vector<std::uint8_t> &message : serverWords)
    {
        RawPeer &raw = session.AddRaw();
        ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
        raw.SendReliable(message);
    }
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedUnauthorized == serverWords.size(); }));
    EXPECT_EQ(session.server->ClientCount(), 0u);
    EXPECT_EQ(session.serverPeer.world.Count(), 3u);
}

TEST(NetworkSessionTest, MalformedAndMisplacedMessagesDropTheClient)
{
    Session session;
    // Garbage, a truncated accept, an unknown type
    const std::vector<std::vector<std::uint8_t>> garbage = {
        {0xFF, 1, 2, 3}, {static_cast<std::uint8_t>(MessageType::ClientAccept), 1}, {0x55}};
    std::size_t dropped = 0;
    for (const std::vector<std::uint8_t> &message : garbage)
    {
        RawPeer &raw = session.AddRaw();
        ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
        raw.SendReliable(message);
        ++dropped;
        ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedMalformed == dropped; }));
    }
    EXPECT_EQ(session.server->ClientCount(), 0u);
    EXPECT_EQ(session.server->Stats().clientsDropped, 3u);

    // A message on the unreliable channel
    RawPeer &unreliable = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return unreliable.Hello().has_value(); }));
    unreliable.SendUnreliable(Encode(ClientRejectMessage{JoinRefusal::SceneContent}));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedProtocol == 1; }));

    // An accept twice
    RawPeer &twice = session.AddRaw();
    JoinRaw(session, twice);
    ClientAcceptMessage accept;
    accept.fingerprint = twice.Hello()->fingerprint;
    twice.SendReliable(Encode(accept));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedProtocol == 2; }));

    // An accept whose fingerprint is not the server's (a client can claim anything; the server still checks)
    RawPeer &liar = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return liar.Hello().has_value(); }));
    ClientAcceptMessage wrong;
    wrong.fingerprint = liar.Hello()->fingerprint;
    wrong.fingerprint.sceneHash += 1;
    liar.SendReliable(Encode(wrong));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedBadFingerprint == 1; }));
    EXPECT_EQ(session.server->JoinedClientCount(), 0u);
    EXPECT_EQ(session.serverPeer.world.Count(), 3u);
}

TEST(NetworkSessionTest, ARejectIsAClientLeavingNotAnOffence)
{
    Session session;
    RawPeer &raw = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
    raw.SendReliable(Encode(ClientRejectMessage{JoinRefusal::Registry}));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().clientsRefused == 1; }));
    session.RunFor(50'000);
    EXPECT_EQ(session.server->ClientCount(), 0u);
    EXPECT_EQ(session.server->Stats().clientsDropped, 0u);
    ASSERT_EQ(session.serverEvents.size(), 1u);
    EXPECT_EQ(session.serverEvents[0].type, NetworkClientEvent::Type::Left);
    EXPECT_FALSE(session.serverEvents[0].wasJoined);
}

TEST(NetworkSessionTest, MoreMessagesThanAllowedInASecondDropTheClient)
{
    SessionConfig config;
    config.maxClientMessagesPerSecond = 1;
    Session session(config);
    RawPeer &raw = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
    ClientAcceptMessage accept;
    accept.fingerprint = raw.Hello()->fingerprint;
    raw.SendReliable(Encode(accept));
    raw.SendReliable(Encode(accept));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedRateLimited == 1; }));
    EXPECT_EQ(session.server->ClientCount(), 0u);
}

TEST(NetworkSessionTest, AClientThatNeverAcceptsIsDroppedAfterTheJoinTimeout)
{
    SessionConfig config;
    config.joinTimeoutMicros = 3'000'000;
    Session session(config);
    RawPeer &silent = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return silent.Hello().has_value(); }));
    EXPECT_EQ(session.server->ClientCount(), 1u);
    session.RunFor(2'000'000);
    EXPECT_EQ(session.server->ClientCount(), 1u);
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedJoinTimeout == 1; }, 5'000'000));
    EXPECT_EQ(session.server->ClientCount(), 0u);
}

TEST(NetworkSessionTest, AnOversizedMessageFailsTheConnectionAndDropsTheClient)
{
    Session session;
    RawPeer &raw = session.AddRaw(4096);
    ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
    raw.SendReliable(std::vector<std::uint8_t>(kMaxMessageBytes + 100, 0x41));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedConnectionFailed == 1; }));
    EXPECT_EQ(session.server->ClientCount(), 0u);
}

TEST(NetworkSessionTest, NoMoreClientsThanTheLimit)
{
    SessionConfig config;
    config.maxClients = 2;
    Session session(config);
    RawPeer &first = session.AddRaw();
    RawPeer &second = session.AddRaw();
    RawPeer &third = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedOverCapacity == 1; }));
    session.RunFor(100'000);
    EXPECT_EQ(session.server->ClientCount(), 2u);
    EXPECT_TRUE(first.Hello().has_value());
    EXPECT_TRUE(second.Hello().has_value());
    EXPECT_FALSE(third.Hello().has_value());
}

// A refused peer is never a client, but a transport observer (N2Server's session list) saw it connect, and the server's
// own Disconnect raises no transport event: the client observer gets a Left for it, so the observer can forget it
TEST(NetworkSessionTest, ARefusedPeerIsReportedAsLeftWithNoClientId)
{
    SessionConfig config;
    config.maxClients = 1;
    Session session(config);
    std::vector<N2Net::PeerId> connected;
    session.server->SetTransportObserver(
        [&connected](const N2Net::TransportEvent &event)
        {
            if (event.type == N2Net::TransportEventType::PeerConnected)
            {
                connected.push_back(event.peer);
            }
        });
    RawPeer &first = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return first.Hello().has_value(); }));
    (void)session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedOverCapacity == 1; }));
    session.RunFor(50'000);
    ASSERT_EQ(connected.size(), 2u);
    EXPECT_EQ(session.server->ClientCount(), 1u);
    ASSERT_EQ(session.serverEvents.size(), 1u);
    const NetworkClientEvent &left = session.serverEvents[0];
    EXPECT_EQ(left.type, NetworkClientEvent::Type::Left);
    EXPECT_EQ(left.client, kServerClientId);
    EXPECT_EQ(left.peer, connected[1]);
    EXPECT_FALSE(left.wasJoined);
    EXPECT_EQ(left.reason, "refused: the server is full");
}

TEST(NetworkSessionTest, ADroppedClientDoesNotDisturbTheOthers)
{
    const ScopedPrefab prefab("SessionTest_Others");
    Session session;
    ClientRig &honest = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return honest.client->IsJoined(); }));
    RawPeer &hostile = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return hostile.Hello().has_value(); }));
    hostile.SendReliable({0xEE});
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedMalformed == 1; }));

    EXPECT_EQ(session.server->JoinedClientCount(), 1u);
    const auto id = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(session.RunUntil([&] { return honest.peer.world.Find(*id) != nullptr; }));
    EXPECT_EQ(honest.client->State(), NetworkClientState::Joined);
}

TEST(NetworkSessionTest, AClientRefusesADifferentScene)
{
    const ScopedPrefab prefab("SessionTest_Refuse");
    Session session;
    ClientRig &rig = session.AddClient({1, 2, 3}, kSceneHash + 1);
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsFinished(); }));

    EXPECT_EQ(rig.client->State(), NetworkClientState::Refused);
    EXPECT_EQ(rig.client->Refusal(), JoinRefusal::SceneContent);
    EXPECT_NE(rig.client->Error().find("Refusing to join"), std::string::npos);
    EXPECT_NE(rig.client->Error().find("scene"), std::string::npos);
    EXPECT_NE(rig.client->Error().find(kScenePath), std::string::npos);
    EXPECT_EQ(rig.Count(NetworkSessionEvent::Type::Refused), 1u);
    EXPECT_EQ(rig.Count(NetworkSessionEvent::Type::Joined), 0u);

    // The server was told, and the client never took part: nothing it spawns reaches it
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().clientsRefused == 1; }));
    EXPECT_EQ(session.server->JoinedClientCount(), 0u);
    EXPECT_EQ(session.server->Stats().clientsDropped, 0u);
    ASSERT_TRUE(session.server->Spawn(prefab.Name()).has_value());
    session.RunFor(500'000);
    EXPECT_EQ(rig.client->Stats().spawnsApplied, 0u);
    EXPECT_EQ(rig.peer.world.Count(), 3u);
}

TEST(NetworkSessionTest, AClientRefusesADifferentPrefabTable)
{
    const ScopedPrefab prefab("SessionTest_RefuseTable");
    Session session;
    ClientRig &rig = session.AddClient({1, 2, 3}, kSceneHash, PrefabTable());
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsFinished(); }));
    EXPECT_EQ(rig.client->State(), NetworkClientState::Refused);
    EXPECT_EQ(rig.client->Refusal(), JoinRefusal::Registry);
    EXPECT_NE(rig.client->Error().find("prefab"), std::string::npos);
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().clientsRefused == 1; }));
}

TEST(NetworkSessionTest, AClientRefusesADifferentNumberOfSceneObjects)
{
    Session session;
    // The same content hash is claimed, but the scene it loaded has two network objects, not three
    ClientRig &rig = session.AddClient({1, 2});
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsFinished(); }));
    EXPECT_EQ(rig.client->State(), NetworkClientState::Refused);
    EXPECT_EQ(rig.client->Refusal(), JoinRefusal::ObjectCount);
    EXPECT_NE(rig.client->Error().find("3"), std::string::npos);
}

namespace
{
    /// A NetworkClient whose server is a raw Connection that sends what the test says
    class RawServerRig
    {
    public:
        explicit RawServerRig(NetworkClientOptions options = DefaultOptions(), const std::vector<std::uint32_t> &uuids = {1, 2, 3})
            : network(7), peer(uuids, "client")
        {
            serverEndpoint = &network.AddEndpoint();
            clientEndpoint = &network.AddEndpoint();
            network.Link(serverEndpoint->Id(), clientEndpoint->Id(), N2Net::SimLinkConfig{});
            connection = std::make_unique<N2Net::Connection>(MakeWorldConnectionConfig(kProtocol, *serverEndpoint), 0);
            client = std::make_unique<NetworkClient>(*clientEndpoint, peer.world, std::move(options));
            client->SetEventObserver([this](const NetworkSessionEvent &event) { events.push_back(event); });
        }

        static NetworkClientOptions DefaultOptions()
        {
            NetworkClientOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            return options;
        }

        void Step(const std::uint64_t micros = 1000)
        {
            network.Advance(micros);
            const std::uint64_t now = network.NowMicros();
            const auto handler = [&](const N2Net::TransportEvent &event)
            {
                if (event.type != N2Net::TransportEventType::Datagram || event.peer != clientEndpoint->Id())
                {
                    return;
                }
                (void)connection->OnPacket(now, event.data,
                                           [&](const N2Net::MessageChannel, const std::span<const std::uint8_t> message)
                                           { fromClient.emplace_back(message.begin(), message.end()); });
            };
            serverEndpoint->Poll(handler);
            connection->Update(now, [&](const std::span<const std::uint8_t> packet)
                               { serverEndpoint->Send(clientEndpoint->Id(), packet); });
            client->Update(now);
        }

        bool RunUntil(const std::function<bool()> &done, const std::uint64_t limitMicros = 10'000'000)
        {
            for (std::uint64_t elapsed = 0; elapsed < limitMicros; elapsed += 1000)
            {
                if (done())
                {
                    return true;
                }
                Step();
            }
            return done();
        }

        void Send(const std::vector<std::uint8_t> &message) { (void)connection->SendReliable(message); }

        void SendHello()
        {
            ServerHelloMessage hello;
            hello.fingerprint = client->LocalFingerprint();
            hello.clientId = 1;
            Send(Encode(hello));
        }

        /// A hello, and every step until the client is Joining (it has accepted)
        bool JoinFirst()
        {
            SendHello();
            return RunUntil([&] { return client->State() == NetworkClientState::Joining; });
        }

        void ExpectFailed(const std::string &fragment)
        {
            ASSERT_TRUE(RunUntil([&] { return client->IsFinished(); }));
            EXPECT_EQ(client->State(), NetworkClientState::Failed);
            EXPECT_NE(client->Error().find(fragment), std::string::npos) << client->Error();
        }

        N2Net::SimNetwork network;
        Peer peer;
        N2Net::SimTransport *serverEndpoint = nullptr;
        N2Net::SimTransport *clientEndpoint = nullptr;
        std::unique_ptr<N2Net::Connection> connection;
        std::unique_ptr<NetworkClient> client;
        std::vector<NetworkSessionEvent> events;
        std::vector<std::vector<std::uint8_t>> fromClient;
    };

    SpawnMessage SpawnOf(const std::uint32_t id, const std::uint16_t prefabIndex = 0)
    {
        SpawnMessage spawn;
        spawn.id = NetworkId{id};
        spawn.prefabIndex = prefabIndex;
        return spawn;
    }

    /// The index of `prefab` in the table a process would capture now
    std::uint16_t IndexOfPrefab(const ScopedPrefab &prefab)
    {
        return PrefabTable::Capture().IndexOf(prefab.Name()).value();
    }
}

TEST(NetworkClientTest, AcceptsAMatchingHelloAndAnswersWithItsOwnFingerprint)
{
    const ScopedPrefab prefab("ClientTest_Accept");
    RawServerRig rig;
    ASSERT_TRUE(rig.JoinFirst());
    ASSERT_TRUE(rig.RunUntil([&] { return !rig.fromClient.empty(); }));
    const auto accept = DecodeClientMessage(rig.fromClient[0]);
    ASSERT_TRUE(accept.has_value());
    const auto *body = std::get_if<ClientAcceptMessage>(&*accept);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(CompareFingerprints(body->fingerprint, rig.client->LocalFingerprint()), JoinRefusal::None);
    EXPECT_EQ(rig.client->LocalClientId(), 1u);

    rig.Send(Encode(WorldReadyMessage{0, 0}));
    ASSERT_TRUE(rig.RunUntil([&] { return rig.client->IsJoined(); }));
}

TEST(NetworkClientTest, RefusesAWireVersionSevenServer)
{
    // Version 8 added the ground a character rides to the character's replicated fields: a version 7 server's entries
    // for a character have another layout, so a version 7 peer is refused at the hello, as any other version is
    RawServerRig rig;
    std::vector<std::uint8_t> hello = Encode(ServerHelloMessage{kWireVersion, rig.client->LocalFingerprint(), 1});
    ASSERT_EQ(kWireVersion, 8);
    hello[1] = 7;
    hello[2] = 0;
    rig.Send(hello);
    ASSERT_TRUE(rig.RunUntil([&] { return rig.client->State() == NetworkClientState::Refused; }));
    EXPECT_EQ(rig.client->Refusal(), JoinRefusal::WireVersion);
    EXPECT_NE(rig.client->Error().find("wire version 7 "), std::string::npos);
    EXPECT_NE(rig.client->Error().find("speaks 8"), std::string::npos);
}

TEST(NetworkClientTest, RefusesAServerOfAnotherWireVersionBeforeReadingAnythingElse)
{
    RawServerRig rig;
    std::vector<std::uint8_t> hello = Encode(ServerHelloMessage{kWireVersion, rig.client->LocalFingerprint(), 1});
    hello[1] = 77;
    hello[2] = 0;
    rig.Send(hello);
    ASSERT_TRUE(rig.RunUntil([&] { return rig.client->State() == NetworkClientState::Refused; }));
    EXPECT_EQ(rig.client->Refusal(), JoinRefusal::WireVersion);
    EXPECT_NE(rig.client->Error().find("wire version 77"), std::string::npos);
    EXPECT_NE(rig.client->Error().find("Refusing to join"), std::string::npos);

    // It told the server why, and left
    ASSERT_TRUE(rig.RunUntil([&] { return !rig.fromClient.empty(); }));
    const auto reject = DecodeClientMessage(rig.fromClient[0]);
    ASSERT_TRUE(reject.has_value());
    const auto *body = std::get_if<ClientRejectMessage>(&*reject);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->reason, JoinRefusal::WireVersion);
    ASSERT_TRUE(rig.RunUntil([&] { return rig.client->IsFinished(); }));
}

TEST(NetworkClientTest, FailsOnWhatItCannotDecode)
{
    RawServerRig rig;
    rig.Send({0x7F, 1, 2});
    rig.ExpectFailed("malformed");
    EXPECT_EQ(rig.events.back().type, NetworkSessionEvent::Type::Failed);
}

TEST(NetworkClientTest, FailsOnAMessageOfTheOtherDirection)
{
    RawServerRig rig;
    ASSERT_TRUE(rig.JoinFirst());
    // The server never sends an accept
    rig.Send(Encode(ClientAcceptMessage{kWireVersion, rig.client->LocalFingerprint()}));
    rig.ExpectFailed("malformed");
}

TEST(NetworkClientTest, FailsOnAWorldMessageBeforeTheHello)
{
    const ScopedPrefab prefab("ClientTest_Early");
    const std::uint16_t index = IndexOfPrefab(prefab);
    {
        RawServerRig rig;
        rig.Send(Encode(SpawnOf(4, index)));
        rig.ExpectFailed("before the world was accepted");
    }
    {
        RawServerRig rig;
        rig.Send(Encode(DespawnMessage{NetworkId{1}}));
        rig.ExpectFailed("before the world was accepted");
    }
    {
        RawServerRig rig;
        rig.Send(Encode(WorldReadyMessage{0, 0}));
        rig.ExpectFailed("wrong time");
    }
}

TEST(NetworkClientTest, FailsOnASecondHello)
{
    RawServerRig rig;
    ASSERT_TRUE(rig.JoinFirst());
    rig.SendHello();
    rig.ExpectFailed("second hello");
}

TEST(NetworkClientTest, FailsOnASpawnWhoseIdIsNotNewAndAbove)
{
    const ScopedPrefab prefab("ClientTest_Ids");
    const std::uint16_t index = IndexOfPrefab(prefab);
    {
        // An id of a scene object
        RawServerRig rig;
        ASSERT_TRUE(rig.JoinFirst());
        rig.Send(Encode(SpawnOf(2, index)));
        rig.ExpectFailed("out of order");
        EXPECT_EQ(rig.client->Stats().spawnsApplied, 0u);
    }
    {
        // The same id twice
        RawServerRig rig;
        ASSERT_TRUE(rig.JoinFirst());
        rig.Send(Encode(SpawnOf(4, index)));
        rig.Send(Encode(SpawnOf(4, index)));
        rig.ExpectFailed("out of order");
        EXPECT_EQ(rig.client->Stats().spawnsApplied, 1u);
    }
    {
        // An id below the last one
        RawServerRig rig;
        ASSERT_TRUE(rig.JoinFirst());
        rig.Send(Encode(SpawnOf(9, index)));
        rig.Send(Encode(SpawnOf(5, index)));
        rig.ExpectFailed("out of order");
        EXPECT_EQ(rig.client->Stats().spawnsApplied, 1u);
    }
}

TEST(NetworkClientTest, FailsOnASpawnOfAPrefabItDoesNotHave)
{
    const ScopedPrefab prefab("ClientTest_Index");
    RawServerRig rig;
    ASSERT_TRUE(rig.JoinFirst());
    const std::size_t roots = rig.peer.scene->GetRootGameObjectCount();
    rig.Send(Encode(SpawnOf(4, 40)));
    rig.ExpectFailed("prefab table");
    EXPECT_EQ(rig.peer.scene->GetRootGameObjectCount(), roots);
}

TEST(NetworkClientTest, FailsWhenTheJoinStateIsNotWhatTheServerSaidItWas)
{
    const ScopedPrefab prefab("ClientTest_Counts");
    const std::uint16_t index = IndexOfPrefab(prefab);
    {
        RawServerRig rig;
        ASSERT_TRUE(rig.JoinFirst());
        rig.Send(Encode(SpawnOf(4, index)));
        rig.Send(Encode(WorldReadyMessage{2, 0}));
        rig.ExpectFailed("not what the server said");
    }
    {
        RawServerRig rig;
        ASSERT_TRUE(rig.JoinFirst());
        rig.Send(Encode(DespawnMessage{NetworkId{1}}));
        rig.Send(Encode(WorldReadyMessage{0, 0}));
        rig.ExpectFailed("not what the server said");
    }
    {
        // Twice the end of the join state
        RawServerRig rig;
        ASSERT_TRUE(rig.JoinFirst());
        rig.Send(Encode(WorldReadyMessage{0, 0}));
        rig.Send(Encode(WorldReadyMessage{0, 0}));
        rig.ExpectFailed("wrong time");
    }
}

TEST(NetworkClientTest, IgnoresADespawnOfAnObjectItDoesNotHave)
{
    RawServerRig rig;
    ASSERT_TRUE(rig.JoinFirst());
    rig.Send(Encode(WorldReadyMessage{0, 0}));
    ASSERT_TRUE(rig.RunUntil([&] { return rig.client->IsJoined(); }));
    rig.Send(Encode(DespawnMessage{NetworkId{999}}));
    ASSERT_TRUE(rig.RunUntil([&] { return rig.client->Stats().despawnsUnknown == 1; }));
    EXPECT_TRUE(rig.client->IsJoined());
    EXPECT_EQ(rig.client->Stats().despawnsApplied, 0u);
}

TEST(NetworkClientTest, FailsOnAMessageOnTheUnreliableChannel)
{
    RawServerRig rig;
    ASSERT_TRUE(rig.JoinFirst());
    (void)rig.connection->SendUnreliable(Encode(WorldReadyMessage{0, 0}));
    rig.ExpectFailed("unreliable channel");
}

TEST(NetworkClientTest, NeverSendsASpawnOrADespawn)
{
    const ScopedPrefab prefab("ClientTest_Silent");
    const std::uint16_t index = IndexOfPrefab(prefab);
    RawServerRig rig;
    ASSERT_TRUE(rig.JoinFirst());
    rig.Send(Encode(SpawnOf(4, index)));
    rig.Send(Encode(WorldReadyMessage{1, 0}));
    ASSERT_TRUE(rig.RunUntil([&] { return rig.client->IsJoined(); }));
    rig.Send(Encode(DespawnMessage{NetworkId{4}}));
    ASSERT_TRUE(rig.RunUntil([&] { return rig.client->Stats().despawnsApplied == 1; }));
    rig.Step(500'000);
    // Everything the client ever said to its server decodes as an accept: nothing else
    ASSERT_FALSE(rig.fromClient.empty());
    // (the clock pings of netcode stage B3 are the one other thing a client sends, and they are no world message)
    std::size_t worldMessages = 0;
    for (const std::vector<std::uint8_t> &message : rig.fromClient)
    {
        if (!message.empty() && message[0] == 0x85)
        {
            continue;
        }
        ++worldMessages;
        const auto decoded = DecodeClientMessage(message);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_NE(std::get_if<ClientAcceptMessage>(&*decoded), nullptr);
    }
    EXPECT_EQ(worldMessages, 1u);
}

TEST(NetworkClientTest, FailsWhenTheServerSendsNoHello)
{
    NetworkClientOptions options = RawServerRig::DefaultOptions();
    options.helloTimeoutMicros = 2'000'000;
    RawServerRig rig(std::move(options));
    rig.RunUntil([&] { return rig.client->State() == NetworkClientState::AwaitingHello; });
    EXPECT_FALSE(rig.RunUntil([&] { return rig.client->IsFinished(); }, 1'500'000));
    rig.ExpectFailed("no hello in time");
}

TEST(NetworkClientTest, EndsWhenTheServerClosesTheConnection)
{
    RawServerRig rig;
    ASSERT_TRUE(rig.JoinFirst());
    rig.serverEndpoint->Disconnect(rig.clientEndpoint->Id());
    ASSERT_TRUE(rig.RunUntil([&] { return rig.client->IsFinished(); }));
    EXPECT_EQ(rig.client->State(), NetworkClientState::Closed);
    EXPECT_NE(rig.client->Error().find("closed the connection"), std::string::npos);
    EXPECT_EQ(rig.events.back().type, NetworkSessionEvent::Type::Closed);
}

TEST(NetworkClientTest, EveryStateHasAName)
{
    for (const NetworkClientState state :
         {NetworkClientState::Connecting, NetworkClientState::AwaitingHello, NetworkClientState::Joining,
          NetworkClientState::Joined, NetworkClientState::Refused, NetworkClientState::Closed,
          NetworkClientState::Failed})
    {
        EXPECT_NE(ToString(state), "unknown");
    }
}
