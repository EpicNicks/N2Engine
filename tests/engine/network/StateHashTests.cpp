#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "NetworkTestSupport.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/io/Resources.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/network/StateHash.hpp"
#include "engine/network/n2net/HashTree.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

// The lockstep hash tree over REAL replicated NetworkObject fields (netcode C3 follow-up): every replicable FieldKind,
// located by Bisect to the exact net id and field name, quantised fields hashed in their encoded form, and a tree that
// does not depend on the order objects or components were created in.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;
using N2Engine::N2Net::Bisect;
using N2Engine::N2Net::Divergence;
using N2Engine::N2Net::HashTree;

namespace
{
    enum class HashMode
    {
        Alpha,
        Beta,
        Gamma
    };

    N2_SERIALIZE_ENUM(HashMode, {{HashMode::Alpha, "Alpha"}, {HashMode::Beta, "Beta"}, {HashMode::Gamma, "Gamma"}})

    /// One replicated field of every replicable FieldKind (most with the quantised variants), and one local field
    class HashAll final : public SerializableComponent
    {
    public:
        explicit HashAll(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("flag", flag).Replicated();
            RegisterMember("count", count).Replicated();
            RegisterMember("ratio", ratio).Replicated();
            RegisterMember("ranged", ranged).Range(0.0, 100.0).Replicated();
            RegisterMember("label", label).Replicated();
            RegisterMember("pos2", pos2).Replicated();
            RegisterMember("pos3", pos3).Replicated(0.01);
            RegisterMember("vec4", vec4).Replicated();
            RegisterMember("rot", rot).Replicated();
            RegisterMember("tint", tint).Replicated();
            RegisterMember("tint3", tint3).AsColor().Replicated();
            RegisterMember("mode", mode).Replicated();
            RegisterAssetRef("texture", texture).Replicated();
            RegisterGameObjectRef("target", target).Replicated();
            RegisterMember("local", local);
        }

        [[nodiscard]] std::string GetTypeName() const override { return "HashAll"; }

        bool flag = false;
        int count = 0;
        float ratio = 0.25f;
        float ranged = 50.0f;
        std::string label = "x";
        Math::Vector2 pos2{1.0f, 2.0f};
        Math::Vector3 pos3{0.0f, 0.0f, 0.0f};
        Math::Vector4 vec4{1.0f, 2.0f, 3.0f, 4.0f};
        Math::Quaternion rot;
        Common::Color tint{0.5f, 0.5f, 0.5f, 1.0f};
        Math::Vector3 tint3{0.2f, 0.4f, 0.6f};
        HashMode mode = HashMode::Alpha;
        std::shared_ptr<Rendering::Texture> texture;
        GameObject *target = nullptr;
        int local = 0;
    };

    class HashOther final : public SerializableComponent
    {
    public:
        explicit HashOther(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("value", value).Replicated();
        }

        [[nodiscard]] std::string GetTypeName() const override { return "HashOther"; }

        int value = 0;
    };

    /// A world over a scene whose objects are the UUID numbers in `uuids`, in that creation order. Net ids follow the
    /// sorted UUIDs (1, 2, 3 ...), whatever the creation order was.
    struct Rig
    {
        std::unique_ptr<Scene> scene;
        NetworkWorld world;
        LayoutCache layouts;

        explicit Rig(const std::vector<std::uint32_t> &uuids, const bool withFields = true)
        {
            scene = Scene::Create("StateHash");
            for (const std::uint32_t n : uuids)
            {
                const GameObject::Ptr gameObject = GameObject::Create("Object" + std::to_string(n));
                gameObject->SetUUID(TestUuid(n));
                gameObject->AddComponent<NetworkObject>();
                if (withFields)
                {
                    gameObject->AddComponent<HashAll>();
                }
                scene->AddRootGameObject(gameObject);
            }
            EXPECT_TRUE(world.BindScene(*scene).has_value());
        }

        HashAll &Probe(const std::uint32_t uuidNumber) const
        {
            return *FindByNumber(*scene, uuidNumber)->GetComponent<HashAll>();
        }

        HashTree Tree(const std::uint32_t tick = 7)
        {
            std::optional<HashTree> tree = BuildStateHashTree(world, layouts, tick);
            EXPECT_TRUE(tree.has_value());
            return tree.value_or(HashTree());
        }
    };

