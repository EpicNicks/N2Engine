#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <math/UUID.hpp>

#include "NetworkTestSupport.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkFingerprint.hpp"
#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/NetworkRpc.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

// RPCs end to end (netcode stage B7): C++ and Lua RPCs over the network simulator, the direction and authority checks,
// argument validation, the per-client budgets and queue bounds, faulting handlers, and hostile peers.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;

namespace
{
    constexpr std::uint32_t kProtocol = 0x4E32'5250;
    constexpr std::uint64_t kSceneHash = 0xB7B7'B7B7ull;
    constexpr const char *kScenePath = "res://rpc.scene";
    constexpr const char *kPrefab = "RpcTest_Pawn";

    /// What the handlers of RpcProbe saw, in order
    std::vector<std::string> g_calls;

    /// A component with C++ RPCs of every direction
    class RpcProbe final : public SerializableComponent
    {
    public:
        explicit RpcProbe(GameObject &gameObject) : SerializableComponent(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "RpcProbe"; }

        void Announce(const RpcContext &, std::string text) { g_calls.push_back("announce:" + text); }
        void Blip(const RpcContext &, int value) { g_calls.push_back("blip:" + std::to_string(value)); }
        void Boom(const RpcContext &) { throw std::runtime_error("handler fault"); }
        void Chat(const RpcContext &context, std::string text)
        {
            g_calls.push_back("chat:" + std::to_string(context.sender) + ":" + text +
                              (context.onServer ? ":server" : ":client"));
        }
        void Fire(const RpcContext &context, float power, int count)
        {
            g_calls.push_back("fire:" + std::to_string(context.sender) + ":" + std::to_string(static_cast<int>(power)) + ":" +
                              std::to_string(count));
        }
        void Order(const RpcContext &, int value) { g_calls.push_back("order:" + std::to_string(value)); }
        void Ping(const RpcContext &, int value) { g_calls.push_back("ping:" + std::to_string(value)); }
        void Tell(const RpcContext &, int value) { g_calls.push_back("tell:" + std::to_string(value)); }
    };

    [[maybe_unused]] const bool kRegistered = []
    {
        const auto cosmetic = [](const RpcDirection direction, const bool reliable)
        { return RpcOptions{direction, reliable, true}; };
        RegisterRpc<RpcProbe, std::string>("RpcProbe", "Announce", &RpcProbe::Announce,
                                           cosmetic(RpcDirection::ServerToClient, true));
        RegisterRpc<RpcProbe, int>("RpcProbe", "Blip", &RpcProbe::Blip, cosmetic(RpcDirection::ServerToClient, false));
        RegisterRpc<RpcProbe>("RpcProbe", "Boom", &RpcProbe::Boom, cosmetic(RpcDirection::ClientToServer, true));
        RegisterRpc<RpcProbe, std::string>("RpcProbe", "Chat", &RpcProbe::Chat, cosmetic(RpcDirection::ClientToServer, true));
        RegisterRpc<RpcProbe, float, int>("RpcProbe", "Fire", &RpcProbe::Fire, RpcOptions{RpcDirection::ClientToServer, true, false});
        RegisterRpc<RpcProbe, int>("RpcProbe", "Order", &RpcProbe::Order, RpcOptions{RpcDirection::ServerToClient, true, false});
        RegisterRpc<RpcProbe, int>("RpcProbe", "Ping", &RpcProbe::Ping, RpcOptions{RpcDirection::ClientToServer, false, false});
        RegisterRpc<RpcProbe, int>("RpcProbe", "Tell", &RpcProbe::Tell, RpcOptions{RpcDirection::ServerToOwner, true, false});
        return true;
    }();

    std::size_t Count(const std::string &prefix)
    {
        std::size_t count = 0;
        for (const std::string &call : g_calls)
        {
            if (call.rfind(prefix, 0) == 0)
            {
                ++count;
            }
        }
        return count;
    }

    struct Peer
    {
        explicit Peer(const std::string &name) : scene(MakeScene(name, {1, 2, 3}))
        {
            bound = world.BindScene(*scene);
        }

        std::unique_ptr<Scene> scene;
        NetworkWorld world;
        std::expected<std::uint32_t, std::string> bound{std::unexpected(std::string("not bound"))};
    };

    struct ClientRig
    {
        ClientRig() : peer("client") {}

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
        std::function<void(NetworkServerOptions &)> tweakServer;
        std::function<void(NetworkClientOptions &)> tweakClient;
    };

    class Session
    {
    public:
        explicit Session(SessionConfig configuration = {})
            : config(std::move(configuration)), network(config.seed), serverPeer("server")
        {
            serverEndpoint = &network.AddEndpoint();
            NetworkServerOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            if (config.tweakServer)
            {
                config.tweakServer(options);
            }
            server = std::make_unique<NetworkServer>(*serverEndpoint, serverPeer.world, std::move(options));
        }

        ClientRig &AddClient()
        {
            auto rig = std::make_unique<ClientRig>();
            N2Net::SimTransport &endpoint = network.AddEndpoint();
            rig->endpoint = &endpoint;
            network.Link(serverEndpoint->Id(), endpoint.Id(), config.link);

            NetworkClientOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            if (config.tweakClient)
            {
                config.tweakClient(options);
            }
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

        ClientId Join(ClientRig &rig)
        {
            EXPECT_TRUE(RunUntil([&] { return rig.client->IsJoined(); }));
            return rig.client->LocalClientId();
        }

        ClientId JoinRaw(RawPeer &raw)
        {
            EXPECT_TRUE(RunUntil([&] { return raw.Hello().has_value(); }));
            ClientAcceptMessage accept;
            accept.fingerprint = raw.Hello()->fingerprint;
            raw.SendReliable(Encode(accept));
            const std::size_t before = server->JoinedClientCount();
            EXPECT_TRUE(RunUntil([&] { return server->JoinedClientCount() == before + 1; }));
            return raw.Hello()->clientId;
        }

        /// Spawns the prefab for `owner`, waits until every client has it, and gives the object on every side an RpcProbe
        NetworkId SpawnProbe(const ClientId owner)
        {
            const auto id = server->Spawn(kPrefab, owner);
            EXPECT_TRUE(id.has_value());
            if (!id.has_value())
            {
                return {};
            }
            server->World().Find(*id)->GetGameObject().AddComponent<RpcProbe>();
            for (const auto &rig : clients)
            {
                EXPECT_TRUE(RunUntil([&] { return rig->peer.world.Find(*id) != nullptr; }));
                if (NetworkObject *const object = rig->peer.world.Find(*id))
                {
                    object->GetGameObject().AddComponent<RpcProbe>();
                }
            }
            return *id;
        }

        RpcProbe &ServerProbe(const NetworkId id) { return *server->World().Find(id)->GetGameObject().GetComponent<RpcProbe>(); }
        static RpcProbe &ClientProbe(ClientRig &rig, const NetworkId id)
        {
            return *rig.peer.world.Find(id)->GetGameObject().GetComponent<RpcProbe>();
        }

        SessionConfig config;
        N2Net::SimNetwork network;
        Peer serverPeer;
        N2Net::SimTransport *serverEndpoint = nullptr;
        std::unique_ptr<NetworkServer> server;
        std::vector<std::unique_ptr<ClientRig>> clients;
        std::vector<std::unique_ptr<RawPeer>> raws;
    };

