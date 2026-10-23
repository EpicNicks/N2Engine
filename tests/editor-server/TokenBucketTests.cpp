#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

#include <editor-server/TokenBucket.hpp>

using namespace N2Engine::Editor;
using std::chrono::hours;
using std::chrono::milliseconds;

// The handshake token buckets with an injected clock: nothing here sleeps or reads the real time.

namespace
{
    class FakeClock
    {
    public:
        explicit FakeClock(const int64_t startMs = 0) : _ms(startMs) {}

        [[nodiscard]] TokenBucket::Clock Get()
        {
            return [this] { return TokenBucket::TimePoint(milliseconds(_ms)); };
        }

        void Advance(const milliseconds by) { _ms += by.count(); }
        void Set(const int64_t ms) { _ms = ms; }

    private:
        int64_t _ms;
    };

    PeerAddress Remote(const uint8_t last)
    {
        return PeerAddress::IPv4(203, 0, 113, last);
    }

    PeerAddress Remote2(const uint8_t third, const uint8_t last)
    {
        return PeerAddress::IPv4(198, 51, third, last);
    }

    const PeerAddress Loopback = PeerAddress::IPv4(127, 0, 0, 1);

    /// How many TryTake calls succeed in a row, up to `limit`
    int TakeUntilRefused(TokenBucket &bucket, const PeerAddress &peer, const int limit)
    {
        int taken = 0;
        while (taken < limit && bucket.TryTake(peer))
        {
            ++taken;
        }
        return taken;
    }
}

TEST(TokenBucketTest, DefaultsAre20PerSecondGlobalAnd5PerAddress)
{
    const TokenBucket bucket;
    EXPECT_EQ(bucket.Options().globalPerSecond, 20u);
    EXPECT_EQ(bucket.Options().perAddressPerSecond, 5u);
    EXPECT_EQ(bucket.Options().globalBurst, 20u) << "a burst of 0 means one second's worth";
    EXPECT_EQ(bucket.Options().perAddressBurst, 5u);
}

TEST(TokenBucketTest, ZeroRatesAreClampedToOne)
{
    TokenBucketOptions options;
    options.globalPerSecond = 0;
    options.perAddressPerSecond = 0;
    options.maxAddresses = 0;
    const TokenBucket bucket(options);
    EXPECT_EQ(bucket.Options().globalPerSecond, 1u);
    EXPECT_EQ(bucket.Options().perAddressPerSecond, 1u);
    EXPECT_EQ(bucket.Options().maxAddresses, 1u);
}

TEST(TokenBucketTest, AnAddressMayBurstFiveThenIsRefusedAndRefillsAtFivePerSecond)
{
    FakeClock clock;
    TokenBucket bucket({}, clock.Get());

    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 100), 5);
    EXPECT_EQ(bucket.TotalCounters().refusedAddress, 1u);

    clock.Advance(milliseconds(199));
    EXPECT_FALSE(bucket.TryTake(Remote(1))) << "5 a second is a token every 200 ms";
    clock.Advance(milliseconds(1));
    EXPECT_TRUE(bucket.TryTake(Remote(1)));
    EXPECT_FALSE(bucket.TryTake(Remote(1)));

    // A long quiet spell refills to the burst and no further
    clock.Advance(milliseconds(60'000));
    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 100), 5);
}

TEST(TokenBucketTest, AddressesAreIsolatedFromEachOther)
{
    FakeClock clock;
    TokenBucket bucket({}, clock.Get());

    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 100), 5);
    EXPECT_EQ(TakeUntilRefused(bucket, Remote(2), 100), 5) << "another address has its own bucket";
    EXPECT_FALSE(bucket.TryTake(Remote(1)));
}

TEST(TokenBucketTest, TheGlobalBucketLimitsAllAddressesTogether)
{
    FakeClock clock;
    TokenBucket bucket({}, clock.Get());

    // 20 different addresses, one handshake each: all fit in the global burst
    for (uint8_t i = 1; i <= 20; ++i)
    {
        EXPECT_TRUE(bucket.TryTake(Remote2(1, i))) << static_cast<int>(i);
    }
    EXPECT_FALSE(bucket.TryTake(Remote2(1, 21))) << "the 21st in the same instant";
    EXPECT_EQ(bucket.TotalCounters().refusedGlobal, 1u);

    // 20 a second: a token every 50 ms
    clock.Advance(milliseconds(49));
    EXPECT_FALSE(bucket.TryTake(Remote2(1, 22)));
    clock.Advance(milliseconds(1));
    EXPECT_TRUE(bucket.TryTake(Remote2(1, 23)));
    EXPECT_FALSE(bucket.TryTake(Remote2(1, 24)));
}

TEST(TokenBucketTest, AnAddressOutOfTokensDoesNotDrainTheGlobalBucket)
{
    FakeClock clock;
    TokenBucket bucket({}, clock.Get());

    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 5), 5);
    // The attacker keeps trying from its one address
    for (int i = 0; i < 1000; ++i)
    {
        EXPECT_FALSE(bucket.TryTake(Remote(1)));
    }
    // 15 of the 20 global tokens are left for everybody else
    int others = 0;
    for (uint8_t i = 1; i <= 30; ++i)
    {
        if (bucket.TryTake(Remote2(2, i)))
            ++others;
    }
    EXPECT_EQ(others, 15);
    EXPECT_EQ(bucket.TotalCounters().refusedAddress, 1000u);
}