    /// Non-trivial, deterministic values in object `n`
    void Fill(HashAll &probe, const std::uint32_t n)
    {
        probe.flag = (n % 2) == 0;
        probe.count = static_cast<int>(n) * 1000 - 7;
        probe.ratio = 0.123456f * static_cast<float>(n);
        probe.ranged = 3.7f + static_cast<float>(n);
        probe.label = "obj" + std::to_string(n);
        probe.pos2 = Math::Vector2(0.37f * static_cast<float>(n), -1.9f);
        probe.pos3 = Math::Vector3(1.234f * static_cast<float>(n), -5.678f, 9.1011f);
        probe.vec4 = Math::Vector4(0.1f * static_cast<float>(n), 0.2f, 0.3f, 0.4f);
        probe.rot = Math::Quaternion(0.1f * static_cast<float>(n), 0.6f, -0.3f, 0.72f);
        probe.rot.Normalize();
        probe.tint = Common::Color(0.1f, 0.2f, 0.3f, 0.4f);
        probe.tint3 = Math::Vector3(0.9f, 0.05f * static_cast<float>(n / 10), 0.3f);
        probe.mode = HashMode::Beta;
    }

    void FillAll(Rig &rig, const std::vector<std::uint32_t> &uuids)
    {
        for (const std::uint32_t n : uuids)
        {
            Fill(rig.Probe(n), n);
        }
    }

    const std::vector<std::uint32_t> kUuids = {10, 20, 30};

    struct Mutation
    {
        const char *field;
        std::function<void(Rig &, HashAll &)> apply;
    };

    /// One change per replicated field (every replicable FieldKind), each big enough to survive its quantisation
    std::vector<Mutation> Mutations()
    {
        return {
            {"flag", [](Rig &, HashAll &p) { p.flag = !p.flag; }},
            {"count", [](Rig &, HashAll &p) { p.count += 1; }},
            {"ratio", [](Rig &, HashAll &p) { p.ratio += 0.5f; }},
            {"ranged", [](Rig &, HashAll &p) { p.ranged += 5.0f; }},
            {"label", [](Rig &, HashAll &p) { p.label += "!"; }},
            {"pos2", [](Rig &, HashAll &p) { p.pos2.y += 1.0f; }},
            {"pos3", [](Rig &, HashAll &p) { p.pos3.z += 0.5f; }},
            {"vec4", [](Rig &, HashAll &p) { p.vec4.w += 1.0f; }},
            {"rot",
             [](Rig &, HashAll &p)
             {
                 p.rot = Math::Quaternion(0.9f, 0.1f, 0.2f, 0.3f);
                 p.rot.Normalize();
             }},
            {"tint", [](Rig &, HashAll &p) { p.tint.a += 0.3f; }},
            {"tint3", [](Rig &, HashAll &p) { p.tint3.y += 0.3f; }},
            {"mode", [](Rig &, HashAll &p) { p.mode = HashMode::Gamma; }},
            {"texture",
             [](Rig &, HashAll &p)
             { p.texture = Rendering::Texture::Create(1, 1, std::vector<std::uint8_t>{1, 2, 3, 255}); }},
            {"target", [](Rig &rig, HashAll &p) { p.target = FindByNumber(*rig.scene, 10).get(); }},
        };
    }
}

TEST(StateHashTest, IdenticalWorldsGiveIdenticalRoots)
{
    Rig a(kUuids);
    Rig b(kUuids);
    FillAll(a, kUuids);
    FillAll(b, kUuids);
    const HashTree ta = a.Tree();
    const HashTree tb = b.Tree();
    EXPECT_EQ(ta.Root(), tb.Root());
    EXPECT_EQ(ta.Objects().size(), 3u);
    for (const HashTree::Object &object : ta.Objects())
    {
        // 14 replicated fields plus the layout and meta leaves
        EXPECT_EQ(object.fields.size(), 14u + 2u);
    }
    HashTree local = ta;
    HashTree remote = tb;
    EXPECT_EQ(Bisect(local, remote).kind, Divergence::Kind::None);

    // The tick is part of the root; building again gives the same tree
    EXPECT_EQ(a.Tree().Root(), ta.Root());
    EXPECT_NE(a.Tree(8).Root(), ta.Root());
}

