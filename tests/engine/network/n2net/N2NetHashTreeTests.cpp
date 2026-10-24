#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "engine/network/n2net/HashTree.hpp"
#include "engine/network/n2net/SimRandom.hpp"

// The state-hash tree and its bisection (netcode C3): the tree is a function of the data and not of the order it was
// walked in, one injected change anywhere is located exactly (object and field), and a peer's hostile or inconsistent
// answers end in Malformed, never in a conclusion or an unbounded allocation.

using namespace N2Engine::N2Net;

namespace
{
    constexpr std::uint32_t kTick = 300;

    std::array<std::uint8_t, 8> Encoded(const std::uint64_t value)
    {
        std::array<std::uint8_t, 8> bytes{};
        for (int i = 0; i < 8; ++i)
        {
            bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
        }
        return bytes;
    }

    // The value of (object, field) in the unmodified world
    std::uint64_t Value(const std::uint32_t id, const std::uint16_t field)
    {
        return (static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::uint64_t>(field) * 0xBF58476D1CE4E5B9ull);
    }

    struct Change
    {
        std::uint32_t id = 0;
        std::uint16_t field = 0;
        std::uint64_t value = 0;
    };

    // `ids` objects with ids i * stride + offset, `fields` fields each, with `changes` applied
    HashTree MakeWorld(const std::uint32_t ids, const std::uint16_t fields, const std::uint32_t stride, const std::uint32_t offset,
                       const std::vector<Change> &changes = {}, const std::vector<std::uint32_t> &skipObjects = {})
    {
        HashTreeBuilder builder(kTick);
        for (std::uint32_t i = 0; i < ids; ++i)
        {
            const std::uint32_t id = i * stride + offset;
            bool skip = false;
            for (const std::uint32_t s : skipObjects)
            {
                skip = skip || s == id;
            }
            if (skip)
            {
                continue;
            }
            for (std::uint16_t f = 0; f < fields; ++f)
            {
                std::uint64_t value = Value(id, f);
                for (const Change &c : changes)
                {
                    if (c.id == id && c.field == f)
                    {
                        value = c.value;
                    }
                }
                const auto bytes = Encoded(value);
                EXPECT_TRUE(builder.AddField(id, f, bytes));
            }
        }
        auto tree = builder.Build();
        EXPECT_TRUE(tree.has_value());
        return tree ? *tree : HashTree{};
    }
}

TEST(HashTree, RootDoesNotDependOnInsertionOrder)
{
    HashTreeBuilder forward(5);
    HashTreeBuilder backward(5);
    for (std::uint32_t id = 1; id <= 20; ++id)
    {
        for (std::uint16_t f = 0; f < 3; ++f)
        {
            const auto bytes = Encoded(Value(id, f));
            ASSERT_TRUE(forward.AddField(id, f, bytes));
            const auto reverse = Encoded(Value(21 - id, static_cast<std::uint16_t>(2 - f)));
            ASSERT_TRUE(backward.AddField(21 - id, static_cast<std::uint16_t>(2 - f), reverse));
        }
    }
    const auto a = forward.Build();
    const auto b = backward.Build();
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->Root(), b->Root());
    EXPECT_EQ(a->Objects().size(), 20u);
    EXPECT_EQ(a->Objects()[0].id, 1u);
}

TEST(HashTree, RootCoversTickValuesFieldIdsLengthsAndObjectExistence)
{
    auto root = [](const std::uint32_t tick, const std::uint32_t id, const std::uint16_t field, const std::vector<std::uint8_t> &bytes,
                   const bool extraEmptyObject)
    {
        HashTreeBuilder builder(tick);
        EXPECT_TRUE(builder.AddField(id, field, bytes));
        if (extraEmptyObject)
        {
            EXPECT_TRUE(builder.AddObject(id + 1));
        }
        return builder.Build()->Root();
    };
    const std::uint64_t base = root(1, 5, 2, {1, 2, 3}, false);
    EXPECT_NE(base, root(2, 5, 2, {1, 2, 3}, false)) << "tick";
    EXPECT_NE(base, root(1, 6, 2, {1, 2, 3}, false)) << "object id";
    EXPECT_NE(base, root(1, 5, 3, {1, 2, 3}, false)) << "field id";
    EXPECT_NE(base, root(1, 5, 2, {1, 2, 4}, false)) << "value";
    EXPECT_NE(base, root(1, 5, 2, {1, 2, 3, 0}, false)) << "length";
    EXPECT_NE(base, root(1, 5, 2, {1, 2, 3}, true)) << "an empty object is state";
    EXPECT_EQ(base, root(1, 5, 2, {1, 2, 3}, false));
}

