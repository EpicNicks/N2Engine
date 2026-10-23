#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

#include "math/DeterministicRandom.hpp"
#include "math/Random.hpp"
#include "math/Vector2.hpp"

// Math::DeterministicRandom and the seedable Math::Random. Nothing here depends on the platform's libm or standard
// library distributions: the generator is integer arithmetic, and the checks on its circles use tolerances.

using N2Engine::Math::DeterministicRandom;
using N2Engine::Math::Vector2;
namespace Random = N2Engine::Math::Random;

TEST(DeterministicRandomTest, SameSeedGivesTheSameSequenceAndADifferentSeedDoesNot)
{
    DeterministicRandom a(42);
    DeterministicRandom b(42);
    DeterministicRandom c(43);
    bool differsFromC = false;
    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t fromA = a.NextU32();
        const std::uint32_t fromB = b.NextU32();
        const std::uint32_t fromC = c.NextU32();
        EXPECT_EQ(fromA, fromB);
        differsFromC = differsFromC || fromA != fromC;
    }
    EXPECT_TRUE(differsFromC);
}

TEST(DeterministicRandomTest, ReseedingRestartsTheSequence)
{
    DeterministicRandom r(7);
    const std::uint32_t first = r.NextU32();
    const std::uint32_t second = r.NextU32();
    r.Seed(7);
    EXPECT_EQ(r.NextU32(), first);
    EXPECT_EQ(r.NextU32(), second);
}

// The values the integer algorithm gives on every platform (computed with a separate re-implementation of the SplitMix32-style steps): a
// platform or compiler that disagrees fails here
TEST(DeterministicRandomTest, KnownSequence)
{
    DeterministicRandom one(1);
    EXPECT_EQ(one.NextU32(), 0x96a0f96bu);
    EXPECT_EQ(one.NextU32(), 0x12bc8390u);
    EXPECT_EQ(one.NextU32(), 0x971e9964u);
    EXPECT_EQ(one.NextU32(), 0x79adc7e7u);

    DeterministicRandom other(12345);
    EXPECT_EQ(other.NextU32(), 0x479199b4u);
    EXPECT_EQ(other.NextU32(), 0x30c2cbd5u);
}

TEST(DeterministicRandomTest, FloatsAreInTheHalfOpenUnitInterval)
{
    DeterministicRandom r(99);
    for (int i = 0; i < 10000; ++i)
    {
        const float value = r.NextFloat();
        ASSERT_GE(value, 0.0f);
        ASSERT_LT(value, 1.0f);
    }
}

TEST(DeterministicRandomTest, RangeStaysInsideAndHandlesAnEmptyRange)
{
    DeterministicRandom r(5);
    for (int i = 0; i < 2000; ++i)
    {
        const float value = r.Range(-3.0f, 4.0f);
        ASSERT_GE(value, -3.0f);
        ASSERT_LE(value, 4.0f);
    }
    EXPECT_EQ(r.Range(2.0f, 2.0f), 2.0f);
    EXPECT_EQ(r.Range(2.0f, 1.0f), 2.0f);
}

TEST(DeterministicRandomTest, IntIsInclusiveOnBothEndsAndReachesEveryValue)
{
    DeterministicRandom r(11);
    std::set<int> seen;
    for (int i = 0; i < 2000; ++i)
    {
        const int value = r.NextInt(-2, 3);
        ASSERT_GE(value, -2);
        ASSERT_LE(value, 3);
        seen.insert(value);
    }
    EXPECT_EQ(seen.size(), 6u);
}

TEST(DeterministicRandomTest, IntWithAnEmptyOrReversedRangeReturnsMin)
{
    DeterministicRandom r(1);
    EXPECT_EQ(r.NextInt(4, 4), 4);
    EXPECT_EQ(r.NextInt(4, 3), 4);
}

TEST(DeterministicRandomTest, IntWorksOverTheWholeIntRange)
{
    constexpr int lo = std::numeric_limits<int>::min();
    constexpr int hi = std::numeric_limits<int>::max();
    DeterministicRandom a(3);
    DeterministicRandom b(3);
    // The whole range (a span of 2^32) takes every 32-bit value as it is: must not overflow or hang, and is reproducible
    for (int i = 0; i < 100; ++i)
    {
        const int first = a.NextInt(lo, hi);
        const int second = b.NextInt(lo, hi);
        ASSERT_EQ(first, second);
    }
    // A span just under 2^32 stays inside its bounds
    for (int i = 0; i < 100; ++i)
    {
        const int value = a.NextInt(lo + 1, hi);
        ASSERT_GE(value, lo + 1);
    }
}

