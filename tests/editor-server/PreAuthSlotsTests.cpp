#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include <editor-server/PreAuthSlots.hpp>

using namespace N2Engine::Editor;
using std::chrono::hours;
using std::chrono::milliseconds;

// The pre-auth slot table with an injected clock: nothing here sleeps or reads the real time.

namespace
{
    class FakeClock
    {
    public:
        [[nodiscard]] PreAuthSlots::Clock Get()
        {
            return [this] { return PreAuthSlots::TimePoint(milliseconds(_ms)); };
        }

        void Advance(const milliseconds by) { _ms += by.count(); }
        void Set(const int64_t ms) { _ms = ms; }

    private:
        int64_t _ms = 0;
    };

    PeerAddress Remote(const uint8_t last)
    {
        return PeerAddress::IPv4(203, 0, 113, last);
    }

    const PeerAddress Loopback = PeerAddress::IPv4(127, 0, 0, 1);

    PreAuthSlotsOptions Slots(const size_t total, const size_t perAddress = 2)
    {
        PreAuthSlotsOptions options;
        options.totalSlots = total;
        options.perAddressSlots = perAddress;
        return options;
    }
}

TEST(PreAuthSlotsTest, DefaultsAreFourSlotsAndTwoPerAddress)
{
    const PreAuthSlots slots;
    EXPECT_EQ(slots.Options().totalSlots, 4u);
    EXPECT_EQ(slots.Options().perAddressSlots, 2u);
    EXPECT_FALSE(slots.Options().reserveForRecentlyAuthenticated) << "the reserved slot is off unless asked for";
}

TEST(PreAuthSlotsTest, TheTotalIsClampedToOneToSixteen)
{
    EXPECT_EQ(PreAuthSlots(Slots(0)).Options().totalSlots, 1u);
    EXPECT_EQ(PreAuthSlots(Slots(100)).Options().totalSlots, 16u);
    EXPECT_EQ(PreAuthSlots(Slots(8)).Options().totalSlots, 8u);
    EXPECT_EQ(PreAuthSlots(Slots(4, 0)).Options().perAddressSlots, 1u);
}

TEST(PreAuthSlotsTest, TheTotalCapRefusesTheNextConnection)
{
    FakeClock clock;
    PreAuthSlots slots(Slots(4), clock.Get());

    std::vector<PreAuthSlots::Handle> held;
    for (uint8_t i = 1; i <= 4; ++i)
    {
        PreAuthSlots::Handle handle = slots.TryAcquire(Remote(i)); // four different addresses
        ASSERT_TRUE(handle) << static_cast<int>(i);
        held.push_back(std::move(handle));
    }
    EXPECT_EQ(slots.InUse(), 4u);

    const PreAuthSlots::Handle refused = slots.TryAcquire(Remote(9));
    EXPECT_FALSE(refused) << "full: the caller closes the connection at once";
    EXPECT_EQ(slots.InUse(), 4u);
    EXPECT_EQ(slots.TotalCounters().refusedFull, 1u);
    EXPECT_EQ(slots.TotalCounters().refusedPerAddress, 0u);
    EXPECT_EQ(slots.TotalCounters().admitted, 4u);
}

TEST(PreAuthSlotsTest, OneAddressHoldsAtMostTwo)
{
    FakeClock clock;
    PreAuthSlots slots(Slots(8), clock.Get());

    PreAuthSlots::Handle a = slots.TryAcquire(Remote(1));
    PreAuthSlots::Handle b = slots.TryAcquire(Remote(1));
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(slots.InUse(Remote(1)), 2u);

    EXPECT_FALSE(slots.TryAcquire(Remote(1))) << "a third from the same address";
    EXPECT_EQ(slots.TotalCounters().refusedPerAddress, 1u);
    EXPECT_EQ(slots.TotalCounters().refusedFull, 0u);

    // Another address is not affected
    const PreAuthSlots::Handle other = slots.TryAcquire(Remote(2));
    EXPECT_TRUE(other);
    EXPECT_EQ(slots.InUse(Remote(2)), 1u);

    // Ten sockets from one address: two are admitted
    PreAuthSlots tenSlots(Slots(8), clock.Get());
    std::vector<PreAuthSlots::Handle> held;
    for (int i = 0; i < 10; ++i)
    {
        PreAuthSlots::Handle handle = tenSlots.TryAcquire(Remote(7));
        if (handle)
            held.push_back(std::move(handle));
    }
    EXPECT_EQ(held.size(), 2u);
}

