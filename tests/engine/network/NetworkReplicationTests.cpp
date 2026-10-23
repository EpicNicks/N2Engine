#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include "NetworkTestSupport.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/network/LagCompensatedHitbox.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/network/NetworkClient.hpp"
#include "engine/network/NetworkFingerprint.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/NetworkReplication.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkTransform.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/SimRandom.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

// Field replication end to end (netcode stage B2): a NetworkServer and NetworkClients in one process over the network
// simulator, each with a scene and a world of its own, and the server half (ServerReplicator) on its own for the
// baseline and ack rules. The simulator's links lose, reorder, duplicate and delay, and the clients must still end with
// the server's values.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;

namespace
{
    constexpr std::uint32_t kProtocol = 0x4E32'5245;
    constexpr std::uint64_t kSceneHash = 0xB2B2'B2B2ull;
    constexpr const char *kScenePath = "res://replication.scene";

    /// A component with one replicated field of most kinds and one local field
    class ReplProbe final : public SerializableComponent
    {
    public:
        explicit ReplProbe(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("score", score).Replicated();
            RegisterMember("health", health).Range(0.0, 100.0).Replicated();
            RegisterMember("label", label).Replicated();
            RegisterMember("flag", flag).Replicated();
            RegisterMember("offset", offset).Replicated(0.01);
            RegisterGameObjectRef("target", target).Replicated();
            RegisterMember("local", local);
        }

        [[nodiscard]] std::string GetTypeName() const override { return "ReplProbe"; }

        int score = 0;
        float health = 50.0f;
        std::string label;
        bool flag = false;
        Math::Vector3 offset{0.0f, 0.0f, 0.0f};
        GameObject *target = nullptr;
        int local = 0;
    };

    /// Another component, so an object that has it has another layout than one with a ReplProbe
    class ReplOther final : public SerializableComponent
    {
    public:
        explicit ReplOther(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("value", value).Replicated();
        }

        [[nodiscard]] std::string GetTypeName() const override { return "ReplOther"; }

        int value = 0;
    };

    /// Registered by name, so a prefab (which is rebuilt from JSON) can have them
    [[maybe_unused]] const bool kRegistered = []
    {
        ComponentRegistry &registry = ComponentRegistry::Instance();
        registry.Register("ReplProbe", [](GameObject &go) -> std::unique_ptr<Component> { return std::make_unique<ReplProbe>(go); },
                          false);
        registry.Register("ReplOther", [](GameObject &go) -> std::unique_ptr<Component> { return std::make_unique<ReplOther>(go); },
                          false);
        return true;
    }();

    enum class Kind
    {
        Probe,
        Other,
    };

    std::unique_ptr<Scene> MakeReplScene(const std::string &name, const std::vector<std::uint32_t> &uuids,
                                         const Kind kind = Kind::Probe)
    {
        std::unique_ptr<Scene> scene = Scene::Create(name);
        for (const std::uint32_t n : uuids)
        {
            const GameObject::Ptr gameObject = GameObject::Create("Object" + std::to_string(n));
            gameObject->SetUUID(TestUuid(n));
            gameObject->AddComponent<NetworkObject>();
            if (kind == Kind::Probe)
            {
                gameObject->AddComponent<ReplProbe>();
            }
            else
            {
                gameObject->AddComponent<ReplOther>();
            }
            gameObject->AddComponent<NetworkTransform>();
            scene->AddRootGameObject(gameObject);
        }
        return scene;
    }

    /// A prefab whose root has a NetworkObject, a ReplProbe and a NetworkTransform, registered for this object's life
    class ScopedProbePrefab
    {
    public:
        explicit ScopedProbePrefab(std::string name) : _name(std::move(name))
        {
            const GameObject::Ptr root = GameObject::Create("Prefab_" + _name);
            root->AddComponent<NetworkObject>();
            root->AddComponent<ReplProbe>();
            root->AddComponent<NetworkTransform>();
            _registered = PrefabManager::RegisterPrefab(_name, root);
        }

        ~ScopedProbePrefab() { PrefabManager::UnregisterPrefab(_name); }
        ScopedProbePrefab(const ScopedProbePrefab &) = delete;
        ScopedProbePrefab &operator=(const ScopedProbePrefab &) = delete;

        [[nodiscard]] const std::string &Name() const { return _name; }
        [[nodiscard]] bool Registered() const { return _registered; }

    private:
        std::string _name;
        bool _registered = false;
    };

    /// A scene and the world bound to it. The world is declared after the scene, so it is destroyed first.
    struct Side
    {
        Side(const std::string &name, const std::vector<std::uint32_t> &uuids, const Kind kind = Kind::Probe)
            : scene(MakeReplScene(name, uuids, kind))
        {
            bound = world.BindScene(*scene);
        }

        ReplProbe *Probe(const std::uint32_t n) const
        {
            const GameObject::Ptr object = FindByNumber(*scene, n);
            return object ? object->GetComponent<ReplProbe>() : nullptr;
        }

        NetworkTransform *NetTransform(const std::uint32_t n) const
        {
            const GameObject::Ptr object = FindByNumber(*scene, n);
            return object ? object->GetComponent<NetworkTransform>() : nullptr;
        }

        /// The probe of the object with this net id (a scene object or a spawned one), or nullptr
        ReplProbe *ProbeOf(const NetworkId id) const
        {
            NetworkObject *const object = world.Find(id);
            return object != nullptr ? object->GetGameObject().GetComponent<ReplProbe>() : nullptr;
        }

        std::unique_ptr<Scene> scene;
        NetworkWorld world;
        std::expected<std::uint32_t, std::string> bound{std::unexpected(std::string("not bound"))};
    };

    bool NearlyEqual(const double a, const double b, const double tolerance)
    {
        return std::fabs(a - b) <= tolerance;
    }

    /// The replicated fields of two probes agree, within what their encodings keep
    bool SameProbe(const ReplProbe &a, const ReplProbe &b)
    {
        return a.score == b.score && a.label == b.label && a.flag == b.flag && NearlyEqual(a.health, b.health, 0.001) &&
               NearlyEqual(a.offset.x, b.offset.x, 0.0055) && NearlyEqual(a.offset.y, b.offset.y, 0.0055) &&
               NearlyEqual(a.offset.z, b.offset.z, 0.0055);
    }