TEST(StateHashTest, ALocalFieldIsNotHashed)
{
    Rig a(kUuids);
    Rig b(kUuids);
    FillAll(a, kUuids);
    FillAll(b, kUuids);
    b.Probe(20).local = 99;
    EXPECT_EQ(a.Tree().Root(), b.Tree().Root());
}

TEST(StateHashTest, ASingleChangedFieldOfEachKindIsFoundByBisect)
{
    Rig a(kUuids);
    Rig b(kUuids);
    FillAll(a, kUuids);
    FillAll(b, kUuids);
    (void)a;
    (void)b;

    // A fresh pair of worlds for every (field, object): the change is the only difference between them
    for (const std::uint32_t object : kUuids)
    {
        const std::uint32_t netId = object / 10; // sorted UUIDs 10, 20, 30 -> 1, 2, 3
        for (const Mutation &mutation : Mutations())
        {
            SCOPED_TRACE(std::string(mutation.field) + " on net id " + std::to_string(netId));
            Rig left(kUuids);
            Rig right(kUuids);
            FillAll(left, kUuids);
            FillAll(right, kUuids);
            ASSERT_EQ(left.Tree().Root(), right.Tree().Root());

            mutation.apply(right, right.Probe(object));
            HashTree remote = right.Tree();
            HashTree local = left.Tree();
            ASSERT_NE(local.Root(), remote.Root());

            const Divergence found = Bisect(local, remote);
            ASSERT_EQ(found.kind, Divergence::Kind::FieldDiffers);
            EXPECT_EQ(found.objectId, netId);
            EXPECT_EQ(DescribeHashedField(left.world, left.layouts, found.objectId, found.fieldId),
                      std::string("HashAll.") + mutation.field);
            EXPECT_LE(found.rounds, 72);

            // The same in the other direction
            const Divergence reverse = Bisect(remote, local);
            EXPECT_EQ(reverse.kind, Divergence::Kind::FieldDiffers);
            EXPECT_EQ(reverse.objectId, netId);
            EXPECT_EQ(reverse.fieldId, found.fieldId);
        }
    }
}

TEST(StateHashTest, FloatsAreHashedInTheirEncodedForm)
{
    Rig a(kUuids);
    Rig b(kUuids);
    FillAll(a, kUuids);
    FillAll(b, kUuids);
    const std::uint64_t root = a.Tree().Root();
    ASSERT_EQ(root, b.Tree().Root());

    // Under the quantum of a replicationStep (0.01) and of a 16-bit range (100/65535): the encoded bytes are the same
    b.Probe(20).pos3.x += 0.0004f;
    b.Probe(20).ranged += 0.0004f;
    EXPECT_EQ(b.Tree().Root(), root) << "a change below the quantum encodes alike";

    // A plain float32 field has no quantum: the smallest step shows
    b.Probe(20).ratio = std::nextafter(b.Probe(20).ratio, 1000.0f);
    EXPECT_NE(b.Tree().Root(), root);
}

