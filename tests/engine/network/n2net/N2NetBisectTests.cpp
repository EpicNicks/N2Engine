#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "N2NetTestUtil.hpp"
#include "engine/network/n2net/BisectProtocol.hpp"
#include "engine/network/n2net/SimRandom.hpp"

// The wire side of hash-tree bisection (netcode C3): bounded codecs, the responder's rate limit, the client's request
// table and budget, and two peers over a lossy SimTransport locating an injected single-field divergence, plus peers
// that stall, lie or flood.

using namespace N2Engine::N2Net;
using namespace N2Engine::N2Net::Testing;

namespace
{
    using Bytes = std::vector<std::uint8_t>;

    Bytes EncodeRequest(const BisectRequest &request)
    {
        Bytes bytes(kBisectRequestSize);
        const std::size_t size = EncodeBisectRequest(request, bytes);
        bytes.resize(size);
        return bytes;
    }

    Bytes EncodeReply(const BisectReply &reply)
    {
        Bytes bytes(kMaxBisectReplySize);
        const std::size_t size = EncodeBisectReply(reply, bytes);
        bytes.resize(size);
        return bytes;
    }

    BisectReply RangeReply()
    {
        BisectReply r;
        r.requestId = 7;
        r.op = BisectOp::Range;
        r.range = RangeSummary{12345, 0x0123456789ABCDEFull};
        return r;
    }

    BisectReply ObjectsReply(const std::size_t n)
    {
        BisectReply r;
        r.requestId = 8;
        r.op = BisectOp::Objects;
        for (std::size_t i = 0; i < n; ++i)
        {
            r.objects.push_back(ObjectDigest{static_cast<std::uint32_t>(100 + i), 0xA000ull + i});
        }
        return r;
    }

    BisectReply FieldsReply(const std::size_t n)
    {
        BisectReply r;
        r.requestId = 9;
        r.op = BisectOp::Fields;
        for (std::size_t i = 0; i < n; ++i)
        {
            r.fields.push_back(HashLeaf{static_cast<std::uint16_t>(i * 3), 0xB000ull * (i + 1)});
        }
        return r;
    }

    std::array<std::uint8_t, 4> Encoded(const std::uint32_t value)
    {
        return {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16),
                static_cast<std::uint8_t>(value >> 24)};
    }

    std::uint32_t Value(const std::uint32_t id, const std::uint16_t field)
    {
        return id * 2654435761u + field * 40503u;
    }

    struct Change
    {
        std::uint32_t id;
        std::uint16_t field;
    };

    HashTree World(const std::uint32_t tick, const std::uint32_t objects, const std::vector<Change> &changes = {})
    {
        HashTreeBuilder builder(tick);
        for (std::uint32_t i = 0; i < objects; ++i)
        {
            const std::uint32_t id = 1 + i * 997u; // spread over the id space
            for (std::uint16_t f = 1; f <= 4; ++f)
            {
                std::uint32_t value = Value(id, f);
                for (const Change &c : changes)
                {
                    if (c.id == id && c.field == f)
                    {
                        value ^= 0x5555u;
                    }
                }
                builder.AddField(id, f, Encoded(value));
            }
        }
        return *builder.Build();
    }

    constexpr std::uint32_t kTick = 90;

    // Peer A asks, peer B answers (or misbehaves) over a SimNetwork; this is A's driver.
    class Harness final : public IBisectDriver
    {
    public:
        Harness(const std::uint64_t seed, const SimLinkConfig &link, HashTree remoteTree,
                const BisectClientConfig &clientConfig = {}, const BisectResponderConfig &responderConfig = {})
            : peers_(seed, link), client_(*peers_.peerA.connection, clientConfig),
              responder_(history_, responderConfig, 0)
        {
            history_.Push(std::move(remoteTree));
        }

        std::uint64_t NowMicros() const override { return peers_.Now(); }

        bool Step() override
        {
            peers_.Step(1000);
            if (peers_.peerA.connection->IsFailed() || peers_.peerB.connection->IsFailed())
            {
                return false;
            }
            const std::uint64_t now = peers_.Now();
            for (const ReceivedMessage &m : peers_.peerA.received)
            {
                if (PeekMessageKind(m.data) == kBisectReplyKind)
                {
                    client_.OnMessage(now, m.data);
                }
            }
            peers_.peerA.received.clear();
            for (const ReceivedMessage &m : peers_.peerB.received)
            {
                if (PeekMessageKind(m.data) == kBisectRequestKind)
                {
                    OnRequest(now, m.data);
                }
            }
            peers_.peerB.received.clear();
            return true;
        }

        // Called with each request B receives; default serves honestly. A lie edits the honest reply.
        std::function<bool(const Bytes &)> hook; // true: handled, do not serve
        std::function<Bytes(Bytes)> tamper;
        std::size_t requestsSeen = 0;
        ServeResult lastResult = ServeResult::Replied;

        void OnRequest(const std::uint64_t now, const Bytes &request)
        {
            ++requestsSeen;
            if (hook && hook(request))
            {
                return;
            }
            std::span<const std::uint8_t> reply;
            lastResult = responder_.Serve(now, request, reply);
            if (lastResult != ServeResult::Replied)
            {
                return;
            }
            Bytes out(reply.begin(), reply.end());
            if (tamper)
            {
                out = tamper(std::move(out));
            }
            peers_.peerB.connection->SendReliable(out);
        }

        TwoPeers peers_;
        HashTreeHistory history_{8};
        BisectClient client_;
        BisectResponder responder_;
    };

    SimLinkConfig LossyLink()
    {
        SimLinkConfig link;
        link.latencyMicros = 8'000;
        link.jitterMicros = 6'000;
        link.lossPpm = 100'000;
        link.reorderPpm = 150'000;
        link.reorderDelayMicros = 20'000;
        link.duplicatePpm = 30'000;
        return link;
    }

    BisectClientConfig PatientClient()
    {
        BisectClientConfig c;
        c.requestTimeoutMicros = 20'000'000;
        return c;
    }
}

