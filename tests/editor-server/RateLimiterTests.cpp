#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <editor-server/RateLimiter.hpp>

using namespace N2Engine::Editor;
using std::chrono::milliseconds;

// The failed-Hello tables with an injected clock: nothing here sleeps or reads the real time, so every duration below
// (seconds, minutes, a quarter of an hour) is a number the test moves the clock by.

namespace
{
    class FakeClock
    {
    public:
        explicit FakeClock(const int64_t startMs = 0) : _ms(startMs) {}

        [[nodiscard]] RateLimiter::Clock Get()
        {
            return [this] { return RateLimiter::TimePoint(milliseconds(_ms)); };
        }

        void Advance(const milliseconds by) { _ms += by.count(); }

    private:
        int64_t _ms;
    };

    const PeerAddress Attacker = PeerAddress::IPv4(203, 0, 113, 7);
    const PeerAddress OtherPeer = PeerAddress::IPv4(198, 51, 100, 20);
    const PeerAddress Loopback = PeerAddress::IPv4(127, 0, 0, 1);

    std::array<uint8_t, 16> Ipv6(const std::initializer_list<uint8_t> head, const uint8_t last = 0)
    {
        std::array<uint8_t, 16> bytes{};
        size_t i = 0;
        for (const uint8_t byte : head)
        {
            bytes[i++] = byte;
        }
        bytes[15] = last;
        return bytes;
    }

    /// Fails `count` Hellos from a peer, returning the last outcome
    RateLimiter::FailureOutcome Fail(RateLimiter &limiter, const PeerAddress &peer, const int count,
                                     const std::optional<std::string_view> user = std::nullopt)
    {
        RateLimiter::FailureOutcome outcome;
        for (int i = 0; i < count; ++i)
        {
            outcome = limiter.RecordFailure(peer, user);
        }
        return outcome;
    }
}

// ==================== Addresses ====================

TEST(PeerAddressTest, IPv4AndIPv6AreSpelledCanonically)
{
    EXPECT_EQ(PeerAddress::IPv4(203, 0, 113, 7).ToString(), "203.0.113.7");
    EXPECT_EQ(PeerAddress::IPv6(Ipv6({0x20, 0x01, 0x0d, 0xb8}, 1)).ToString(), "2001:db8::1");
    EXPECT_EQ(PeerAddress::IPv6(Ipv6({}, 1)).ToString(), "::1");
    EXPECT_EQ(PeerAddress::IPv6(Ipv6({})).ToString(), "::");
    EXPECT_EQ(PeerAddress::IPv6(Ipv6({0xfe, 0x80})).ToString(), "fe80::");
    // One zero group is written out, not compressed
    EXPECT_EQ(PeerAddress::IPv6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5}).ToString(),
              "2001:db8:0:1:2:3:4:5");
    EXPECT_EQ(PeerAddress{}.ToString(), "unknown");
}

TEST(PeerAddressTest, AnIPv4MappedAddressIsTheIPv4Peer)
{
    std::array<uint8_t, 16> mapped{};
    mapped[10] = 0xFF;
    mapped[11] = 0xFF;
    mapped[12] = 192;
    mapped[13] = 0;
    mapped[14] = 2;
    mapped[15] = 9;
    const PeerAddress address = PeerAddress::IPv6(mapped);
    EXPECT_EQ(address.GetFamily(), PeerAddress::Family::IPv4);
    EXPECT_EQ(address.ToString(), "192.0.2.9");
    EXPECT_EQ(address, PeerAddress::IPv4(192, 0, 2, 9));

    // Not mapped when anything before the ffff is set
    mapped[0] = 1;
    EXPECT_EQ(PeerAddress::IPv6(mapped).GetFamily(), PeerAddress::Family::IPv6);
}

