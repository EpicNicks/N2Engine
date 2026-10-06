#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <assetimport/ImageDecoder.hpp>

#include "TestImages.hpp"

// DecodeImage on images built in the test: every PNG colour type the engine meets, JPEG, TGA and BMP; the
// RGBA expansion and the row order (flipY); and corrupt, truncated and oversized input.

using namespace N2Engine::AssetImport;
using namespace AssetImportTests;

namespace
{
    using Rgba = std::array<std::uint8_t, 4>;

    /// The pixel at column x, `row` rows from the start of the stored data
    Rgba StoredPixel(const ImportedImage &image, const std::uint32_t x, const std::uint32_t row)
    {
        const std::size_t i = (static_cast<std::size_t>(row) * image.width + x) * 4;
        return Rgba{image.pixels[i], image.pixels[i + 1], image.pixels[i + 2], image.pixels[i + 3]};
    }

    /// The pixel at column x, `fromTop` rows from the top of the picture, for an image decoded with flipY
    Rgba PictureFlipped(const ImportedImage &image, const std::uint32_t x, const std::uint32_t fromTop)
    {
        return StoredPixel(image, x, image.height - 1 - fromTop);
    }

    ImportedImage DecodeOrFail(const std::vector<std::uint8_t> &bytes, const ImageDecodeOptions &options = {})
    {
        auto result = DecodeImage(bytes, options);
        if (!result)
        {
            ADD_FAILURE() << "decode failed: " << ToString(result.error().code) << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void ExpectNear(const Rgba &actual, const Rgba &expected, const int tolerance, const std::string &what)
    {
        for (std::size_t c = 0; c < 4; ++c)
        {
            EXPECT_LE(std::abs(static_cast<int>(actual[c]) - static_cast<int>(expected[c])), tolerance)
                << what << ", channel " << c << ": got " << static_cast<int>(actual[c]) << ", expected "
                << static_cast<int>(expected[c]);
        }
    }

    // A 2 x 2 RGBA picture, top row first: red, green over blue, white, with assorted alphas
    constexpr Rgba kTopLeft{255, 0, 0, 255};
    constexpr Rgba kTopRight{0, 255, 0, 128};
    constexpr Rgba kBottomLeft{0, 0, 255, 255};
    constexpr Rgba kBottomRight{255, 255, 255, 0};

    PngSpec Rgba2x2()
    {
        PngSpec spec;
        spec.width = 2;
        spec.height = 2;
        spec.colorType = PngColorType::Rgba;
        for (const Rgba &p : {kTopLeft, kTopRight, kBottomLeft, kBottomRight})
        {
            spec.samples.insert(spec.samples.end(), p.begin(), p.end());
        }
        return spec;
    }

    /// 3 x 2 RGB samples, top row first, all distinct
    std::vector<std::uint8_t> Rgb3x2()
    {
        return {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180};
    }
}

// ============================================================================
// PNG colour types and bit depths
// ============================================================================

TEST(ImageDecoderTest, PngRgbaDecodesExactlyInFileOrderWithoutFlipY)
{
    const auto png = MakePng(Rgba2x2());
    const ImportedImage image = DecodeOrFail(png, ImageDecodeOptions{.flipY = false});
    ASSERT_EQ(image.width, 2u);
    ASSERT_EQ(image.height, 2u);
    EXPECT_EQ(image.channels, 4u);
    EXPECT_EQ(image.sourceChannels, 4u);
    ASSERT_EQ(image.pixels.size(), 16u);
    EXPECT_EQ(StoredPixel(image, 0, 0), kTopLeft) << "the top row comes first";
    EXPECT_EQ(StoredPixel(image, 1, 0), kTopRight);
    EXPECT_EQ(StoredPixel(image, 0, 1), kBottomLeft);
    EXPECT_EQ(StoredPixel(image, 1, 1), kBottomRight);
}

TEST(ImageDecoderTest, FlipYStoresTheBottomRowFirstByDefault)
{
    const auto png = MakePng(Rgba2x2());
    const ImportedImage image = DecodeOrFail(png);
    ASSERT_EQ(image.pixels.size(), 16u);
    EXPECT_EQ(StoredPixel(image, 0, 0), kBottomLeft) << "row 0 (texture v = 0) is the bottom of the picture";
    EXPECT_EQ(StoredPixel(image, 1, 0), kBottomRight);
    EXPECT_EQ(StoredPixel(image, 0, 1), kTopLeft);
    EXPECT_EQ(StoredPixel(image, 1, 1), kTopRight);
}

TEST(ImageDecoderTest, GreyPngExpandsToRgba)
{
    PngSpec spec;
    spec.width = 3;
    spec.height = 1;
    spec.colorType = PngColorType::Grey;
    spec.samples = {0, 128, 255};
    const ImportedImage image = DecodeOrFail(MakePng(spec));
    ASSERT_EQ(image.width, 3u);
    EXPECT_EQ(image.channels, 4u);
    EXPECT_EQ(image.sourceChannels, 1u);
    EXPECT_EQ(StoredPixel(image, 0, 0), (Rgba{0, 0, 0, 255}));
    EXPECT_EQ(StoredPixel(image, 1, 0), (Rgba{128, 128, 128, 255})) << "grey, not red: every channel gets the value";
    EXPECT_EQ(StoredPixel(image, 2, 0), (Rgba{255, 255, 255, 255}));
}

TEST(ImageDecoderTest, GreyAlphaPngExpandsToRgba)
{
    PngSpec spec;
    spec.width = 2;
    spec.height = 1;
    spec.colorType = PngColorType::GreyAlpha;
    spec.samples = {10, 20, 200, 255};
    const ImportedImage image = DecodeOrFail(MakePng(spec));
    EXPECT_EQ(image.sourceChannels, 2u);
    EXPECT_EQ(StoredPixel(image, 0, 0), (Rgba{10, 10, 10, 20}));
    EXPECT_EQ(StoredPixel(image, 1, 0), (Rgba{200, 200, 200, 255}));
}

TEST(ImageDecoderTest, PalettePngWithTransparencyExpandsToRgba)
{
    PngSpec spec;
    spec.width = 3;
    spec.height = 1;
    spec.colorType = PngColorType::Palette;
    spec.samples = {2, 0, 1};
    spec.palette = {255, 0, 0, 0, 255, 0, 0, 0, 255};
    spec.transparency = {255, 128, 0};
    const ImportedImage image = DecodeOrFail(MakePng(spec));
    ASSERT_EQ(image.width, 3u);
    EXPECT_EQ(StoredPixel(image, 0, 0), (Rgba{0, 0, 255, 0}));
    EXPECT_EQ(StoredPixel(image, 1, 0), (Rgba{255, 0, 0, 255}));
    EXPECT_EQ(StoredPixel(image, 2, 0), (Rgba{0, 255, 0, 128}));
}

TEST(ImageDecoderTest, PalettePngWithoutTransparencyIsOpaque)
{
    PngSpec spec;
    spec.width = 2;
    spec.height = 1;
    spec.colorType = PngColorType::Palette;
    spec.samples = {1, 0};
    spec.palette = {10, 20, 30, 40, 50, 60};
    const ImportedImage image = DecodeOrFail(MakePng(spec));
    EXPECT_EQ(StoredPixel(image, 0, 0), (Rgba{40, 50, 60, 255}));
    EXPECT_EQ(StoredPixel(image, 1, 0), (Rgba{10, 20, 30, 255}));
}

TEST(ImageDecoderTest, SixteenBitPngIsReducedToEightBits)
{
    PngSpec spec;
    spec.width = 2;
    spec.height = 1;
    spec.bitDepth = 16;
    spec.colorType = PngColorType::Rgb;
    // Big-endian samples: (0xAB12, 0x0000, 0xFFFF) and (0x1234, 0x8000, 0x00FF)
    spec.samples = {0xAB, 0x12, 0x00, 0x00, 0xFF, 0xFF, 0x12, 0x34, 0x80, 0x00, 0x00, 0xFF};
    const ImportedImage image = DecodeOrFail(MakePng(spec));
    ASSERT_EQ(image.pixels.size(), 8u) << "8 bits per channel out";
    EXPECT_EQ(StoredPixel(image, 0, 0), (Rgba{0xAB, 0x00, 0xFF, 255}));
    EXPECT_EQ(StoredPixel(image, 1, 0), (Rgba{0x12, 0x80, 0x00, 255}));
}

TEST(ImageDecoderTest, SixteenBitGreyAlphaPngIsReducedToEightBits)
{
    PngSpec spec;
    spec.width = 1;
    spec.height = 1;
    spec.bitDepth = 16;
    spec.colorType = PngColorType::GreyAlpha;
    spec.samples = {0x7F, 0xFF, 0x40, 0x00};
    const ImportedImage image = DecodeOrFail(MakePng(spec));
    EXPECT_EQ(StoredPixel(image, 0, 0), (Rgba{0x7F, 0x7F, 0x7F, 0x40}));
}

TEST(ImageDecoderTest, CompressedPngFromStbImageWriteDecodes)
{
    const auto rgb = Rgb3x2();
    const ImportedImage image = DecodeOrFail(EncodePngStb(3, 2, 3, rgb), ImageDecodeOptions{.flipY = false});
    ASSERT_EQ(image.width, 3u);
    ASSERT_EQ(image.height, 2u);
    EXPECT_EQ(image.sourceChannels, 3u);
    for (std::uint32_t y = 0; y < 2; ++y)
    {
        for (std::uint32_t x = 0; x < 3; ++x)
        {
            const std::size_t i = (y * 3 + x) * 3;
            EXPECT_EQ(StoredPixel(image, x, y), (Rgba{rgb[i], rgb[i + 1], rgb[i + 2], 255})) << x << ", " << y;
        }
    }
}

// ============================================================================
// JPEG, TGA, BMP
// ============================================================================

TEST(ImageDecoderTest, JpegDecodesWithinTolerance)
{
    // 16 x 16 in four solid 8 x 8 quadrants (one JPEG block each, so there's little ringing)
    constexpr int size = 16;
    const Rgba quadrants[2][2] = {{Rgba{220, 30, 30, 255}, Rgba{30, 200, 40, 255}},
                                  {Rgba{40, 50, 210, 255}, Rgba{230, 220, 40, 255}}};
    std::vector<std::uint8_t> rgb;
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const Rgba &q = quadrants[y / 8][x / 8];
            rgb.insert(rgb.end(), {q[0], q[1], q[2]});
        }
    }