// ---- codecs ----

TEST(BisectCodec, RequestRoundTripsForEveryOp)
{
    for (BisectOp op : {BisectOp::Range, BisectOp::Objects, BisectOp::Fields})
    {
        BisectRequest r;
        r.requestId = 0xDEADBEEF;
        r.op = op;
        r.tick = 0xFFFFFFF0u;
        r.lo = op == BisectOp::Fields ? 0x80000001u : 5;
        r.hi = op == BisectOp::Fields ? 0 : 0xFFFFFFFFu;
        const Bytes bytes = EncodeRequest(r);
        ASSERT_EQ(bytes.size(), kBisectRequestSize);
        EXPECT_EQ(bytes[0], kBisectRequestKind);
        const auto back = DecodeBisectRequest(bytes);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, r);
    }
}

TEST(BisectCodec, RequestRejectsBadFieldsAndEveryTruncation)
{
    BisectRequest r;
    r.op = BisectOp::Range;
    r.lo = 1;
    r.hi = 2;
    const Bytes good = EncodeRequest(r);
    for (std::size_t len = 0; len < good.size(); ++len)
    {
        EXPECT_FALSE(DecodeBisectRequest(std::span<const std::uint8_t>(good.data(), len))) << len;
    }
    Bytes longer = good;
    longer.push_back(0);
    EXPECT_FALSE(DecodeBisectRequest(longer));

    Bytes bad = good;
    bad[0] = kBisectReplyKind;
    EXPECT_FALSE(DecodeBisectRequest(bad));
    bad = good;
    bad[5] = 3; // op
    EXPECT_FALSE(DecodeBisectRequest(bad));
    // lo > hi is refused on both sides
    r.lo = 9;
    r.hi = 3;
    std::array<std::uint8_t, kBisectRequestSize> buffer{};
    EXPECT_EQ(EncodeBisectRequest(r, buffer), 0u);
    Bytes swapped = good;
    swapped[10] = 9; // lo = 9 > hi = 2
    EXPECT_FALSE(DecodeBisectRequest(swapped));
    // Fields with a nonzero hi
    r.op = BisectOp::Fields;
    r.lo = 4;
    r.hi = 1;
    EXPECT_EQ(EncodeBisectRequest(r, buffer), 0u);
    // too small a buffer
    r.hi = 0;
    EXPECT_EQ(EncodeBisectRequest(r, std::span<std::uint8_t>(buffer.data(), kBisectRequestSize - 1)), 0u);
}