TEST(PreAuthSlotsTest, LoopbackHasNoPerAddressCapAndMayUseEverySlot)
{
    FakeClock clock;
    PreAuthSlots slots(Slots(4), clock.Get());

    std::vector<PreAuthSlots::Handle> held;
    for (int i = 0; i < 4; ++i)
    {
        PreAuthSlots::Handle handle = slots.TryAcquire(Loopback);
        ASSERT_TRUE(handle) << i;
        held.push_back(std::move(handle));
    }
    EXPECT_EQ(slots.InUse(), 4u);
    EXPECT_EQ(slots.InUse(Loopback), 0u) << "loopback has no per-address count";
    EXPECT_FALSE(slots.TryAcquire(Loopback)) << "but the total still holds";
    EXPECT_FALSE(slots.TryAcquire(Remote(1)));
    EXPECT_EQ(slots.TotalCounters().refusedFull, 2u);
    EXPECT_EQ(slots.TotalCounters().refusedPerAddress, 0u);

    // ::1 and 127.0.0.2 are loopback too
    PreAuthSlots other(Slots(4), clock.Get());
    std::array<uint8_t, 16> v6{};
    v6[15] = 1;
    EXPECT_TRUE(other.TryAcquire(PeerAddress::IPv6(v6)));
}

TEST(PreAuthSlotsTest, ReleasingFreesTheSlotAndTheAddressShare)
{
    FakeClock clock;
    PreAuthSlots slots(Slots(2), clock.Get());

    PreAuthSlots::Handle a = slots.TryAcquire(Remote(1));
    PreAuthSlots::Handle b = slots.TryAcquire(Remote(2));
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_FALSE(slots.TryAcquire(Remote(3)));

    a.Release();
    EXPECT_FALSE(a);
    EXPECT_EQ(slots.InUse(), 1u);
    EXPECT_EQ(slots.InUse(Remote(1)), 0u);
    a.Release(); // twice is harmless
    EXPECT_EQ(slots.InUse(), 1u);

    PreAuthSlots::Handle c = slots.TryAcquire(Remote(3));
    EXPECT_TRUE(c);
    EXPECT_EQ(slots.InUse(), 2u);
}

TEST(PreAuthSlotsTest, DestroyingAHandleReleasesAndMovingDoesNotDoubleRelease)
{
    FakeClock clock;
    PreAuthSlots slots(Slots(2), clock.Get());
    {
        PreAuthSlots::Handle outer;
        {
            PreAuthSlots::Handle inner = slots.TryAcquire(Remote(1));
            ASSERT_TRUE(inner);
            outer = std::move(inner);
            EXPECT_FALSE(inner) << "moved from";
            EXPECT_EQ(slots.InUse(), 1u);
        }
        EXPECT_EQ(slots.InUse(), 1u) << "the moved-from handle released nothing";
        EXPECT_TRUE(outer);
    }
    EXPECT_EQ(slots.InUse(), 0u);
    EXPECT_EQ(slots.InUse(Remote(1)), 0u);

    // Move-assigning over a held handle releases what it held
    PreAuthSlots::Handle first = slots.TryAcquire(Remote(1));
    PreAuthSlots::Handle second = slots.TryAcquire(Remote(2));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    first = std::move(second);
    EXPECT_EQ(slots.InUse(), 1u);
    EXPECT_EQ(slots.InUse(Remote(1)), 0u);
    EXPECT_EQ(slots.InUse(Remote(2)), 1u);
}

TEST(PreAuthSlotsTest, CountersAreTakenAndResetForThePeriodicSummary)
{
    FakeClock clock;
    PreAuthSlots slots(Slots(1), clock.Get());

    PreAuthSlots::Handle held = slots.TryAcquire(Remote(1));
    ASSERT_TRUE(held);
    EXPECT_FALSE(slots.TryAcquire(Remote(2)));
    EXPECT_FALSE(slots.TryAcquire(Remote(3)));
    EXPECT_FALSE(slots.TryAcquire(Remote(1)));

    const PreAuthSlots::Counters first = slots.TakeCounters();
    EXPECT_EQ(first.admitted, 1u);
    EXPECT_EQ(first.refusedFull, 3u);
    EXPECT_EQ(first.refusedPerAddress, 0u) << "one slot in all: the address's own share (2) was not the limit";

    const PreAuthSlots::Counters second = slots.TakeCounters();
    EXPECT_EQ(second.admitted, 0u);
    EXPECT_EQ(second.refusedFull, 0u);

    // The totals keep counting
    EXPECT_EQ(slots.TotalCounters().refusedFull, 3u);
}

TEST(PreAuthSlotsTest, TheReservedSlotIsOffByDefault)
{
    FakeClock clock;
    PreAuthSlots slots(Slots(2), clock.Get());
    slots.RecordAuthenticated(Remote(1)); // ignored: the option is off
    EXPECT_EQ(slots.RecentAddressCount(), 0u);

    PreAuthSlots::Handle a = slots.TryAcquire(Remote(2));
    PreAuthSlots::Handle b = slots.TryAcquire(Remote(3));
    EXPECT_TRUE(a);
    EXPECT_TRUE(b) << "both slots are open to everybody";
}