TEST(PeerAddressTest, LoopbackIsTheWholeOf127AndIPv6Loopback)
{
    EXPECT_TRUE(PeerAddress::IPv4(127, 0, 0, 1).IsLoopback());
    EXPECT_TRUE(PeerAddress::IPv4(127, 5, 6, 7).IsLoopback());
    EXPECT_TRUE(PeerAddress::IPv6(Ipv6({}, 1)).IsLoopback());
    // ::ffff:127.0.0.1 is the IPv4 loopback
    std::array<uint8_t, 16> mapped{};
    mapped[10] = 0xFF;
    mapped[11] = 0xFF;
    mapped[12] = 127;
    mapped[15] = 1;
    EXPECT_TRUE(PeerAddress::IPv6(mapped).IsLoopback());

    EXPECT_FALSE(PeerAddress::IPv4(128, 0, 0, 1).IsLoopback());
    EXPECT_FALSE(PeerAddress::IPv4(10, 0, 0, 1).IsLoopback());
    EXPECT_FALSE(PeerAddress::IPv6(Ipv6({}, 2)).IsLoopback());
    EXPECT_FALSE(PeerAddress::IPv6(Ipv6({0x20, 0x01, 0x0d, 0xb8}, 1)).IsLoopback());
    EXPECT_FALSE(PeerAddress{}.IsLoopback()) << "an unknown peer is not exempt";
}

// ==================== Per address: backoff ====================

TEST(RateLimiterTest, TheThirdFailedHelloStartsABackoffOfOneSecond)
{
    FakeClock clock(5000);
    RateLimiter limiter({}, clock.Get());

    EXPECT_FALSE(Fail(limiter, Attacker, 1).address.has_value());
    EXPECT_TRUE(limiter.AdmitConnection(Attacker).allowed);
    EXPECT_FALSE(Fail(limiter, Attacker, 1).address.has_value());
    EXPECT_TRUE(limiter.AdmitConnection(Attacker).allowed);

    const auto third = limiter.RecordFailure(Attacker, std::nullopt);
    ASSERT_TRUE(third.address.has_value());
    EXPECT_EQ(third.address->duration, milliseconds(1000));
    EXPECT_EQ(third.address->failures, 3u);

    const auto refused = limiter.AdmitConnection(Attacker);
    EXPECT_FALSE(refused.allowed);
    EXPECT_EQ(refused.retryAfter, milliseconds(1000));
    // Another address is not touched
    EXPECT_TRUE(limiter.AdmitConnection(OtherPeer).allowed);

    clock.Advance(milliseconds(999));
    EXPECT_FALSE(limiter.AdmitConnection(Attacker).allowed);
    clock.Advance(milliseconds(1));
    EXPECT_TRUE(limiter.AdmitConnection(Attacker).allowed);
}

TEST(RateLimiterTest, TheBackoffDoublesWithEachFurtherFailureAndStopsAtTheCap)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    Fail(limiter, Attacker, 2);

    const std::vector<int64_t> expected = {1000, 2000, 4000, 8000, 16000, 32000, 64000, 128000, 256000, 300000, 300000};
    for (const int64_t wanted : expected)
    {
        const auto outcome = limiter.RecordFailure(Attacker, std::nullopt);
        ASSERT_TRUE(outcome.address.has_value());
        EXPECT_EQ(outcome.address->duration.count(), wanted);
        // Served in full, then the next failure is the peer's next try
        clock.Advance(outcome.address->duration);
        EXPECT_TRUE(limiter.AdmitConnection(Attacker).allowed);
    }

    // A thousand more failures overflow nothing: still the cap
    for (int i = 0; i < 1000; ++i)
    {
        const auto outcome = limiter.RecordFailure(Attacker, std::nullopt);
        ASSERT_TRUE(outcome.address.has_value());
        EXPECT_EQ(outcome.address->duration, milliseconds(300000));
        clock.Advance(outcome.address->duration);
    }
}

TEST(RateLimiterTest, RefusedConnectionsDontExtendTheBackoffAndAreCountedOnce)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    Fail(limiter, Attacker, 3);

    // A hundred connections inside the window: each refused, none moves the end of the backoff
    for (int i = 0; i < 100; ++i)
    {
        clock.Advance(milliseconds(5));
        EXPECT_FALSE(limiter.AdmitConnection(Attacker).allowed);
    }
    clock.Advance(milliseconds(500));
    const auto after = limiter.AdmitConnection(Attacker);
    ASSERT_TRUE(after.allowed);
    EXPECT_EQ(after.suppressed, 100u) << "reported once, for one log line, not one per connection";
    EXPECT_EQ(limiter.AdmitConnection(Attacker).suppressed, 0u);
}