    const ImportedImage image = DecodeOrFail(EncodeJpgStb(size, size, 3, rgb, 100));
    ASSERT_EQ(image.width, 16u);
    ASSERT_EQ(image.height, 16u);
    EXPECT_EQ(image.sourceChannels, 3u);
    // Quadrant centres, picture coordinates (flipY on)
    ExpectNear(PictureFlipped(image, 4, 4), quadrants[0][0], 12, "top-left");
    ExpectNear(PictureFlipped(image, 12, 4), quadrants[0][1], 12, "top-right");
    ExpectNear(PictureFlipped(image, 4, 12), quadrants[1][0], 12, "bottom-left");
    ExpectNear(PictureFlipped(image, 12, 12), quadrants[1][1], 12, "bottom-right");
}

TEST(ImageDecoderTest, TgaDecodesExactlyWithAndWithoutRle)
{
    const std::vector<std::uint8_t> rgba = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0,
                                            1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4};
    for (const bool rle : {false, true})
    {
        const ImportedImage image = DecodeOrFail(EncodeTgaStb(3, 2, 4, rgba, rle), ImageDecodeOptions{.flipY = false});
        ASSERT_EQ(image.width, 3u) << "rle " << rle;
        ASSERT_EQ(image.height, 2u) << "rle " << rle;
        EXPECT_EQ(image.pixels, rgba) << "rle " << rle;
    }
}