    /// One random change to a probe (every random draw is a statement of its own)
    void Mutate(ReplProbe &probe, N2Net::SimRandom &random)
    {
        const std::uint32_t pick = random.NextBelow(5);
        switch (pick)
        {
        case 0:
            probe.score = static_cast<int>(random.NextBelow(100000));
            break;
        case 1:
            probe.health = static_cast<float>(random.NextBelow(10001)) / 100.0f;
            break;
        case 2:
            probe.label = "label-" + std::to_string(random.NextBelow(1000));
            break;
        case 3:
            probe.flag = !probe.flag;
            break;
        default:
        {
            const float x = static_cast<float>(random.NextBelow(2000)) / 10.0f;
            const float y = static_cast<float>(random.NextBelow(2000)) / 10.0f;
            const float z = static_cast<float>(random.NextBelow(2000)) / 10.0f;
            probe.offset = Math::Vector3(x, y, z);
            break;
        }
        }
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
                                               else
                                               {
                                                   states.emplace_back(message.begin(), message.end());
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
        std::vector<std::vector<std::uint8_t>> states;

    private:
        N2Net::SimTransport *_endpoint;
        N2Net::PeerId _server;
        N2Net::Connection _connection;
    };

    struct ClientRig
    {
        ClientRig(const std::vector<std::uint32_t> &uuids, const std::string &name, const Kind kind) : side(name, uuids, kind) {}

        Side side;
        N2Net::SimTransport *endpoint = nullptr;
        std::unique_ptr<NetworkClient> client;
    };

    struct SessionConfig
    {
        N2Net::SimLinkConfig link;
        std::uint64_t seed = 1;
        std::vector<std::uint32_t> sceneObjects = {1, 2, 3};
        std::uint32_t sendRateHz = 30;
        ServerReplicationOptions replication;
    };

    class Session
    {
    public:
        explicit Session(SessionConfig configuration = {})
            : config(std::move(configuration)), network(config.seed), serverSide("server", config.sceneObjects)
        {
            serverEndpoint = &network.AddEndpoint();
            NetworkServerOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            options.stateSendRateHz = config.sendRateHz;
            options.replication = config.replication;
            server = std::make_unique<NetworkServer>(*serverEndpoint, serverSide.world, std::move(options));
        }

        ClientRig &AddClient(const Kind kind = Kind::Probe)
        {
            auto rig = std::make_unique<ClientRig>(config.sceneObjects, "client", kind);
            N2Net::SimTransport &endpoint = network.AddEndpoint();
            rig->endpoint = &endpoint;
            network.Link(serverEndpoint->Id(), endpoint.Id(), config.link);

            NetworkClientOptions options;
            options.protocolId = kProtocol;
            options.scenePath = kScenePath;
            options.sceneHash = kSceneHash;
            options.prefabs = PrefabTable::Capture();
            rig->client = std::make_unique<NetworkClient>(endpoint, rig->side.world, std::move(options));
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

        /// Every client's probe of every scene object has the server's values
        bool AllConverged() const
        {
            for (const auto &rig : clients)
            {
                if (!rig->client->IsJoined())
                {
                    return false;
                }
                for (const auto &entry : serverSide.world.Objects())
                {
                    const ReplProbe *const mine = serverSide.ProbeOf(NetworkId{entry.first});
                    const ReplProbe *const theirs = rig->side.ProbeOf(NetworkId{entry.first});
                    if (mine == nullptr || theirs == nullptr || !SameProbe(*mine, *theirs))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        SessionConfig config;
        N2Net::SimNetwork network;
        Side serverSide;
        N2Net::SimTransport *serverEndpoint = nullptr;
        std::unique_ptr<NetworkServer> server;
        std::vector<std::unique_ptr<ClientRig>> clients;
        std::vector<std::unique_ptr<RawPeer>> raws;
    };

    void JoinRaw(Session &session, RawPeer &raw)
    {
        ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
        ClientAcceptMessage accept;
        accept.fingerprint = raw.Hello()->fingerprint;
        raw.SendReliable(Encode(accept));
        const std::size_t before = session.server->JoinedClientCount();
        ASSERT_TRUE(session.RunUntil([&] { return session.server->JoinedClientCount() == before + 1; }));
    }

    /// The server half on its own: a world, a replicator and one client of it
    struct DirectRig
    {
        explicit DirectRig(const ServerReplicationOptions &options = {}, const std::vector<std::uint32_t> &uuids = {1, 2, 3})
            : side("direct", uuids), replicator(options)
        {
            replicator.AddClient(1);
        }

        /// One send tick: the messages for the client
        std::vector<std::vector<std::uint8_t>> Tick(const std::uint64_t now = 0)
        {
            replicator.BeginTick(side.world);
            return replicator.BuildForClient(1, now, kMaxStateUpdateBytes);
        }

        Side side;
        ServerReplicator replicator;
    };

    struct ParsedEntry
    {
        std::uint32_t id = 0;
        std::uint64_t mask = 0;

        [[nodiscard]] std::size_t FieldCount() const { return static_cast<std::size_t>(std::popcount(mask)); }
    };

    struct ParsedMessage
    {
        std::uint32_t sequence = 0;
        std::vector<ParsedEntry> entries;
    };

    /// The sequence and, per entry, the net id and the field mask (an object has `maskBytes` of mask)
    ParsedMessage Parse(const std::vector<std::uint8_t> &message, const std::size_t maskBytes)
    {
        ParsedMessage parsed;
        const auto view = DecodeStateUpdate(message);
        EXPECT_TRUE(view.has_value());
        if (!view.has_value())
        {
            return parsed;
        }
        parsed.sequence = view->header.sequence;
        N2Net::ByteReader reader(view->entries);
        for (std::uint16_t i = 0; i < view->header.entryCount; ++i)
        {
            ParsedEntry entry;
            std::uint64_t id = 0;
            std::uint64_t length = 0;
            EXPECT_TRUE(ReadVarint(reader, id));
            (void)reader.U16();
            EXPECT_TRUE(ReadVarint(reader, length));
            const std::span<const std::uint8_t> payload = reader.Bytes(static_cast<std::size_t>(length));
            EXPECT_FALSE(reader.Failed());
            entry.id = static_cast<std::uint32_t>(id);
            for (std::size_t b = 0; b < maskBytes && b < payload.size(); ++b)
            {
                entry.mask |= static_cast<std::uint64_t>(payload[b]) << (8 * b);
            }
            parsed.entries.push_back(entry);
        }
        EXPECT_EQ(reader.Remaining(), 0u);
        return parsed;
    }

    /// The client's ack for the messages it received (their sequences), as NetworkClient would send it
    StateAckMessage AckFor(const std::vector<std::uint32_t> &sequences)
    {
        StateAckMessage ack;
        for (const std::uint32_t sequence : sequences)
        {
            ack.latest = std::max(ack.latest, sequence);
        }
        for (const std::uint32_t sequence : sequences)
        {
            if (sequence < ack.latest && ack.latest - sequence <= 32)
            {
                ack.bits |= std::uint32_t{1} << (ack.latest - sequence - 1);
            }
        }
        return ack;
    }

    std::size_t TotalBytes(const std::vector<std::vector<std::uint8_t>> &messages)
    {
        std::size_t total = 0;
        for (const std::vector<std::uint8_t> &message : messages)
        {
            total += message.size();
        }
        return total;
    }

    std::vector<std::uint32_t> SequencesOf(const std::vector<std::vector<std::uint8_t>> &messages)
    {
        std::vector<std::uint32_t> sequences;
        for (const std::vector<std::uint8_t> &message : messages)
        {
            const auto view = DecodeStateUpdate(message);
            EXPECT_TRUE(view.has_value());
            if (view.has_value())
            {
                sequences.push_back(view->header.sequence);
            }
        }
        return sequences;
    }

    /// A probe object's mask is this many bytes: the nine fields of a ReplProbe's six and a NetworkTransform's three
    std::size_t ProbeMaskBytes(const Side &side)
    {
        const GameObject::Ptr object = FindByNumber(*side.scene, 1);
        return BuildObjectLayout(*object).MaskBytes();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Layouts and registration

TEST(ReplicationLayoutTest, ListsTheReplicatedFieldsOfEveryComponentInOrder)
{
    Side side("layout", {1});
    const GameObject::Ptr object = FindByNumber(*side.scene, 1);
    const ObjectLayout layout = BuildObjectLayout(*object);
    ASSERT_TRUE(layout.usable);
    ASSERT_EQ(layout.components.size(), 2u); // the ReplProbe and the NetworkTransform; NetworkObject has none

    std::vector<std::string> names;
    for (const FieldInfo &field : layout.fields)
    {
        names.push_back(field.name);
    }
    const std::vector<std::string> expected = {"score", "health", "label", "flag", "offset", "target",
                                               "position", "rotation", "teleportSerial"};
    EXPECT_EQ(names, expected);
    EXPECT_EQ(layout.MaskBytes(), 2u);
    EXPECT_EQ(layout.fieldComponent.size(), layout.fields.size());
    EXPECT_EQ(layout.fieldComponent[0], 0u);
    EXPECT_EQ(layout.fieldComponent[8], 1u);

    // The local field and the NetworkTransform's disabled scale are not in it
    EXPECT_EQ(std::count(names.begin(), names.end(), "local"), 0);
    EXPECT_EQ(std::count(names.begin(), names.end(), "scale"), 0);
    EXPECT_EQ(BuildObjectLayout(*object).hash, layout.hash) << "the hash is a function of the layout";
}

TEST(ReplicationLayoutTest, TheHashSeesAnotherComponentAndAnotherStep)
{
    Side probe("probe", {1});
    Side other("other", {1}, Kind::Other);
    const ObjectLayout probeLayout = BuildObjectLayout(*FindByNumber(*probe.scene, 1));
    const ObjectLayout otherLayout = BuildObjectLayout(*FindByNumber(*other.scene, 1));
    ASSERT_TRUE(otherLayout.usable);
    EXPECT_NE(probeLayout.hash, otherLayout.hash);

    NetworkTransform *const transform = probe.NetTransform(1);
    ASSERT_NE(transform, nullptr);
    transform->SetEditorFields(nlohmann::json{{"positionStep", 0.5}}, nullptr);
    EXPECT_NE(BuildObjectLayout(*FindByNumber(*probe.scene, 1)).hash, probeLayout.hash)
        << "a position step changes the bytes of the position";
    transform->SetEditorFields(nlohmann::json{{"replicatePosition", false}}, nullptr);
    const ObjectLayout without = BuildObjectLayout(*FindByNumber(*probe.scene, 1));
    EXPECT_EQ(without.fields.size(), probeLayout.fields.size() - 1);
}

namespace
{
    class ManyFlags final : public SerializableComponent
    {
    public:
        explicit ManyFlags(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            for (std::size_t i = 0; i < flags.size(); ++i)
            {
                RegisterMember("flag" + std::to_string(i), flags[i]).Replicated();
            }
        }

        [[nodiscard]] std::string GetTypeName() const override { return "ManyFlags"; }

        std::array<bool, 70> flags{};
    };

    class ManyStrings final : public SerializableComponent
    {
    public:
        explicit ManyStrings(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            for (std::size_t i = 0; i < strings.size(); ++i)
            {
                RegisterMember("text" + std::to_string(i), strings[i]).Replicated();
            }
        }

        [[nodiscard]] std::string GetTypeName() const override { return "ManyStrings"; }

        std::array<std::string, 8> strings;
    };

    class WrongKind final : public SerializableComponent
    {
    public:
        explicit WrongKind(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("blob", blob).Replicated();
            RegisterComponentRef("owner", owner).Replicated();
            RegisterGameObjectRefVector("targets", targets).Replicated();
            RegisterMember("fine", fine).Replicated();
        }

        [[nodiscard]] std::string GetTypeName() const override { return "WrongKind"; }

        nlohmann::json blob = nlohmann::json::object();
        Component *owner = nullptr;
        std::vector<GameObject *> targets;
        int fine = 0;
    };
}

TEST(ReplicationLayoutTest, AnObjectHasAtMostSixtyFourReplicatedFields)
{
    const GameObject::Ptr object = GameObject::Create("Flags");
    object->AddComponent<ManyFlags>();
    const ObjectLayout layout = BuildObjectLayout(*object);
    EXPECT_EQ(layout.fields.size(), kMaxReplicatedFieldsPerObject);
    EXPECT_TRUE(layout.usable);
    EXPECT_EQ(layout.MaskBytes(), 8u);
}

TEST(ReplicationLayoutTest, AnObjectWhoseFieldsCouldOutgrowAnEntryReplicatesNothing)
{
    const GameObject::Ptr object = GameObject::Create("Strings");
    object->AddComponent<ManyStrings>();
    const ObjectLayout layout = BuildObjectLayout(*object);
    EXPECT_EQ(layout.fields.size(), 8u);
    EXPECT_FALSE(layout.usable) << "8 strings of up to 130 bytes pass " << kMaxReplicatedEntryBytes;
}

TEST(ReplicationLayoutTest, AKindThatCannotBeReplicatedStaysLocal)
{
    const GameObject::Ptr object = GameObject::Create("Wrong");
    auto *const component = object->AddComponent<WrongKind>();
    const std::vector<FieldInfo> fields = component->DescribeFields();
    for (const FieldInfo &field : fields)
    {
        EXPECT_EQ(field.replicated, field.name == "fine") << field.name;
    }
    const ObjectLayout layout = BuildObjectLayout(*object);
    ASSERT_EQ(layout.fields.size(), 1u);
    EXPECT_EQ(layout.fields[0].name, "fine");
}

TEST(ReplicationLayoutTest, ALayoutIsRebuiltWhenTheComponentsChange)
{
    Side side("cache", {1});
    LayoutCache cache;
    const GameObject::Ptr object = FindByNumber(*side.scene, 1);
    const std::uint16_t before = cache.Get(NetworkId{1}, *object).hash;
    EXPECT_EQ(cache.Get(NetworkId{1}, *object).hash, before);
    object->AddComponent<ReplOther>();
    const ObjectLayout &after = cache.Get(NetworkId{1}, *object);
    EXPECT_NE(after.hash, before);
    EXPECT_EQ(after.fields.size(), 10u);
    cache.Forget(NetworkId{1});
    EXPECT_EQ(cache.Size(), 0u);
}

TEST(ReplicationFingerprintTest, TheRegistryHashCoversWhichFieldsAreReplicatedAndHow)
{
    struct Variant
    {
        bool replicated = true;
        double step = 0.0;
    };
    static Variant variant;
    class Probe final : public SerializableComponent
    {
    public:
        explicit Probe(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("a", a);
            if (variant.replicated)
            {
                RegisterMember("b", b).Replicated(variant.step);
            }
            else
            {
                RegisterMember("b", b);
            }
        }

        [[nodiscard]] std::string GetTypeName() const override { return "ReplFingerprintProbe"; }

        int a = 0;
        float b = 0.0f;
    };

    ComponentRegistry &registry = ComponentRegistry::Instance();
    const auto registerProbe = [&registry]
    {
        registry.Register("ReplFingerprintProbe",
                          [](GameObject &go) -> std::unique_ptr<Component> { return std::make_unique<Probe>(go); }, false);
    };
    const PrefabTable table = PrefabTable::Capture();

    variant = {true, 0.0};
    registerProbe();
    const std::uint64_t replicated = HashRegistry(table);
    EXPECT_EQ(HashRegistry(table), replicated) << "the same registry hashes the same";

    variant = {false, 0.0};
    registerProbe();
    EXPECT_NE(HashRegistry(table), replicated) << "a field that is no longer replicated";

    variant = {true, 0.25};
    registerProbe();
    const std::uint64_t stepped = HashRegistry(table);
    EXPECT_NE(stepped, replicated) << "a step changes the bytes of the field";

    variant = {true, 0.0};
    registerProbe();
    EXPECT_EQ(HashRegistry(table), replicated);
}

// ---------------------------------------------------------------------------------------------------------------------
// The server half: baselines and acknowledgements

TEST(ServerReplicatorTest, TheFirstTickSendsEverythingAndAnAckSilencesIt)
{
    DirectRig rig;
    const std::size_t maskBytes = ProbeMaskBytes(rig.side);
    ASSERT_EQ(maskBytes, 2u);

    const auto first = rig.Tick();
    ASSERT_EQ(first.size(), 1u);
    const ParsedMessage parsed = Parse(first[0], maskBytes);
    EXPECT_EQ(parsed.sequence, 1u);
    ASSERT_EQ(parsed.entries.size(), 3u) << "one entry per object, in net id order";
    for (std::size_t i = 0; i < parsed.entries.size(); ++i)
    {
        EXPECT_EQ(parsed.entries[i].id, i + 1);
        EXPECT_EQ(parsed.entries[i].FieldCount(), 9u) << "a new object is sent whole";
    }
    EXPECT_EQ(rig.replicator.InFlightMessages(1), 1u);

    // Not acknowledged: what it carried is carried again (the client may not have it)
    const auto second = rig.Tick();
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(Parse(second[0], maskBytes).entries[0].FieldCount(), 9u);

    ASSERT_TRUE(rig.replicator.OnAck(1, AckFor({1, 2})));
    EXPECT_EQ(rig.replicator.InFlightMessages(1), 0u);
    EXPECT_TRUE(rig.Tick().empty()) << "nothing changed since what the client has";
    EXPECT_EQ(rig.replicator.Stats().messagesAcked, 2u);
}

TEST(ServerReplicatorTest, OnlyTheChangedFieldsAreSentAgainstTheAcknowledgedBaseline)
{
    DirectRig rig;
    const std::size_t maskBytes = ProbeMaskBytes(rig.side);
    const auto first = rig.Tick();
    ASSERT_TRUE(rig.replicator.OnAck(1, AckFor(SequencesOf(first))));
    ASSERT_TRUE(rig.Tick().empty());

    rig.side.Probe(2)->score = 41;
    const auto changed = rig.Tick();
    ASSERT_EQ(changed.size(), 1u);
    const ParsedMessage parsed = Parse(changed[0], maskBytes);
    ASSERT_EQ(parsed.entries.size(), 1u);
    EXPECT_EQ(parsed.entries[0].id, 2u);
    EXPECT_EQ(parsed.entries[0].FieldCount(), 1u) << "just the score";
    EXPECT_EQ(parsed.entries[0].mask, 1u) << "the score is the first field";

    // Changed again before the ack: still just the score, now its newest value
    rig.side.Probe(2)->score = 42;
    const auto again = rig.Tick();
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(Parse(again[0], maskBytes).entries[0].FieldCount(), 1u);

    // Both acknowledged: quiet again
    ASSERT_TRUE(rig.replicator.OnAck(1, AckFor(SequencesOf({changed[0], again[0]}))));
    EXPECT_TRUE(rig.Tick().empty());

    // A field that changes and changes back between ticks costs nothing the second time
    rig.side.Probe(1)->flag = true;
    const auto flagged = rig.Tick();
    ASSERT_EQ(flagged.size(), 1u);
    ASSERT_TRUE(rig.replicator.OnAck(1, AckFor(SequencesOf(flagged))));
    rig.side.Probe(1)->flag = false;
    rig.side.Probe(1)->flag = true;
    EXPECT_TRUE(rig.Tick().empty()) << "the value at the tick is what the client has";
}

TEST(ServerReplicatorTest, AnOlderAckAfterANewerOneDoesNotTakeTheBaselineBack)
{
    DirectRig rig;
    const auto first = rig.Tick();
    ASSERT_TRUE(rig.replicator.OnAck(1, AckFor(SequencesOf(first))));

    rig.side.Probe(1)->score = 1;
    const auto older = rig.Tick();
    rig.side.Probe(1)->score = 2;
    const auto newer = rig.Tick();
    ASSERT_EQ(older.size(), 1u);
    ASSERT_EQ(newer.size(), 1u);
    const std::uint32_t olderSequence = SequencesOf(older)[0];
    const std::uint32_t newerSequence = SequencesOf(newer)[0];

    // The newer one is acknowledged first; the older one's ack arrives late
    ASSERT_TRUE(rig.replicator.OnAck(1, StateAckMessage{newerSequence, 0}));
    ASSERT_TRUE(rig.replicator.OnAck(1, StateAckMessage{olderSequence, 0}));
    EXPECT_EQ(rig.replicator.InFlightMessages(1), 0u);
    EXPECT_TRUE(rig.Tick().empty()) << "the baseline is the value 2; had the late ack won it would be 1";
}

TEST(ServerReplicatorTest, AckedTwiceChangesNothingTheSecondTimeAndAnUnsentSequenceIsRefused)
{
    DirectRig rig;
    const auto first = rig.Tick();
    const StateAckMessage ack = AckFor(SequencesOf(first));
    ASSERT_TRUE(rig.replicator.OnAck(1, ack));
    EXPECT_EQ(rig.replicator.Stats().messagesAcked, 1u);
    ASSERT_TRUE(rig.replicator.OnAck(1, ack));
    EXPECT_EQ(rig.replicator.Stats().messagesAcked, 1u) << "a duplicate ack is nothing";

    EXPECT_FALSE(rig.replicator.OnAck(1, StateAckMessage{99, 0})) << "a sequence never sent";
    EXPECT_EQ(rig.replicator.Stats().acksInvalid, 1u);
    // Bits naming sequences before the first one are nothing (no underflow)
    EXPECT_TRUE(rig.replicator.OnAck(1, StateAckMessage{1, 0xFFFFFFFFu}));
    // An ack for a client the replicator does not know is ignored
    EXPECT_TRUE(rig.replicator.OnAck(77, StateAckMessage{5, 0}));
}

TEST(ServerReplicatorTest, TheHistoryIsBoundedAndAGivenUpMessageSendsTheObjectWhole)
{
    ServerReplicationOptions options;
    options.maxInFlightMessages = 8;
    DirectRig rig(options);
    const std::size_t maskBytes = ProbeMaskBytes(rig.side);

    ASSERT_TRUE(rig.replicator.OnAck(1, AckFor(SequencesOf(rig.Tick()))));
    // The client never answers again: a change every tick
    std::size_t lastFields = 0;
    for (int tick = 0; tick < 100; ++tick)
    {
        rig.side.Probe(1)->score = tick + 10;
        const auto messages = rig.Tick(static_cast<std::uint64_t>(tick));
        EXPECT_LE(rig.replicator.InFlightMessages(1), 8u);
        if (!messages.empty())
        {
            lastFields = Parse(messages[0], maskBytes).entries[0].FieldCount();
        }
    }
    EXPECT_GT(rig.replicator.Stats().messagesExpired, 0u);
    EXPECT_EQ(lastFields, 9u) << "with the baseline given up, the object goes out whole";
}

TEST(ServerReplicatorTest, AMessageWithNoAckIsGivenUpAfterTheTimeout)
{
    ServerReplicationOptions options;
    options.recordTimeoutMicros = 1'000'000;
    DirectRig rig(options);
    (void)rig.Tick(0);
    EXPECT_EQ(rig.replicator.InFlightMessages(1), 1u);
    (void)rig.Tick(500'000);
    EXPECT_EQ(rig.replicator.Stats().messagesExpired, 0u);
    (void)rig.Tick(1'200'000);
    EXPECT_EQ(rig.replicator.Stats().messagesExpired, 1u) << "the first is past a second";
}

TEST(ServerReplicatorTest, ADespawnedObjectHasNoBaselineLeft)
{
    DirectRig rig;
    ASSERT_TRUE(rig.replicator.OnAck(1, AckFor(SequencesOf(rig.Tick()))));
    EXPECT_EQ(rig.replicator.BaselineCount(1), 3u);
    rig.replicator.ForgetObject(NetworkId{2});
    EXPECT_EQ(rig.replicator.BaselineCount(1), 2u);
    rig.replicator.RemoveClient(1);
    EXPECT_EQ(rig.replicator.BaselineCount(1), 0u);
    EXPECT_TRUE(rig.replicator.BuildForClient(1, 0, kMaxStateUpdateBytes).empty());
}

TEST(ServerReplicatorTest, TheSameStateIsTheSameBytes)
{
    DirectRig a;
    DirectRig b;
    N2Net::SimRandom randomA(99);
    N2Net::SimRandom randomB(99);
    for (int tick = 0; tick < 40; ++tick)
    {
        Mutate(*a.side.Probe(1 + static_cast<std::uint32_t>(tick % 3)), randomA);
        Mutate(*b.side.Probe(1 + static_cast<std::uint32_t>(tick % 3)), randomB);
        const auto fromA = a.Tick(static_cast<std::uint64_t>(tick));
        const auto fromB = b.Tick(static_cast<std::uint64_t>(tick));
        ASSERT_EQ(fromA, fromB) << "tick " << tick;
        // Acknowledge every other tick, the same way on both
        if (tick % 2 == 0 && !fromA.empty())
        {
            ASSERT_TRUE(a.replicator.OnAck(1, AckFor(SequencesOf(fromA))));
            ASSERT_TRUE(b.replicator.OnAck(1, AckFor(SequencesOf(fromB))));
        }
    }
}

TEST(ServerReplicatorTest, MessagesStayWithinTheLimitAndTheObjectsThatDidNotFitComeFirstNextTick)
{
    // Fifty objects with a long label each do not fit in one message, and the budget of one tick is two messages
    std::vector<std::uint32_t> uuids;
    for (std::uint32_t n = 1; n <= 50; ++n)
    {
        uuids.push_back(n);
    }
    ServerReplicationOptions options;
    options.maxMessagesPerTick = 2;
    DirectRig rig(options, uuids);
    for (std::uint32_t n = 1; n <= 50; ++n)
    {
        rig.side.Probe(n)->label = std::string(100, 'x');
    }
    const std::size_t maskBytes = ProbeMaskBytes(rig.side);

    std::vector<std::uint32_t> seen;
    for (int tick = 0; tick < 12 && seen.size() < 50; ++tick)
    {
        const auto messages = rig.Tick(static_cast<std::uint64_t>(tick));
        EXPECT_LE(messages.size(), 2u);
        for (const std::vector<std::uint8_t> &message : messages)
        {
            EXPECT_LE(message.size(), kMaxStateUpdateBytes);
            for (const ParsedEntry &entry : Parse(message, maskBytes).entries)
            {
                if (std::find(seen.begin(), seen.end(), entry.id) == seen.end())
                {
                    seen.push_back(entry.id);
                }
            }
        }
        ASSERT_TRUE(rig.replicator.OnAck(1, AckFor(SequencesOf(messages))));
    }
    EXPECT_EQ(seen.size(), 50u) << "every object was sent within a few ticks, none starved";
}

TEST(ServerReplicatorTest, ADeltaIsMuchSmallerThanSendingEverythingEveryTick)
{
    std::vector<std::uint32_t> uuids;
    for (std::uint32_t n = 1; n <= 50; ++n)
    {
        uuids.push_back(n);
    }
    ServerReplicationOptions options;
    options.maxMessagesPerTick = 64;
    DirectRig rig(options, uuids);
    for (std::uint32_t n = 1; n <= 50; ++n)
    {
        rig.side.Probe(n)->label = std::string(60, 'y');
    }

    // The cost of the whole state once: what resending everything every tick would pay each time
    const auto whole = rig.Tick(0);
    const std::size_t wholeBytes = TotalBytes(whole);
    ASSERT_GT(wholeBytes, 2000u);
    ASSERT_TRUE(rig.replicator.OnAck(1, AckFor(SequencesOf(whole))));

    // 300 ticks in which one object's score changes, every tick acknowledged
    std::size_t deltaBytes = 0;
    for (std::uint32_t tick = 1; tick <= 300; ++tick)
    {
        rig.side.Probe(1 + tick % 50)->score = static_cast<int>(tick);
        const auto messages = rig.Tick(tick);
        deltaBytes += TotalBytes(messages);
        ASSERT_TRUE(rig.replicator.OnAck(1, AckFor(SequencesOf(messages))));
    }
    const std::size_t everything = wholeBytes * 300;
    EXPECT_LT(deltaBytes * 20, everything) << deltaBytes << " bytes of deltas against " << everything;
}

// ---------------------------------------------------------------------------------------------------------------------
// The client half on its own

namespace
{
    /// A server world and a client world with the same objects, a replicator for one client, and the receiving end
    struct HalvesRig
    {
        explicit HalvesRig(const std::vector<std::uint32_t> &serverObjects = {1, 2, 3},
                           const std::vector<std::uint32_t> &clientObjects = {1, 2, 3})
            : server("half-server", serverObjects), client("half-client", clientObjects)
        {
            replicator.AddClient(1);
        }

        /// The messages of one server tick
        std::vector<std::vector<std::uint8_t>> Tick()
        {
            replicator.BeginTick(server.world);
            return replicator.BuildForClient(1, 0, kMaxStateUpdateBytes);
        }

        Side server;
        Side client;
        ServerReplicator replicator;
        ClientReplicator receiver;
    };
}

TEST(ClientReplicatorTest, AppliesAMessageAcknowledgesItAndIgnoresAReplay)
{
    HalvesRig rig;
    rig.server.Probe(1)->score = 5;
    rig.server.Probe(2)->label = "two";
    const auto messages = rig.Tick();
    ASSERT_EQ(messages.size(), 1u);

    EXPECT_FALSE(rig.receiver.AckPending());
    EXPECT_EQ(rig.receiver.OnStateUpdate(rig.client.world, messages[0]), ClientReplicator::Result::Applied);
    EXPECT_EQ(rig.client.Probe(1)->score, 5);
    EXPECT_EQ(rig.client.Probe(2)->label, "two");
    EXPECT_TRUE(rig.receiver.AckPending());
    const StateAckMessage ack = rig.receiver.TakeAck();
    EXPECT_EQ(ack.latest, 1u);
    EXPECT_EQ(ack.bits, 0u);
    EXPECT_FALSE(rig.receiver.AckPending()) << "taking the ack clears it";
    EXPECT_EQ(rig.receiver.Stats().messagesApplied, 1u);
    EXPECT_GE(rig.receiver.Stats().entriesApplied, 3u);

    // The same message again (a duplicate the connection did not catch): older or equal is ignored, whatever it says
    rig.client.Probe(1)->score = 99;
    EXPECT_EQ(rig.receiver.OnStateUpdate(rig.client.world, messages[0]), ClientReplicator::Result::Stale);
    EXPECT_EQ(rig.client.Probe(1)->score, 99) << "a stale message changes nothing";
    EXPECT_EQ(rig.receiver.Stats().messagesStale, 1u);
    EXPECT_FALSE(rig.receiver.AckPending());
}

TEST(ClientReplicatorTest, TheAckNamesTheNewestAndTheThirtyTwoBeforeIt)
{
    HalvesRig rig;
    std::vector<std::vector<std::uint8_t>> messages;
    for (int tick = 0; tick < 4; ++tick)
    {
        rig.server.Probe(1)->score = tick + 1;
        const auto built = rig.Tick();
        ASSERT_EQ(built.size(), 1u);
        messages.push_back(built[0]);
    }
    // Messages 1, 2 and 4 arrive; 3 is lost
    for (const std::size_t index : {std::size_t{0}, std::size_t{1}, std::size_t{3}})
    {
        EXPECT_EQ(rig.receiver.OnStateUpdate(rig.client.world, messages[index]), ClientReplicator::Result::Applied);
    }
    const StateAckMessage ack = rig.receiver.TakeAck();
    EXPECT_EQ(ack.latest, 4u);
    EXPECT_EQ(ack.bits, 0b110u) << "bit 0 is sequence 3 (missing), bit 1 is 2, bit 2 is 1";
    EXPECT_EQ(rig.client.Probe(1)->score, 4);
    // The one that was late arrives after a newer one: stale
    EXPECT_EQ(rig.receiver.OnStateUpdate(rig.client.world, messages[2]), ClientReplicator::Result::Stale);
    EXPECT_EQ(rig.client.Probe(1)->score, 4);

    // The ack is what the server understands: sequences 1, 2 and 4 are confirmed, 3 stays in flight
    EXPECT_TRUE(rig.replicator.OnAck(1, ack));
    EXPECT_EQ(rig.replicator.InFlightMessages(1), 1u);
    EXPECT_EQ(rig.replicator.Stats().messagesAcked, 3u);
}

TEST(ClientReplicatorTest, AnEntryForAnObjectItDoesNotHaveIsSkippedAndTheMessageIsNotAcknowledged)
{
    // The server has ten objects, the client three (the others' spawns are still on their way)
    std::vector<std::uint32_t> ten;
    for (std::uint32_t n = 1; n <= 10; ++n)
    {
        ten.push_back(n);
    }
    HalvesRig rig(ten, {1, 2, 3});
    for (std::uint32_t n = 1; n <= 10; ++n)
    {
        rig.server.Probe(n)->score = static_cast<int>(n) * 10;
    }
    const auto messages = rig.Tick();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(rig.receiver.OnStateUpdate(rig.client.world, messages[0]), ClientReplicator::Result::Partial);
    EXPECT_EQ(rig.client.Probe(1)->score, 10);
    EXPECT_EQ(rig.client.Probe(3)->score, 30) << "the entries it could use are applied";
    EXPECT_EQ(rig.receiver.Stats().entriesUnknownObject, 7u);
    EXPECT_EQ(rig.receiver.Stats().messagesPartial, 1u);
    EXPECT_FALSE(rig.receiver.AckPending()) << "not acknowledged, so the server keeps those fields in what it sends";
    EXPECT_EQ(rig.receiver.LatestApplied(), 0u);
}

TEST(ClientReplicatorTest, GarbageIsMalformedAndAppliesNothingOutsideTheLayout)
{
    HalvesRig rig;
    rig.server.Probe(1)->score = 5;
    const auto messages = rig.Tick();
    ASSERT_EQ(messages.size(), 1u);
    const std::vector<std::uint8_t> &valid = messages[0];

    // Cut anywhere: the header or an entry is short
    for (std::size_t length = 0; length < valid.size(); ++length)
    {
        ClientReplicator receiver;
        const std::vector<std::uint8_t> cut(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length));
        const ClientReplicator::Result result = receiver.OnStateUpdate(rig.client.world, cut);
        EXPECT_NE(result, ClientReplicator::Result::Applied) << "cut to " << length;
        EXPECT_FALSE(receiver.AckPending()) << "cut to " << length;
    }
    // Bytes after the last entry
    {
        ClientReplicator receiver;
        std::vector<std::uint8_t> longer = valid;
        longer.push_back(0);
        EXPECT_EQ(receiver.OnStateUpdate(rig.client.world, longer), ClientReplicator::Result::Malformed);
    }
    // Every byte changed to a spread of values (the extremes, and the original with a bit flipped): never a crash, and
    // the world keeps its objects
    const std::size_t objects = rig.client.world.Count();
    for (std::size_t position = 0; position < valid.size(); ++position)
    {
        for (const int value : {0, 1, 2, 0x55, 0x7F, 0x80, 0xAA, 0xFF, valid[position] ^ 0x01, valid[position] ^ 0x80})
        {
            std::vector<std::uint8_t> changed = valid;
            changed[position] = static_cast<std::uint8_t>(value);
            ClientReplicator receiver;
            (void)receiver.OnStateUpdate(rig.client.world, changed);
        }
    }
    EXPECT_EQ(rig.client.world.Count(), objects);
    // Random buffers
    N2Net::SimRandom random(31337);
    for (int round = 0; round < 3000; ++round)
    {
        std::vector<std::uint8_t> bytes(random.NextBelow(80));
        for (std::uint8_t &byte : bytes)
        {
            byte = static_cast<std::uint8_t>(random.NextBelow(256));
        }
        if (!bytes.empty() && random.NextBelow(2) == 0)
        {
            bytes[0] = kStateUpdateType;
        }
        ClientReplicator receiver;
        (void)receiver.OnStateUpdate(rig.client.world, bytes);
    }
}

TEST(ClientReplicatorTest, AnEntryForAnotherLayoutIsSkipped)
{
    // A message whose layout hash is wrong for the object: skipped, counted, nothing applied, acknowledged (a layout that differs stays different)
    HalvesRig rig;
    rig.server.Probe(1)->score = 77;
    auto messages = rig.Tick();
    ASSERT_EQ(messages.size(), 1u);
    // The first entry's layout hash is the two bytes after its (one byte) net id: byte 7 (header) + 1
    std::vector<std::uint8_t> bent = messages[0];
    ASSERT_GT(bent.size(), kStateUpdateHeaderBytes + 3);
    bent[kStateUpdateHeaderBytes + 1] = static_cast<std::uint8_t>(bent[kStateUpdateHeaderBytes + 1] ^ 0xFF);
    EXPECT_EQ(rig.receiver.OnStateUpdate(rig.client.world, bent), ClientReplicator::Result::Applied);
    EXPECT_EQ(rig.receiver.Stats().entriesLayoutMismatch, 1u);
    EXPECT_NE(rig.client.Probe(1)->score, 77);
    EXPECT_TRUE(rig.receiver.AckPending());
}

// ---------------------------------------------------------------------------------------------------------------------
// Server to client over the simulator

TEST(NetworkReplicationTest, AClientGetsTheServersValues)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));

    ReplProbe &serverProbe = *session.serverSide.Probe(2);
    serverProbe.score = 77;
    serverProbe.health = 12.5f;
    serverProbe.label = "caf\xC3\xA9";
    serverProbe.flag = true;
    serverProbe.offset = Math::Vector3(1.25f, -3.5f, 100.0f);
    serverProbe.local = 5;
    ASSERT_TRUE(session.RunUntil([&] { return session.AllConverged(); }));

    const ReplProbe &clientProbe = *rig.side.Probe(2);
    EXPECT_EQ(clientProbe.score, 77);
    EXPECT_NEAR(clientProbe.health, 12.5f, 0.001f);
    EXPECT_EQ(clientProbe.label, "caf\xC3\xA9");
    EXPECT_TRUE(clientProbe.flag);
    EXPECT_NEAR(clientProbe.offset.x, 1.25f, 0.0055f);
    EXPECT_NEAR(clientProbe.offset.y, -3.5f, 0.0055f);
    EXPECT_NEAR(clientProbe.offset.z, 100.0f, 0.0055f);
    EXPECT_EQ(clientProbe.local, 0) << "a field that is not replicated is not sent";
    EXPECT_GT(rig.client->ReplicationStats().entriesApplied, 0u);
    EXPECT_EQ(rig.client->ReplicationStats().messagesMalformed, 0u);
    EXPECT_TRUE(session.RunUntil([&] { return session.server->ReplicationStats().messagesAcked > 0; }));
}

TEST(NetworkReplicationTest, AnIdleWorldCostsNothingAndAChangeCostsOneField)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    session.serverSide.Probe(1)->score = 5;
    ASSERT_TRUE(session.RunUntil([&] { return session.AllConverged(); }));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Replicator().InFlightMessages(1) == 0; }));

