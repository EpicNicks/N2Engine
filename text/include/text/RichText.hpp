#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::Text
{
    /// How rich text styles one character
    struct RichStyle
    {
        bool bold = false;
        bool italic = false;
        /// A <color> tag applies: colour is red, green, blue, alpha in 0..1
        bool hasColor = false;
        float colorR = 1.0f;
        float colorG = 1.0f;
        float colorB = 1.0f;
        float colorA = 1.0f;
        /// The size from a <size> tag, as a multiple of the base font size (1 without one)
        float scale = 1.0f;

        bool operator==(const RichStyle &) const = default;
    };

    /// A marked-up string taken apart: the characters that show, and one style for each of them
    struct RichText
    {
        /// The input with the recognised tags removed; every other byte is kept as it was
        std::string text;
        /// One per code point of `text` (ill-formed bytes count as DecodeUtf8 counts them)
        std::vector<RichStyle> styles;
    };

    /// The largest size a <size> tag gives, as a multiple of the base size; a larger one is this
    inline constexpr float kMaxRichScale = 100.0f;

    /**
     * Reads rich text markup: a small subset of Unity's TextMeshPro tags.
     *
     * - <b>...</b> bold, <i>...</i> italic
     * - <color=#RGB>, <color=#RRGGBB>, <color=#RRGGBBAA> or <color=name> ... </color>; the names are black, blue, cyan,
     *   gray, green, grey, magenta, orange, purple, red, white and yellow
     * - <size=N> (N in the units of `baseSize`) or <size=N%> (of the base size) ... </size>
     * - <noparse>...</noparse>: what is between is shown as it is
     *
     * Tag names, colour names and hex digits are read without regard to case, and a value may be in double quotes. Tags
     * nest; a closing tag ends the innermost open tag of its name, so overlapping tags work as written.
     *
     * Bad markup is never an error. Anything that is not exactly one of the tags above is ordinary text, shown as written:
     * an unknown name, a missing or unreadable value (or one on a tag that takes none), spaces inside the brackets, a
     * size that is not a positive number, or a closing tag with nothing of its name open. A tag left open applies to the
     * end of the text. A non-positive or non-finite `baseSize` makes every <size=N> unreadable.
     */
    [[nodiscard]] RichText ParseRichText(std::string_view utf8, float baseSize);
}
