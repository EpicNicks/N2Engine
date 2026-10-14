// UUID::Random: one seeded generator per thread. These tests check uniqueness and the v4 bits only; they do not
// depend on the quality of the machine's entropy source.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "math/UUID.hpp"

using N2Engine::Math::UUID;

namespace
{
    using Bytes = std::array<std::uint8_t, 16>;

    bool HasValidV4Bits(const UUID& id)
    {
        const Bytes& b = id.GetBytes();
        return (b[6] >> 4) == 0x4 && (b[8] >> 6) == 0x2;
    }

    bool AllUnique(std::vector<Bytes>& all)
    {
        std::sort(all.begin(), all.end());
        return std::adjacent_find(all.begin(), all.end()) == all.end();
    }

    std::vector<Bytes> Generate(std::size_t count, bool& bitsOk)
    {
        std::vector<Bytes> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const UUID id = UUID::Random();
            if (!HasValidV4Bits(id))
            {
                bitsOk = false;
            }
            out.push_back(id.GetBytes());
        }
        return out;
    }
}

TEST(UUIDRandomTest, TwoHundredThousandOnOneThreadAreUniqueWithV4Bits)
{
    bool bitsOk = true;
    std::vector<Bytes> all = Generate(200000, bitsOk);
    EXPECT_TRUE(bitsOk);
    EXPECT_TRUE(AllUnique(all));
}

TEST(UUIDRandomTest, EightThreadsOfTwentyFiveThousandAreGloballyUnique)
{
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kPerThread = 25000;
    std::vector<std::vector<Bytes>> results(kThreads);
    std::array<bool, kThreads> bitsOk{};
    bitsOk.fill(true);

    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&results, &bitsOk, t]() { results[t] = Generate(kPerThread, bitsOk[t]); });
    }
    for (auto& th : threads)
    {
        th.join();
    }

    std::vector<Bytes> all;
    all.reserve(kThreads * kPerThread);
    for (std::size_t t = 0; t < kThreads; ++t)
    {
        EXPECT_TRUE(bitsOk[t]);
        all.insert(all.end(), results[t].begin(), results[t].end());
    }
    EXPECT_EQ(all.size(), kThreads * kPerThread);
    EXPECT_TRUE(AllUnique(all));
}

TEST(UUIDRandomTest, FirstUUIDsOfTwoThreadsDiffer)
{
    UUID a = UUID::ZERO;
    UUID b = UUID::ZERO;
    std::thread ta([&a]() { a = UUID::Random(); });
    std::thread tb([&b]() { b = UUID::Random(); });
    ta.join();
    tb.join();

    EXPECT_FALSE(a == UUID::ZERO);
    EXPECT_FALSE(b == UUID::ZERO);
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(HasValidV4Bits(a));
    EXPECT_TRUE(HasValidV4Bits(b));
}

TEST(UUIDRandomTest, ThreadsStartingAndExitingRepeatedlyStayUnique)
{
    constexpr int kRounds = 100;
    std::vector<Bytes> all;
    all.reserve(kRounds * 4);
    for (int round = 0; round < kRounds; ++round)
    {
        std::array<UUID, 4> ids{UUID::ZERO, UUID::ZERO, UUID::ZERO, UUID::ZERO};
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            threads.emplace_back([&ids, i]() { ids[i] = UUID::Random(); });
        }
        for (auto& th : threads)
        {
            th.join();
        }
        for (const UUID& id : ids)
        {
            EXPECT_TRUE(HasValidV4Bits(id));
            all.push_back(id.GetBytes());
        }
    }
    EXPECT_TRUE(AllUnique(all));
}