    /// A raw client-to-server RPC message for an object the server holds
    struct RawCall
    {
        std::uint8_t type = kRpcClientReliableType;
        std::uint32_t object = 0;
        std::uint32_t component = 0;
        std::uint8_t rpcIndex = 0;
        std::uint16_t hash = 0;
        std::vector<std::uint8_t> args;

        [[nodiscard]] std::vector<std::uint8_t> Bytes() const
        {
            std::vector<std::uint8_t> out;
            WireWriter writer(out);
            writer.U8(type);
            writer.Varint(object);
            writer.Varint(component);
            writer.U8(rpcIndex);
            writer.U16(hash);
            writer.Bytes(args);
            return out;
        }
    };

    /// Where rpc `name` of the probe on `id` is on the server, as a raw call with no arguments yet
    RawCall CallFor(Session &session, const NetworkId id, const char *name, const std::uint8_t type = kRpcClientReliableType)
    {
        RawCall call;
        call.type = type;
        call.object = id.value;
        GameObject &object = session.server->World().Find(id)->GetGameObject();
        ResolvedRpc resolved;
        EXPECT_TRUE(FindRpc(object, *object.GetComponent<RpcProbe>(), name, resolved));
        call.component = static_cast<std::uint32_t>(resolved.componentIndex);
        call.rpcIndex = static_cast<std::uint8_t>(resolved.rpcIndex);
        call.hash = HashRpcTable(*RpcRegistry::Instance().Find("RpcProbe"));
        return call;
    }

    std::vector<std::uint8_t> FireArgs(const float power, const std::int64_t count)
    {
        std::vector<std::uint8_t> out;
        WireWriter writer(out);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &power, sizeof bits);
        writer.U32(bits);
        writer.ZigZag(count);
        return out;
    }

    std::vector<std::uint8_t> IntArgs(const std::int64_t value)
    {
        std::vector<std::uint8_t> out;
        WireWriter writer(out);
        writer.ZigZag(value);
        return out;
    }

    std::vector<std::uint8_t> StringArgs(const std::vector<std::uint8_t> &bytes)
    {
        std::vector<std::uint8_t> out;
        WireWriter writer(out);
        writer.Varint(bytes.size());
        writer.Bytes(bytes);
        return out;
    }

    struct NetworkRpcSessionTest : ::testing::Test
    {
        void SetUp() override { g_calls.clear(); }
        ScopedPrefab prefab{kPrefab};
    };
}

// ---- Declarations ---------------------------------------------------------------------------------------------------------

TEST(NetworkRpcDeclarationTest, ValidatesNamesParametersAndSize)
{
    RpcDescriptor ok;
    ok.name = "Fire";
    ok.params.push_back(MakeFieldInfo<float>("a"));
    EXPECT_TRUE(ValidateRpcDescriptor(ok).empty());

    RpcDescriptor bad = ok;
    bad.name = "";
    EXPECT_FALSE(ValidateRpcDescriptor(bad).empty());
    bad.name = "9lives";
    EXPECT_FALSE(ValidateRpcDescriptor(bad).empty());
    bad.name = "has space";
    EXPECT_FALSE(ValidateRpcDescriptor(bad).empty());
    bad.name = std::string(kMaxRpcNameLength + 1, 'a');
    EXPECT_FALSE(ValidateRpcDescriptor(bad).empty());

    bad = ok;
    bad.params.assign(kMaxRpcParams + 1, MakeFieldInfo<int>("p"));
    EXPECT_FALSE(ValidateRpcDescriptor(bad).empty());

    // Kinds that are not plain values
    bad = ok;
    bad.params = {MakeFieldInfo<float>("a")};
    bad.params[0].kind = FieldKind::GameObjectRef;
    EXPECT_FALSE(ValidateRpcDescriptor(bad).empty());
    bad.params[0].kind = FieldKind::Json;
    EXPECT_FALSE(ValidateRpcDescriptor(bad).empty());
}

TEST(NetworkRpcDeclarationTest, TheRegistryKeepsEachListSortedAndRefusesDuplicates)
{
    const std::vector<RpcDescriptor> *table = RpcRegistry::Instance().Find("RpcProbe");
    ASSERT_NE(table, nullptr);
    for (std::size_t i = 1; i < table->size(); ++i)
    {
        EXPECT_LT((*table)[i - 1].name, (*table)[i].name);
    }
    RpcDescriptor duplicate;
    duplicate.name = "Fire";
    EXPECT_FALSE(RpcRegistry::Instance().Register("RpcProbe", duplicate));
    EXPECT_EQ(RpcRegistry::Instance().Find("Nothing"), nullptr);

    // Too many RPCs on one type
    for (std::size_t i = 0; i < kMaxRpcsPerComponent; ++i)
    {
        RpcDescriptor rpc;
        rpc.name = "R" + std::to_string(i);
        EXPECT_TRUE(RpcRegistry::Instance().Register("RpcFull", rpc));
    }
    RpcDescriptor extra;
    extra.name = "Extra";
    EXPECT_FALSE(RpcRegistry::Instance().Register("RpcFull", extra));
    RpcRegistry::Instance().Unregister("RpcFull");
}

TEST(NetworkRpcDeclarationTest, RpcsAreInTheRegistryFingerprintAndTheTableHash)
{
    const PrefabTable table;
    const std::uint64_t before = HashRegistry(table);
    RpcDescriptor rpc;
    rpc.name = "Extra";
    ASSERT_TRUE(RpcRegistry::Instance().Register("RpcFingerprintProbe", rpc));
    const std::uint64_t after = HashRegistry(table);
    EXPECT_NE(before, after);
    RpcRegistry::Instance().Unregister("RpcFingerprintProbe");
    EXPECT_EQ(HashRegistry(table), before);

    // Direction, delivery and parameters change the table hash
    std::vector<RpcDescriptor> a(1);
    a[0].name = "X";
    std::vector<RpcDescriptor> b = a;
    EXPECT_EQ(HashRpcTable(a), HashRpcTable(b));
    b[0].reliable = !b[0].reliable;
    EXPECT_NE(HashRpcTable(a), HashRpcTable(b));
    b = a;
    b[0].direction = RpcDirection::ServerToOwner;
    EXPECT_NE(HashRpcTable(a), HashRpcTable(b));
    b = a;
    b[0].params.push_back(MakeFieldInfo<int>("p"));
    EXPECT_NE(HashRpcTable(a), HashRpcTable(b));
}