TEST(ImageDecoderTest, BmpDecodesExactlyAndGainsOpaqueAlpha)
{
    const auto rgb = Rgb3x2();
    const auto bmp = EncodeBmpStb(3, 2, 3, rgb);
    for (const bool flipY : {false, true})
    {
        const ImportedImage image = DecodeOrFail(bmp, ImageDecodeOptions{.flipY = flipY});
        ASSERT_EQ(image.width, 3u);
        ASSERT_EQ(image.height, 2u);
        for (std::uint32_t y = 0; y < 2; ++y)
        {
            for (std::uint32_t x = 0; x < 3; ++x)
            {
                const std::size_t i = (y * 3 + x) * 3;
                const Rgba pixel = flipY ? PictureFlipped(image, x, y) : StoredPixel(image, x, y);
                EXPECT_EQ(pixel, (Rgba{rgb[i], rgb[i + 1], rgb[i + 2], 255})) << x << ", " << y << " flipY " << flipY;
            }
        }
    }
}

TEST(ImageDecoderTest, TopDownBmpWithANegativeHeightDecodes)
{
    // stb_image_write writes bottom-up rows; negate the height and swap the two rows to make it top-down
    const auto rgb = Rgb3x2();
    auto bmp = EncodeBmpStb(3, 2, 3, rgb);
    const std::uint32_t offset = bmp[10] | (bmp[11] << 8) | (bmp[12] << 16) | (static_cast<std::uint32_t>(bmp[13]) << 24);
    const std::int32_t negativeTwo = -2;
    const auto height = static_cast<std::uint32_t>(negativeTwo);
    bmp[22] = static_cast<std::uint8_t>(height);
    bmp[23] = static_cast<std::uint8_t>(height >> 8);
    bmp[24] = static_cast<std::uint8_t>(height >> 16);
    bmp[25] = static_cast<std::uint8_t>(height >> 24);
    constexpr std::size_t rowBytes = 12; // 9 bytes of BGR padded to 4
    ASSERT_GE(bmp.size(), offset + 2 * rowBytes);
    std::swap_ranges(bmp.begin() + offset, bmp.begin() + offset + rowBytes, bmp.begin() + offset + rowBytes);

    const ImportedImage image = DecodeOrFail(bmp, ImageDecodeOptions{.flipY = false});
    ASSERT_EQ(image.height, 2u);
    EXPECT_EQ(StoredPixel(image, 0, 0), (Rgba{10, 20, 30, 255}));
    EXPECT_EQ(StoredPixel(image, 2, 1), (Rgba{160, 170, 180, 255}));
}