TEST(RateLimiterTest, ASuccessClearsTheAddress)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    Fail(limiter, Attacker, 2);
    EXPECT_EQ(limiter.AddressCount(), 1u);
    limiter.RecordSuccess(Attacker, std::nullopt);
    EXPECT_EQ(limiter.AddressCount(), 0u);

    // Two more failures are again within the free ones
    EXPECT_FALSE(Fail(limiter, Attacker, 2).address.has_value());
    EXPECT_TRUE(Fail(limiter, Attacker, 1).address.has_value());

    // After a ban served, a success resets the doubling too
    clock.Advance(milliseconds(1000));
    ASSERT_TRUE(limiter.AdmitConnection(Attacker).allowed);
    limiter.RecordSuccess(Attacker, std::nullopt);
    Fail(limiter, Attacker, 2);
    EXPECT_EQ(limiter.RecordFailure(Attacker, std::nullopt).address->duration, milliseconds(1000));
}

TEST(RateLimiterTest, AnAddressIsForgottenAfterFifteenIdleMinutes)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    Fail(limiter, Attacker, 2);

    clock.Advance(milliseconds(15 * 60 * 1000 - 1));
    // One short of idle: the failures still count, so the next one starts a backoff
    EXPECT_TRUE(Fail(limiter, Attacker, 1).address.has_value());

    FakeClock second;
    RateLimiter other({}, second.Get());
    Fail(other, Attacker, 2);
    second.Advance(milliseconds(15 * 60 * 1000));
    EXPECT_FALSE(other.RecordFailure(Attacker, std::nullopt).address.has_value()) << "idle: counted from one again";
    EXPECT_EQ(other.AddressCount(), 1u);
}

TEST(RateLimiterTest, ABackoffIsNeverForgottenBeforeItEnds)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    Fail(limiter, Attacker, 12); // far into the doubling: the 5 minute cap
    EXPECT_FALSE(limiter.AdmitConnection(Attacker).allowed);
    clock.Advance(milliseconds(4 * 60 * 1000));
    EXPECT_FALSE(limiter.AdmitConnection(Attacker).allowed);
    clock.Advance(milliseconds(60 * 1000));
    EXPECT_TRUE(limiter.AdmitConnection(Attacker).allowed);
}

TEST(RateLimiterTest, AnAddressWithFailuresOnRecordIsAPriorOffender)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    EXPECT_FALSE(limiter.AdmitConnection(Attacker).priorOffender);
    EXPECT_EQ(limiter.Policy().priorOffenderBudget, milliseconds(2000));

    Fail(limiter, Attacker, 1);
    const auto admission = limiter.AdmitConnection(Attacker);
    EXPECT_TRUE(admission.allowed) << "below the backoff it is still served, on the short budget";
    EXPECT_TRUE(admission.priorOffender);
    EXPECT_FALSE(limiter.AdmitConnection(OtherPeer).priorOffender);

    limiter.RecordSuccess(Attacker, std::nullopt);
    EXPECT_FALSE(limiter.AdmitConnection(Attacker).priorOffender);
}

// ==================== Per address: the table is bounded ====================

TEST(RateLimiterTest, TheTableNeverGrowsPastItsCap)
{
    FakeClock clock;
    RateLimitPolicy policy;
    policy.maxEntries = 100;
    RateLimiter limiter(policy, clock.Get());

    for (int i = 0; i < 10000; ++i)
    {
        clock.Advance(milliseconds(1));
        const auto peer = PeerAddress::IPv4(10, static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i & 0xFF), 1);
        limiter.RecordFailure(peer, std::nullopt);
        ASSERT_LE(limiter.AddressCount(), 100u);
    }
    EXPECT_EQ(limiter.AddressCount(), 100u);
}

TEST(RateLimiterTest, TheOldestEntryIsEvictedAtTheCap)
{
    FakeClock clock;
    RateLimitPolicy policy;
    policy.maxEntries = 3;
    RateLimiter limiter(policy, clock.Get());

    const PeerAddress a = PeerAddress::IPv4(10, 0, 0, 1);
    const PeerAddress b = PeerAddress::IPv4(10, 0, 0, 2);
    const PeerAddress c = PeerAddress::IPv4(10, 0, 0, 3);
    const PeerAddress d = PeerAddress::IPv4(10, 0, 0, 4);
    for (const PeerAddress &peer : {a, b, c})
    {
        clock.Advance(milliseconds(10));
        limiter.RecordFailure(peer, std::nullopt);
    }
    ASSERT_TRUE(limiter.AdmitConnection(a).priorOffender);

    clock.Advance(milliseconds(10));
    limiter.RecordFailure(d, std::nullopt);
    EXPECT_EQ(limiter.AddressCount(), 3u);
    EXPECT_FALSE(limiter.AdmitConnection(a).priorOffender) << "a, the oldest, was forgotten";
    EXPECT_TRUE(limiter.AdmitConnection(b).priorOffender);
    EXPECT_TRUE(limiter.AdmitConnection(c).priorOffender);
    EXPECT_TRUE(limiter.AdmitConnection(d).priorOffender);
}