TEST(DeterministicRandomTest, InUnitCircleStaysInsideTheDisc)
{
    DeterministicRandom r(21);
    for (int i = 0; i < 5000; ++i)
    {
        const Vector2 p = r.InUnitCircle();
        ASSERT_LE(p.x * p.x + p.y * p.y, 1.0f + 1e-6f);
    }
}

TEST(DeterministicRandomTest, OnUnitCircleHasLengthOne)
{
    DeterministicRandom r(22);
    for (int i = 0; i < 5000; ++i)
    {
        const Vector2 p = r.OnUnitCircle();
        ASSERT_NEAR(p.x * p.x + p.y * p.y, 1.0f, 1e-5f);
    }
}

TEST(DeterministicRandomTest, CirclesAreReproducibleFromTheSeed)
{
    DeterministicRandom a(8);
    DeterministicRandom b(8);
    for (int i = 0; i < 100; ++i)
    {
        const Vector2 inA = a.InUnitCircle();
        const Vector2 inB = b.InUnitCircle();
        EXPECT_EQ(inA.x, inB.x);
        EXPECT_EQ(inA.y, inB.y);
        const Vector2 onA = a.OnUnitCircle();
        const Vector2 onB = b.OnUnitCircle();
        EXPECT_EQ(onA.x, onB.x);
        EXPECT_EQ(onA.y, onB.y);
    }
}

// The global Random functions: seeded they draw from one DeterministicRandom stream
class SeededRandomTest : public ::testing::Test
{
protected:
    void SetUp() override { Random::Unseed(); }
    void TearDown() override { Random::Unseed(); }
};

TEST_F(SeededRandomTest, IsNotSeededUntilSeedIsCalled)
{
    EXPECT_FALSE(Random::IsSeeded());
    Random::Seed(1);
    EXPECT_TRUE(Random::IsSeeded());
    Random::Unseed();
    EXPECT_FALSE(Random::IsSeeded());
}

TEST_F(SeededRandomTest, SameSeedGivesTheSameSequenceAcrossTheFunctions)
{
    std::vector<float> floats[2];
    std::vector<int> ints[2];
    std::vector<float> circle[2];
    for (int run = 0; run < 2; ++run)
    {
        Random::Seed(1234);
        for (int i = 0; i < 20; ++i)
        {
            floats[run].push_back(Random::RandomFloat());
            floats[run].push_back(Random::RandomRange(-5.0f, 5.0f));
            ints[run].push_back(Random::RandomInt(-10, 10));
            const Vector2 inside = Random::RandomInUnitCircle();
            circle[run].push_back(inside.x);
            circle[run].push_back(inside.y);
            const Vector2 on = Random::RandomOnUnitCircle();
            circle[run].push_back(on.x);
            circle[run].push_back(on.y);
        }
    }
    EXPECT_EQ(floats[0], floats[1]);
    EXPECT_EQ(ints[0], ints[1]);
    EXPECT_EQ(circle[0], circle[1]);
}

TEST_F(SeededRandomTest, SeedingAgainRestartsTheSequence)
{
    Random::Seed(77);
    const float first = Random::RandomFloat();
    const float second = Random::RandomFloat();
    Random::Seed(77);
    EXPECT_EQ(Random::RandomFloat(), first);
    EXPECT_EQ(Random::RandomFloat(), second);
}

TEST_F(SeededRandomTest, MatchesADeterministicRandomWithTheSameSeed)
{
    DeterministicRandom stream(500);
    Random::Seed(500);
    const float expectedFloat = stream.NextFloat();
    EXPECT_EQ(Random::RandomFloat(), expectedFloat);
    const int expectedInt = stream.NextInt(3, 9);
    EXPECT_EQ(Random::RandomInt(3, 9), expectedInt);
}

TEST_F(SeededRandomTest, DifferentSeedsGiveDifferentSequences)
{
    Random::Seed(1);
    std::array<float, 8> a{};
    for (auto &value : a)
    {
        value = Random::RandomFloat();
    }
    Random::Seed(2);
    std::array<float, 8> b{};
    for (auto &value : b)
    {
        value = Random::RandomFloat();
    }
    EXPECT_NE(a, b);
}

// Without a Seed call the functions are what they were: in range (the sequence is not reproducible, and nothing here
// asserts anything about it)
TEST_F(SeededRandomTest, UnseededStillWorksAndStaysInRange)
{
    for (int i = 0; i < 500; ++i)
    {
        const float f = Random::RandomFloat();
        ASSERT_GE(f, 0.0f);
        ASSERT_LE(f, 1.0f);
        const int n = Random::RandomInt(1, 6);
        ASSERT_GE(n, 1);
        ASSERT_LE(n, 6);
    }
    EXPECT_FALSE(Random::IsSeeded());
}