TEST(BisectCodec, RepliesRoundTrip)
{
    std::vector<BisectReply> replies = {RangeReply(), ObjectsReply(0), ObjectsReply(1), ObjectsReply(kMaxReplyObjects),
                                        FieldsReply(0), FieldsReply(5), FieldsReply(kMaxFieldsPerObject)};
    for (const BisectOp op : {BisectOp::Range, BisectOp::Objects, BisectOp::Fields})
    {
        BisectReply r;
        r.op = op;
        r.status = BisectStatus::UnknownTick;
        replies.push_back(r);
    }
    BisectReply tooMany;
    tooMany.op = BisectOp::Objects;
    tooMany.status = BisectStatus::TooMany;
    replies.push_back(tooMany);
    BisectReply noObject;
    noObject.op = BisectOp::Fields;
    noObject.status = BisectStatus::NoSuchObject;
    replies.push_back(noObject);

    for (const BisectReply &reply : replies)
    {
        const Bytes bytes = EncodeReply(reply);
        ASSERT_FALSE(bytes.empty());
        EXPECT_LE(bytes.size(), kMaxBisectReplySize);
        const auto back = DecodeBisectReply(bytes);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, reply);
    }
    EXPECT_EQ(EncodeReply(FieldsReply(kMaxFieldsPerObject)).size(), kMaxBisectReplySize);
}

TEST(BisectCodec, ReplyTruncatedAtEveryLengthOrWithTrailingBytesIsRefused)
{
    BisectReply tooMany;
    tooMany.op = BisectOp::Objects;
    tooMany.status = BisectStatus::TooMany;
    for (const BisectReply &reply :
         {RangeReply(), ObjectsReply(3), ObjectsReply(kMaxReplyObjects), FieldsReply(6), FieldsReply(kMaxFieldsPerObject), tooMany})
    {
        const Bytes good = EncodeReply(reply);
        for (std::size_t len = 0; len < good.size(); ++len)
        {
            EXPECT_FALSE(DecodeBisectReply(std::span<const std::uint8_t>(good.data(), len))) << "len " << len;
        }
        Bytes longer = good;
        longer.push_back(0);
        EXPECT_FALSE(DecodeBisectReply(longer));
        longer.insert(longer.end(), 40, 0xFF);
        EXPECT_FALSE(DecodeBisectReply(longer));
    }
}

TEST(BisectCodec, OverCapAndUnbackedCountsAreRefusedBeforeAllocating)
{
    // Objects with count 9 and 9 real entries: over the reply cap
    Bytes nine = EncodeReply(ObjectsReply(kMaxReplyObjects));
    nine[7] = 9;
    nine.insert(nine.end(), kBisectObjectEntrySize, 1);
    EXPECT_FALSE(DecodeBisectReply(nine));
    // Count says 8, 1 entry present; count says 0, entries present; count 255 with nothing behind it
    Bytes one = EncodeReply(ObjectsReply(1));
    one[7] = 8;
    EXPECT_FALSE(DecodeBisectReply(one));
    one[7] = 0;
    EXPECT_FALSE(DecodeBisectReply(one));
    Bytes huge = EncodeReply(ObjectsReply(0));
    huge[7] = 255;
    EXPECT_FALSE(DecodeBisectReply(huge));

    // Fields: 257 entries (over the cap) cannot even fit the size limit; 65535 claimed with nothing behind it
    Bytes over = EncodeReply(FieldsReply(kMaxFieldsPerObject));
    over[7] = 0x01;
    over[8] = 0x01; // 257
    EXPECT_FALSE(DecodeBisectReply(over));
    Bytes claimed = EncodeReply(FieldsReply(0));
    claimed[7] = 0xFF;
    claimed[8] = 0xFF;
    EXPECT_FALSE(DecodeBisectReply(claimed));
    // Larger than the largest possible reply is refused up front
    EXPECT_FALSE(DecodeBisectReply(Bytes(kMaxBisectReplySize + 1, 4)));
    // The encoder refuses too many entries as well
    std::array<std::uint8_t, 4096> big{};
    EXPECT_EQ(EncodeBisectReply(ObjectsReply(kMaxReplyObjects + 1), big), 0u);
    EXPECT_EQ(EncodeBisectReply(FieldsReply(kMaxFieldsPerObject + 1), big), 0u);
}

TEST(BisectCodec, ReplyWithInconsistentOpStatusOrKindIsRefused)
{
    const Bytes good = EncodeReply(RangeReply());
    Bytes bad = good;
    bad[0] = kBisectRequestKind;
    EXPECT_FALSE(DecodeBisectReply(bad));
    bad = good;
    bad[5] = 3; // op
    EXPECT_FALSE(DecodeBisectReply(bad));
    bad = good;
    bad[6] = 4; // status
    EXPECT_FALSE(DecodeBisectReply(bad));
    bad = good;
    bad[6] = static_cast<std::uint8_t>(BisectStatus::TooMany); // TooMany on a Range
    EXPECT_FALSE(DecodeBisectReply(bad));
    bad = good;
    bad[6] = static_cast<std::uint8_t>(BisectStatus::NoSuchObject); // NoSuchObject on a Range
    EXPECT_FALSE(DecodeBisectReply(bad));
    // The encoder agrees
    BisectReply r = RangeReply();
    r.status = BisectStatus::TooMany;
    std::array<std::uint8_t, 64> small{};
    EXPECT_EQ(EncodeBisectReply(r, small), 0u);
}