TEST(PreAuthSlotsTest, TheReservedSlotIsOnlyForRecentlyAuthenticatedAddresses)
{
    FakeClock clock;
    PreAuthSlotsOptions options = Slots(3);
    options.reserveForRecentlyAuthenticated = true;
    options.reservedSlots = 1;
    PreAuthSlots slots(options, clock.Get());

    slots.RecordAuthenticated(Remote(50));
    slots.RecordAuthenticated(Loopback); // loopback never counts
    EXPECT_EQ(slots.RecentAddressCount(), 1u);

    // Strangers (and loopback) can fill two of the three
    PreAuthSlots::Handle a = slots.TryAcquire(Remote(1));
    PreAuthSlots::Handle b = slots.TryAcquire(Remote(2));
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_FALSE(slots.TryAcquire(Remote(3))) << "the last slot is kept back";
    EXPECT_FALSE(slots.TryAcquire(Loopback)) << "loopback is not a known address";

    // The known address gets in through the reserve, and only once the open slots are taken
    PreAuthSlots::Handle known = slots.TryAcquire(Remote(50));
    EXPECT_TRUE(known);
    EXPECT_EQ(slots.InUse(), 3u);
    EXPECT_FALSE(slots.TryAcquire(Remote(50))) << "nothing is left, not even for a known address";

    // Releasing frees the reserve again for the next known address
    known.Release();
    EXPECT_FALSE(slots.TryAcquire(Remote(4)));
    EXPECT_TRUE(slots.TryAcquire(Remote(50)));
}

TEST(PreAuthSlotsTest, TheReservedSlotLapsesAfter24Hours)
{
    FakeClock clock;
    PreAuthSlotsOptions options = Slots(2);
    options.reserveForRecentlyAuthenticated = true;
    PreAuthSlots slots(options, clock.Get());

    slots.RecordAuthenticated(Remote(50));
    PreAuthSlots::Handle stranger = slots.TryAcquire(Remote(1));
    ASSERT_TRUE(stranger);

    clock.Advance(hours(23));
    EXPECT_TRUE(slots.TryAcquire(Remote(50))) << "still recent";

    PreAuthSlots second(options, clock.Get());
    second.RecordAuthenticated(Remote(50));
    PreAuthSlots::Handle fill = second.TryAcquire(Remote(1));
    ASSERT_TRUE(fill);
    clock.Advance(hours(25));
    EXPECT_FALSE(second.TryAcquire(Remote(50))) << "24 hours on, it is a stranger again";
    EXPECT_EQ(second.RecentAddressCount(), 0u) << "and the stale entry is dropped";
}

TEST(PreAuthSlotsTest, TheReserveNeverTakesTheLastOpenSlot)
{
    FakeClock clock;
    PreAuthSlotsOptions options = Slots(1);
    options.reserveForRecentlyAuthenticated = true;
    options.reservedSlots = 5;
    const PreAuthSlots slots(options, clock.Get());
    EXPECT_EQ(slots.Options().reservedSlots, 0u) << "one slot in all: nothing can be reserved";

    PreAuthSlotsOptions more = Slots(4);
    more.reserveForRecentlyAuthenticated = true;
    more.reservedSlots = 99;
    const PreAuthSlots many(more, clock.Get());
    EXPECT_EQ(many.Options().reservedSlots, 3u) << "at least one stays open to everyone";
}

TEST(PreAuthSlotsTest, TheRecentTableIsBounded)
{
    FakeClock clock;
    PreAuthSlotsOptions options = Slots(4);
    options.reserveForRecentlyAuthenticated = true;
    options.maxRecentAddresses = 3;
    PreAuthSlots slots(options, clock.Get());

    for (uint8_t i = 1; i <= 10; ++i)
    {
        slots.RecordAuthenticated(Remote(i));
        clock.Advance(milliseconds(10));
    }
    EXPECT_EQ(slots.RecentAddressCount(), 3u);
}

TEST(PreAuthSlotsTest, AClockGoingBackwardsLosesNothingAndBreaksNothing)
{
    FakeClock clock;
    clock.Set(1'000'000);
    PreAuthSlotsOptions options = Slots(2);
    options.reserveForRecentlyAuthenticated = true;
    PreAuthSlots slots(options, clock.Get());

    slots.RecordAuthenticated(Remote(50));
    clock.Set(10); // far in the past
    PreAuthSlots::Handle stranger = slots.TryAcquire(Remote(1));
    ASSERT_TRUE(stranger);
    EXPECT_TRUE(slots.TryAcquire(Remote(50))) << "a negative age is inside the window";
    EXPECT_EQ(slots.RecentAddressCount(), 1u);
}

TEST(PreAuthSlotsTest, AnUnknownFamilyPeerSharesOneAddress)
{
    FakeClock clock;
    PreAuthSlots slots(Slots(8), clock.Get());
    const PeerAddress unknown;
    PreAuthSlots::Handle a = slots.TryAcquire(unknown);
    PreAuthSlots::Handle b = slots.TryAcquire(unknown);
    EXPECT_TRUE(a);
    EXPECT_TRUE(b);
    EXPECT_FALSE(slots.TryAcquire(unknown)) << "conservative: they all count as one address";
}