    const ServerReplicationStats idleStart = session.server->ReplicationStats();
    session.RunFor(1'000'000);
    const ServerReplicationStats idleEnd = session.server->ReplicationStats();
    EXPECT_EQ(idleEnd.messagesSent, idleStart.messagesSent) << "nothing changed: nothing is sent";
    EXPECT_EQ(idleEnd.bytesSent, idleStart.bytesSent);

    session.serverSide.Probe(3)->score = 6;
    ASSERT_TRUE(session.RunUntil([&] { return rig.side.Probe(3)->score == 6; }));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Replicator().InFlightMessages(1) == 0; }));
    const ServerReplicationStats after = session.server->ReplicationStats();
    EXPECT_EQ(after.fieldsSent - idleEnd.fieldsSent, 1u) << "one changed field, sent once, acknowledged";
    EXPECT_EQ(after.entriesSent - idleEnd.entriesSent, 1u);
}

TEST(NetworkReplicationTest, AClientCannotWriteAReplicatedFieldItsOwnCopyIsOverwritten)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    session.serverSide.Probe(1)->score = 10;
    ASSERT_TRUE(session.RunUntil([&] { return rig.side.Probe(1)->score == 10; }));

    // The client writes its copy: nothing travels back, and the next server change overwrites it
    rig.side.Probe(1)->score = 999;
    rig.side.Probe(1)->local = 3;
    session.RunFor(500'000);
    EXPECT_EQ(session.serverSide.Probe(1)->score, 10) << "the server's value is the server's own";
    EXPECT_EQ(session.serverSide.Probe(1)->local, 0);
    session.serverSide.Probe(1)->score = 11;
    ASSERT_TRUE(session.RunUntil([&] { return rig.side.Probe(1)->score == 11; }));
    EXPECT_EQ(rig.side.Probe(1)->local, 3) << "only replicated fields are written";
}