// ---- Wire -----------------------------------------------------------------------------------------------------------------

TEST(NetworkRpcWireTest, ArgumentsRoundTripAndEveryTruncationIsRefused)
{
    const std::vector<RpcDescriptor> *table = RpcRegistry::Instance().Find("RpcProbe");
    ASSERT_NE(table, nullptr);
    const auto fire = std::find_if(table->begin(), table->end(), [](const RpcDescriptor &r) { return r.name == "Fire"; });
    ASSERT_NE(fire, table->end());
    ResolvedRpc resolved;
    resolved.descriptor = &*fire;
    resolved.componentIndex = 3;
    resolved.rpcIndex = static_cast<std::size_t>(fire - table->begin());

    std::string error;
    const auto message = EncodeRpcMessage(kRpcClientReliableType, NetworkId{77}, resolved, 0xBEEF,
                                          {nlohmann::json(2.5), nlohmann::json(-9)}, error);
    ASSERT_TRUE(message.has_value()) << error;
    RpcHeader header;
    ASSERT_TRUE(DecodeRpcHeader(*message, header));
    EXPECT_EQ(header.object.value, 77u);
    EXPECT_EQ(header.componentIndex, 3u);
    EXPECT_EQ(header.tableHash, 0xBEEF);
    std::vector<nlohmann::json> args;
    ASSERT_TRUE(DecodeRpcArgs(*fire, header.args, args));
    ASSERT_EQ(args.size(), 2u);
    EXPECT_FLOAT_EQ(args[0].get<float>(), 2.5f);
    EXPECT_EQ(args[1].get<int>(), -9);

    // A truncated message never decodes into more than it holds, and nothing throws
    for (std::size_t cut = 0; cut < message->size(); ++cut)
    {
        const std::span<const std::uint8_t> part(message->data(), cut);
        RpcHeader partial;
        std::vector<nlohmann::json> partialArgs;
        if (DecodeRpcHeader(part, partial))
        {
            EXPECT_FALSE(DecodeRpcArgs(*fire, partial.args, partialArgs)) << "cut at " << cut;
        }
    }
    // A byte left over
    std::vector<std::uint8_t> longer = *message;
    longer.push_back(0);
    ASSERT_TRUE(DecodeRpcHeader(longer, header));
    EXPECT_FALSE(DecodeRpcArgs(*fire, header.args, args));
    // Zero id, a wrong type byte, an empty message and an oversized one
    std::vector<std::uint8_t> zero = *message;
    zero[1] = 0;
    EXPECT_FALSE(DecodeRpcHeader(zero, header));
    std::vector<std::uint8_t> wrongType = *message;
    wrongType[0] = 0x55;
    EXPECT_FALSE(DecodeRpcHeader(wrongType, header));
    EXPECT_FALSE(DecodeRpcHeader({}, header));
    EXPECT_FALSE(DecodeRpcHeader(std::vector<std::uint8_t>(kMaxRpcMessageBytes + 1, kRpcClientReliableType), header));

    // Wrong argument counts and types are refused on send
    EXPECT_FALSE(EncodeRpcMessage(kRpcClientReliableType, NetworkId{1}, resolved, 0, {nlohmann::json(1.0)}, error).has_value());
    EXPECT_FALSE(EncodeRpcMessage(kRpcClientReliableType, NetworkId{1}, resolved, 0,
                                  {nlohmann::json("text"), nlohmann::json(1)}, error)
                     .has_value());
}

TEST(NetworkRpcWireTest, ServerMessagesAreNotWorldMessages)
{
    // The world decoders refuse the RPC types: they are routed apart
    const std::vector<std::uint8_t> server = {kRpcServerReliableType, 1, 0, 0, 0, 0};
    const std::vector<std::uint8_t> client = {kRpcClientReliableType, 1, 0, 0, 0, 0};
    EXPECT_FALSE(DecodeServerMessage(server).has_value());
    EXPECT_FALSE(DecodeClientMessage(client).has_value());
}

TEST(NetworkRpcBudgetTest, WindowsLimitRateBytesAndTrackedRpcs)
{
    RpcLimits limits;
    limits.maxPerSecond = 5;
    limits.maxPerRpcPerSecond = 3;
    limits.maxBytesPerSecond = 100;
    RpcBudget budget;
    EXPECT_EQ(budget.Admit(1'000'000, 1, 10, limits), RpcBudget::Verdict::Allowed);
    EXPECT_EQ(budget.Admit(1'000'001, 1, 10, limits), RpcBudget::Verdict::Allowed);
    EXPECT_EQ(budget.Admit(1'000'002, 1, 10, limits), RpcBudget::Verdict::Allowed);
    EXPECT_EQ(budget.Admit(1'000'003, 1, 10, limits), RpcBudget::Verdict::OverRate) << "per RPC";
    EXPECT_EQ(budget.Admit(1'000'004, 2, 10, limits), RpcBudget::Verdict::Allowed);
    EXPECT_EQ(budget.Admit(1'000'005, 2, 10, limits), RpcBudget::Verdict::Allowed);
    EXPECT_EQ(budget.Admit(1'000'006, 3, 10, limits), RpcBudget::Verdict::OverRate) << "in total";
    // A new second starts again
    EXPECT_EQ(budget.Admit(2'100'000, 1, 90, limits), RpcBudget::Verdict::Allowed);
    EXPECT_EQ(budget.Admit(2'100'001, 1, 20, limits), RpcBudget::Verdict::OverBytes);

    // Many distinct RPC keys never grow the table beyond its bound
    RpcLimits wide;
    wide.maxPerSecond = 1000;
    RpcBudget many;
    for (std::uint64_t key = 0; key < 5000; ++key)
    {
        (void)many.Admit(1'000'000, key, 1, wide);
    }
    EXPECT_LE(many.TrackedRpcs(), RpcBudget::kMaxTrackedRpcs + 1);

    // The send budget
    RpcLimits send;
    send.maxSendPerSecond = 2;
    RpcBudget sender;
    EXPECT_TRUE(sender.AdmitSend(5'000'000, send));
    EXPECT_TRUE(sender.AdmitSend(5'000'001, send));
    EXPECT_FALSE(sender.AdmitSend(5'000'002, send));
    EXPECT_TRUE(sender.AdmitSend(6'100'000, send));

    // Limits are clamped
    RpcLimits crazy;
    crazy.maxPerSecond = 0xFFFFFFFFu;
    crazy.maxQueued = 0;
    const RpcLimits clamped = crazy.Clamped();
    EXPECT_LE(clamped.maxPerSecond, 1000u);
    EXPECT_GE(clamped.maxQueued, 1u);
}

