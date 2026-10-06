#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

// Image decoding for texture assets (#3 P1). CPU-only and library-neutral: no stb type appears here, so the
// decoder behind it (stb_image today) can change without touching its users.
namespace N2Engine::AssetImport
{
    /// The largest width or height DecodeImage accepts (stb_image is also built with this as
    /// STBI_MAX_DIMENSIONS)
    inline constexpr std::uint32_t kMaxImageDimension = 16384;

    /// The most decoded pixel data DecodeImage produces (width * height * 4 bytes): 256 MiB, an 8192 x 8192
    /// RGBA image. A larger image is rejected before anything is decoded. Decoding may briefly need more than
    /// this (stb_image's own buffers, about twice it for 16-bit PNGs).
    inline constexpr std::size_t kMaxDecodedImageBytes = std::size_t{256} * 1024 * 1024;

    /// A decoded image: always 8-bit RGBA (4 bytes per pixel), rows tightly packed
    struct ImportedImage
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        /// Always 4: grey, grey + alpha and RGB images are expanded to RGBA (alpha 255 where the file has none)
        std::uint32_t channels = 4;
        /// The channels stb_image reports for the file (1 grey, 2 grey + alpha, 3 RGB, 4 RGBA; a palette image
        /// reports its expanded 3 or 4), for information
        std::uint32_t sourceChannels = 0;
        /// width * height * 4 bytes. With ImageDecodeOptions::flipY (the default) the bottom row of the
        /// picture comes first, otherwise the top row.
        std::vector<std::uint8_t> pixels;
    };

    struct ImageDecodeOptions
    {
        /// Store the rows bottom to top, so the first row of pixel data (texture v = 0) is the bottom of the
        /// picture: the engine's texture convention (docs/meshes.html#uv-convention). False keeps the file's
        /// order, top row first.
        bool flipY = true;
        /// Caps for this call. Values above kMaxImageDimension and kMaxDecodedImageBytes are lowered to them.
        std::uint32_t maxDimension = kMaxImageDimension;
        std::size_t maxDecodedBytes = kMaxDecodedImageBytes;
    };

    enum class ImageDecodeErrorCode
    {
        /// No bytes at all
        EmptyInput,
        /// More encoded bytes than the decoder can address (over 2 GiB)
        InputTooLarge,
        /// Not a PNG, JPEG, TGA or BMP image, or its header is corrupt or truncated
        UnrecognizedFormat,
        /// A width or height over the dimension cap (or zero)
        DimensionsTooLarge,
        /// width * height * 4 over the decoded-bytes cap
        DecodedSizeTooLarge,
        /// The header was readable but the pixel data is corrupt, truncated or uses an unsupported feature
        DecodeFailed
    };

    struct ImageDecodeError
    {
        ImageDecodeErrorCode code = ImageDecodeErrorCode::DecodeFailed;
        /// A sentence for the log: what was wrong, with the decoder's own reason where it gave one
        std::string message;
    };

    /// A short name for the code ("DecodeFailed")
    [[nodiscard]] std::string ToString(ImageDecodeErrorCode code);

    /**
     * Decodes a PNG (8 or 16 bits per channel, any colour type including palette), JPEG (baseline and
     * progressive), TGA or BMP image held in memory. The result is always 8-bit RGBA (16-bit channels are
     * reduced to 8), with rows ordered as options.flipY says.
     *
     * The header is read first: an image wider or taller than the dimension cap, or whose decoded pixels
     * would exceed the byte cap, is rejected before any pixel memory is allocated. Truncated or corrupt data
     * is an error, never a crash. Thread-safe: no global decoder state is used.
     */
    [[nodiscard]] std::expected<ImportedImage, ImageDecodeError> DecodeImage(std::span<const std::uint8_t> data,
                                                                            const ImageDecodeOptions &options = {});
}