TEST(TokenBucketTest, LoopbackIsHeldToTheGlobalBucketOnly)
{
    FakeClock clock;
    TokenBucket bucket({}, clock.Get());

    EXPECT_EQ(TakeUntilRefused(bucket, Loopback, 100), 20) << "no per-address bucket: the whole global burst";
    EXPECT_EQ(bucket.AddressCount(), 0u);
    EXPECT_EQ(bucket.TotalCounters().refusedAddress, 0u);
    EXPECT_EQ(bucket.TotalCounters().refusedGlobal, 1u);
}

TEST(TokenBucketTest, CustomRatesAndBursts)
{
    FakeClock clock;
    TokenBucketOptions options;
    options.globalPerSecond = 2;
    options.globalBurst = 10;
    options.perAddressPerSecond = 1;
    options.perAddressBurst = 3;
    TokenBucket bucket(options, clock.Get());

    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 100), 3);
    clock.Advance(milliseconds(999));
    EXPECT_FALSE(bucket.TryTake(Remote(1)));
    clock.Advance(milliseconds(1));
    EXPECT_TRUE(bucket.TryTake(Remote(1)));
}

TEST(TokenBucketTest, ThePerAddressTableIsBounded)
{
    FakeClock clock;
    TokenBucketOptions options;
    options.maxAddresses = 8;
    options.globalPerSecond = 1'000'000;
    options.globalBurst = 1000;
    TokenBucket bucket(options, clock.Get());

    // An attacker spraying addresses (each use leaves a bucket that has not refilled) cannot grow the table
    for (int i = 0; i < 500; ++i)
    {
        ASSERT_TRUE(bucket.TryTake(Remote2(static_cast<uint8_t>(i / 200), static_cast<uint8_t>(i % 200)))) << i;
        EXPECT_LE(bucket.AddressCount(), 8u) << i;
    }
    EXPECT_EQ(bucket.AddressCount(), 8u);
}

TEST(TokenBucketTest, BucketsThatHaveRefilledAreForgotten)
{
    FakeClock clock;
    TokenBucketOptions options;
    options.maxAddresses = 8;
    options.globalPerSecond = 1000;
    TokenBucket bucket(options, clock.Get());

    for (uint8_t i = 1; i <= 8; ++i)
    {
        ASSERT_TRUE(bucket.TryTake(Remote(i)));
    }
    EXPECT_EQ(bucket.AddressCount(), 8u);

    // A second later every one of them is full again, i.e. the same as unknown: making room drops them all
    clock.Advance(milliseconds(1000));
    ASSERT_TRUE(bucket.TryTake(Remote(100)));
    EXPECT_EQ(bucket.AddressCount(), 1u);
}

TEST(TokenBucketTest, WhenTheTableIsFullTheLeastRecentlyUsedGoes)
{
    FakeClock clock;
    TokenBucketOptions options;
    options.maxAddresses = 2;
    options.globalPerSecond = 1000;
    TokenBucket bucket(options, clock.Get());

    // Remote(1) is drained (and so is not forgotten as refilled), then Remote(2) used a little later
    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 100), 5);
    clock.Advance(milliseconds(10));
    ASSERT_TRUE(bucket.TryTake(Remote(2)));
    clock.Advance(milliseconds(10));

    // A third address: the least recently used (Remote(1)) is evicted, so it starts again with a full bucket
    ASSERT_TRUE(bucket.TryTake(Remote(3)));
    EXPECT_EQ(bucket.AddressCount(), 2u);
    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 100), 5) << "evicted, so it has a fresh bucket (the bound's price)";
}

TEST(TokenBucketTest, AClockGoingBackwardsAddsNothingAndDoesNotStallTheBucket)
{
    FakeClock clock(100'000);
    TokenBucket bucket({}, clock.Get());

    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 100), 5);

    clock.Set(0); // 100 s into the past
    EXPECT_FALSE(bucket.TryTake(Remote(1))) << "no time has passed, so no refill";
    // The bucket is anchored at the new time: 200 ms later one token is back (not 100 s later)
    clock.Set(200);
    EXPECT_TRUE(bucket.TryTake(Remote(1)));
    EXPECT_FALSE(bucket.TryTake(Remote(1)));
}

TEST(TokenBucketTest, AHugeForwardJumpDoesNotOverflowOrOverfill)
{
    FakeClock clock;
    TokenBucket bucket({}, clock.Get());

    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 100), 5);
    clock.Advance(hours(24 * 365 * 100)); // a century
    EXPECT_EQ(TakeUntilRefused(bucket, Remote(1), 100), 5) << "full, not more than full";
}

TEST(TokenBucketTest, CallsInTheSameMillisecondAddNothing)
{
    FakeClock clock;
    TokenBucketOptions options;
    options.perAddressPerSecond = 1000; // a token per millisecond
    options.perAddressBurst = 1;
    options.globalPerSecond = 100000;
    TokenBucket bucket(options, clock.Get());

    ASSERT_TRUE(bucket.TryTake(Remote(1)));
    EXPECT_FALSE(bucket.TryTake(Remote(1)));
    // Another call in the same millisecond changes nothing
    EXPECT_FALSE(bucket.TryTake(Remote(1)));
    clock.Advance(milliseconds(1));
    EXPECT_TRUE(bucket.TryTake(Remote(1)));
}
