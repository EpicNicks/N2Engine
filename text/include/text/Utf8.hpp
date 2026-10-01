#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace N2Engine::Text
{
    inline constexpr char32_t kReplacementCharacter = U'\uFFFD';

    /// Decodes the codepoint starting at text[position] and moves position past it.
    /// Never throws. An ill-formed sequence (a stray continuation byte, an overlong form, a surrogate,
    /// a value above U+10FFFF or a truncated sequence) gives U+FFFD and consumes its maximal valid
    /// prefix, at least one byte: the Unicode "maximal subpart" rule, so "\xE2\x82" then "A" decodes
    /// to U+FFFD, 'A'. Returns U+FFFD without moving if position is at or past the end.
    char32_t DecodeUtf8Next(std::string_view text, std::size_t &position);

    /// The whole string, one char32_t per codepoint (ill-formed bytes become U+FFFD as above)
    std::u32string DecodeUtf8(std::string_view text);

    /// UTF-8 for the codepoints. Surrogates and values above U+10FFFF are written as U+FFFD.
    std::string EncodeUtf8(std::u32string_view codepoints);
}