TEST(RateLimiterTest, AnAddressServingABackoffOutlastsOlderQuietOnes)
{
    FakeClock clock;
    RateLimitPolicy policy;
    policy.maxEntries = 3;
    RateLimiter limiter(policy, clock.Get());

    const PeerAddress banned = PeerAddress::IPv4(10, 0, 0, 1);
    Fail(limiter, banned, 3);
    for (const uint8_t host : {2, 3})
    {
        clock.Advance(milliseconds(10));
        limiter.RecordFailure(PeerAddress::IPv4(10, 0, 0, host), std::nullopt);
    }
    // The table is full; the banned one is the oldest, but a quiet one goes first
    clock.Advance(milliseconds(10));
    limiter.RecordFailure(PeerAddress::IPv4(10, 0, 0, 4), std::nullopt);
    EXPECT_EQ(limiter.AddressCount(), 3u);
    EXPECT_FALSE(limiter.AdmitConnection(banned).allowed);
    EXPECT_FALSE(limiter.AdmitConnection(PeerAddress::IPv4(10, 0, 0, 2)).priorOffender);
}

TEST(RateLimiterTest, IdleEntriesAreForgottenBeforeAnythingLiveIsEvicted)
{
    FakeClock clock;
    RateLimitPolicy policy;
    policy.maxEntries = 2;
    RateLimiter limiter(policy, clock.Get());

    const PeerAddress stale = PeerAddress::IPv4(10, 0, 0, 1);
    const PeerAddress live = PeerAddress::IPv4(10, 0, 0, 2);
    limiter.RecordFailure(stale, std::nullopt);
    clock.Advance(milliseconds(14 * 60 * 1000));
    limiter.RecordFailure(live, std::nullopt);
    clock.Advance(milliseconds(2 * 60 * 1000)); // stale is now over fifteen minutes quiet, live is two

    limiter.RecordFailure(PeerAddress::IPv4(10, 0, 0, 3), std::nullopt);
    EXPECT_EQ(limiter.AddressCount(), 2u);
    EXPECT_TRUE(limiter.AdmitConnection(live).priorOffender);
    EXPECT_FALSE(limiter.AdmitConnection(stale).priorOffender);
}

// ==================== Loopback ====================

TEST(RateLimiterTest, LoopbackIsExemptFromTheAddressRule)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());

    // More failures than an address is allowed
    for (int i = 0; i < 9; ++i)
    {
        const auto outcome = limiter.RecordFailure(Loopback, std::nullopt);
        EXPECT_FALSE(outcome.address.has_value());
        EXPECT_FALSE(outcome.user.has_value());
        EXPECT_TRUE(limiter.AdmitConnection(Loopback).allowed);
        EXPECT_FALSE(limiter.AdmitConnection(Loopback).priorOffender) << "no short budget for a tunnel";
    }
    EXPECT_EQ(limiter.AddressCount(), 0u) << "no per-address entry for loopback";
}

// PR S-4: the shared loopback counter, which refused every loopback connection after ten failed Hellos in a minute
// (a client with the right token included, and everybody behind a proxy), is gone. Loopback is bounded by the slots and
// the handshake rate (PreAuthServerTests), not by a count of failures.
TEST(RateLimiterTest, FailuresFromLoopbackNeverRefuseLoopback)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());

    // Far more than the ten in a minute that used to start the global backoff, over every spelling of loopback
    for (int i = 0; i < 300; ++i)
    {
        const PeerAddress peer = i % 3 == 0   ? Loopback
                                 : i % 3 == 1 ? PeerAddress::IPv4(127, 9, 9, 9)
                                              : PeerAddress::IPv6(Ipv6({}, 1));
        const auto outcome = limiter.RecordFailure(peer, std::nullopt);
        ASSERT_FALSE(outcome.address.has_value()) << i;
        ASSERT_FALSE(outcome.user.has_value()) << i;
        ASSERT_TRUE(limiter.AdmitConnection(peer).allowed) << i;
        clock.Advance(milliseconds(10));
    }
    EXPECT_EQ(limiter.AddressCount(), 0u);
    EXPECT_EQ(limiter.UserCount(), 0u);
    EXPECT_TRUE(limiter.AdmitConnection(OtherPeer).allowed);
}

