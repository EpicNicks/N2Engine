#include "text/RichText.hpp"

#include <algorithm>

#include <cmath>
#include <cstddef>
#include <optional>


#include "text/Utf8.hpp"

namespace N2Engine::Text
{
    namespace
    {
        /// No recognised tag is longer than this, brackets included
        constexpr std::size_t kMaxTagLength = 32;
        constexpr std::string_view kNoParseEnd = "</noparse>";

        enum class TagName
        {
            Bold,
            Italic,
            Color,
            Size,
            NoParse
        };

        struct Tag
        {
            TagName name;
            bool closing = false;
            std::optional<std::string_view> value;
            std::size_t length = 0; // bytes, brackets included
        };

        char Lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

        bool SameWord(const std::string_view a, const std::string_view b)
        {
            return a.size() == b.size() &&
                   std::ranges::equal(a, b, [](const char x, const char y) { return Lower(x) == Lower(y); });
        }

        std::optional<TagName> NameOf(const std::string_view word)
        {
            if (SameWord(word, "b"))
            {
                return TagName::Bold;
            }
            if (SameWord(word, "i"))
            {
                return TagName::Italic;
            }
            if (SameWord(word, "color"))
            {
                return TagName::Color;
            }
            if (SameWord(word, "size"))
            {
                return TagName::Size;
            }
            if (SameWord(word, "noparse"))
            {
                return TagName::NoParse;
            }
            return std::nullopt;
        }

        /// The tag that starts at text[0] == '<', when it is well formed (the value is checked by whoever uses it)
        std::optional<Tag> ReadTag(const std::string_view text)
        {
            const std::size_t end = text.substr(0, kMaxTagLength).find('>');
            if (end == std::string_view::npos || end < 2)
            {
                return std::nullopt;
            }
            std::string_view inside = text.substr(1, end - 1);
            bool closing = false;
            if (inside.front() == '/')
            {
                closing = true;
                inside.remove_prefix(1);
            }
            std::optional<std::string_view> value;
            if (const std::size_t equals = inside.find('='); equals != std::string_view::npos)
            {
                std::string_view raw = inside.substr(equals + 1);
                if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
                {
                    raw = raw.substr(1, raw.size() - 2);
                }
                value = raw;
                inside = inside.substr(0, equals);
            }
            const std::optional<TagName> name = NameOf(inside);
            if (!name)
            {
                return std::nullopt;
            }
            const bool takesValue = !closing && (*name == TagName::Color || *name == TagName::Size);
            if (takesValue != value.has_value())
            {
                return std::nullopt;
            }
            return Tag{*name, closing, value, end + 1};
        }

        int HexDigit(const char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            const char lower = Lower(c);
            return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
        }

        struct Rgba
        {
            float r, g, b, a;
        };

        std::optional<Rgba> ReadColor(const std::string_view value)
        {
            if (!value.empty() && value.front() == '#')
            {
                const std::string_view digits = value.substr(1);
                if (digits.size() != 3 && digits.size() != 6 && digits.size() != 8)
                {
                    return std::nullopt;
                }
                int parts[8] = {};
                for (std::size_t i = 0; i < digits.size(); ++i)
                {
                    parts[i] = HexDigit(digits[i]);
                    if (parts[i] < 0)
                    {
                        return std::nullopt;
                    }
                }
                const auto byte = [](const int high, const int low) { return static_cast<float>(high * 16 + low) / 255.0f; };
                if (digits.size() == 3)
                {
                    return Rgba{byte(parts[0], parts[0]), byte(parts[1], parts[1]), byte(parts[2], parts[2]), 1.0f};
                }
                return Rgba{byte(parts[0], parts[1]), byte(parts[2], parts[3]), byte(parts[4], parts[5]),
                            digits.size() == 8 ? byte(parts[6], parts[7]) : 1.0f};
            }
            struct Named
            {
                std::string_view name;
                Rgba color;
            };
            // Unity's values for the names it has
            static constexpr Named names[] = {
                {"black", {0.0f, 0.0f, 0.0f, 1.0f}},   {"blue", {0.0f, 0.0f, 1.0f, 1.0f}},
                {"cyan", {0.0f, 1.0f, 1.0f, 1.0f}},    {"gray", {0.5f, 0.5f, 0.5f, 1.0f}},
                {"green", {0.0f, 1.0f, 0.0f, 1.0f}},   {"grey", {0.5f, 0.5f, 0.5f, 1.0f}},
                {"magenta", {1.0f, 0.0f, 1.0f, 1.0f}}, {"orange", {1.0f, 0.5f, 0.0f, 1.0f}},
                {"purple", {0.625f, 0.125f, 0.9375f, 1.0f}}, {"red", {1.0f, 0.0f, 0.0f, 1.0f}},
                {"white", {1.0f, 1.0f, 1.0f, 1.0f}},   {"yellow", {1.0f, 0.92f, 0.016f, 1.0f}},
            };
            for (const Named &named : names)
            {
                if (SameWord(value, named.name))
                {
                    return named.color;
                }
            }
            return std::nullopt;
        }