TEST(BisectCodec, RandomBytesNeverCrashAndAcceptedMessagesAreCanonical)
{
    SimRandom rng(0xB15EC7);
    const std::vector<Bytes> seeds = {EncodeRequest(BisectRequest{1, BisectOp::Range, 2, 3, 4}), EncodeReply(RangeReply()),
                                      EncodeReply(ObjectsReply(4)), EncodeReply(FieldsReply(9))};
    std::size_t accepted = 0;
    for (int i = 0; i < 40000; ++i)
    {
        Bytes bytes;
        if (i % 2 == 0)
        {
            bytes.resize(rng.Next() % 300);
            for (auto &b : bytes)
            {
                b = static_cast<std::uint8_t>(rng.Next());
            }
            if (!bytes.empty() && i % 4 == 0)
            {
                bytes[0] = (i % 8 == 0) ? kBisectRequestKind : kBisectReplyKind;
            }
        }
        else
        {
            bytes = seeds[rng.Next() % seeds.size()];
            const int edits = 1 + static_cast<int>(rng.Next() % 3);
            for (int e = 0; e < edits; ++e)
            {
                bytes[rng.Next() % bytes.size()] = static_cast<std::uint8_t>(rng.Next());
            }
            if (rng.Next() % 4 == 0)
            {
                bytes.resize(rng.Next() % (bytes.size() + 1));
            }
        }
        if (const auto request = DecodeBisectRequest(bytes))
        {
            ++accepted;
            EXPECT_EQ(EncodeRequest(*request), bytes);
        }
        if (const auto reply = DecodeBisectReply(bytes))
        {
            ++accepted;
            EXPECT_EQ(EncodeReply(*reply), bytes);
            EXPECT_LE(reply->objects.size(), kMaxReplyObjects);
            EXPECT_LE(reply->fields.size(), kMaxFieldsPerObject);
        }
    }
    EXPECT_GT(accepted, 0u); // the mutations of valid messages do sometimes stay valid
}

// ---- responder ----

TEST(BisectResponder, AnswersFromTheTreeAndSaysWhenItCannot)
{
    HashTreeHistory history(4);
    HashTree tree = World(kTick, 20);
    const RangeSummary expectedRange = [&]
    {
        HashTree copy = World(kTick, 20);
        RangeSummary s;
        copy.Range(0, 0xFFFFFFFFu, s);
        return s;
    }();
    history.Push(std::move(tree));
    BisectResponder responder(history, {}, 0);
    std::span<const std::uint8_t> reply;

    auto ask = [&](const BisectRequest &request)
    {
        const Bytes bytes = EncodeRequest(request);
        EXPECT_EQ(responder.Serve(0, bytes, reply), ServeResult::Replied);
        return *DecodeBisectReply(reply);
    };
    const BisectReply range = ask(BisectRequest{1, BisectOp::Range, kTick, 0, 0xFFFFFFFFu});
    EXPECT_EQ(range.status, BisectStatus::Ok);
    EXPECT_EQ(range.requestId, 1u);
    EXPECT_EQ(range.range, expectedRange);
    EXPECT_EQ(ask(BisectRequest{2, BisectOp::Range, kTick + 1, 0, 5}).status, BisectStatus::UnknownTick);
    EXPECT_EQ(ask(BisectRequest{3, BisectOp::Objects, kTick, 0, 0xFFFFFFFFu}).status, BisectStatus::TooMany);
    const BisectReply objects = ask(BisectRequest{4, BisectOp::Objects, kTick, 0, 500});
    EXPECT_EQ(objects.status, BisectStatus::Ok);
    EXPECT_EQ(objects.objects.size(), 1u);
    EXPECT_EQ(ask(BisectRequest{5, BisectOp::Fields, kTick, 12345, 0}).status, BisectStatus::NoSuchObject);
    const BisectReply fields = ask(BisectRequest{6, BisectOp::Fields, kTick, 1, 0});
    EXPECT_EQ(fields.status, BisectStatus::Ok);
    EXPECT_EQ(fields.fields.size(), 4u);
}