TEST(RateLimiterTest, LoopbackFailuresThatNameAUserStillBackThatUserOffButNotLoopback)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());

    // Wrong tokens naming alice through a tunnel: the user's backoff (short, capped) is what holds an attacker there
    Fail(limiter, Loopback, 2, "alice");
    const auto third = limiter.RecordFailure(Loopback, "alice");
    ASSERT_TRUE(third.user.has_value());
    EXPECT_TRUE(limiter.IsUserBackedOff("alice"));
    EXPECT_FALSE(limiter.IsUserBackedOff("bob"));
    // ... and every loopback connection is still admitted, so bob (and alice, whose right token passes the user's
    // backoff in the server) get in
    EXPECT_TRUE(limiter.AdmitConnection(Loopback).allowed);
    EXPECT_FALSE(third.address.has_value());
}

TEST(RateLimiterTest, AnUnknownPeerIsAnAddressLikeAnyOther)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    EXPECT_TRUE(Fail(limiter, PeerAddress{}, 3).address.has_value());
    EXPECT_FALSE(limiter.AdmitConnection(PeerAddress{}).allowed);
    EXPECT_TRUE(limiter.AdmitConnection(OtherPeer).allowed);
}

// ==================== Per user ====================

TEST(RateLimiterTest, AUserIsBackedOffWhereverTheFailuresCameFrom)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());

    // Three different peers, one of them loopback, each name alice once or twice
    EXPECT_FALSE(limiter.RecordFailure(Attacker, "alice").user.has_value());
    EXPECT_FALSE(limiter.RecordFailure(OtherPeer, "alice").user.has_value());
    EXPECT_FALSE(limiter.IsUserBackedOff("alice"));
    const auto third = limiter.RecordFailure(Loopback, "alice");
    ASSERT_TRUE(third.user.has_value());
    EXPECT_EQ(third.user->duration, milliseconds(1000));
    EXPECT_TRUE(limiter.IsUserBackedOff("alice"));
    EXPECT_FALSE(limiter.IsUserBackedOff("bob"));
    EXPECT_FALSE(limiter.IsUserBackedOff("Alice")) << "names are case-sensitive, as in the tokens file";

    // Only the user is backed off here: loopback is served, so a tunnel's other users get in
    EXPECT_TRUE(limiter.AdmitConnection(Loopback).allowed);

    clock.Advance(milliseconds(1000));
    EXPECT_FALSE(limiter.IsUserBackedOff("alice"));
}

TEST(RateLimiterTest, AUserBackoffIsCappedShort)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    Fail(limiter, Loopback, 2, "alice");

    const std::vector<int64_t> expected = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
    for (const int64_t wanted : expected)
    {
        const auto outcome = limiter.RecordFailure(Loopback, "alice");
        ASSERT_TRUE(outcome.user.has_value());
        EXPECT_EQ(outcome.user->duration.count(), wanted);
        clock.Advance(outcome.user->duration);
        EXPECT_FALSE(limiter.IsUserBackedOff("alice"));
    }
    for (int i = 0; i < 1000; ++i)
    {
        const auto outcome = limiter.RecordFailure(Loopback, "alice");
        ASSERT_TRUE(outcome.user.has_value());
        EXPECT_LE(outcome.user->duration, milliseconds(30000));
        clock.Advance(outcome.user->duration);
    }
}

TEST(RateLimiterTest, AUsersSuccessClearsTheUserAndAnAnonymousFailureNamesNobody)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    Fail(limiter, Attacker, 2, "alice");
    EXPECT_EQ(limiter.UserCount(), 1u);
    limiter.RecordSuccess(OtherPeer, "alice");
    EXPECT_EQ(limiter.UserCount(), 0u);
    EXPECT_FALSE(Fail(limiter, Attacker, 1, "alice").user.has_value()) << "counted from one again";

    // A Hello that named no (valid) user, or an empty one, has no user to count
    const size_t users = limiter.UserCount();
    limiter.RecordFailure(Attacker, std::nullopt);
    limiter.RecordFailure(Attacker, "");
    EXPECT_EQ(limiter.UserCount(), users);
}