// ============================================================================
// Bad input
// ============================================================================

TEST(ImageDecoderTest, EmptyAndUnrecognisedDataAreErrors)
{
    const auto empty = DecodeImage(std::span<const std::uint8_t>{});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ImageDecodeErrorCode::EmptyInput);

    const std::string text = "hello, this is not an image";
    const auto garbage = DecodeImage(std::span(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
    ASSERT_FALSE(garbage);
    EXPECT_EQ(garbage.error().code, ImageDecodeErrorCode::UnrecognizedFormat);
    EXPECT_FALSE(garbage.error().message.empty());
}

TEST(ImageDecoderTest, CorruptPixelDataAfterAGoodHeaderIsADecodeError)
{
    auto png = MakePng(Rgba2x2());
    // Signature (8) + IHDR (25), then IDAT: length, type, zlib header (2), block header (1), LEN (2), NLEN
    constexpr std::size_t nlen = 8 + 25 + 8 + 2 + 1 + 2;
    ASSERT_GT(png.size(), nlen);
    png[nlen] ^= 0xFF; // NLEN no longer complements LEN
    const auto result = DecodeImage(png);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ImageDecodeErrorCode::DecodeFailed) << result.error().message;
}

TEST(ImageDecoderTest, EveryTruncationOfAPngIsAnErrorNotACrash)
{
    PngSpec spec;
    spec.width = 3;
    spec.height = 2;
    spec.colorType = PngColorType::Rgba;
    for (int i = 0; i < 24; ++i)
    {
        spec.samples.push_back(static_cast<std::uint8_t>(i * 10));
    }

    // A stored (uncompressed) PNG and a deflate-compressed one
    const std::vector<std::vector<std::uint8_t>> files = {MakePng(spec), EncodePngStb(3, 2, 4, spec.samples)};
    for (const auto &file : files)
    {
        const auto whole = DecodeImage(file);
        ASSERT_TRUE(whole.has_value()) << "the whole file decodes";
        for (std::size_t length = 0; length < file.size(); ++length)
        {
            const auto result = DecodeImage(std::span(file.data(), length));
            if (length < file.size() - 4)
            {
                EXPECT_FALSE(result.has_value()) << "truncated to " << length << " of " << file.size() << " bytes";
            }
            if (result)
            {
                // Only the final IEND chunk's CRC is missing, which stb_image doesn't check: the image is whole
                EXPECT_EQ(result->pixels, whole->pixels) << length;
            }
            else
            {
                EXPECT_FALSE(result.error().message.empty()) << length;
            }
        }
    }
}