TEST(StateHashTest, FloatsAndQuaternionsRoundTripThroughTheReplicationCodec)
{
    Rig server(kUuids);
    Rig client(kUuids);
    FillAll(server, kUuids);
    // Values that are not on any quantum, a quaternion off the unit grid
    server.Probe(30).ratio = 0.1f + 0.2f;
    server.Probe(30).rot = Math::Quaternion(0.123456f, -0.654321f, 0.222222f, 0.7f);
    server.Probe(30).rot.Normalize();
    server.Probe(10).ranged = 33.333333f;
    server.Probe(20).pos3 = Math::Vector3(-123.456789f, 0.00499f, 7.777777f);

    // Send every field of every object, as the replication wire would, and apply it on the client
    const WorldReferences serverRefs(server.world);
    const WorldReferences clientRefs(client.world);
    for (const std::uint32_t n : kUuids)
    {
        GameObject &from = *FindByNumber(*server.scene, n);
        GameObject &to = *FindByNumber(*client.scene, n);
        const ObjectLayout serverLayout = BuildObjectLayout(from);
        const ObjectLayout clientLayout = BuildObjectLayout(to);
        ASSERT_TRUE(serverLayout.usable);
        const ObjectState state = EncodeObjectState(serverLayout, from, &serverRefs);
        // A null reference or asset has no bytes (it is never sent), so only the fields with bytes go in the mask
        std::vector<std::uint8_t> payload(serverLayout.MaskBytes(), 0);
        for (std::size_t i = 0; i < state.fields.size(); ++i)
        {
            if (!state.fields[i].empty())
            {
                payload[i / 8] = static_cast<std::uint8_t>(payload[i / 8] | (1u << (i % 8)));
            }
        }
        for (const auto &bytes : state.fields)
        {
            payload.insert(payload.end(), bytes.begin(), bytes.end());
        }
        N2Net::ByteReader reader(payload);
        std::size_t refused = 0;
        ASSERT_EQ(ApplyEntry(clientLayout, to, reader, clientRefs, refused), EntryResult::Applied);
        EXPECT_EQ(refused, 0u);
    }

    // The client now holds the decoded (quantised) values; they encode to the same bytes, so the trees match exactly
    EXPECT_EQ(server.Tree().Root(), client.Tree().Root());
    HashTree s = server.Tree();
    HashTree c = client.Tree();
    EXPECT_EQ(Bisect(s, c).kind, Divergence::Kind::None);

    // And the decoded values are close to the originals (the codec's documented error), so the equality is not vacuous
    EXPECT_NEAR(client.Probe(20).pos3.x, -123.456789f, 0.006f);
    EXPECT_NEAR(client.Probe(10).ranged, 33.333333f, 0.002f);
    const Math::Quaternion &sent = server.Probe(30).rot;
    const Math::Quaternion &got = client.Probe(30).rot;
    EXPECT_NEAR(std::fabs(sent.w * got.w + sent.x * got.x + sent.y * got.y + sent.z * got.z), 1.0f, 1e-3f);
}

TEST(StateHashTest, ANonFiniteFloatHashesAsUnreadable)
{
    // The codec refuses a NaN or an infinity (a JSON number cannot hold one), so replication never sends the field; the
    // leaf is then over zero bytes, the same for every non-finite value, and different from any finite one
    Rig a(kUuids);
    Rig b(kUuids);
    Rig c(kUuids);
    FillAll(a, kUuids);
    FillAll(b, kUuids);
    FillAll(c, kUuids);
    a.Probe(20).ratio = std::numeric_limits<float>::quiet_NaN();
    b.Probe(20).ratio = std::numeric_limits<float>::infinity();
    EXPECT_EQ(a.Tree().Root(), b.Tree().Root());
    EXPECT_NE(a.Tree().Root(), c.Tree().Root());
    HashTree local = c.Tree();
    HashTree remote = a.Tree();
    const Divergence found = Bisect(local, remote);
    EXPECT_EQ(found.kind, Divergence::Kind::FieldDiffers);
    EXPECT_EQ(found.objectId, 2u);
    EXPECT_EQ(DescribeHashedField(c.world, c.layouts, 2, found.fieldId), "HashAll.ratio");
}

TEST(StateHashTest, ObjectsAddedAndRemovedAreFound)
{
    Rig a({10, 20, 30});
    Rig fewer({10, 20});
    Rig more({10, 20, 30, 40});
    FillAll(a, {10, 20, 30});
    FillAll(fewer, {10, 20});
    FillAll(more, {10, 20, 30, 40});

    HashTree full = a.Tree();
    HashTree missing = fewer.Tree();
    HashTree extra = more.Tree();
    EXPECT_NE(full.Root(), missing.Root());
    EXPECT_NE(full.Root(), extra.Root());

    const Divergence lacks = Bisect(full, missing);
    EXPECT_EQ(lacks.kind, Divergence::Kind::ObjectOnlyLocal);
    EXPECT_EQ(lacks.objectId, 3u);
    const Divergence gains = Bisect(full, extra);
    EXPECT_EQ(gains.kind, Divergence::Kind::ObjectOnlyRemote);
    EXPECT_EQ(gains.objectId, 4u);
}