TEST(RateLimiterTest, AUserIsForgottenAfterFifteenIdleMinutesAndTheTableIsBounded)
{
    FakeClock clock;
    RateLimitPolicy policy;
    policy.maxEntries = 5;
    RateLimiter limiter(policy, clock.Get());

    Fail(limiter, OtherPeer, 2, "alice");
    clock.Advance(milliseconds(15 * 60 * 1000));
    EXPECT_FALSE(limiter.RecordFailure(OtherPeer, "alice").user.has_value()) << "idle: counted from one again";

    for (int i = 0; i < 1000; ++i)
    {
        limiter.RecordFailure(OtherPeer, "user" + std::to_string(i));
        ASSERT_LE(limiter.UserCount(), 5u);
    }
}

TEST(RateLimiterTest, AFailureCanStartTheAddressAndTheUserBackoffTogether)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    Fail(limiter, Attacker, 2, "alice");
    const auto outcome = limiter.RecordFailure(Attacker, "alice");
    ASSERT_TRUE(outcome.address.has_value());
    ASSERT_TRUE(outcome.user.has_value());
}

TEST(RateLimiterTest, AUserBackoffIsNotAFailureAndDoesNotChangeTheAddress)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());
    Fail(limiter, Loopback, 3, "alice");
    ASSERT_TRUE(limiter.IsUserBackedOff("alice"));

    // Asking whether the user is backed off counts nothing: no failure is recorded, so the backoff ends on time
    for (int i = 0; i < 50; ++i)
    {
        EXPECT_TRUE(limiter.IsUserBackedOff("alice"));
    }
    clock.Advance(milliseconds(1000));
    EXPECT_FALSE(limiter.IsUserBackedOff("alice"));
}

// ==================== Policy ====================

// ==================== A connection that never finished a Hello ====================

TEST(RateLimiterTest, AnUnfinishedHelloCountsLikeAFailedHelloForItsAddress)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());

    // Two are free, the third starts the same 1 s backoff a wrong token would, naming no user
    EXPECT_FALSE(limiter.RecordUnfinishedHello(Attacker).address.has_value());
    EXPECT_FALSE(limiter.RecordUnfinishedHello(Attacker).address.has_value());
    const auto third = limiter.RecordUnfinishedHello(Attacker);
    ASSERT_TRUE(third.address.has_value());
    EXPECT_EQ(third.address->duration, milliseconds(1000));
    EXPECT_EQ(third.address->failures, 3u);
    EXPECT_FALSE(third.user.has_value());
    EXPECT_EQ(limiter.UserCount(), 0u);

    EXPECT_FALSE(limiter.AdmitConnection(Attacker).allowed);
    EXPECT_TRUE(limiter.AdmitConnection(OtherPeer).allowed);

    // And it adds to what wrong tokens did: one more failure of either kind doubles the backoff
    clock.Advance(milliseconds(1000));
    const auto wrongToken = limiter.RecordFailure(Attacker, std::nullopt);
    ASSERT_TRUE(wrongToken.address.has_value());
    EXPECT_EQ(wrongToken.address->duration, milliseconds(2000));
    EXPECT_EQ(wrongToken.address->failures, 4u);
}

TEST(RateLimiterTest, AnUnfinishedHelloFromLoopbackIsNotRecordedAtAll)
{
    FakeClock clock;
    RateLimiter limiter({}, clock.Get());

    // Behind a tunnel or a proxy every peer is loopback: nothing is recorded for a mere connect, however many there are
    for (int i = 0; i < 50; ++i)
    {
        const auto outcome = limiter.RecordUnfinishedHello(i % 2 == 0 ? Loopback : PeerAddress::IPv4(127, 7, 7, 7));
        EXPECT_FALSE(outcome.address.has_value());
        EXPECT_FALSE(outcome.user.has_value());
    }
    EXPECT_TRUE(limiter.AdmitConnection(Loopback).allowed);
    EXPECT_EQ(limiter.AddressCount(), 0u);

    // ... and neither does a wrong token from loopback: there is no shared loopback counter to feed (PR S-4)
    Fail(limiter, Loopback, 30);
    EXPECT_TRUE(limiter.AdmitConnection(Loopback).allowed);
}