        /// "24" or "150%": digits with at most one point, nothing else
        std::optional<float> ReadScale(std::string_view value, const float baseSize)
        {
            bool percent = false;
            if (!value.empty() && value.back() == '%')
            {
                percent = true;
                value.remove_suffix(1);
            }
            // By hand: std::from_chars for floating point is missing from some of the C++ libraries this builds with
            double number = 0.0;
            double place = 0.0; // 0 before the point
            bool digit = false;
            for (const char c : value)
            {
                if (c >= '0' && c <= '9')
                {
                    digit = true;
                    if (place == 0.0)
                    {
                        number = number * 10.0 + (c - '0');
                    }
                    else
                    {
                        number += (c - '0') * place;
                        place /= 10.0;
                    }
                }
                else if (c == '.' && place == 0.0)
                {
                    place = 0.1;
                }
                else
                {
                    return std::nullopt;
                }
            }
            if (!digit || !(number > 0.0))
            {
                return std::nullopt;
            }
            if (!percent && !(std::isfinite(baseSize) && baseSize > 0.0f))
            {
                return std::nullopt;
            }
            const double scale = percent ? number / 100.0 : number / static_cast<double>(baseSize);
            return static_cast<float>(std::min(scale, static_cast<double>(kMaxRichScale)));
        }
    }

    RichText ParseRichText(const std::string_view utf8, const float baseSize)
    {
        RichText result;
        result.text.reserve(utf8.size());

        // The open tags of each kind that carries a value, innermost last
        int bold = 0;
        int italic = 0;
        std::vector<Rgba> colors;
        std::vector<float> scales;

        const auto currentStyle = [&]()
        {
            RichStyle style;
            style.bold = bold > 0;
            style.italic = italic > 0;
            if (!colors.empty())
            {
                style.hasColor = true;
                style.colorR = colors.back().r;
                style.colorG = colors.back().g;
                style.colorB = colors.back().b;
                style.colorA = colors.back().a;
            }
            if (!scales.empty())
            {
                style.scale = scales.back();
            }
            return style;
        };
        // Shows the bytes [begin, end) of the input
        const auto show = [&](const std::size_t begin, const std::size_t end)
        {
            const std::string_view shown = utf8.substr(begin, end - begin);
            const RichStyle style = currentStyle();
            for (std::size_t position = 0; position < shown.size();)
            {
                static_cast<void>(DecodeUtf8Next(shown, position));
                result.styles.push_back(style);
            }
            result.text += shown;
        };

        std::size_t position = 0;
        while (position < utf8.size())
        {
            const std::size_t open = utf8.find('<', position);
            if (open != position)
            {
                const std::size_t end = open == std::string_view::npos ? utf8.size() : open;
                show(position, end);
                position = end;
                continue;
            }
            const std::optional<Tag> tag = ReadTag(utf8.substr(position));
            bool used = false;
            if (tag)
            {
                switch (tag->name)
                {
                case TagName::Bold:
                case TagName::Italic:
                {
                    int &depth = tag->name == TagName::Bold ? bold : italic;
                    if (!tag->closing)
                    {
                        ++depth;
                        used = true;
                    }
                    else if (depth > 0)
                    {
                        --depth;
                        used = true;
                    }
                    break;
                }
                case TagName::Color:
                    if (tag->closing)
                    {
                        used = !colors.empty();
                        if (used)
                        {
                            colors.pop_back();
                        }
                    }
                    else if (const std::optional<Rgba> color = ReadColor(*tag->value))
                    {
                        colors.push_back(*color);
                        used = true;
                    }
                    break;
                case TagName::Size:
                    if (tag->closing)
                    {
                        used = !scales.empty();
                        if (used)
                        {
                            scales.pop_back();
                        }
                    }
                    else if (const std::optional<float> scale = ReadScale(*tag->value, baseSize))
                    {
                        scales.push_back(*scale);
                        used = true;
                    }
                    break;
                case TagName::NoParse:
                    if (!tag->closing)
                    {
                        // Everything up to the closing tag, or the end, as it is
                        const std::size_t begin = position + tag->length;
                        std::size_t end = begin;
                        while (end < utf8.size() && !SameWord(utf8.substr(end, kNoParseEnd.size()), kNoParseEnd))
                        {
                            ++end;
                        }
                        show(begin, end);
                        position = std::min(utf8.size(), end + kNoParseEnd.size());
                        continue;
                    }
                    break; // a closing tag with nothing open is text
                }
            }
            if (used)
            {
                position += tag->length;
            }
            else
            {
                show(position, position + 1); // just the "<": what follows is read as usual
                ++position;
            }
        }
        return result;
    }
}