// ---- Sessions: C++ RPCs ---------------------------------------------------------------------------------------------------

TEST_F(NetworkRpcSessionTest, AClientCallsAServerRpcOnItsOwnObjectAndTheHandlerGetsTheValidatedArguments)
{
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const NetworkId pawn = session.SpawnProbe(id);
    ASSERT_TRUE(pawn.IsValid());

    EXPECT_EQ(rig.client->CallRpc(Session::ClientProbe(rig, pawn), "Fire", {nlohmann::json(7.9), nlohmann::json(3)}),
              RpcSendResult::Sent);
    session.RunFor(50);
    ASSERT_EQ(g_calls.size(), 1u);
    EXPECT_EQ(g_calls[0], "fire:" + std::to_string(id) + ":7:3");
    EXPECT_EQ(session.server->RpcStatistics().executed, 1u);
    EXPECT_EQ(rig.client->RpcStatistics().sent, 1u);

    // A chat line is a cosmetic RPC: the handler learns it runs on the server
    EXPECT_EQ(rig.client->CallRpc(Session::ClientProbe(rig, pawn), "Chat", {nlohmann::json("hello")}), RpcSendResult::Sent);
    session.RunFor(50);
    EXPECT_EQ(Count("chat:" + std::to_string(id) + ":hello:server"), 1u);

    // Bad calls are refused on the sending side
    RpcProbe &probe = Session::ClientProbe(rig, pawn);
    EXPECT_EQ(rig.client->CallRpc(probe, "Nope", {}), RpcSendResult::UnknownRpc);
    EXPECT_EQ(rig.client->CallRpc(probe, "Fire", {nlohmann::json(1.0)}), RpcSendResult::BadArguments);
    EXPECT_EQ(rig.client->CallRpc(probe, "Fire", {nlohmann::json("x"), nlohmann::json(1)}), RpcSendResult::BadArguments);
    EXPECT_EQ(rig.client->CallRpc(probe, "Announce", {nlohmann::json("x")}), RpcSendResult::WrongDirection)
        << "a client cannot call a server-to-client RPC";
    EXPECT_EQ(g_calls.size(), 2u);
}

TEST_F(NetworkRpcSessionTest, AClientCannotCallAnRpcOnAnObjectItDoesNotOwn)
{
    Session session;
    ClientRig &owner = session.AddClient();
    ClientRig &other = session.AddClient();
    const ClientId ownerId = session.Join(owner);
    (void)session.Join(other);
    const NetworkId pawn = session.SpawnProbe(ownerId);
    const NetworkId serverOwned = session.SpawnProbe(kServerClientId);

    EXPECT_EQ(other.client->CallRpc(Session::ClientProbe(other, pawn), "Fire", {nlohmann::json(1.0), nlohmann::json(1)}),
              RpcSendResult::NotOwner);
    EXPECT_EQ(owner.client->CallRpc(Session::ClientProbe(owner, serverOwned), "Fire", {nlohmann::json(1.0), nlohmann::json(1)}),
              RpcSendResult::NotOwner);

    // A hostile peer skips the client-side check: the server still refuses, counts it and keeps the client
    RawPeer &raw = session.AddRaw();
    const ClientId rawId = session.JoinRaw(raw);
    ASSERT_NE(rawId, ownerId);
    RawCall call = CallFor(session, pawn, "Fire");
    call.args = FireArgs(1.0f, 1);
    raw.SendReliable(call.Bytes());
    RawCall onServer = CallFor(session, serverOwned, "Fire");
    onServer.args = FireArgs(1.0f, 1);
    raw.SendReliable(onServer.Bytes());
    session.RunFor(100);
    EXPECT_EQ(session.server->RpcStatistics().rejectedAuthority, 2u);
    EXPECT_EQ(session.server->RpcStatistics().executed, 0u);
    EXPECT_TRUE(g_calls.empty());
    EXPECT_TRUE(session.server->IsClientJoined(rawId)) << "an unauthorised call is dropped, not punished";
}

TEST_F(NetworkRpcSessionTest, TheServerNeverRunsAServerToClientRpcAClientSends)
{
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const NetworkId pawn = session.SpawnProbe(id);
    RawPeer &raw = session.AddRaw();
    const ClientId rawId = session.JoinRaw(raw);
    const NetworkId rawPawn = session.SpawnProbe(rawId);

    for (const char *name : {"Announce", "Tell", "Order"})
    {
        RawCall call = CallFor(session, rawPawn, name);
        call.args = name == std::string("Announce") ? StringArgs({'h', 'i'}) : IntArgs(1);
        raw.SendReliable(call.Bytes());
    }
    session.RunFor(100);
    EXPECT_EQ(session.server->RpcStatistics().rejectedDirection, 3u);
    EXPECT_TRUE(g_calls.empty());
    (void)pawn;
    EXPECT_TRUE(session.server->IsClientJoined(rawId));
}

TEST_F(NetworkRpcSessionTest, ServerCallsReachTheRightClientsOnlyAndOwnerRpcsReachTheOwner)
{
    Session session;
    ClientRig &first = session.AddClient();
    ClientRig &second = session.AddClient();
    const ClientId a = session.Join(first);
    const ClientId b = session.Join(second);
    const NetworkId pawn = session.SpawnProbe(a);
    RpcProbe &probe = session.ServerProbe(pawn);

    // All
    EXPECT_EQ(session.server->CallRpc(probe, "Announce", {nlohmann::json("all")}), RpcSendResult::Sent);
    session.RunFor(50);
    EXPECT_EQ(Count("announce:all"), 2u) << "both clients ran it";

    // One client
    g_calls.clear();
    EXPECT_EQ(session.server->CallRpc(probe, "Announce", {nlohmann::json("one")}, RpcTarget::Client, b), RpcSendResult::Sent);
    session.RunFor(50);
    EXPECT_EQ(Count("announce:one"), 1u);

    // The owner: only the owner runs a server-to-owner RPC
    g_calls.clear();
    EXPECT_EQ(session.server->CallRpc(probe, "Tell", {nlohmann::json(5)}, RpcTarget::Owner), RpcSendResult::Sent);
    session.RunFor(50);
    EXPECT_EQ(g_calls.size(), 1u);
    EXPECT_EQ(g_calls[0], "tell:5");
    // A server-to-owner RPC cannot be aimed at everybody or at somebody else
    EXPECT_EQ(session.server->CallRpc(probe, "Tell", {nlohmann::json(5)}, RpcTarget::All), RpcSendResult::WrongDirection);
    EXPECT_EQ(session.server->CallRpc(probe, "Tell", {nlohmann::json(5)}, RpcTarget::Client, b), RpcSendResult::WrongDirection);
    // The server cannot call a client-to-server RPC, and an unknown client gets nothing
    EXPECT_EQ(session.server->CallRpc(probe, "Fire", {nlohmann::json(1.0), nlohmann::json(1)}), RpcSendResult::WrongDirection);
    EXPECT_EQ(session.server->CallRpc(probe, "Announce", {nlohmann::json("x")}, RpcTarget::Client, 9999), RpcSendResult::NoTarget);
    // An owner-targeted call on an object the server owns has nobody to go to
    const NetworkId own = session.SpawnProbe(kServerClientId);
    EXPECT_EQ(session.server->CallRpc(session.ServerProbe(own), "Tell", {nlohmann::json(1)}, RpcTarget::Owner),
              RpcSendResult::NoTarget);
    // A client that is not the owner never runs a Tell it is sent by a hostile server: counted
    session.RunFor(50);
    EXPECT_EQ(first.client->RpcStatistics().rejectedAuthority, 0u);
}

