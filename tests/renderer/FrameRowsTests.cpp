#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <renderer/common/FrameRows.hpp>

using Renderer::Common::FlipRows;

namespace
{
    /// height rows of rowBytes bytes, every byte of row y set to y + 1
    std::vector<std::uint8_t> NumberedRows(const std::size_t rowBytes, const std::size_t height)
    {
        std::vector<std::uint8_t> pixels(rowBytes * height);
        for (std::size_t y = 0; y < height; ++y)
        {
            for (std::size_t x = 0; x < rowBytes; ++x)
            {
                pixels[y * rowBytes + x] = static_cast<std::uint8_t>(y + 1);
            }
        }
        return pixels;
    }

    /// The row number (see NumberedRows) each row holds, checking the row is whole
    std::vector<int> RowNumbers(const std::vector<std::uint8_t> &pixels, const std::size_t rowBytes)
    {
        std::vector<int> numbers;
        for (std::size_t start = 0; start < pixels.size(); start += rowBytes)
        {
            numbers.push_back(pixels[start]);
            for (std::size_t x = 1; x < rowBytes; ++x)
            {
                EXPECT_EQ(pixels[start + x], pixels[start]) << "row starting at byte " << start << " was split";
            }
        }
        return numbers;
    }
}

TEST(FlipRowsTest, ReversesTheRowsOfAnEvenHeight)
{
    auto pixels = NumberedRows(8, 4);
    FlipRows(pixels.data(), 8, 4);
    EXPECT_EQ(RowNumbers(pixels, 8), (std::vector<int>{4, 3, 2, 1}));
}

TEST(FlipRowsTest, LeavesTheMiddleRowOfAnOddHeightInPlace)
{
    auto pixels = NumberedRows(12, 5);
    FlipRows(pixels.data(), 12, 5);
    EXPECT_EQ(RowNumbers(pixels, 12), (std::vector<int>{5, 4, 3, 2, 1}));
}

TEST(FlipRowsTest, KeepsTheBytesWithinARowInOrder)
{
    // Two RGBA pixels per row: a flip moves rows, it never swizzles channels or mirrors pixels
    std::vector<std::uint8_t> pixels = {
        1, 2, 3, 4, 5, 6, 7, 8, // bottom row
        9, 10, 11, 12, 13, 14, 15, 16, // top row
    };
    FlipRows(pixels.data(), 8, 2);
    EXPECT_EQ(pixels, (std::vector<std::uint8_t>{9, 10, 11, 12, 13, 14, 15, 16, 1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(FlipRowsTest, FlippingTwiceRestoresTheImage)
{
    const auto original = NumberedRows(3, 7); // rows needn't be a multiple of 4 bytes
    auto pixels = original;
    FlipRows(pixels.data(), 3, 7);
    EXPECT_NE(pixels, original);
    FlipRows(pixels.data(), 3, 7);
    EXPECT_EQ(pixels, original);
}

TEST(FlipRowsTest, OneRowZeroRowsAndNullDoNothing)
{
    auto oneRow = NumberedRows(4, 1);
    FlipRows(oneRow.data(), 4, 1);
    EXPECT_EQ(oneRow, NumberedRows(4, 1));

    auto pixels = NumberedRows(4, 2);
    FlipRows(pixels.data(), 4, 0);
    FlipRows(pixels.data(), 0, 2);
    EXPECT_EQ(pixels, NumberedRows(4, 2));

    FlipRows(nullptr, 4, 2); // must not crash
}
