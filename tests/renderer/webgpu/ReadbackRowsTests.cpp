#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <renderer/webgpu/ReadbackRows.hpp>

using Renderer::WebGpu::StripRowPadding;

// The pure part of the browser's asynchronous ReadFramebuffer (stage G4 of #155): stripping the 256-byte row padding of a mapped
// readback buffer. The rest of AdvanceAsyncReadback needs a WebGPU device and a browser's event loop and has not been run.
namespace
{
    /// height rows of paddedRowBytes bytes: the first rowBytes of row y hold y + 1, the padding holds 0xEE
    std::vector<std::uint8_t> PaddedRows(const std::size_t paddedRowBytes, const std::size_t rowBytes, const std::size_t height)
    {
        std::vector<std::uint8_t> mapped(paddedRowBytes * height, 0xEE);
        for (std::size_t y = 0; y < height; ++y)
        {
            for (std::size_t x = 0; x < rowBytes; ++x)
            {
                mapped[y * paddedRowBytes + x] = static_cast<std::uint8_t>(y + 1);
            }
        }
        return mapped;
    }
}

TEST(WebGpuReadbackRowsTests, StripsThePaddingOfEveryRow)
{
    const std::vector<std::uint8_t> mapped = PaddedRows(256, 20, 3); // a 5 pixel wide RGBA8 image
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(StripRowPadding(mapped.data(), 256, 20, 3, out));
    ASSERT_EQ(out.size(), 60u);
    for (std::size_t y = 0; y < 3; ++y)
    {
        for (std::size_t x = 0; x < 20; ++x)
        {
            EXPECT_EQ(out[y * 20 + x], static_cast<std::uint8_t>(y + 1)) << "row " << y << " byte " << x;
        }
    }
}

TEST(WebGpuReadbackRowsTests, AnUnpaddedBufferIsCopiedAsItIs)
{
    const std::vector<std::uint8_t> mapped = PaddedRows(256, 256, 2);
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(StripRowPadding(mapped.data(), 256, 256, 2, out));
    EXPECT_EQ(out, mapped);
}

TEST(WebGpuReadbackRowsTests, ReplacesWhatTheOutputHeldAndResizesIt)
{
    const std::vector<std::uint8_t> mapped = PaddedRows(256, 8, 2);
    std::vector<std::uint8_t> out(1000, 0x77);
    ASSERT_TRUE(StripRowPadding(mapped.data(), 256, 8, 2, out));
    ASSERT_EQ(out.size(), 16u);
    EXPECT_EQ(out.front(), 1u);
    EXPECT_EQ(out.back(), 2u);
}

TEST(WebGpuReadbackRowsTests, ZeroRowsGiveAnEmptyResult)
{
    const std::vector<std::uint8_t> mapped(256, 1);
    std::vector<std::uint8_t> out(4, 0x77);
    ASSERT_TRUE(StripRowPadding(mapped.data(), 256, 16, 0, out));
    EXPECT_TRUE(out.empty());
}

TEST(WebGpuReadbackRowsTests, BadArgumentsReturnFalseAndLeaveTheOutputAlone)
{
    const std::vector<std::uint8_t> mapped(512, 1);
    std::vector<std::uint8_t> out(4, 0x77);
    EXPECT_FALSE(StripRowPadding(nullptr, 256, 16, 2, out));
    EXPECT_FALSE(StripRowPadding(mapped.data(), 256, 0, 2, out));
    EXPECT_FALSE(StripRowPadding(mapped.data(), 8, 16, 2, out));
    EXPECT_EQ(out, std::vector<std::uint8_t>(4, 0x77));
}
