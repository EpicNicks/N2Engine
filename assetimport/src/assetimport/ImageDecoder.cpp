// The one translation unit that compiles stb_image. STB_IMAGE_STATIC keeps every stb symbol private to this
// file, so no stb type or function leaks into the assetimport library's interface or clashes with another
// copy of stb_image linked into the same program.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
// Only the four formats texture assets accept (PSD, GIF, HDR, PIC and PNM are left out), from memory only
// (the engine reads files itself), with no float/HDR paths
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 16384
// stb's asserts are internal invariants. In Release they are compiled out so a bad file can't abort the
// program: stb's own checks reject malformed data, and an assert that would have fired just doesn't stop it.
// In Debug they stay on.
#ifdef NDEBUG
#define STBI_ASSERT(x) ((void)0)
#else
#include <cassert>
#define STBI_ASSERT(x) assert(x)
#endif

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image/stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "assetimport/ImageDecoder.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <utility>

namespace N2Engine::AssetImport
{
    static_assert(kMaxImageDimension == STBI_MAX_DIMENSIONS, "the dimension cap and stb_image's must agree");

    namespace
    {
        struct StbFree
        {
            void operator()(stbi_uc *pixels) const { stbi_image_free(pixels); }
        };

        std::string StbReason()
        {
            const char *reason = stbi_failure_reason();
            return reason && *reason ? reason : "unknown reason";
        }

        ImageDecodeError Error(const ImageDecodeErrorCode code, std::string message)
        {
            return ImageDecodeError{code, std::move(message)};
        }
    }

    std::string ToString(const ImageDecodeErrorCode code)
    {
        switch (code)
        {
        case ImageDecodeErrorCode::EmptyInput: return "EmptyInput";
        case ImageDecodeErrorCode::InputTooLarge: return "InputTooLarge";
        case ImageDecodeErrorCode::UnrecognizedFormat: return "UnrecognizedFormat";
        case ImageDecodeErrorCode::DimensionsTooLarge: return "DimensionsTooLarge";
        case ImageDecodeErrorCode::DecodedSizeTooLarge: return "DecodedSizeTooLarge";
        case ImageDecodeErrorCode::DecodeFailed: return "DecodeFailed";
        }
        return "Unknown";
    }

    std::expected<ImportedImage, ImageDecodeError> DecodeImage(const std::span<const std::uint8_t> data,
                                                               const ImageDecodeOptions &options)
    {
        if (data.empty())
        {
            return std::unexpected(Error(ImageDecodeErrorCode::EmptyInput, "the image data is empty"));
        }
        if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return std::unexpected(Error(ImageDecodeErrorCode::InputTooLarge,
                                         std::format("the image data is {} bytes, over the 2 GiB limit", data.size())));
        }
        const int length = static_cast<int>(data.size());
        const std::uint32_t maxDimension = std::min(options.maxDimension, kMaxImageDimension);
        const std::size_t maxBytes = std::min(options.maxDecodedBytes, kMaxDecodedImageBytes);

        // The header alone: the size is checked before any pixel memory is allocated
        int width = 0;
        int height = 0;
        int fileChannels = 0;
        if (!stbi_info_from_memory(data.data(), length, &width, &height, &fileChannels))
        {
            return std::unexpected(Error(ImageDecodeErrorCode::UnrecognizedFormat,
                                         std::format("not a readable PNG, JPEG, TGA or BMP image ({})", StbReason())));
        }
        // A top-down BMP stores a negative height, and stbi_info passes it on as read (the decode itself
        // takes its absolute value)
        if (height < 0 && height != std::numeric_limits<int>::min())
        {
            height = -height;
        }
        if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) > maxDimension ||
            static_cast<std::uint32_t>(height) > maxDimension)
        {
            return std::unexpected(Error(ImageDecodeErrorCode::DimensionsTooLarge,
                                         std::format("the image is {} x {}; each side must be from 1 to {} pixels",
                                                     width, height, maxDimension)));
        }
        // Both sides are at most 16384, so this can't overflow
        const std::size_t decodedBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        if (decodedBytes > maxBytes)
        {
            return std::unexpected(Error(ImageDecodeErrorCode::DecodedSizeTooLarge,
                                         std::format("the image is {} x {}, {} bytes decoded, over the {}-byte limit",
                                                     width, height, decodedBytes, maxBytes)));
        }

        int decodedWidth = 0;
        int decodedHeight = 0;
        int decodedChannels = 0;
        // Always 4 channels out; 16-bit images come back reduced to 8 bits
        const std::unique_ptr<stbi_uc, StbFree> pixels(
            stbi_load_from_memory(data.data(), length, &decodedWidth, &decodedHeight, &decodedChannels, 4));
        if (!pixels)
        {
            return std::unexpected(Error(ImageDecodeErrorCode::DecodeFailed,
                                         std::format("the {} x {} image can't be decoded ({})", width, height, StbReason())));
        }
        if (decodedWidth != width || decodedHeight != height)
        {
            // The header and the decode disagree: don't trust either
            return std::unexpected(Error(ImageDecodeErrorCode::DecodeFailed,
                                         std::format("the header says {} x {} but {} x {} was decoded", width, height,
                                                     decodedWidth, decodedHeight)));
        }

        ImportedImage image;
        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);
        image.channels = 4;
        image.sourceChannels = static_cast<std::uint32_t>(std::clamp(decodedChannels, 1, 4));
        image.pixels.resize(decodedBytes);

        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
        const std::uint8_t *source = pixels.get();
        for (std::size_t row = 0; row < static_cast<std::size_t>(height); ++row)
        {
            // stb returns the top row first; flipped, it becomes the last
            const std::size_t target = options.flipY ? static_cast<std::size_t>(height) - 1 - row : row;
            std::memcpy(image.pixels.data() + target * rowBytes, source + row * rowBytes, rowBytes);
        }
        return image;
    }
}