TEST(NetworkReplicationTest, AHostileClientCannotSendState)
{
    Session session;
    RawPeer &raw = session.AddRaw();
    JoinRaw(session, raw);
    session.serverSide.Probe(1)->score = 3;

    // A state update, which only a server sends, on the channel state travels on
    std::vector<std::uint8_t> forged = EncodeStateUpdateHeader(StateUpdateHeader{1, 1});
    forged.insert(forged.end(), {1, 0, 0, 3, 1, 0, 0});
    raw.SendUnreliable(forged);
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedProtocol == 1; }));
    EXPECT_EQ(session.server->JoinedClientCount(), 0u) << "it was dropped";
    EXPECT_EQ(session.serverSide.Probe(1)->score, 3);

    // The same bytes as a reliable message: not a client message at all
    RawPeer &second = session.AddRaw();
    JoinRaw(session, second);
    second.SendReliable(forged);
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedMalformed == 1; }));
    EXPECT_EQ(session.serverSide.Probe(1)->score, 3);

    // An acknowledgement of a state update the server never sent
    RawPeer &third = session.AddRaw();
    JoinRaw(session, third);
    third.SendUnreliable(Encode(StateAckMessage{4000, 0}));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedProtocol == 2; }));
    EXPECT_EQ(session.server->ReplicationStats().acksInvalid, 1u);

    // A truncated acknowledgement
    RawPeer &fourth = session.AddRaw();
    JoinRaw(session, fourth);
    fourth.SendUnreliable({kStateAckType, 1, 0});
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedMalformed == 2; }));
}