TEST(ImageDecoderTest, OversizedDimensionsAreRejectedFromTheHeader)
{
    // BMP headers with no pixel data: only the header is read before the size is rejected
    for (const auto &[width, height] : {std::pair{20000, 1}, std::pair{1, 20000}, std::pair{16385, 16}})
    {
        const auto result = DecodeImage(MakeBmpHeader(width, height));
        ASSERT_FALSE(result) << width << " x " << height;
        EXPECT_EQ(result.error().code, ImageDecodeErrorCode::DimensionsTooLarge) << result.error().message;
    }

    // A PNG over 16384 wide: stb_image's own STBI_MAX_DIMENSIONS check refuses its header
    PngSpec wide;
    wide.width = 20000;
    wide.height = 1;
    wide.colorType = PngColorType::Grey;
    wide.samples.assign(20000, 0);
    const auto png = DecodeImage(MakePng(wide));
    ASSERT_FALSE(png);
    EXPECT_TRUE(png.error().code == ImageDecodeErrorCode::DimensionsTooLarge ||
                png.error().code == ImageDecodeErrorCode::UnrecognizedFormat)
        << ToString(png.error().code) << ": " << png.error().message;
}

TEST(ImageDecoderTest, OversizedDecodedBytesAreRejectedFromTheHeader)
{
    // Each side within 16384, but the RGBA pixels would be over 256 MiB
    for (const auto &[width, height] : {std::pair{16384, 16384}, std::pair{8192, 8193}})
    {
        const auto result = DecodeImage(MakeBmpHeader(width, height));
        ASSERT_FALSE(result) << width << " x " << height;
        EXPECT_EQ(result.error().code, ImageDecodeErrorCode::DecodedSizeTooLarge) << result.error().message;
    }
}

TEST(ImageDecoderTest, CallerCapsApplyAndCantRaiseTheBuiltInOnes)
{
    const auto png = MakePng(Rgba2x2()); // 2 x 2, 16 bytes decoded

    const auto narrow = DecodeImage(png, ImageDecodeOptions{.maxDimension = 1});
    ASSERT_FALSE(narrow);
    EXPECT_EQ(narrow.error().code, ImageDecodeErrorCode::DimensionsTooLarge);

    const auto small = DecodeImage(png, ImageDecodeOptions{.maxDecodedBytes = 15});
    ASSERT_FALSE(small);
    EXPECT_EQ(small.error().code, ImageDecodeErrorCode::DecodedSizeTooLarge);

    EXPECT_TRUE(DecodeImage(png, ImageDecodeOptions{.maxDecodedBytes = 16}).has_value()) << "exactly at the cap";

    // Raising a cap above the built-in one has no effect
    const auto raised = DecodeImage(MakeBmpHeader(20000, 1), ImageDecodeOptions{.maxDimension = 100000});
    ASSERT_FALSE(raised);
    EXPECT_EQ(raised.error().code, ImageDecodeErrorCode::DimensionsTooLarge);
}

TEST(ImageDecoderTest, ErrorCodesHaveNames)
{
    EXPECT_EQ(ToString(ImageDecodeErrorCode::EmptyInput), "EmptyInput");
    EXPECT_EQ(ToString(ImageDecodeErrorCode::DecodedSizeTooLarge), "DecodedSizeTooLarge");
    EXPECT_EQ(ToString(ImageDecodeErrorCode::DecodeFailed), "DecodeFailed");
}