TEST_F(NetworkRpcSessionTest, ReliableRpcsArriveOnceAndInOrderOverALossyLinkAndUnreliableOnesNeedNotArrive)
{
    SessionConfig config;
    config.link.latencyMicros = 20'000;
    config.link.jitterMicros = 5'000;
    config.link.lossPpm = 200'000;
    config.link.reorderPpm = 100'000;
    config.link.reorderDelayMicros = 30'000;
    config.link.duplicatePpm = 100'000;
    config.tweakServer = [](NetworkServerOptions &options) { options.rpc.maxSendPerSecond = 2000; };
    config.tweakClient = [](NetworkClientOptions &options)
    {
        options.rpc.maxPerSecond = 1000;
        options.rpc.maxPerRpcPerSecond = 1000;
        options.rpc.maxQueued = 256;
    };
    Session session(config);
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const NetworkId pawn = session.SpawnProbe(id);
    RpcProbe &probe = session.ServerProbe(pawn);

    constexpr int kCount = 60;
    for (int i = 0; i < kCount; ++i)
    {
        EXPECT_EQ(session.server->CallRpc(probe, "Order", {nlohmann::json(i)}), RpcSendResult::Sent);
        EXPECT_EQ(session.server->CallRpc(probe, "Blip", {nlohmann::json(i)}), RpcSendResult::Sent);
        session.RunFor(2);
    }
    session.RunFor(3000);

    // Reliable: every one, once, in order
    std::vector<int> order;
    std::vector<int> blips;
    for (const std::string &call : g_calls)
    {
        if (call.rfind("order:", 0) == 0)
        {
            order.push_back(std::stoi(call.substr(6)));
        }
        else if (call.rfind("blip:", 0) == 0)
        {
            blips.push_back(std::stoi(call.substr(5)));
        }
    }
    ASSERT_EQ(order.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i)
    {
        EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
    }
    // Unreliable: a subset, never more than were sent, newest wins (so ascending)
    EXPECT_LE(blips.size(), static_cast<std::size_t>(kCount));
    for (std::size_t i = 1; i < blips.size(); ++i)
    {
        EXPECT_GT(blips[i], blips[i - 1]);
    }
}

TEST_F(NetworkRpcSessionTest, UnreliableClientRpcsAreRunWhenTheyArrive)
{
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const NetworkId pawn = session.SpawnProbe(id);
    EXPECT_EQ(rig.client->CallRpc(Session::ClientProbe(rig, pawn), "Ping", {nlohmann::json(11)}), RpcSendResult::Sent);
    session.RunFor(50);
    EXPECT_EQ(Count("ping:11"), 1u);
}

TEST_F(NetworkRpcSessionTest, ARateLimitedClientLosesTheExcessAndStaysConnected)
{
    SessionConfig config;
    config.tweakServer = [](NetworkServerOptions &options)
    {
        options.rpc.maxPerSecond = 1000;
        options.rpc.maxPerRpcPerSecond = 5;
    };
    config.tweakClient = [](NetworkClientOptions &options) { options.rpc.maxSendPerSecond = 2000; };
    Session session(config);
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const NetworkId pawn = session.SpawnProbe(id);

    for (int i = 0; i < 40; ++i)
    {
        (void)rig.client->CallRpc(Session::ClientProbe(rig, pawn), "Fire", {nlohmann::json(1.0), nlohmann::json(i)});
        session.Step(500);
    }
    session.RunFor(100);
    EXPECT_EQ(Count("fire:"), 5u);
    EXPECT_EQ(session.server->RpcStatistics().droppedRate, 35u);
    EXPECT_TRUE(session.server->IsClientJoined(id));
    // The next second the budget is back
    session.RunFor(1100);
    EXPECT_EQ(rig.client->CallRpc(Session::ClientProbe(rig, pawn), "Fire", {nlohmann::json(1.0), nlohmann::json(99)}),
              RpcSendResult::Sent);
    session.RunFor(100);
    EXPECT_EQ(Count("fire:"), 6u);
}

TEST_F(NetworkRpcSessionTest, TheByteBudgetAndTheQueueBoundDropTheRest)
{
    {
        SessionConfig config;
        config.tweakServer = [](NetworkServerOptions &options) { options.rpc.maxBytesPerSecond = 120; };
        Session session(config);
        RawPeer &raw = session.AddRaw();
        const ClientId id = session.JoinRaw(raw);
        const NetworkId pawn = session.SpawnProbe(id);
        for (int i = 0; i < 20; ++i)
        {
            RawCall call = CallFor(session, pawn, "Fire");
            call.args = FireArgs(1.0f, i);
            raw.SendReliable(call.Bytes());
        }
        session.RunFor(100);
        EXPECT_GT(session.server->RpcStatistics().droppedRate, 0u);
        EXPECT_LT(Count("fire:"), 20u);
        EXPECT_GT(Count("fire:"), 0u);
        EXPECT_TRUE(session.server->IsClientJoined(id));
    }
    g_calls.clear();
    {
        SessionConfig config;
        config.tweakServer = [](NetworkServerOptions &options)
        {
            options.rpc.maxQueued = 3;
            options.rpc.maxPerSecond = 1000;
            options.rpc.maxPerRpcPerSecond = 1000;
            options.rpc.maxBytesPerSecond = 1u << 20;
        };
        Session session(config);
        RawPeer &raw = session.AddRaw();
        const ClientId id = session.JoinRaw(raw);
        const NetworkId pawn = session.SpawnProbe(id);
        for (int i = 0; i < 40; ++i)
        {
            RawCall call = CallFor(session, pawn, "Fire");
            call.args = FireArgs(1.0f, i);
            raw.SendReliable(call.Bytes());
        }
        session.RunFor(100);
        EXPECT_GT(session.server->RpcStatistics().droppedQueueFull, 0u);
        EXPECT_LT(Count("fire:"), 40u);
        EXPECT_TRUE(session.server->IsClientJoined(id));
    }
}