TEST(NetworkReplicationTest, AnAckBeforeJoiningIsAProtocolViolation)
{
    Session session;
    RawPeer &raw = session.AddRaw();
    ASSERT_TRUE(session.RunUntil([&] { return raw.Hello().has_value(); }));
    raw.SendUnreliable(Encode(StateAckMessage{1, 0}));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedProtocol == 1; }));
}

TEST(NetworkReplicationTest, TooManyAcksPerSecondDropTheClient)
{
    Session session;
    RawPeer &raw = session.AddRaw();
    JoinRaw(session, raw);
    // Let the server send some state first, so the acknowledgements below name a sequence it did send
    session.RunFor(200'000);
    ASSERT_GT(session.server->ReplicationStats().messagesSent, 0u);
    // Far more than maxClientAcksPerSecond (1024) in well under a second
    for (int round = 0; round < 20; ++round)
    {
        for (int i = 0; i < 100; ++i)
        {
            raw.SendUnreliable(Encode(StateAckMessage{1, 0}));
        }
        session.Step();
    }
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Stats().droppedRateLimited >= 1; }, 5'000'000));
    EXPECT_EQ(session.server->JoinedClientCount(), 0u);
}

TEST(NetworkReplicationTest, ConvergesUnderLossReorderingAndDuplication)
{
    const ScopedProbePrefab prefab("ReplTest_Spawned");
    ASSERT_TRUE(prefab.Registered());
    for (const std::uint64_t seed : {1ull, 2ull, 3ull, 4ull})
    {
        SessionConfig config;
        config.seed = seed;
        config.sceneObjects = {1, 2, 3, 4, 5};
        config.link.latencyMicros = 20'000;
        config.link.jitterMicros = 15'000;
        config.link.lossPpm = 250'000;
        config.link.reorderPpm = 100'000;
        config.link.reorderDelayMicros = 40'000;
        config.link.duplicatePpm = 100'000;
        Session session(config);
        ClientRig &first = session.AddClient();
        ClientRig &second = session.AddClient();
        ASSERT_TRUE(session.RunUntil([&] { return first.client->IsJoined() && second.client->IsJoined(); }))
            << "seed " << seed;

        N2Net::SimRandom random(seed * 7919);
        std::optional<NetworkId> spawned;
        for (int round = 0; round < 80; ++round)
        {
            for (int change = 0; change < 3; ++change)
            {
                const std::uint32_t target = 1 + random.NextBelow(5);
                Mutate(*session.serverSide.Probe(target), random);
            }
            if (round == 20)
            {
                const auto id = session.server->Spawn(prefab.Name());
                ASSERT_TRUE(id.has_value());
                spawned = *id;
            }
            if (spawned.has_value())
            {
                Mutate(*session.serverSide.ProbeOf(*spawned), random);
            }
            session.RunFor(50'000);
        }
        ASSERT_TRUE(spawned.has_value());
        // Mutation stops: the clients must reach the server's values whatever was lost on the way
        ASSERT_TRUE(session.RunUntil([&] { return session.AllConverged() && first.side.ProbeOf(*spawned) != nullptr &&
                                                  second.side.ProbeOf(*spawned) != nullptr; },
                                     60'000'000))
            << "seed " << seed;
        EXPECT_TRUE(SameProbe(*session.serverSide.ProbeOf(*spawned), *first.side.ProbeOf(*spawned)));
        EXPECT_TRUE(SameProbe(*session.serverSide.ProbeOf(*spawned), *second.side.ProbeOf(*spawned)));

        // The link really did lose and reorder (the test is not an easy one), and the history stayed bounded
        const N2Net::SimLinkStats *const link = session.network.GetLinkStats(session.serverEndpoint->Id(), first.endpoint->Id());
        ASSERT_NE(link, nullptr);
        EXPECT_GT(link->droppedByLoss, 0u);
        EXPECT_GT(link->duplicated, 0u);
        EXPECT_LE(session.server->Replicator().InFlightMessages(1), session.config.replication.maxInFlightMessages);
        EXPECT_EQ(first.client->ReplicationStats().messagesMalformed, 0u);
    }
}

TEST(NetworkReplicationTest, AnObjectSpawnedWhileTheSpawnMessageIsLateStillArrives)
{
    // Heavy loss on the reliable channel delays spawns: state for an object the client does not have yet is not
    // acknowledged and is sent again
    const ScopedProbePrefab prefab("ReplTest_LateSpawn");
    SessionConfig config;
    config.seed = 11;
    config.link.latencyMicros = 30'000;
    config.link.lossPpm = 400'000;
    Session session(config);
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));

    std::vector<NetworkId> ids;
    for (int i = 0; i < 6; ++i)
    {
        const auto id = session.server->Spawn(prefab.Name());
        ASSERT_TRUE(id.has_value());
        session.serverSide.ProbeOf(*id)->score = 100 + i;
        session.serverSide.ProbeOf(*id)->label = "spawned" + std::to_string(i);
        ids.push_back(*id);
        session.RunFor(10'000);
    }
    ASSERT_TRUE(session.RunUntil([&] { return session.AllConverged() && rig.side.world.Count() == 9; }, 60'000'000));
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        ASSERT_NE(rig.side.ProbeOf(ids[i]), nullptr);
        EXPECT_EQ(rig.side.ProbeOf(ids[i])->score, 100 + static_cast<int>(i));
        EXPECT_EQ(rig.side.ProbeOf(ids[i])->label, "spawned" + std::to_string(i));
    }
}