TEST(BisectResponder, RateLimitsDropsAndRefills)
{
    HashTreeHistory history(2);
    history.Push(World(kTick, 4));
    BisectResponderConfig config;
    config.burst = 4;
    config.refillMicros = 1000;
    config.maxViolations = 100;
    BisectResponder responder(history, config, 0);
    const Bytes request = EncodeRequest(BisectRequest{1, BisectOp::Range, kTick, 0, 10});
    std::span<const std::uint8_t> reply;
    int replied = 0;
    for (int i = 0; i < 50; ++i)
    {
        replied += responder.Serve(0, request, reply) == ServeResult::Replied ? 1 : 0;
    }
    EXPECT_EQ(replied, 4);
    EXPECT_EQ(responder.RateLimited(), 46u);
    EXPECT_TRUE(reply.empty());
    // 2.5 ms later: two tokens (the half carries over), then the third at 3 ms
    EXPECT_EQ(responder.Serve(2500, request, reply), ServeResult::Replied);
    EXPECT_EQ(responder.Serve(2500, request, reply), ServeResult::Replied);
    EXPECT_EQ(responder.Serve(2500, request, reply), ServeResult::RateLimited);
    EXPECT_EQ(responder.Serve(3000, request, reply), ServeResult::Replied);
    // A long pause refills to the burst and no more
    int after = 0;
    for (int i = 0; i < 20; ++i)
    {
        after += responder.Serve(10'000'000, request, reply) == ServeResult::Replied ? 1 : 0;
    }
    EXPECT_EQ(after, 4);
}

TEST(BisectResponder, AFloodingOrGarbagePeerBecomesAbusiveAndIsIgnored)
{
    HashTreeHistory history(2);
    history.Push(World(kTick, 4));
    BisectResponderConfig config;
    config.burst = 2;
    config.maxViolations = 10;
    BisectResponder responder(history, config, 0);
    const Bytes request = EncodeRequest(BisectRequest{1, BisectOp::Range, kTick, 0, 10});
    std::span<const std::uint8_t> reply;
    ServeResult last = ServeResult::Replied;
    for (int i = 0; i < 40 && !responder.Abusive(); ++i)
    {
        last = responder.Serve(0, request, reply);
    }
    EXPECT_TRUE(responder.Abusive());
    EXPECT_EQ(last, ServeResult::Abusive);
    // Even after time passes it answers nothing
    EXPECT_EQ(responder.Serve(100'000'000, request, reply), ServeResult::Abusive);
    EXPECT_TRUE(reply.empty());

    BisectResponder garbage(history, BisectResponderConfig{.burst = 100, .refillMicros = 1, .maxViolations = 5}, 0);
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_NE(garbage.Serve(0, Bytes{1, 2, 3}, reply), ServeResult::Replied);
    }
    EXPECT_TRUE(garbage.Abusive());
    EXPECT_EQ(garbage.MalformedCount(), 5u);
}

// ---- end to end over SimTransport ----

TEST(BisectNetwork, FindsAnInjectedSingleFieldDivergenceOverALossyLink)
{
    SimRandom pick(0xC0FFEE);
    for (int trial = 0; trial < 6; ++trial)
    {
        const std::uint32_t objects = 3000;
        const std::uint32_t victim = 1 + (pick.Next() % objects) * 997u;
        const std::uint16_t field = static_cast<std::uint16_t>(1 + pick.Next() % 4);
        HashTree local = World(kTick, objects);
        Harness h(1000 + trial, LossyLink(), World(kTick, objects, {Change{victim, field}}), PatientClient());
        NetworkedHashTreePeer peer(h.client_, h, kTick);

        const Divergence d = Bisect(local, peer);
        EXPECT_EQ(d.kind, Divergence::Kind::FieldDiffers) << trial;
        EXPECT_EQ(d.objectId, victim);
        EXPECT_EQ(d.fieldId, field);
        EXPECT_NE(d.localHash, d.remoteHash);
        EXPECT_EQ(h.client_.Failure(), BisectFailure::None);
        EXPECT_LE(d.rounds, 72);
        EXPECT_LE(h.client_.RequestsSent(), 72u);
        EXPECT_EQ(h.client_.Outstanding(), 0u);
        EXPECT_EQ(h.requestsSeen, h.client_.RequestsSent());
        EXPECT_EQ(h.responder_.RateLimited(), 0u);
        EXPECT_FALSE(h.peers_.peerA.connection->IsFailed());
    }
}

