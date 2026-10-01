#include "SfntValidation.hpp"

#include <cstdint>
#include <cstring>
#include <format>
#include <utility>

namespace N2Engine::Text::Detail
{
    namespace
    {
        std::uint16_t ReadU16(const unsigned char *p)
        {
            return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
        }

        std::uint32_t ReadU32(const unsigned char *p)
        {
            return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
        }

        bool HasTag(const unsigned char *p, const char (&tag)[5])
        {
            return std::memcmp(p, tag, 4) == 0;
        }

        // The sfnt versions stb_truetype accepts as a single font
        bool IsSingleFont(const unsigned char *p)
        {
            static constexpr unsigned char trueType1[4] = {'1', 0, 0, 0};
            static constexpr unsigned char openType1[4] = {0, 1, 0, 0};
            return std::memcmp(p, trueType1, 4) == 0 || std::memcmp(p, openType1, 4) == 0 || HasTag(p, "typ1") ||
                   HasTag(p, "OTTO") || HasTag(p, "true");
        }

        // Backends are handed a pointer, not a size, and trust the table directory. Checking that every
        // table lies inside the buffer first stops a truncated file from sending them past the end.
        bool TableDirectoryFits(const std::span<const unsigned char> data, const std::size_t fontStart, std::string &error)
        {
            if (fontStart > data.size() || data.size() - fontStart < 12)
            {
                error = "font data is too short for an sfnt header";
                return false;
            }
            const unsigned char *header = data.data() + fontStart;
            const std::size_t tableCount = ReadU16(header + 4);
            if (fontStart + 12 + tableCount * 16 > data.size())
            {
                error = "font table directory runs past the end of the data";
                return false;
            }
            for (std::size_t i = 0; i < tableCount; ++i)
            {
                const unsigned char *record = header + 12 + i * 16;
                const std::uint64_t offset = ReadU32(record + 8);
                const std::uint64_t length = ReadU32(record + 12);
                if (offset + length > data.size())
                {
                    error = std::format("font table {} runs past the end of the data", i);
                    return false;
                }
            }
            return true;
        }

        struct TableSpan
        {
            std::size_t offset = 0;
            std::size_t length = 0;
        };

        // The first table with this tag, as stb_truetype finds it (the directory must already fit the data)
        std::optional<TableSpan> FindTable(const std::span<const unsigned char> data, const std::size_t fontStart,
                                           const char (&tag)[5])
        {
            const unsigned char *header = data.data() + fontStart;
            const std::size_t tableCount = ReadU16(header + 4);
            for (std::size_t i = 0; i < tableCount; ++i)
            {
                const unsigned char *record = header + 12 + i * 16;
                if (HasTag(record, tag))
                {
                    return TableSpan{ReadU32(record + 8), ReadU32(record + 12)};
                }
            }
            return std::nullopt;
        }

        // stb_truetype reads these tables at fixed offsets and indexes loca and hmtx by glyph id without
        // checking their lengths. Checking the sizes and the loca offsets up front turns a corrupt or cut-down
        // table into a load error instead of a read past the table (or the buffer).
        bool TablesAreConsistent(const std::span<const unsigned char> data, const std::size_t fontStart, std::string &error)
        {
            const auto fail = [&error](std::string reason)
            {
                error = std::move(reason);
                return false;
            };
            const unsigned char *bytes = data.data();

            const auto head = FindTable(data, fontStart, "head");
            if (!head || head->length < 54)
                return fail("the head table is missing or shorter than 54 bytes");
            const auto indexToLocFormat = static_cast<std::int16_t>(ReadU16(bytes + head->offset + 50));
            if (indexToLocFormat != 0 && indexToLocFormat != 1)
                return fail(std::format("head.indexToLocFormat is {} (must be 0 or 1)", indexToLocFormat));

            const auto maxp = FindTable(data, fontStart, "maxp");
            if (!maxp || maxp->length < 6)
                return fail("the maxp table is missing or too short");
            const std::size_t glyphCount = ReadU16(bytes + maxp->offset + 4);
            if (glyphCount < 1)
                return fail("the font has no glyphs (maxp.numGlyphs is 0)");

            const auto hhea = FindTable(data, fontStart, "hhea");
            if (!hhea || hhea->length < 36)
                return fail("the hhea table is missing or shorter than 36 bytes");
            const std::size_t hMetricCount = ReadU16(bytes + hhea->offset + 34);
            if (hMetricCount < 1 || hMetricCount > glyphCount)
                return fail(std::format("hhea.numberOfHMetrics is {} (must be 1 to {})", hMetricCount, glyphCount));

            const auto hmtx = FindTable(data, fontStart, "hmtx");
            if (!hmtx || hmtx->length < 4 * hMetricCount + 2 * (glyphCount - hMetricCount))
                return fail("the hmtx table is missing or too short for the glyph count");

            const auto glyf = FindTable(data, fontStart, "glyf");
            if (!glyf)
            {
                if (!FindTable(data, fontStart, "CFF "))
                    return fail("the font has no glyf or CFF outlines");
                return true; // CFF outlines have no loca table
            }

            const auto loca = FindTable(data, fontStart, "loca");
            const std::size_t entrySize = indexToLocFormat == 0 ? 2 : 4;
            if (!loca || loca->length < (glyphCount + 1) * entrySize)
                return fail("the loca table is missing or too short for the glyph count");
            std::size_t previous = 0;
            for (std::size_t i = 0; i <= glyphCount; ++i)
            {
                const unsigned char *entry = bytes + loca->offset + i * entrySize;
                const std::size_t offset = entrySize == 2 ? std::size_t{ReadU16(entry)} * 2 : std::size_t{ReadU32(entry)};
                if (offset < previous || offset > glyf->length)
                    return fail(std::format("loca entry {} is out of order or past the end of glyf", i));
                previous = offset;
            }
            return true;
        }
    }

    std::optional<std::size_t> FindFirstFontOffset(const std::span<const unsigned char> data)
    {
        if (data.size() < 4)
        {
            return std::nullopt;
        }
        if (IsSingleFont(data.data()))
        {
            return 0;
        }
        if (HasTag(data.data(), "ttcf") && data.size() >= 16)
        {
            const std::uint32_t version = ReadU32(data.data() + 4);
            const auto fontCount = static_cast<std::int32_t>(ReadU32(data.data() + 8));
            if ((version == 0x00010000 || version == 0x00020000) && fontCount > 0)
            {
                return ReadU32(data.data() + 12);
            }
        }
        return std::nullopt;
    }

    bool ValidateSfnt(const std::span<const unsigned char> data, const std::size_t fontStart, std::string &error)
    {
        return TableDirectoryFits(data, fontStart, error) && TablesAreConsistent(data, fontStart, error);
    }
}
