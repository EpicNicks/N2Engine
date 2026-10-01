#include "text/Utf8.hpp"

#include <cstdint>

namespace N2Engine::Text
{
    char32_t DecodeUtf8Next(const std::string_view text, std::size_t &position)
    {
        if (position >= text.size())
        {
            return kReplacementCharacter;
        }

        const auto byteAt = [&text](const std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
        const std::uint8_t lead = byteAt(position);

        if (lead < 0x80)
        {
            ++position;
            return lead;
        }

        // Length and the allowed range of the second byte (Unicode table 3-7, well-formed UTF-8). The
        // narrowed ranges after E0, ED, F0 and F4 rule out overlong forms, surrogates and > U+10FFFF.
        int length = 0;
        std::uint8_t secondMin = 0x80;
        std::uint8_t secondMax = 0xBF;
        char32_t value = 0;
        if (lead >= 0xC2 && lead <= 0xDF)
        {
            length = 2;
            value = lead & 0x1F;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            length = 3;
            value = lead & 0x0F;
            if (lead == 0xE0)
                secondMin = 0xA0;
            else if (lead == 0xED)
                secondMax = 0x9F;
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            length = 4;
            value = lead & 0x07;
            if (lead == 0xF0)
                secondMin = 0x90;
            else if (lead == 0xF4)
                secondMax = 0x8F;
        }
        else
        {
            // A continuation byte with no lead, C0/C1 (always overlong) or F5..FF (never valid)
            ++position;
            return kReplacementCharacter;
        }

        std::size_t next = position + 1;
        for (int i = 1; i < length; ++i, ++next)
        {
            if (next >= text.size())
            {
                position = next; // truncated: the valid prefix is one replacement
                return kReplacementCharacter;
            }
            const std::uint8_t byte = byteAt(next);
            const std::uint8_t low = i == 1 ? secondMin : std::uint8_t{0x80};
            const std::uint8_t high = i == 1 ? secondMax : std::uint8_t{0xBF};
            if (byte < low || byte > high)
            {
                position = next; // the offending byte starts the next decode
                return kReplacementCharacter;
            }
            value = (value << 6) | (byte & 0x3F);
        }

        position = next;
        return value;
    }

    std::u32string DecodeUtf8(const std::string_view text)
    {
        std::u32string result;
        result.reserve(text.size());
        std::size_t position = 0;
        while (position < text.size())
        {
            result.push_back(DecodeUtf8Next(text, position));
        }
        return result;
    }

    std::string EncodeUtf8(const std::u32string_view codepoints)
    {
        std::string result;
        result.reserve(codepoints.size());
        for (char32_t c : codepoints)
        {
            if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            {
                c = kReplacementCharacter;
            }

            if (c < 0x80)
            {
                result.push_back(static_cast<char>(c));
            }
            else if (c < 0x800)
            {
                result.push_back(static_cast<char>(0xC0 | (c >> 6)));
                result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
            else if (c < 0x10000)
            {
                result.push_back(static_cast<char>(0xE0 | (c >> 12)));
                result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
            else
            {
                result.push_back(static_cast<char>(0xF0 | (c >> 18)));
                result.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        return result;
    }
}