TEST(HashTree, BuilderRefusesDuplicatesAndOversizedObjects)
{
    const std::array<std::uint8_t, 1> byte{1};
    HashTreeBuilder duplicate(0);
    ASSERT_TRUE(duplicate.AddField(1, 1, byte));
    ASSERT_TRUE(duplicate.AddField(1, 1, byte));
    EXPECT_FALSE(duplicate.Build().has_value());

    HashTreeBuilder wide(0);
    for (std::size_t f = 0; f <= kMaxFieldsPerObject; ++f)
    {
        ASSERT_TRUE(wide.AddField(7, static_cast<std::uint16_t>(f), byte));
    }
    EXPECT_FALSE(wide.Build().has_value()) << "257 fields";

    HashTreeBuilder exact(0);
    for (std::size_t f = 0; f < kMaxFieldsPerObject; ++f)
    {
        ASSERT_TRUE(exact.AddField(7, static_cast<std::uint16_t>(f), byte));
    }
    EXPECT_TRUE(exact.Build().has_value());

    HashTreeBuilder empty(9);
    const auto tree = empty.Build();
    ASSERT_TRUE(tree.has_value());
    EXPECT_TRUE(tree->Objects().empty());
}

TEST(HashTree, RangeSummariesAddUp)
{
    HashTree tree = MakeWorld(100, 2, 10, 5);
    RangeSummary all;
    ASSERT_TRUE(tree.Range(0, 0xFFFFFFFFu, all));
    EXPECT_EQ(all.count, 100u);
    RangeSummary low;
    RangeSummary high;
    ASSERT_TRUE(tree.Range(0, 504, low));
    ASSERT_TRUE(tree.Range(505, 0xFFFFFFFFu, high));
    EXPECT_EQ(low.count, 50u);
    EXPECT_EQ(high.count, 50u);
    RangeSummary none;
    ASSERT_TRUE(tree.Range(6, 14, none));
    EXPECT_EQ(none.count, 0u);
    ASSERT_TRUE(tree.Range(9, 3, none));
    EXPECT_EQ(none.count, 0u);
    std::vector<ObjectDigest> objects;
    EXPECT_FALSE(tree.Objects(0, 0xFFFFFFFFu, objects)) << "more than kMaxReplyObjects";
    EXPECT_TRUE(tree.Objects(0, 5 + 10 * 3, objects));
    EXPECT_EQ(objects.size(), 4u);
    std::vector<HashLeaf> fields;
    EXPECT_TRUE(tree.Fields(15, fields));
    EXPECT_EQ(fields.size(), 2u);
    EXPECT_FALSE(tree.Fields(16, fields));
}

TEST(HashTreeBisect, EqualTreesTakeOneRequest)
{
    HashTree a = MakeWorld(500, 4, 3, 1);
    HashTree b = MakeWorld(500, 4, 3, 1);
    ASSERT_EQ(a.Root(), b.Root());
    const Divergence d = Bisect(a, b);
    EXPECT_EQ(d.kind, Divergence::Kind::None);
    EXPECT_EQ(d.rounds, 1);
}

