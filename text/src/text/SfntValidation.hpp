#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace N2Engine::Text::Detail
{
    // Structural checks on a TrueType/OpenType (sfnt) file, shared by every font backend so they all
    // accept and reject the same data. stb_truetype trusts the table directory and the table sizes;
    // FreeType is more defensive, but running the same checks keeps the backends interchangeable.

    /// A table's place in the font data, in bytes
    struct SfntTable
    {
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    /// Where the first font in the data starts: 0 for a plain .ttf/.otf, the first entry's offset for a
    /// collection (.ttc), or nullopt if the data isn't an sfnt font. Mirrors stbtt_GetFontOffsetForIndex(0).
    [[nodiscard]] std::optional<std::size_t> FindFirstFontOffset(std::span<const unsigned char> data);

    /// True if the table directory at fontStart fits the data, every table lies inside it, and the tables
    /// the backends index without bounds checks (head, maxp, hhea, hmtx, loca/glyf or CFF) are present
    /// and consistent. Otherwise false, with the reason in error.
    [[nodiscard]] bool ValidateSfnt(std::span<const unsigned char> data, std::size_t fontStart, std::string &error);

    /// The first table with this tag, as stb_truetype finds it. Only call it after ValidateSfnt has accepted
    /// the data at fontStart: it trusts the table directory, and every table it returns lies inside the data.
    [[nodiscard]] std::optional<SfntTable> FindSfntTable(std::span<const unsigned char> data, std::size_t fontStart,
                                                         const char (&tag)[5]);
}