TEST(BisectNetwork, IdenticalTreesAreOneRequestAndMissingObjectsAreFound)
{
    {
        HashTree local = World(kTick, 500);
        Harness h(5, LossyLink(), World(kTick, 500), PatientClient());
        NetworkedHashTreePeer peer(h.client_, h, kTick);
        const Divergence d = Bisect(local, peer);
        EXPECT_EQ(d.kind, Divergence::Kind::None);
        EXPECT_EQ(h.client_.RequestsSent(), 1u);
    }
    {
        HashTree local = World(kTick, 500);
        HashTreeBuilder b(kTick);
        for (std::uint32_t i = 0; i < 500; ++i)
        {
            const std::uint32_t id = 1 + i * 997u;
            if (id == 1 + 250u * 997u)
            {
                continue;
            }
            for (std::uint16_t f = 1; f <= 4; ++f)
            {
                b.AddField(id, f, Encoded(Value(id, f)));
            }
        }
        Harness h(6, LossyLink(), *b.Build(), PatientClient());
        NetworkedHashTreePeer peer(h.client_, h, kTick);
        const Divergence d = Bisect(local, peer);
        EXPECT_EQ(d.kind, Divergence::Kind::ObjectOnlyLocal);
        EXPECT_EQ(d.objectId, 1 + 250u * 997u);
    }
}

TEST(BisectNetwork, SameSeedSameTrace)
{
    auto run = []
    {
        HashTree local = World(kTick, 800);
        Harness h(77, LossyLink(), World(kTick, 800, {Change{1 + 400u * 997u, 2}}), PatientClient());
        NetworkedHashTreePeer peer(h.client_, h, kTick);
        const Divergence d = Bisect(local, peer);
        return std::pair{d.objectId * 31 + d.rounds, h.peers_.Now()};
    };
    EXPECT_EQ(run(), run());
}