TEST(HashTreeBisect, LocatesAnInjectedSingleFieldDivergenceExactly)
{
    constexpr std::uint32_t kObjects = 1500;
    constexpr std::uint16_t kFields = 6;
    SimRandom random(0xD35C);
    int worst = 0;
    for (int trial = 0; trial < 60; ++trial)
    {
        const std::uint32_t index = random.NextBelow(kObjects);
        const std::uint32_t id = index * 7 + 3;
        const std::uint16_t field = static_cast<std::uint16_t>(random.NextBelow(kFields));
        HashTree local = MakeWorld(kObjects, kFields, 7, 3);
        HashTree remote = MakeWorld(kObjects, kFields, 7, 3, {Change{id, field, Value(id, field) ^ (1ull << random.NextBelow(64))}});
        ASSERT_NE(local.Root(), remote.Root());
        const Divergence d = Bisect(local, remote);
        ASSERT_EQ(d.kind, Divergence::Kind::FieldDiffers) << "trial " << trial;
        EXPECT_EQ(d.objectId, id) << "trial " << trial;
        EXPECT_EQ(d.fieldId, field) << "trial " << trial;
        EXPECT_NE(d.localHash, d.remoteHash);
        EXPECT_GT(d.rounds, 1);
        worst = std::max(worst, d.rounds);
        // The other direction finds the same place
        const Divergence reverse = Bisect(remote, local);
        EXPECT_EQ(reverse.objectId, id);
        EXPECT_EQ(reverse.fieldId, field);
    }
    EXPECT_LE(worst, 72) << "two requests per level of the 32-level id space at worst, plus the leaf lists";
}

TEST(HashTreeBisect, FindsTheFirstOfSeveralDivergences)
{
    HashTree local = MakeWorld(400, 3, 11, 0);
    HashTree remote = MakeWorld(400, 3, 11, 0,
                                {Change{11 * 300, 0, 1}, Change{11 * 120, 2, 2}, Change{11 * 120, 1, 3}, Change{11 * 399, 0, 4}});
    const Divergence d = Bisect(local, remote);
    ASSERT_EQ(d.kind, Divergence::Kind::FieldDiffers);
    EXPECT_EQ(d.objectId, 11u * 120u);
    EXPECT_EQ(d.fieldId, 1) << "the lowest field of the lowest object";
}

TEST(HashTreeBisect, ObjectsAtTheEdgesOfTheIdSpace)
{
    const std::array<std::uint8_t, 1> one{1};
    const std::array<std::uint8_t, 1> two{2};
    for (const std::uint32_t id : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu})
    {
        HashTreeBuilder a(1);
        HashTreeBuilder b(1);
        for (std::uint32_t pad = 0; pad < 40; ++pad)
        {
            const std::uint32_t padId = 1000 + pad * 100000;
            ASSERT_TRUE(a.AddField(padId, 0, one));
            ASSERT_TRUE(b.AddField(padId, 0, one));
        }
        ASSERT_TRUE(a.AddField(id, 9, one));
        ASSERT_TRUE(b.AddField(id, 9, two));
        auto local = *a.Build();
        auto remote = *b.Build();
        const Divergence d = Bisect(local, remote);
        ASSERT_EQ(d.kind, Divergence::Kind::FieldDiffers) << id;
        EXPECT_EQ(d.objectId, id);
        EXPECT_EQ(d.fieldId, 9);
    }
}

TEST(HashTreeBisect, MissingObjectsAndFields)
{
    {
        HashTree local = MakeWorld(300, 2, 5, 0);
        HashTree remote = MakeWorld(300, 2, 5, 0, {}, {5 * 77});
        const Divergence d = Bisect(local, remote);
        EXPECT_EQ(d.kind, Divergence::Kind::ObjectOnlyLocal);
        EXPECT_EQ(d.objectId, 5u * 77u);
        const Divergence reverse = Bisect(remote, local);
        EXPECT_EQ(reverse.kind, Divergence::Kind::ObjectOnlyRemote);
        EXPECT_EQ(reverse.objectId, 5u * 77u);
    }
    {
        const std::array<std::uint8_t, 1> one{1};
        HashTreeBuilder a(1);
        HashTreeBuilder b(1);
        ASSERT_TRUE(a.AddField(4, 1, one));
        ASSERT_TRUE(a.AddField(4, 2, one));
        ASSERT_TRUE(b.AddField(4, 1, one));
        auto local = *a.Build();
        auto remote = *b.Build();
        const Divergence d = Bisect(local, remote);
        EXPECT_EQ(d.kind, Divergence::Kind::FieldOnlyLocal);
        EXPECT_EQ(d.objectId, 4u);
        EXPECT_EQ(d.fieldId, 2);
        EXPECT_EQ(Bisect(remote, local).kind, Divergence::Kind::FieldOnlyRemote);
    }
    {
        HashTreeBuilder a(1);
        HashTreeBuilder b(1);
        ASSERT_TRUE(a.AddObject(3));
        auto local = *a.Build();
        auto remote = *b.Build();
        EXPECT_EQ(Bisect(local, remote).kind, Divergence::Kind::ObjectOnlyLocal);
    }
}