TEST(NetworkReplicationTest, ADespawnedObjectIsForgotten)
{
    const ScopedProbePrefab prefab("ReplTest_Despawn");
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    const auto id = session.server->Spawn(prefab.Name());
    ASSERT_TRUE(id.has_value());
    session.serverSide.ProbeOf(*id)->score = 4;
    ASSERT_TRUE(session.RunUntil([&] { return rig.side.ProbeOf(*id) != nullptr && rig.side.ProbeOf(*id)->score == 4; }));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Replicator().InFlightMessages(1) == 0; }));
    const std::size_t baselines = session.server->Replicator().BaselineCount(1);
    EXPECT_TRUE(session.server->Despawn(*id));
    EXPECT_EQ(session.server->Replicator().BaselineCount(1), baselines - 1);
    ASSERT_TRUE(session.RunUntil([&] { return rig.side.world.Find(*id) == nullptr; }));
    session.RunFor(300'000);
    EXPECT_TRUE(rig.client->IsJoined());
    EXPECT_EQ(rig.client->ReplicationStats().messagesMalformed, 0u);
}

TEST(NetworkReplicationTest, AClientWhoseObjectHasAnotherLayoutSkipsTheEntryAndIsNotHurt)
{
    Session session;
    ClientRig &rig = session.AddClient(Kind::Other);
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    session.serverSide.Probe(1)->score = 9;
    session.RunFor(500'000);
    EXPECT_GT(rig.client->ReplicationStats().entriesLayoutMismatch, 0u);
    EXPECT_EQ(rig.client->ReplicationStats().entriesApplied, 0u);
    EXPECT_TRUE(rig.client->IsJoined());
    // Acknowledged, so the history drains and the unusable object does not hold other objects back
    EXPECT_TRUE(session.RunUntil([&] { return session.server->Replicator().InFlightMessages(1) == 0; }));
    EXPECT_LE(session.server->Replicator().InFlightMessages(1), session.config.replication.maxInFlightMessages);
}

TEST(NetworkReplicationTest, AGameObjectReferenceTravelsAsANetId)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));

    session.serverSide.Probe(1)->target = FindByNumber(*session.serverSide.scene, 3).get();
    ASSERT_TRUE(session.RunUntil([&] { return rig.side.Probe(1)->target != nullptr; }));
    EXPECT_EQ(rig.side.Probe(1)->target, FindByNumber(*rig.side.scene, 3).get())
        << "the client's own object 3, found by its net id";

    // Cleared again
    session.serverSide.Probe(1)->target = nullptr;
    ASSERT_TRUE(session.RunUntil([&] { return rig.side.Probe(1)->target == nullptr; }));
}

