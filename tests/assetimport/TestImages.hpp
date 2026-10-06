#pragma once

#include <cstdint>
#include <span>
#include <vector>

// Image files built in the tests, so no binary test data is committed. PNGs are written by hand (stored,
// uncompressed deflate blocks), which reaches the colour types and bit depths stb_image_write can't (palette,
// grey + alpha, 16-bit); stb_image_write (test-only) encodes the compressed PNG, JPEG, TGA and BMP files.
namespace AssetImportTests
{
    enum class PngColorType : std::uint8_t
    {
        Grey = 0,
        Rgb = 2,
        Palette = 3,
        GreyAlpha = 4,
        Rgba = 6
    };

    struct PngSpec
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint8_t bitDepth = 8;
        PngColorType colorType = PngColorType::Rgba;
        /// The sample data, top row first, without filter bytes: width * samples-per-pixel * bytes-per-sample
        /// per row, 16-bit samples big-endian (as PNG stores them)
        std::vector<std::uint8_t> samples;
        /// PLTE entries (r, g, b) for Palette images
        std::vector<std::uint8_t> palette;
        /// tRNS: one alpha per palette entry for Palette images
        std::vector<std::uint8_t> transparency;
    };

    /// A PNG file for the spec, uncompressed (stored deflate blocks), with correct CRCs and Adler-32
    std::vector<std::uint8_t> MakePng(const PngSpec &spec);

    /// A 24-bit BMP header (54 bytes, no pixel data) claiming the given size: for the size-cap tests
    std::vector<std::uint8_t> MakeBmpHeader(std::int32_t width, std::int32_t height);

    // stb_image_write encoders. `pixels` is top row first with `components` bytes per pixel.
    std::vector<std::uint8_t> EncodePngStb(int width, int height, int components, std::span<const std::uint8_t> pixels);
    std::vector<std::uint8_t> EncodeJpgStb(int width, int height, int components, std::span<const std::uint8_t> pixels,
                                           int quality);
    std::vector<std::uint8_t> EncodeTgaStb(int width, int height, int components, std::span<const std::uint8_t> pixels,
                                           bool rle);
    std::vector<std::uint8_t> EncodeBmpStb(int width, int height, int components, std::span<const std::uint8_t> pixels);
}
