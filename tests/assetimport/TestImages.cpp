// stb_image_write is compiled here, and only here, for the tests (it is never part of the engine)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image/stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "TestImages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace AssetImportTests
{
    namespace
    {
        std::uint32_t Crc32(const std::uint8_t *data, const std::size_t size, std::uint32_t crc = 0xFFFFFFFFu)
        {
            static const std::array<std::uint32_t, 256> table = []
            {
                std::array<std::uint32_t, 256> t{};
                for (std::uint32_t n = 0; n < 256; ++n)
                {
                    std::uint32_t c = n;
                    for (int k = 0; k < 8; ++k)
                    {
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    }
                    t[n] = c;
                }
                return t;
            }();
            for (std::size_t i = 0; i < size; ++i)
            {
                crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
            }
            return crc;
        }

        std::uint32_t Adler32(const std::vector<std::uint8_t> &data)
        {
            std::uint32_t a = 1;
            std::uint32_t b = 0;
            for (const std::uint8_t byte : data)
            {
                a = (a + byte) % 65521u;
                b = (b + a) % 65521u;
            }
            return (b << 16) | a;
        }

        void PutU32BE(std::vector<std::uint8_t> &out, const std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value >> 24));
            out.push_back(static_cast<std::uint8_t>(value >> 16));
            out.push_back(static_cast<std::uint8_t>(value >> 8));
            out.push_back(static_cast<std::uint8_t>(value));
        }

        void PutU32LE(std::vector<std::uint8_t> &out, const std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value));
            out.push_back(static_cast<std::uint8_t>(value >> 8));
            out.push_back(static_cast<std::uint8_t>(value >> 16));
            out.push_back(static_cast<std::uint8_t>(value >> 24));
        }

        void PutU16LE(std::vector<std::uint8_t> &out, const std::uint16_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value));
            out.push_back(static_cast<std::uint8_t>(value >> 8));
        }

        void PutChunk(std::vector<std::uint8_t> &out, const std::string_view type, const std::vector<std::uint8_t> &data)
        {
            PutU32BE(out, static_cast<std::uint32_t>(data.size()));
            const std::size_t typeStart = out.size();
            out.insert(out.end(), type.begin(), type.end());
            out.insert(out.end(), data.begin(), data.end());
            PutU32BE(out, Crc32(out.data() + typeStart, out.size() - typeStart) ^ 0xFFFFFFFFu);
        }

        std::uint32_t SamplesPerPixel(const PngColorType type)
        {
            switch (type)
            {
            case PngColorType::Grey: return 1;
            case PngColorType::Rgb: return 3;
            case PngColorType::Palette: return 1;
            case PngColorType::GreyAlpha: return 2;
            case PngColorType::Rgba: return 4;
            }
            return 0;
        }

        void AppendBytes(void *context, void *data, const int size)
        {
            auto *out = static_cast<std::vector<std::uint8_t> *>(context);
            const auto *bytes = static_cast<const std::uint8_t *>(data);
            out->insert(out->end(), bytes, bytes + size);
        }
    }

    std::vector<std::uint8_t> MakePng(const PngSpec &spec)
    {
        const std::size_t rowBytes =
            static_cast<std::size_t>(spec.width) * SamplesPerPixel(spec.colorType) * (spec.bitDepth == 16 ? 2u : 1u);
        if (spec.samples.size() != rowBytes * spec.height)
        {
            throw std::invalid_argument("PngSpec: the sample data doesn't match the size");
        }

        std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

        std::vector<std::uint8_t> header;
        PutU32BE(header, spec.width);
        PutU32BE(header, spec.height);
        header.push_back(spec.bitDepth);
        header.push_back(static_cast<std::uint8_t>(spec.colorType));
        header.push_back(0); // deflate
        header.push_back(0); // adaptive filtering
        header.push_back(0); // not interlaced
        PutChunk(png, "IHDR", header);

        if (!spec.palette.empty())
        {
            PutChunk(png, "PLTE", spec.palette);
        }
        if (!spec.transparency.empty())
        {
            PutChunk(png, "tRNS", spec.transparency);
        }

        // Each row: filter type 0 (none), then its samples
        std::vector<std::uint8_t> raw;
        for (std::uint32_t row = 0; row < spec.height; ++row)
        {
            raw.push_back(0);
            const auto first = spec.samples.begin() + static_cast<std::ptrdiff_t>(row * rowBytes);
            raw.insert(raw.end(), first, first + static_cast<std::ptrdiff_t>(rowBytes));
        }

        // zlib: header 78 01, stored deflate blocks of up to 65535 bytes, Adler-32
        std::vector<std::uint8_t> zlib = {0x78, 0x01};
        std::size_t offset = 0;
        do
        {
            const std::size_t length = std::min<std::size_t>(raw.size() - offset, 65535);
            const bool last = offset + length == raw.size();
            zlib.push_back(last ? 1 : 0);
            PutU16LE(zlib, static_cast<std::uint16_t>(length));
            PutU16LE(zlib, static_cast<std::uint16_t>(~length & 0xFFFFu));
            zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                        raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
            offset += length;
        } while (offset < raw.size());
        PutU32BE(zlib, Adler32(raw));
        PutChunk(png, "IDAT", zlib);

        PutChunk(png, "IEND", {});
        return png;
    }

    std::vector<std::uint8_t> MakeBmpHeader(const std::int32_t width, const std::int32_t height)
    {
        std::vector<std::uint8_t> bmp = {'B', 'M'};
        PutU32LE(bmp, 54);  // file size (not checked)
        PutU32LE(bmp, 0);   // reserved
        PutU32LE(bmp, 54);  // pixel data offset
        PutU32LE(bmp, 40);  // BITMAPINFOHEADER
        PutU32LE(bmp, static_cast<std::uint32_t>(width));
        PutU32LE(bmp, static_cast<std::uint32_t>(height));
        PutU16LE(bmp, 1);   // planes
        PutU16LE(bmp, 24);  // bits per pixel
        PutU32LE(bmp, 0);   // BI_RGB
        PutU32LE(bmp, 0);   // image size
        PutU32LE(bmp, 2835); // 72 dpi
        PutU32LE(bmp, 2835);
        PutU32LE(bmp, 0);   // colours used
        PutU32LE(bmp, 0);   // important colours
        return bmp;
    }

    std::vector<std::uint8_t> EncodePngStb(const int width, const int height, const int components,
                                           const std::span<const std::uint8_t> pixels)
    {
        std::vector<std::uint8_t> out;
        if (!stbi_write_png_to_func(AppendBytes, &out, width, height, components, pixels.data(), width * components))
        {
            throw std::runtime_error("stbi_write_png_to_func failed");
        }
        return out;
    }

    std::vector<std::uint8_t> EncodeJpgStb(const int width, const int height, const int components,
                                           const std::span<const std::uint8_t> pixels, const int quality)
    {
        std::vector<std::uint8_t> out;
        if (!stbi_write_jpg_to_func(AppendBytes, &out, width, height, components, pixels.data(), quality))
        {
            throw std::runtime_error("stbi_write_jpg_to_func failed");
        }
        return out;
    }

    std::vector<std::uint8_t> EncodeTgaStb(const int width, const int height, const int components,
                                           const std::span<const std::uint8_t> pixels, const bool rle)
    {
        std::vector<std::uint8_t> out;
        stbi_write_tga_with_rle = rle ? 1 : 0;
        const int ok = stbi_write_tga_to_func(AppendBytes, &out, width, height, components, pixels.data());
        stbi_write_tga_with_rle = 1;
        if (!ok)
        {
            throw std::runtime_error("stbi_write_tga_to_func failed");
        }
        return out;
    }

    std::vector<std::uint8_t> EncodeBmpStb(const int width, const int height, const int components,
                                           const std::span<const std::uint8_t> pixels)
    {
        std::vector<std::uint8_t> out;
        if (!stbi_write_bmp_to_func(AppendBytes, &out, width, height, components, pixels.data()))
        {
            throw std::runtime_error("stbi_write_bmp_to_func failed");
        }
        return out;
    }
}