TEST(NetworkReplicationTest, StateIsAppliedOnlyOnceJoined)
{
    // A client still joining ignores state updates (and does not acknowledge them); once joined it catches up
    SessionConfig config;
    config.link.latencyMicros = 5'000;
    Session session(config);
    session.serverSide.Probe(2)->score = 31;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    ASSERT_TRUE(session.RunUntil([&] { return rig.side.Probe(2)->score == 31; }));
    EXPECT_EQ(rig.client->ReplicationStats().messagesMalformed, 0u);
}

TEST(NetworkReplicationTest, ReplicationCanBeTurnedOff)
{
    SessionConfig config;
    config.sendRateHz = 0;
    Session session(config);
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    session.serverSide.Probe(1)->score = 8;
    session.RunFor(500'000);
    EXPECT_EQ(rig.side.Probe(1)->score, 0);
    EXPECT_EQ(session.server->ReplicationStats().messagesSent, 0u);
}

TEST(NetworkReplicationTest, TheSendRateIsHonoured)
{
    SessionConfig config;
    config.sendRateHz = 10;
    Session session(config);
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    // One change every 10 ms for a second: at 10 Hz the server sends at most about ten times
    const std::uint64_t before = session.server->ReplicationStats().messagesSent;
    for (int i = 0; i < 100; ++i)
    {
        session.serverSide.Probe(1)->score = i + 1;
        session.RunFor(10'000);
    }
    const std::uint64_t sent = session.server->ReplicationStats().messagesSent - before;
    EXPECT_GE(sent, 8u);
    EXPECT_LE(sent, 13u);
}

// ---------------------------------------------------------------------------------------------------------------------
// NetworkTransform

TEST(NetworkTransformTest, ReplicatesPositionAndRotationWithinTheirQuantisation)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));

    const GameObject::Ptr serverObject = FindByNumber(*session.serverSide.scene, 2);
    if (!serverObject->HasPositionable())
    {
        serverObject->CreatePositionable();
    }
    const Math::Quaternion rotation(Math::Vector3(0.0f, 1.0f, 0.0f), 1.0f);
    serverObject->GetPositionable()->SetLocalPosition(Math::Vector3(12.345f, -6.789f, 100.004f));
    serverObject->GetPositionable()->SetLocalRotation(rotation);

    const GameObject::Ptr clientObject = FindByNumber(*rig.side.scene, 2);
    ASSERT_TRUE(session.RunUntil(
        [&]
        {
            return clientObject->HasPositionable() &&
                   std::fabs(clientObject->GetPositionable()->GetLocalPosition().x - 12.345f) < 0.006f;
        }));
    const Math::Vector3 position = clientObject->GetPositionable()->GetLocalPosition();
    EXPECT_NEAR(position.x, 12.345f, 0.0051f);
    EXPECT_NEAR(position.y, -6.789f, 0.0051f);
    EXPECT_NEAR(position.z, 100.004f, 0.0051f);
    const Math::Quaternion got = clientObject->GetPositionable()->GetLocalRotation();
    const double dot = static_cast<double>(got.GetW()) * rotation.GetW() + static_cast<double>(got.GetX()) * rotation.GetX() +
                       static_cast<double>(got.GetY()) * rotation.GetY() + static_cast<double>(got.GetZ()) * rotation.GetZ();
    EXPECT_LT(2.0 * std::acos(std::min(1.0, std::fabs(dot))), 0.005);

    // The scale is not replicated unless asked to be
    serverObject->GetPositionable()->SetLocalScale(Math::Vector3(2.0f, 2.0f, 2.0f));
    session.RunFor(300'000);
    EXPECT_FLOAT_EQ(clientObject->GetPositionable()->GetLocalScale().x, 1.0f);
}