TEST(RateLimiterTest, ThePolicyIsTheDocumentedOne)
{
    const RateLimitPolicy policy;
    EXPECT_EQ(policy.freeFailures, 2u) << "the third failed Hello starts the backoff";
    EXPECT_EQ(policy.initialBackoff, milliseconds(1000));
    EXPECT_EQ(policy.maxBackoff, milliseconds(5 * 60 * 1000));
    EXPECT_EQ(policy.idleReset, milliseconds(15 * 60 * 1000));
    EXPECT_EQ(policy.maxEntries, 4096u);
    EXPECT_EQ(policy.userMaxBackoff, milliseconds(30 * 1000));
    EXPECT_EQ(policy.priorOffenderBudget, milliseconds(2000));
}

TEST(RateLimiterTest, WithoutAnInjectedClockItUsesTheSteadyClock)
{
    RateLimiter limiter;
    Fail(limiter, Attacker, 3);
    // No sleeping: a backoff of a second has certainly not ended, and nothing else is asserted about real time
    const auto admission = limiter.AdmitConnection(Attacker);
    EXPECT_FALSE(admission.allowed);
    EXPECT_GT(admission.retryAfter, milliseconds(0));
}

// ---- thread safety: every public method is locked (no sanitizer leg runs these; a data race would show as a crash, a
// corrupt table or a wrong count)

TEST(RateLimiterThreadingTest, ManyThreadsHammeringTheLimiterKeepItConsistent)
{
    RateLimitPolicy policy;
    policy.maxEntries = 64; // small, so the eviction paths run too
    RateLimiter limiter(policy, [] { return RateLimiter::TimePoint(milliseconds(1000)); });

    constexpr int ThreadCount = 8;
    constexpr int Rounds = 3000;
    std::vector<std::thread> threads;
    for (int t = 0; t < ThreadCount; ++t)
    {
        threads.emplace_back(
            [&limiter, t]
            {
                for (int i = 0; i < Rounds; ++i)
                {
                    const uint8_t host = static_cast<uint8_t>((t * 31 + i) % 250);
                    const PeerAddress peer = (i % 17 == 0) ? PeerAddress::IPv4(127, 0, 0, 1)
                                                           : PeerAddress::IPv4(203, 0, static_cast<uint8_t>(i % 7), host);
                    const std::string user = "user" + std::to_string((t + i) % 90);
                    switch (i % 5)
                    {
                    case 0:
                        (void)limiter.AdmitConnection(peer);
                        break;
                    case 1:
                        (void)limiter.RecordFailure(peer, std::string_view(user));
                        break;
                    case 2:
                        (void)limiter.IsUserBackedOff(user);
                        break;
                    case 3:
                        (void)limiter.RecordUnfinishedHello(peer);
                        break;
                    default:
                        limiter.RecordSuccess(peer, std::string_view(user));
                        (void)limiter.AddressCount();
                        (void)limiter.UserCount();
                        break;
                    }
                }
            });
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    EXPECT_LE(limiter.AddressCount(), policy.maxEntries);
    EXPECT_LE(limiter.UserCount(), policy.maxEntries);
}

TEST(RateLimiterThreadingTest, ConcurrentFailuresAreAllCounted)
{
    RateLimiter limiter({}, [] { return RateLimiter::TimePoint(milliseconds(1000)); });

    constexpr int ThreadCount = 8;
    constexpr int Failures = 250;
    std::atomic<int> backoffsStarted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < ThreadCount; ++t)
    {
        threads.emplace_back(
            [&limiter, &backoffsStarted, t]
            {
                // Different addresses (so no address rule interferes), one user: its failures are counted wherever they
                // come from
                const PeerAddress peer = PeerAddress::IPv4(203, 0, 113, static_cast<uint8_t>(t + 1));
                for (int i = 0; i < Failures; ++i)
                {
                    if (limiter.RecordFailure(peer, std::string_view("alice")).user.has_value())
                    {
                        ++backoffsStarted;
                    }
                }
            });
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    // Two failures are free, every one after starts a backoff: a lost update would leave a gap
    EXPECT_EQ(backoffsStarted.load(), ThreadCount * Failures - static_cast<int>(RateLimitPolicy{}.userFreeFailures));
    EXPECT_TRUE(limiter.IsUserBackedOff("alice"));
}