TEST(HashTreeBisect, DifferentTicksDifferAtTheRootButHaveNoDivergentObject)
{
    HashTreeBuilder a(1);
    HashTreeBuilder b(2);
    const std::array<std::uint8_t, 1> one{1};
    ASSERT_TRUE(a.AddField(3, 0, one));
    ASSERT_TRUE(b.AddField(3, 0, one));
    auto local = *a.Build();
    auto remote = *b.Build();
    EXPECT_NE(local.Root(), remote.Root());
    // The range hashes cover only the objects, so this is a comparison of two different checkpoints, not a desync
    EXPECT_EQ(Bisect(local, remote).kind, Divergence::Kind::None);
}

namespace
{
    // A peer that answers from a real tree but lies in one way
    class LyingPeer final : public IHashTreePeer
    {
    public:
        enum class Lie
        {
            None,
            RangeRefuses,
            WrongObjectCount,
            ObjectsOutOfRange,
            ObjectsUnsorted,
            FieldsUnsorted,
            TooManyFields,
            FieldsRefuse,
            EverythingEqualButRootDiffers,
        };

        LyingPeer(HashTree &truth, const Lie lie) : truth_(truth), lie_(lie) {}

        bool Range(const std::uint32_t lo, const std::uint32_t hi, RangeSummary &out) override
        {
            if (lie_ == Lie::RangeRefuses)
            {
                return false;
            }
            const bool ok = truth_.Range(lo, hi, out);
            if (lie_ == Lie::EverythingEqualButRootDiffers)
            {
                out.hash ^= 1;
            }
            return ok;
        }

        bool Objects(const std::uint32_t lo, const std::uint32_t hi, std::vector<ObjectDigest> &out) override
        {
            const bool ok = truth_.Objects(lo, hi, out);
            switch (lie_)
            {
            case Lie::WrongObjectCount:
                if (!out.empty())
                {
                    out.pop_back();
                }
                break;
            case Lie::ObjectsOutOfRange:
                out.push_back(ObjectDigest{hi == 0xFFFFFFFFu ? 0u : hi + 1, 1});
                break;
            case Lie::ObjectsUnsorted:
                if (out.size() > 1)
                {
                    std::swap(out.front(), out.back());
                }
                break;
            default:
                break;
            }
            return ok;
        }

        bool Fields(const std::uint32_t id, std::vector<HashLeaf> &out) override
        {
            if (lie_ == Lie::FieldsRefuse)
            {
                return false;
            }
            const bool ok = truth_.Fields(id, out);
            if (lie_ == Lie::FieldsUnsorted && out.size() > 1)
            {
                std::swap(out.front(), out.back());
            }
            if (lie_ == Lie::TooManyFields)
            {
                out.resize(kMaxFieldsPerObject + 1);
                for (std::size_t i = 0; i < out.size(); ++i)
                {
                    out[i].fieldId = static_cast<std::uint16_t>(i);
                }
            }
            return ok;
        }

    private:
        HashTree &truth_;
        Lie lie_;
    };
}