TEST(BisectNetwork, ASilentPeerTimesOut)
{
    HashTree local = World(kTick, 100);
    BisectClientConfig config;
    config.requestTimeoutMicros = 500'000;
    Harness h(3, SimLinkConfig{}, World(kTick, 100, {Change{1, 1}}), config);
    h.hook = [](const Bytes &) { return true; }; // swallow every request
    NetworkedHashTreePeer peer(h.client_, h, kTick);
    const std::uint64_t start = h.NowMicros();
    const Divergence d = Bisect(local, peer);
    EXPECT_EQ(d.kind, Divergence::Kind::Malformed);
    EXPECT_EQ(h.client_.Failure(), BisectFailure::TimedOut);
    EXPECT_EQ(peer.Failure(), BisectFailure::TimedOut);
    EXPECT_GE(h.NowMicros() - start, 500'000u);
    EXPECT_LT(h.NowMicros() - start, 600'000u);
    EXPECT_EQ(h.client_.RequestsSent(), 1u);
    // Once failed, nothing more is sent
    EXPECT_FALSE(h.client_.Submit(h.NowMicros(), BisectOp::Range, kTick, 0, 1).has_value());
    EXPECT_EQ(h.client_.RequestsSent(), 1u);
}

TEST(BisectNetwork, APeerThatStopsHalfWayTimesOut)
{
    HashTree local = World(kTick, 1000);
    BisectClientConfig config;
    config.requestTimeoutMicros = 300'000;
    Harness h(4, SimLinkConfig{}, World(kTick, 1000, {Change{1 + 7u * 997u, 3}}), config);
    h.hook = [&h](const Bytes &) { return h.requestsSeen > 5; };
    NetworkedHashTreePeer peer(h.client_, h, kTick);
    const Divergence d = Bisect(local, peer);
    EXPECT_EQ(d.kind, Divergence::Kind::Malformed);
    EXPECT_EQ(peer.Failure(), BisectFailure::TimedOut);
    EXPECT_EQ(h.client_.RequestsSent(), 6u);
}

TEST(BisectNetwork, LyingRepliesEndTheSessionAsMalformed)
{
    struct Lie
    {
        const char *name;
        std::function<Bytes(Bytes)> tamper;
        BisectFailure expected;
    };
    const std::vector<Lie> lies = {
        {"wrong op", [](Bytes b) { b[5] = (b[5] + 1) % 3; return b; }, BisectFailure::Malformed},
        {"truncated", [](Bytes b) { b.pop_back(); return b; }, BisectFailure::Malformed},
        {"trailing byte", [](Bytes b) { b.push_back(0); return b; }, BisectFailure::Malformed},
        {"garbage with the right id", [](Bytes b) { return Bytes{b[0], b[1], b[2], b[3], b[4], 9, 9, 9, 9}; }, BisectFailure::Malformed},
        {"unknown tick", [](Bytes b) { b[6] = static_cast<std::uint8_t>(BisectStatus::UnknownTick); b.resize(7); return b; },
         BisectFailure::UnknownTick},
    };
    for (const Lie &lie : lies)
    {
        HashTree local = World(kTick, 400);
        Harness h(9, SimLinkConfig{}, World(kTick, 400, {Change{1 + 100u * 997u, 2}}), PatientClient());
        h.tamper = lie.tamper;
        NetworkedHashTreePeer peer(h.client_, h, kTick);
        const Divergence d = Bisect(local, peer);
        EXPECT_EQ(d.kind, Divergence::Kind::Malformed) << lie.name;
        EXPECT_EQ(peer.Failure(), lie.expected) << lie.name;
    }
}

TEST(BisectNetwork, ConsistentLooksButInconsistentContentIsMalformed)
{
    // Objects outside the asked range, unsorted objects, unsorted fields, and a hash-colliding Range that claims a
    // count over the cap at a single id are all caught by the client or Bisect; none ends in a conclusion.
    auto lieAbout = [](BisectOp op, std::function<void(BisectReply &)> edit)
    {
        HashTree local = World(kTick, 6); // few objects: Bisect goes straight to Objects and Fields
        Harness h(11, SimLinkConfig{}, World(kTick, 6, {Change{1 + 3u * 997u, 2}}), PatientClient());
        h.tamper = [op, edit](Bytes bytes)
        {
            auto reply = DecodeBisectReply(bytes);
            if (!reply || reply->op != op)
            {
                return bytes;
            }
            edit(*reply);
            return EncodeReply(*reply);
        };
        NetworkedHashTreePeer peer(h.client_, h, kTick);
        return std::pair{Bisect(local, peer).kind, peer.Failure()};
    };
    EXPECT_EQ(lieAbout(BisectOp::Objects, [](BisectReply &r) { r.objects[0].id += 5000; }).second, BisectFailure::Malformed);
    EXPECT_EQ(lieAbout(BisectOp::Objects, [](BisectReply &r) { std::swap(r.objects[0], r.objects[1]); }).second, BisectFailure::Malformed);
    EXPECT_EQ(lieAbout(BisectOp::Fields, [](BisectReply &r) { std::swap(r.fields[0], r.fields[1]); }).second, BisectFailure::Malformed);
    // A reply that is well formed but says there is no such object where there is: Bisect gives Malformed, the
    // session itself is not failed (this is the peer saying "false")
    const auto noObject = lieAbout(BisectOp::Fields, [](BisectReply &r) { r.status = BisectStatus::NoSuchObject; r.fields.clear(); });
    EXPECT_EQ(noObject.first, Divergence::Kind::Malformed);
    // Dropping one object from an Objects answer is inconsistent with the Range summary
    EXPECT_EQ(lieAbout(BisectOp::Objects, [](BisectReply &r) { r.objects.pop_back(); }).first, Divergence::Kind::Malformed);
}

TEST(BisectNetwork, UnsolicitedAndDuplicateRepliesAreBoundedThenMalformed)
{
    SimLinkConfig link;
    Harness h(12, link, World(kTick, 10), BisectClientConfig{});
    BisectReply stray = RangeReply();
    stray.requestId = 4242;
    const Bytes bytes = EncodeReply(stray);
    BisectClientConfig defaults;
    for (std::uint32_t i = 0; i < defaults.maxUnsolicited; ++i)
    {
        EXPECT_TRUE(h.client_.OnMessage(0, bytes));
    }
    EXPECT_EQ(h.client_.Failure(), BisectFailure::None);
    EXPECT_FALSE(h.client_.OnMessage(0, bytes));
    EXPECT_EQ(h.client_.Failure(), BisectFailure::Malformed);

    // A reply of the wrong kind, or empty, is Malformed at once
    Harness g(13, link, World(kTick, 10));
    EXPECT_FALSE(g.client_.OnMessage(0, Bytes{}));
    EXPECT_EQ(g.client_.Failure(), BisectFailure::Malformed);
}

TEST(BisectNetwork, OutstandingTableAndRequestBudgetAreBounded)
{
    BisectClientConfig config;
    config.maxOutstanding = 3;
    config.maxRequests = 5;
    Harness h(14, SimLinkConfig{}, World(kTick, 10), config);
    BisectClient &c = h.client_;
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < 3; ++i)
    {
        const auto id = c.Submit(0, BisectOp::Range, kTick, 0, 10);
        ASSERT_TRUE(id.has_value());
        ids.push_back(*id);
    }
    EXPECT_EQ(c.Outstanding(), 3u);
    EXPECT_FALSE(c.Submit(0, BisectOp::Range, kTick, 0, 10).has_value()); // table full, not a failure
    EXPECT_EQ(c.Failure(), BisectFailure::None);
    EXPECT_EQ(c.RequestsSent(), 3u);

    // Let the answers arrive, take them, and the table is free again
    for (int i = 0; i < 50; ++i)
    {
        h.Step();
    }
    BisectReply reply;
    for (const std::uint32_t id : ids)
    {
        EXPECT_EQ(c.Take(id, reply), TakeResult::Ready);
        EXPECT_EQ(reply.requestId, id);
    }
    EXPECT_EQ(c.Outstanding(), 0u);
    EXPECT_EQ(c.Take(ids[0], reply), TakeResult::Failed); // not outstanding any more

    EXPECT_TRUE(c.Submit(0, BisectOp::Range, kTick, 0, 10).has_value());
    EXPECT_TRUE(c.Submit(0, BisectOp::Range, kTick, 0, 10).has_value());
    EXPECT_FALSE(c.Submit(0, BisectOp::Range, kTick, 0, 10).has_value()); // the sixth: over the budget of five
    EXPECT_EQ(c.Failure(), BisectFailure::BudgetExceeded);
    EXPECT_EQ(c.RequestsSent(), 5u);
}

TEST(BisectNetwork, ABisectionOverTheBudgetIsMalformedNotTruncated)
{
    HashTree local = World(kTick, 3000);
    BisectClientConfig config;
    config.maxRequests = 10; // a 3000-object bisection needs more
    Harness h(15, SimLinkConfig{}, World(kTick, 3000, {Change{1 + 1234u * 997u, 1}}), config);
    NetworkedHashTreePeer peer(h.client_, h, kTick);
    const Divergence d = Bisect(local, peer);
    EXPECT_EQ(d.kind, Divergence::Kind::Malformed);
    EXPECT_EQ(peer.Failure(), BisectFailure::BudgetExceeded);
    EXPECT_EQ(h.client_.RequestsSent(), 10u);
}

TEST(BisectNetwork, ARateLimitedResponderLooksLikeASilentOneToTheAsker)
{
    HashTree local = World(kTick, 3000);
    BisectResponderConfig tight;
    tight.burst = 3;
    tight.refillMicros = 10'000'000; // practically no refill
    tight.maxViolations = 1000;
    BisectClientConfig config;
    config.requestTimeoutMicros = 400'000;
    Harness h(16, SimLinkConfig{}, World(kTick, 3000, {Change{1 + 5u * 997u, 1}}), config, tight);
    NetworkedHashTreePeer peer(h.client_, h, kTick);
    const Divergence d = Bisect(local, peer);
    EXPECT_EQ(d.kind, Divergence::Kind::Malformed);
    EXPECT_EQ(peer.Failure(), BisectFailure::TimedOut);
    EXPECT_EQ(h.responder_.Answered(), 3u);
    EXPECT_GE(h.responder_.RateLimited(), 1u);
}

TEST(BisectNetwork, ADeadConnectionEndsTheWait)
{
    HashTree local = World(kTick, 100);
    Harness h(17, SimLinkConfig{}, World(kTick, 100, {Change{1, 1}}), PatientClient());
    h.hook = [](const Bytes &) { return true; };
    // The peer vanishes: unlink so nothing arrives and the idle timeout fails the connection
    h.peers_.Network().Unlink(h.peers_.peerA.transport->Id(), h.peers_.peerB.transport->Id());
    NetworkedHashTreePeer peer(h.client_, h, kTick);
    const Divergence d = Bisect(local, peer);
    EXPECT_EQ(d.kind, Divergence::Kind::Malformed);
    EXPECT_TRUE(peer.Failure() == BisectFailure::ConnectionFailed || peer.Failure() == BisectFailure::TimedOut);
    EXPECT_NE(peer.Failure(), BisectFailure::None);
}

TEST(BisectNetwork, ReplyToAnAskedTickItDoesNotHaveIsReported)
{
    HashTree local = World(kTick + 30, 100);
    Harness h(18, SimLinkConfig{}, World(kTick, 100), PatientClient());
    NetworkedHashTreePeer peer(h.client_, h, kTick + 30);
    const Divergence d = Bisect(local, peer);
    EXPECT_EQ(d.kind, Divergence::Kind::Malformed);
    EXPECT_EQ(peer.Failure(), BisectFailure::UnknownTick);
}