TEST_F(NetworkRpcSessionTest, MalformedArgumentsAreRefusedWithoutRunningAnythingOrDroppingTheClient)
{
    Session session;
    RawPeer &raw = session.AddRaw();
    const ClientId id = session.JoinRaw(raw);
    const NetworkId pawn = session.SpawnProbe(id);

    // Invalid UTF-8 in a string, a string longer than its cap, a truncated float, bytes left over
    RawCall chat = CallFor(session, pawn, "Chat");
    chat.args = StringArgs({0xC3, 0x28});
    raw.SendReliable(chat.Bytes());
    chat.args = StringArgs(std::vector<std::uint8_t>(kMaxReplicatedStringBytes + 40, 'a'));
    raw.SendReliable(chat.Bytes());
    RawCall fire = CallFor(session, pawn, "Fire");
    fire.args = {0x00, 0x00};
    raw.SendReliable(fire.Bytes());
    fire.args = FireArgs(1.0f, 1);
    fire.args.push_back(0x01);
    raw.SendReliable(fire.Bytes());
    // A float that is not finite
    fire.args = FireArgs(std::numeric_limits<float>::quiet_NaN(), 1);
    raw.SendReliable(fire.Bytes());
    session.RunFor(100);
    EXPECT_EQ(session.server->RpcStatistics().rejectedArguments, 5u);
    EXPECT_TRUE(g_calls.empty());
    EXPECT_TRUE(session.server->IsClientJoined(id));
}

TEST_F(NetworkRpcSessionTest, UnknownObjectsComponentsRpcsAndTableHashesAreRefused)
{
    Session session;
    RawPeer &raw = session.AddRaw();
    const ClientId id = session.JoinRaw(raw);
    const NetworkId pawn = session.SpawnProbe(id);

    RawCall call = CallFor(session, pawn, "Chat");
    call.args = StringArgs({'h', 'i'});
    RawCall noObject = call;
    noObject.object = 9999;
    RawCall noComponent = call;
    noComponent.component = 200;
    RawCall noRpc = call;
    noRpc.rpcIndex = 31;
    RawCall wrongHash = call;
    wrongHash.hash = static_cast<std::uint16_t>(call.hash + 1);
    RawCall wrongChannel = call;
    wrongChannel.type = kRpcClientUnreliableType;
    for (const RawCall &c : {noObject, noComponent, noRpc, wrongHash})
    {
        raw.SendReliable(c.Bytes());
    }
    raw.SendUnreliable(wrongChannel.Bytes());
    session.RunFor(100);
    EXPECT_EQ(session.server->RpcStatistics().rejectedUnknown, 5u);
    EXPECT_TRUE(g_calls.empty());
    EXPECT_TRUE(session.server->IsClientJoined(id));

    // The good call still goes through
    raw.SendReliable(call.Bytes());
    session.RunFor(100);
    EXPECT_EQ(Count("chat:"), 1u);
}

TEST_F(NetworkRpcSessionTest, AnUndecodableRpcHeaderDropsTheClientAndAnRpcBeforeJoiningToo)
{
    {
        Session session;
        RawPeer &raw = session.AddRaw();
        const ClientId id = session.JoinRaw(raw);
        raw.SendReliable({kRpcClientReliableType, 0x80});
        session.RunFor(100);
        EXPECT_FALSE(session.server->IsClientJoined(id));
        EXPECT_EQ(session.server->Stats().droppedMalformed, 1u);
    }
    {
        Session session;
        RawPeer &raw = session.AddRaw();
        ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
        raw.SendReliable({kRpcClientReliableType, 1, 0, 0, 0, 0});
        session.RunFor(100);
        EXPECT_EQ(session.server->Stats().droppedProtocol, 1u);
    }
}

TEST_F(NetworkRpcSessionTest, AFaultingHandlerIsCountedAndTheServerKeepsRunning)
{
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const NetworkId pawn = session.SpawnProbe(id);
    RpcProbe &probe = Session::ClientProbe(rig, pawn);
    EXPECT_EQ(rig.client->CallRpc(probe, "Boom", {}), RpcSendResult::Sent);
    EXPECT_EQ(rig.client->CallRpc(probe, "Chat", {nlohmann::json("after")}), RpcSendResult::Sent);
    session.RunFor(100);
    EXPECT_EQ(session.server->RpcStatistics().handlerFailed, 1u);
    EXPECT_EQ(Count("chat:"), 1u) << "the call after the fault still ran";
    EXPECT_TRUE(session.server->IsClientJoined(id));
}

TEST_F(NetworkRpcSessionTest, OnlyCosmeticRpcsRunWhereTheSimulationIsDeterministic)
{
    SessionConfig config;
    config.tweakServer = [](NetworkServerOptions &options) { options.cosmeticRpcsOnly = true; };
    config.tweakClient = [](NetworkClientOptions &options) { options.cosmeticRpcsOnly = true; };
    Session session(config);
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const NetworkId pawn = session.SpawnProbe(id);
    RpcProbe &clientProbe = Session::ClientProbe(rig, pawn);

    EXPECT_EQ(rig.client->CallRpc(clientProbe, "Fire", {nlohmann::json(1.0), nlohmann::json(1)}),
              RpcSendResult::NotSimulationSafe);
    EXPECT_EQ(rig.client->CallRpc(clientProbe, "Chat", {nlohmann::json("gg")}), RpcSendResult::Sent);
    EXPECT_EQ(session.server->CallRpc(session.ServerProbe(pawn), "Order", {nlohmann::json(1)}),
              RpcSendResult::NotSimulationSafe);
    EXPECT_EQ(session.server->CallRpc(session.ServerProbe(pawn), "Announce", {nlohmann::json("hi")}), RpcSendResult::Sent);
    session.RunFor(100);
    EXPECT_EQ(Count("chat:"), 1u);
    EXPECT_EQ(Count("announce:"), 1u);
    EXPECT_EQ(Count("fire:"), 0u);

    // A hostile client that sends the simulation RPC anyway is refused by the server
    RawPeer &raw = session.AddRaw();
    const ClientId rawId = session.JoinRaw(raw);
    const NetworkId rawPawn = session.SpawnProbe(rawId);
    RawCall fire = CallFor(session, rawPawn, "Fire");
    fire.args = FireArgs(1.0f, 1);
    raw.SendReliable(fire.Bytes());
    session.RunFor(100);
    EXPECT_EQ(session.server->RpcStatistics().rejectedSimulation, 1u + 1u) << "the server's own refused call counts too";
    EXPECT_EQ(Count("fire:"), 0u);
}