TEST(HashTreeBisect, InconsistentPeerAnswersAreMalformedNotConclusions)
{
    using Lie = LyingPeer::Lie;
    for (const Lie lie : {Lie::RangeRefuses, Lie::WrongObjectCount, Lie::ObjectsOutOfRange, Lie::ObjectsUnsorted, Lie::FieldsUnsorted,
                          Lie::TooManyFields, Lie::FieldsRefuse})
    {
        // Few objects (so the leaf list is asked for at once) with several fields, one of which differs
        HashTree local = MakeWorld(5, 4, 3, 1);
        HashTree truth = MakeWorld(5, 4, 3, 1, {Change{4, 1, 99}, Change{4, 3, 98}});
        LyingPeer peer(truth, lie);
        const Divergence d = Bisect(local, peer);
        EXPECT_EQ(d.kind, Divergence::Kind::Malformed) << static_cast<int>(lie);
    }
}

TEST(HashTreeBisect, ADivergenceBeyondTheLeafListNeverAsksForMoreThanTheCap)
{
    class CapChecking final : public IHashTreePeer
    {
    public:
        explicit CapChecking(HashTree &truth) : truth_(truth) {}
        bool Range(const std::uint32_t lo, const std::uint32_t hi, RangeSummary &out) override { return truth_.Range(lo, hi, out); }
        bool Objects(const std::uint32_t lo, const std::uint32_t hi, std::vector<ObjectDigest> &out) override
        {
            const bool ok = truth_.Objects(lo, hi, out);
            ++objectRequests;
            EXPECT_TRUE(ok) << "Bisect asked for a list over the cap";
            EXPECT_LE(out.size(), kMaxReplyObjects);
            return ok;
        }
        bool Fields(const std::uint32_t id, std::vector<HashLeaf> &out) override { return truth_.Fields(id, out); }
        int objectRequests = 0;

    private:
        HashTree &truth_;
    };
    HashTree local = MakeWorld(5000, 2, 1, 0);
    HashTree truth = MakeWorld(5000, 2, 1, 0, {Change{4321, 1, 5}});
    CapChecking peer(truth);
    const Divergence d = Bisect(local, peer);
    EXPECT_EQ(d.kind, Divergence::Kind::FieldDiffers);
    EXPECT_EQ(d.objectId, 4321u);
    EXPECT_EQ(peer.objectRequests, 1);
}

TEST(HashTreeHistory, KeepsTheLastKTrees)
{
    HashTreeHistory history(3);
    for (std::uint32_t tick = 1; tick <= 5; ++tick)
    {
        HashTreeBuilder builder(tick * 30);
        history.Push(*builder.Build());
    }
    ASSERT_EQ(history.Trees().size(), 3u);
    EXPECT_EQ(history.Trees().front().Tick(), 90u);
    EXPECT_EQ(history.Trees().back().Tick(), 150u);
    EXPECT_EQ(history.Find(60), nullptr);
    ASSERT_NE(history.Find(120), nullptr);
    EXPECT_EQ(history.Find(120)->Tick(), 120u);
    EXPECT_EQ(HashTreeHistory(0).Capacity(), 1u);
    EXPECT_EQ(HashTreeHistory(100000).Capacity(), kMaxHistory);
}

TEST(RootMessage, RoundTripsAndRefusesMalformed)
{
    const RootMessage message{0xFFFFFFF0u, 0x0123456789ABCDEFull};
    std::array<std::uint8_t, 32> buffer{};
    ASSERT_EQ(EncodeRootMessage(message, buffer), kRootMessageSize);
    const auto decoded = DecodeRootMessage(std::span<const std::uint8_t>(buffer.data(), kRootMessageSize));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, message);
    EXPECT_FALSE(DecodeRootMessage(std::span<const std::uint8_t>(buffer.data(), kRootMessageSize - 1)).has_value());
    EXPECT_FALSE(DecodeRootMessage(std::span<const std::uint8_t>(buffer.data(), kRootMessageSize + 1)).has_value());
    buffer[0] = 1;
    EXPECT_FALSE(DecodeRootMessage(std::span<const std::uint8_t>(buffer.data(), kRootMessageSize)).has_value());
    EXPECT_EQ(EncodeRootMessage(message, std::span<std::uint8_t>(buffer.data(), 5)), 0u);
}