TEST(StateHashTest, ASpawnedAndDespawnedObjectChangesTheRootAndRestoresIt)
{
    ScopedPrefab prefab("StateHashPrefab");
    ASSERT_TRUE(prefab.Registered());
    Rig a(kUuids);
    FillAll(a, kUuids);
    const std::uint64_t before = a.Tree().Root();

    const NetworkId id{500};
    ASSERT_TRUE(a.world.SpawnInstance(id, prefab.Name(), 3).has_value());
    HashTree withSpawn = a.Tree();
    EXPECT_NE(withSpawn.Root(), before);
    EXPECT_EQ(withSpawn.Objects().size(), 4u);
    // The spawned object has no replicated fields: it is its layout and meta leaves (owner 3, spawned)
    EXPECT_EQ(withSpawn.Objects().back().id, 500u);
    EXPECT_EQ(withSpawn.Objects().back().fields.size(), 2u);

    EXPECT_TRUE(a.world.Despawn(id).found);
    a.layouts.Forget(id);
    EXPECT_EQ(a.Tree().Root(), before);
}

TEST(StateHashTest, OwnerIsPartOfTheState)
{
    ScopedPrefab prefab("StateHashOwnerPrefab");
    Rig a(kUuids);
    Rig b(kUuids);
    ASSERT_TRUE(a.world.SpawnInstance(NetworkId{500}, prefab.Name(), 1).has_value());
    ASSERT_TRUE(b.world.SpawnInstance(NetworkId{500}, prefab.Name(), 2).has_value());
    HashTree ta = a.Tree();
    HashTree tb = b.Tree();
    const Divergence found = Bisect(ta, tb);
    ASSERT_EQ(found.kind, Divergence::Kind::FieldDiffers);
    EXPECT_EQ(found.objectId, 500u);
    EXPECT_EQ(found.fieldId, kMetaFieldId);
    EXPECT_EQ(DescribeHashedField(a.world, a.layouts, 500, kMetaFieldId), "<meta>");
}

TEST(StateHashTest, TheTreeDoesNotDependOnCreationOrder)
{
    Rig forward({10, 20, 30});
    Rig shuffled({30, 10, 20});
    Rig reversed({20, 30, 10});
    FillAll(forward, {10, 20, 30});
    FillAll(shuffled, {10, 20, 30});
    FillAll(reversed, {10, 20, 30});
    EXPECT_EQ(forward.Tree().Root(), shuffled.Tree().Root());
    EXPECT_EQ(forward.Tree().Root(), reversed.Tree().Root());

    // A cold layout cache and a warm one agree, and so do repeated builds
    LayoutCache cold;
    const std::optional<HashTree> fromCold = BuildStateHashTree(forward.world, cold, 7);
    ASSERT_TRUE(fromCold.has_value());
    EXPECT_EQ(fromCold->Root(), forward.Tree().Root());
    EXPECT_EQ(forward.Tree().Root(), forward.Tree().Root());
}

TEST(StateHashTest, ADifferentLayoutIsADifference)
{
    Rig plain({10}, true);
    Rig extended({10}, true);
    FindByNumber(*extended.scene, 10)->AddComponent<HashOther>();
    HashTree p = plain.Tree();
    HashTree e = extended.Tree();
    const Divergence found = Bisect(p, e);
    // Either the layout leaf or an extra field is the first difference; it must not be None
    EXPECT_NE(found.kind, Divergence::Kind::None);
    EXPECT_EQ(found.objectId, 1u);
}

TEST(StateHashTest, AnObjectWithNoReplicatedFieldsIsStillCounted)
{
    Rig bare({10, 20}, false);
    HashTree tree = bare.Tree();
    ASSERT_EQ(tree.Objects().size(), 2u);
    EXPECT_EQ(tree.Objects()[0].fields.size(), 2u);
    Rig one({10}, false);
    EXPECT_NE(one.Tree().Root(), tree.Root());
}

TEST(StateHashTest, AFullWorldStaysWithinTheTreeBounds)
{
    std::vector<std::uint32_t> uuids;
    for (std::uint32_t n = 1; n <= kMaxNetworkObjects; ++n)
    {
        uuids.push_back(n);
    }
    Rig big(uuids, false);
    EXPECT_EQ(big.world.Count(), kMaxNetworkObjects);
    const std::optional<HashTree> tree = BuildStateHashTree(big.world, big.layouts, 1);
    ASSERT_TRUE(tree.has_value());
    EXPECT_EQ(tree->Objects().size(), kMaxNetworkObjects);
    EXPECT_LE(tree->Objects().size(), N2Net::kMaxTreeObjects);
    for (const HashTree::Object &object : tree->Objects())
    {
        EXPECT_LE(object.fields.size(), N2Net::kMaxFieldsPerObject);
    }
}
