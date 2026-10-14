// Regression: stable_sort over an over-aligned element type. On MSVC toolsets whose STL temporary buffer uses
// std::aligned_storage this fails to COMPILE (C2338) unless _ENABLE_EXTENDED_ALIGNED_STORAGE is defined (root CMakeLists).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace
{
    struct alignas(16) AlignedEntry
    {
        int key;
        int seq;
        char pad[72];
    };

    static_assert(sizeof(AlignedEntry) == 80);
    static_assert(alignof(AlignedEntry) == 16);

    std::vector<AlignedEntry> Make(std::size_t n)
    {
        std::vector<AlignedEntry> v(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            v[i] = AlignedEntry{};
            v[i].key = static_cast<int>((i * 7) % 5);
            v[i].seq = static_cast<int>(i);
        }
        return v;
    }

    void ExpectStable(const std::vector<AlignedEntry> &v, std::size_t n)
    {
        ASSERT_EQ(v.size(), n);
        for (std::size_t i = 1; i < v.size(); ++i)
        {
            ASSERT_LE(v[i - 1].key, v[i].key);
            if (v[i - 1].key == v[i].key)
            {
                ASSERT_LT(v[i - 1].seq, v[i].seq);
            }
        }
    }
}

TEST(StableSortAlignedTest, StdStableSort)
{
    for (std::size_t n : {0u, 1u, 2u, 7u, 64u, 1000u})
    {
        auto v = Make(n);
        std::stable_sort(v.begin(), v.end(), [](const AlignedEntry &a, const AlignedEntry &b) { return a.key < b.key; });
        ExpectStable(v, n);
    }
}

TEST(StableSortAlignedTest, RangesStableSort)
{
    for (std::size_t n : {0u, 1u, 2u, 7u, 64u, 1000u})
    {
        auto v = Make(n);
        std::ranges::stable_sort(v, [](const AlignedEntry &a, const AlignedEntry &b) { return a.key < b.key; });
        ExpectStable(v, n);
    }
}