TEST_F(NetworkRpcSessionTest, CallsFromSeveralClientsRunInClientIdOrderEveryTime)
{
    std::vector<std::string> first;
    for (int run = 0; run < 2; ++run)
    {
        g_calls.clear();
        Session session;
        ClientRig &a = session.AddClient();
        ClientRig &b = session.AddClient();
        const ClientId idA = session.Join(a);
        const ClientId idB = session.Join(b);
        const NetworkId pawnA = session.SpawnProbe(idA);
        const NetworkId pawnB = session.SpawnProbe(idB);
        // B calls first, then A, in the same update
        (void)b.client->CallRpc(Session::ClientProbe(b, pawnB), "Chat", {nlohmann::json("b")});
        (void)a.client->CallRpc(Session::ClientProbe(a, pawnA), "Chat", {nlohmann::json("a")});
        session.RunFor(100);
        ASSERT_EQ(g_calls.size(), 2u);
        if (run == 0)
        {
            first = g_calls;
        }
        else
        {
            EXPECT_EQ(g_calls, first);
        }
    }
}

TEST_F(NetworkRpcSessionTest, ARpcOnADespawnedObjectIsDroppedBeforeItRuns)
{
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const NetworkId pawn = session.SpawnProbe(id);
    EXPECT_EQ(rig.client->CallRpc(Session::ClientProbe(rig, pawn), "Chat", {nlohmann::json("late")}), RpcSendResult::Sent);
    // Let the message arrive, then destroy the object before the next dispatch
    session.network.Advance(5000);
    EXPECT_TRUE(session.server->Despawn(pawn));
    session.RunFor(100);
    EXPECT_EQ(Count("chat:"), 0u);
}