TEST(NetworkTransformTest, AMoveSmallerThanAStepCostsNothing)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    const GameObject::Ptr serverObject = FindByNumber(*session.serverSide.scene, 1);
    if (!serverObject->HasPositionable())
    {
        serverObject->CreatePositionable();
    }
    serverObject->GetPositionable()->SetLocalPosition(Math::Vector3(1.0f, 0.0f, 0.0f));
    ASSERT_TRUE(session.RunUntil([&] { return session.AllConverged(); }));
    ASSERT_TRUE(session.RunUntil([&] { return session.server->Replicator().InFlightMessages(1) == 0; }));
    session.RunFor(300'000);
    const std::uint64_t before = session.server->ReplicationStats().fieldsSent;
    serverObject->GetPositionable()->SetLocalPosition(Math::Vector3(1.002f, 0.0f, 0.0f)); // a fifth of the 1 cm step
    session.RunFor(500'000);
    EXPECT_EQ(session.server->ReplicationStats().fieldsSent, before);
}

TEST(NetworkTransformTest, ATeleportIsMarkedForTheClient)
{
    Session session;
    ClientRig &rig = session.AddClient();
    ASSERT_TRUE(session.RunUntil([&] { return rig.client->IsJoined(); }));
    NetworkTransform *const serverTransform = session.serverSide.NetTransform(3);
    NetworkTransform *const clientTransform = rig.side.NetTransform(3);
    ASSERT_NE(serverTransform, nullptr);
    ASSERT_NE(clientTransform, nullptr);
    EXPECT_FALSE(clientTransform->ConsumeTeleport());

    serverTransform->Teleport(Math::Vector3(50.0f, 0.0f, 0.0f));
    EXPECT_EQ(serverTransform->GetTeleportSerial(), 1);
    ASSERT_TRUE(session.RunUntil([&] { return clientTransform->GetTeleportSerial() == 1; }));
    EXPECT_TRUE(clientTransform->ConsumeTeleport());
    EXPECT_FALSE(clientTransform->ConsumeTeleport()) << "once";
    const GameObject::Ptr clientObject = FindByNumber(*rig.side.scene, 3);
    ASSERT_TRUE(clientObject->HasPositionable());
    EXPECT_NEAR(clientObject->GetPositionable()->GetLocalPosition().x, 50.0f, 0.0051f);

    // A plain move is not a teleport
    FindByNumber(*session.serverSide.scene, 3)->GetPositionable()->SetLocalPosition(Math::Vector3(51.0f, 0.0f, 0.0f));
    ASSERT_TRUE(session.RunUntil([&] { return std::fabs(clientObject->GetPositionable()->GetLocalPosition().x - 51.0f) < 0.006f; }));
    EXPECT_FALSE(clientTransform->ConsumeTeleport());
}

TEST(NetworkTransformTest, IsABuiltinSingletonWithItsSettingsSaved)
{
    ComponentRegistry &registry = ComponentRegistry::Instance();
    EXPECT_TRUE(registry.IsRegistered("NetworkTransform"));
    EXPECT_TRUE(registry.IsSingleton("NetworkTransform"));
    const GameObject::Ptr object = GameObject::Create("Transformed");
    auto *const transform = object->AddComponent<NetworkTransform>();
    EXPECT_EQ(object->AddComponent<NetworkTransform>(), transform);
    EXPECT_EQ(transform->GetTypeName(), "NetworkTransform");
    EXPECT_TRUE(transform->ReplicatesPosition());
    EXPECT_TRUE(transform->ReplicatesRotation());
    EXPECT_FALSE(transform->ReplicatesScale());
    EXPECT_FLOAT_EQ(transform->GetPositionStep(), 0.01f);

    const std::optional<ComponentSchema> schema = registry.Describe("NetworkTransform");
    ASSERT_TRUE(schema.has_value());
    std::vector<std::string> replicated;
    for (const FieldInfo &field : schema->fields)
    {
        if (field.replicated)
        {
            replicated.push_back(field.name);
        }
    }
    const std::vector<std::string> expected = {"position", "rotation", "teleportSerial"};
    EXPECT_EQ(replicated, expected);
}


TEST(ServerReplicatorTest, InterestOwnerOnlyAndReentryResetBaseline)
{
    DirectRig rig;
    auto *hidden = rig.side.world.Find(NetworkId{2});
    ASSERT_TRUE(hidden);
    ASSERT_TRUE(hidden->SetInterestPolicy({Relevance::OwnerOnly,100,1}));
    const auto messages = rig.Tick();
    for (const auto &message : messages)
        for (const auto &entry : Parse(message,2).entries) EXPECT_NE(entry.id,2u);
    EXPECT_EQ(rig.replicator.BaselineCount(1),2u);
    ASSERT_TRUE(hidden->SetInterestPolicy({Relevance::Always,100,1}));
    const auto reentry = rig.Tick(1);
    bool found = false;
    for (const auto &message : reentry)
        for (const auto &entry : Parse(message,2).entries) if (entry.id==2) found=true;
    EXPECT_TRUE(found);
    EXPECT_EQ(rig.replicator.BaselineCount(1),3u);
}

TEST(ServerReplicatorTest, SnapshotByteBudgetAndPriorityDoNotStarveObjects)
{
    ServerReplicationOptions options;
    options.maxBytesPerTick = 64;
    options.maxMessageBytes = 150;
    DirectRig rig(options);
    ASSERT_TRUE(rig.side.world.Find(NetworkId{3})->SetInterestPolicy({Relevance::Always,100,4}));
    std::set<std::uint32_t> seen;
    std::map<std::uint32_t,std::size_t> counts;
    bool firstSelection=true;
    for (std::uint64_t tick=0;tick<30;++tick)
    {
        const auto messages=rig.Tick(tick*33'333);
        EXPECT_LE(TotalBytes(messages),64u);
        for(const auto &message:messages)
            for(const auto &entry:Parse(message,2).entries) { seen.insert(entry.id); ++counts[entry.id]; if(firstSelection) { EXPECT_EQ(entry.id,3u); firstSelection=false; } }
    }
    EXPECT_EQ(seen,(std::set<std::uint32_t>{1,2,3}));
    EXPECT_GT(rig.replicator.Stats().entriesBudgetDeferred,0u);
    EXPECT_GT(counts[3],counts[1]);
    EXPECT_GT(counts[3],counts[2]);
}


TEST(InterestGridTest, RadiusBoundaryUsesOwnedObserverAndTracksMovement)
{
    ScopedProbePrefab prefab("interest-observer");
    ASSERT_TRUE(prefab.Registered());
    Side side("interest-grid",{1,2,3});
    const auto spawned=side.world.SpawnInstance(NetworkId{4},prefab.Name(),1);
    ASSERT_TRUE(spawned);
    for(const auto &[id,object]:side.world.Objects())
    {
        auto &go=object->GetGameObject();
        if(!go.HasPositionable())go.CreatePositionable();
        go.GetPositionable()->SetPosition(Math::Vector3{0,0,0});
    }
    auto *target=side.world.Find(NetworkId{2});
    ASSERT_TRUE(target->SetInterestPolicy({Relevance::Radius,10,1}));
    target->GetGameObject().GetPositionable()->SetPosition(Math::Vector3{10,0,0});
    ASSERT_TRUE(side.world.Find(NetworkId{3})->SetInterestPolicy({Relevance::OwnerOnly,10,1}));
    InterestGrid grid;grid.Rebuild(side.world);
    EXPECT_TRUE(grid.Visible(1,NetworkId{4}).contains(2));
    EXPECT_FALSE(grid.Visible(1,NetworkId{4}).contains(3));
    EXPECT_FALSE(grid.Visible(2,NetworkId{4}).contains(2));
    EXPECT_FALSE(grid.Visible(1,NetworkId{}).contains(2));
    target->GetGameObject().GetPositionable()->SetPosition(Math::Vector3{10.01f,0,0});
    grid.Rebuild(side.world);EXPECT_FALSE(grid.Visible(1,NetworkId{4}).contains(2));
    EXPECT_TRUE(grid.Visible(1,NetworkId{4}).contains(4));
}

TEST(NetworkReplicationTest, VisibilityAndFullReentryConvergeUnderLossAndReorder)
{
    SessionConfig config;
    config.link.latencyMicros=10'000;config.link.jitterMicros=10'000;
    config.link.lossPpm=100'000;config.link.reorderPpm=100'000;config.link.reorderDelayMicros=30'000;
    Session session(config);auto &client=session.AddClient();
    ASSERT_TRUE(session.RunUntil([&]{return session.AllConverged();}));
    auto *serverObject=session.serverSide.world.Find(NetworkId{2});
    auto *clientObject=client.side.world.Find(NetworkId{2});
    ASSERT_TRUE(serverObject);ASSERT_TRUE(clientObject);
    ASSERT_TRUE(serverObject->SetInterestPolicy({Relevance::OwnerOnly,100,1}));
    ASSERT_TRUE(session.RunUntil([&]{return !clientObject->GetGameObject().IsActive();}));
    // Let old state leave the network, then change a hidden field: it must not be delivered.
    session.RunFor(500'000);
    session.serverSide.ProbeOf(NetworkId{2})->score=4321;
    session.RunFor(500'000);
    EXPECT_NE(client.side.ProbeOf(NetworkId{2})->score,4321);
    ASSERT_TRUE(serverObject->SetInterestPolicy({Relevance::Always,100,1}));
    ASSERT_TRUE(session.RunUntil([&]{return clientObject->GetGameObject().IsActive() &&
        client.side.ProbeOf(NetworkId{2})->score==4321;}));
}


TEST(NetworkReplicationTest, HistoricalServerQueriesRequireAJoinedRequesterAndForgetDespawns)
{
    SessionConfig config;config.link.latencyMicros=10'000;
    Session session(config);auto &client=session.AddClient();
    ASSERT_TRUE(session.RunUntil([&]{return client.client->IsJoined();}));
    session.RunFor(100'000); // let connection acknowledgements establish the measured RTT
    std::array<HistoricalHitbox,1> boxes;
    boxes[0].object=NetworkId{2};boxes[0].centre=Math::Vector3{0,0,5};
    const auto now=session.network.NowMicros();
    ASSERT_TRUE(session.server->CaptureHitboxes(now,boxes));
    EXPECT_FALSE(session.server->CastLagCompensated(9999,now,{0,0,0},{0,0,1},20));
    const auto hit=session.server->CastLagCompensated(client.client->LocalClientId(),now,{0,0,0},{0,0,1},20);
    ASSERT_TRUE(hit);EXPECT_NEAR(hit->distance,4.5f,0.001f);
    ASSERT_TRUE(session.server->Despawn(NetworkId{2}));
    EXPECT_FALSE(session.server->CastLagCompensated(client.client->LocalClientId(),now,{0,0,0},{0,0,1},20));
}


TEST(NetworkReplicationTest, OptInHitboxComponentIsCapturedAutomaticallyByTheServer)
{
    Session session;
    auto *object=session.serverSide.world.Find(NetworkId{2});
    ASSERT_TRUE(object);
    auto &go=object->GetGameObject();
    if(!go.HasPositionable())go.CreatePositionable();
    go.GetPositionable()->SetPosition(Math::Vector3{0,0,5});
    go.AddComponent<LagCompensatedHitbox>();
    auto &client=session.AddClient();
    ASSERT_TRUE(session.RunUntil([&]{return client.client->IsJoined();}));
    session.RunFor(50'000);
    const auto hit=session.server->CastLagCompensated(client.client->LocalClientId(),session.network.NowMicros(),
        {0,0,0},{0,0,1},20);
    ASSERT_TRUE(hit);EXPECT_EQ(hit->object,NetworkId{2});EXPECT_NEAR(hit->distance,4.5f,0.001f);
}


TEST(NetworkReplicationTest, LuaQueriesAndInterestPoliciesUseTheBoundAuthoritativeWorld)
{
    Session session;auto &client=session.AddClient();
    ASSERT_TRUE(session.RunUntil([&]{return client.client->IsJoined();}));
    session.RunFor(50'000);
    std::array<HistoricalHitbox,1> boxes;
    boxes[0].object=NetworkId{2};boxes[0].centre=Math::Vector3{0,0,5};
    ASSERT_TRUE(session.server->CaptureHitboxes(session.network.NowMicros(),boxes));
    session.server->BindPhysicsQueries();
    auto &runtime=Scripting::LuaRuntime::Instance();ASSERT_TRUE(runtime.Initialize());
    auto &lua=runtime.GetState();
    lua["b8910Object"]=Scripting::GameObjectRef(session.serverSide.world.Find(NetworkId{2})->GetGameObject());
    lua["b8910Client"]=client.client->LocalClientId();lua["b8910Time"]=session.network.NowMicros();
    const auto result=lua.safe_script(R"(
        local hit=Physics.RaycastLagCompensated(b8910Client,b8910Time,Vector3(0,0,0),Vector3(0,0,1),20)
        assert(hit and hit.netId==2 and math.abs(hit.distance-4.5)<0.001)
        assert(Physics.RaycastLagCompensated(9999,b8910Time,Vector3(0,0,0),Vector3(0,0,1),20)==nil)
        assert(Network.SetInterestPolicy(b8910Object,"owner",100,2))
        assert(not Network.SetObserver(b8910Client,b8910Object))
        assert(Network.ClearObserver(b8910Client))
    )",sol::script_pass_on_error);
    if(!result.valid()) {const sol::error error=result;FAIL()<<error.what();}
    EXPECT_EQ(session.serverSide.world.Find(NetworkId{2})->GetInterestPolicy().relevance,Relevance::OwnerOnly);
    lua["b8910Object"]=sol::nil;lua["b8910Client"]=sol::nil;lua["b8910Time"]=sol::nil;
}