// ---- Lua ------------------------------------------------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    class NetworkRpcLuaTest : public NetworkRpcSessionTest
    {
    protected:
        static inline fs::path s_root;

        static void WriteAsset(const std::string &relative, const std::string &source)
        {
            const fs::path path = s_root / "assets" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path) << source;
        }

        static void SetUpTestSuite()
        {
            ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
            s_root = fs::temp_directory_path() / "n2engine_rpc_lua_tests";
            std::error_code ec;
            fs::remove_all(s_root, ec);
            WriteAsset("rpc/Gun.lua", R"(
                local Gun = {}
                Gun.__index = Gun
                Gun.Rpcs = {
                    Fire = { target = "server", reliable = true, args = { "Vector3", { type = "float", min = 0, max = 10 } } },
                    Shout = { target = "client", cosmetic = true, args = { "string" } },
                    Hurt = { target = "owner", args = { "int" } },
                    Crash = { target = "server", args = {} },
                    Spin = { target = "server", args = {} },
                    Ping = { target = "server", reliable = false, args = { "bool" } },
                }
                function Gun:OnRpc_Fire(sender, dir, power)
                    rpc_log[#rpc_log + 1] = "fire:" .. sender .. ":" .. dir.x .. ":" .. dir.y .. ":" .. dir.z .. ":" .. power
                end
                function Gun:OnRpc_Shout(sender, text) rpc_log[#rpc_log + 1] = "shout:" .. sender .. ":" .. text end
                function Gun:OnRpc_Hurt(sender, amount) rpc_log[#rpc_log + 1] = "hurt:" .. amount end
                function Gun:OnRpc_Crash(sender) error("boom") end
                function Gun:OnRpc_Spin(sender) while true do end end
                function Gun:OnRpc_Ping(sender, flag) rpc_log[#rpc_log + 1] = "ping:" .. tostring(flag) end
                function Gun:OnUpdate()
                    if rpc_do then
                        local f = rpc_do
                        rpc_do = nil
                        f(self)
                    end
                end
                return Gun
            )");
            WriteAsset("rpc/Bad.lua", R"(
                local Bad = {}
                Bad.__index = Bad
                Bad.Rpcs = {
                    Good = { target = "server", args = { "int" } },
                    NoTarget = { args = {} },
                    BadTarget = { target = "everyone" },
                    BadType = { target = "server", args = { "table" } },
                    TooMany = { target = "server", args = { "int", "int", "int", "int", "int", "int", "int", "int", "int" } },
                    BadRange = { target = "server", args = { { type = "float", min = 5, max = 1 } } },
                    Typo = { target = "server", reliabel = true },
                    ["bad name"] = { target = "server" },
                }
                return Bad
            )");
            IO::ResourceUUID::Initialize(Math::UUID::Random());
            IO::ResourceLoader::Instance().Initialize(s_root);
        }

        static void TearDownTestSuite()
        {
            std::error_code ec;
            fs::remove_all(s_root, ec);
        }

        static sol::state &Lua() { return Scripting::LuaRuntime::Instance().GetState(); }

        void SetUp() override
        {
            NetworkRpcSessionTest::SetUp();
            Lua().script("rpc_log = {}");
            _saved = Scripting::LuaRuntime::Instance().GetLimits();
            Scripting::LuaRuntime::Instance().SetLimits(
                {200'000, Scripting::LuaRuntime::Instance().GetMemoryUsed() + 16 * 1024 * 1024});
        }

        void TearDown() override { Scripting::LuaRuntime::Instance().SetLimits(_saved); }

        static std::size_t LogSize() { return Lua()["rpc_log"].get<sol::table>().size(); }
        static std::string LogAt(const std::size_t i) { return Lua()["rpc_log"].get<sol::table>().get<std::string>(i); }

        /// Gives the object on the server and on every client a Gun script component
        static Scripting::LuaComponent *AddGun(NetworkObject *object)
        {
            auto *component = object->GetGameObject().AddComponent<Scripting::LuaComponent>();
            component->SetScript(IO::ResourcePath("res://rpc/Gun.lua"));
            return component;
        }

        Scripting::LuaLimits _saved;
    };
}

TEST_F(NetworkRpcLuaTest, ADeclaredLuaRpcRunsOnTheServerWithClampedTypedArguments)
{
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const auto pawn = session.server->Spawn(kPrefab, id);
    ASSERT_TRUE(pawn.has_value());
    ASSERT_TRUE(session.RunUntil([&] { return rig.peer.world.Find(*pawn) != nullptr; }));
    auto *serverGun = AddGun(session.server->World().Find(*pawn));
    auto *clientGun = AddGun(rig.peer.world.Find(*pawn));
    ASSERT_FALSE(serverGun->HasMissingScript());
    ASSERT_FALSE(clientGun->HasMissingScript());

    ASSERT_NE(clientGun->GetRpcTable(), nullptr);
    EXPECT_EQ(clientGun->GetRpcTable()->size(), 6u);
    EXPECT_EQ(HashRpcTable(*clientGun->GetRpcTable()), HashRpcTable(*serverGun->GetRpcTable()));

    Lua().script("rpc_do = function(self) rpc_result = self:Rpc('Fire', Vector3(1, 2, 3), 40) end");
    clientGun->OnUpdate();
    EXPECT_TRUE(Lua()["rpc_result"].get<bool>());
    session.RunFor(100);
    ASSERT_EQ(LogSize(), 1u);
    // The power 40 was clamped to the declared maximum of 10 before it was sent
    EXPECT_NE(LogAt(1).find("fire:" + std::to_string(id) + ":1.0:2.0:3.0:10"), std::string::npos) << LogAt(1);

    // An unreliable one
    Lua().script("rpc_do = function(self) self:Rpc('Ping', true) end");
    clientGun->OnUpdate();
    session.RunFor(100);
    EXPECT_EQ(LogSize(), 2u);
    EXPECT_EQ(LogAt(2), "ping:true");
}

TEST_F(NetworkRpcLuaTest, LuaCallsWithTheWrongArgumentsOrNamesAreLuaErrorsAndSendNothing)
{
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const auto pawn = session.server->Spawn(kPrefab, id);
    ASSERT_TRUE(session.RunUntil([&] { return rig.peer.world.Find(*pawn) != nullptr; }));
    (void)AddGun(session.server->World().Find(*pawn));
    auto *clientGun = AddGun(rig.peer.world.Find(*pawn));

    const char *bad[] = {
        "self:Rpc('Nope')",
        "self:Rpc('Fire')",
        "self:Rpc('Fire', 1, 2)",
        "self:Rpc('Fire', Vector3(1, 2, 3), 'x')",
        "self:Rpc('Fire', Vector3(1, 2, 3), 0/0)",
        "self:Rpc('Fire', Vector3(1, 2, 3), 1, 1)",
        "self:Rpc('Ping', 1)",
        "self:RpcTo(0, 'Shout', 'x')",
    };
    for (const char *code : bad)
    {
        Lua().script(std::string("rpc_do = function(self) rpc_ok = pcall(function() ") + code + " end) end");
        Lua()["rpc_ok"] = true;
        clientGun->OnUpdate();
        EXPECT_FALSE(Lua()["rpc_ok"].get<bool>()) << code;
    }
    session.RunFor(100);
    EXPECT_EQ(LogSize(), 0u);
    EXPECT_EQ(rig.client->RpcStatistics().sent, 0u);

    // A client cannot call a server-to-client Lua RPC: the call is refused on its side
    Lua().script("rpc_do = function(self) rpc_result = self:Rpc('Shout', 'x') end");
    clientGun->OnUpdate();
    EXPECT_FALSE(Lua()["rpc_result"].get<bool>());
}

TEST_F(NetworkRpcLuaTest, TheServerCallsLuaRpcsOnClientsAndOnTheOwner)
{
    Session session;
    ClientRig &first = session.AddClient();
    ClientRig &second = session.AddClient();
    const ClientId a = session.Join(first);
    (void)session.Join(second);
    const auto pawn = session.server->Spawn(kPrefab, a);
    ASSERT_TRUE(session.RunUntil([&] { return first.peer.world.Find(*pawn) != nullptr && second.peer.world.Find(*pawn) != nullptr; }));
    auto *serverGun = AddGun(session.server->World().Find(*pawn));
    (void)AddGun(first.peer.world.Find(*pawn));
    (void)AddGun(second.peer.world.Find(*pawn));

    Lua().script("rpc_do = function(self) rpc_result = self:Rpc('Shout', 'hello') end");
    serverGun->OnUpdate();
    EXPECT_TRUE(Lua()["rpc_result"].get<bool>());
    session.RunFor(100);
    EXPECT_EQ(LogSize(), 2u) << "every client ran Shout";
    EXPECT_EQ(LogAt(1), "shout:0:hello");

    Lua().script("rpc_log = {}");
    Lua().script("rpc_do = function(self) rpc_result = self:Rpc('Hurt', 5) end");
    serverGun->OnUpdate();
    session.RunFor(100);
    ASSERT_EQ(LogSize(), 1u) << "only the owner ran Hurt";
    EXPECT_EQ(LogAt(1), "hurt:5");

    Lua().script("rpc_log = {}");
    Lua().script("rpc_do = function(self) rpc_result = self:RpcTo(" + std::to_string(a) + ", 'Shout', 'just you') end");
    serverGun->OnUpdate();
    session.RunFor(100);
    ASSERT_EQ(LogSize(), 1u);
    EXPECT_EQ(LogAt(1), "shout:0:just you");
}

TEST_F(NetworkRpcLuaTest, AFaultingOrLoopingLuaHandlerDoesNotStopTheServer)
{
    Session session;
    ClientRig &rig = session.AddClient();
    const ClientId id = session.Join(rig);
    const auto pawn = session.server->Spawn(kPrefab, id);
    ASSERT_TRUE(session.RunUntil([&] { return rig.peer.world.Find(*pawn) != nullptr; }));
    auto *serverGun = AddGun(session.server->World().Find(*pawn));
    auto *clientGun = AddGun(rig.peer.world.Find(*pawn));
    (void)serverGun;

    Lua().script("rpc_do = function(self) self:Rpc('Crash') end");
    clientGun->OnUpdate();
    session.RunFor(100);
    Lua().script("rpc_do = function(self) self:Rpc('Fire', Vector3(0, 0, 1), 1) end");
    clientGun->OnUpdate();
    session.RunFor(100);
    EXPECT_EQ(LogSize(), 1u) << "the call after the error ran";
    EXPECT_TRUE(session.server->IsClientJoined(id));

    // A handler that never returns is stopped by the instruction budget; the server goes on
    Lua().script("rpc_do = function(self) self:Rpc('Spin') end");
    clientGun->OnUpdate();
    session.RunFor(100);
    EXPECT_TRUE(session.server->IsClientJoined(id));
    EXPECT_EQ(session.server->RpcStatistics().executed, 2u);
    EXPECT_EQ(session.server->RpcStatistics().handlerFailed, 1u) << "the looping handler was stopped by the budget";
}

TEST_F(NetworkRpcLuaTest, AMalformedRpcsTableLeavesOutOnlyTheBadEntries)
{
    GameObject::Ptr object = GameObject::Create("Bad");
    auto *component = object->AddComponent<Scripting::LuaComponent>();
    component->SetScript(IO::ResourcePath("res://rpc/Bad.lua"));
    ASSERT_FALSE(component->HasMissingScript());
    const std::vector<RpcDescriptor> *table = component->GetRpcTable();
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->size(), 1u);
    EXPECT_EQ((*table)[0].name, "Good");
}
