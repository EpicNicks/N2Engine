#include "text/TextLayout.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "text/SdfFont.hpp"
#include "text/Shaper.hpp"
#include "text/Utf8.hpp"

namespace N2Engine::Text
{
    namespace
    {
        // Spaces a line may break after, which don't count towards its width when trailing. No-break
        // space (U+00A0) is deliberately not one.
        bool IsBreakingSpace(const char32_t c)
        {
            return c == U' ' || c == U'\t';
        }

        bool IsHyphen(const char32_t c)
        {
            return c == U'-' || c == U'\u2010';
        }

        bool IsIgnoredControl(const char32_t c)
        {
            return (c < 0x20 && c != U'\t') || (c >= 0x7F && c <= 0x9F);
        }

        // One paragraph's shaped glyphs (the text between line breaks) and its byte range
        struct Paragraph
        {
            std::vector<ShapedGlyph> glyphs;
            std::uint32_t byteBegin = 0;
            std::uint32_t byteEnd = 0;
        };

        class LineBuilder
        {
        public:
            LineBuilder(const SdfFont &font, const LayoutOptions &options, TextLayout &layout)
                : _atlas(font.GetAtlas()), _options(options), _layout(layout)
            {
                _letterSpacing = options.letterSpacing * options.fontSize;
                const AtlasGlyph *space = _atlas.FindCodepoint(U' ');
                const float spaceWidth = (space != nullptr ? space->advance : 0.5f) * options.fontSize;
                _tabStop = std::max(0.0f, options.tabWidth) * spaceWidth;
            }

            void AddParagraph(const Paragraph &paragraph)
            {
                const std::vector<ShapedGlyph> &glyphs = paragraph.glyphs;
                const std::size_t count = glyphs.size();
                const bool wrap = _options.maxWidth > 0.0f;
                const float limit = _options.maxWidth * (1.0f + 1e-5f) + 1e-6f; // float slack

                std::size_t start = 0;
                while (true)
                {
                    std::size_t end = count;
                    if (wrap)
                    {
                        float pen = 0.0f;
                        std::size_t lastBreak = start; // == start: no break opportunity yet
                        for (std::size_t i = start; i < count; ++i)
                        {
                            const ShapedGlyph &glyph = glyphs[i];
                            const bool space = IsBreakingSpace(glyph.codepoint);
                            if (!space && i > start && pen + glyph.xAdvance * _options.fontSize > limit)
                            {
                                // Break after the last space/hyphen, or mid-word if the word fills the line
                                end = lastBreak > start ? lastBreak : i;
                                break;
                            }
                            pen = Advance(glyph, pen);
                            if (space || IsHyphen(glyph.codepoint))
                            {
                                lastBreak = i + 1;
                            }
                        }
                    }

                    EmitLine(paragraph, start, end);
                    if (end >= count)
                    {
                        break;
                    }
                    start = end;
                }
            }

        private:
            float Advance(const ShapedGlyph &glyph, const float pen) const
            {
                if (glyph.codepoint == U'\t' && _tabStop > 0.0f)
                {
                    return (std::floor(pen / _tabStop + 1e-4f) + 1.0f) * _tabStop;
                }
                return pen + glyph.xAdvance * _options.fontSize + _letterSpacing;
            }

            // Adds glyphs [start, end) as a line, x from the line start and y from the baseline (LayoutText
            // aligns them once every line is known)
            void EmitLine(const Paragraph &paragraph, const std::size_t start, const std::size_t end)
            {
                const std::vector<ShapedGlyph> &glyphs = paragraph.glyphs;
                LineInfo line;
                line.firstQuad = _layout.quads.size();
                line.byteBegin = start < glyphs.size() ? glyphs[start].cluster : paragraph.byteBegin;
                line.byteEnd = end < glyphs.size() ? glyphs[end].cluster : paragraph.byteEnd;

                const float size = _options.fontSize;
                float pen = 0.0f;
                float visibleEnd = 0.0f;
                for (std::size_t i = start; i < end; ++i)
                {
                    const ShapedGlyph &glyph = glyphs[i];
                    if (!IsBreakingSpace(glyph.codepoint))
                    {
                        const AtlasGlyph *entry = glyph.missing ? nullptr : _atlas.FindGlyph(glyph.glyph);
                        if (entry == nullptr)
                        {
                            entry = _atlas.GetFallbackGlyph();
                        }
                        if (entry != nullptr && entry->HasBitmap())
                        {
                            const float originX = pen + glyph.xOffset * size;
                            const float originY = glyph.yOffset * size;
                            GlyphQuad quad;
                            quad.position.minX = originX + entry->plane.minX * size;
                            quad.position.maxX = originX + entry->plane.maxX * size;
                            quad.position.minY = originY + entry->plane.minY * size;
                            quad.position.maxY = originY + entry->plane.maxY * size;
                            quad.uv = entry->uv;
                            quad.codepoint = glyph.codepoint;
                            quad.cluster = glyph.cluster;
                            _layout.quads.push_back(quad);
                        }
                        visibleEnd = pen + glyph.xAdvance * size;
                    }
                    pen = Advance(glyph, pen);
                }

                line.quadCount = _layout.quads.size() - line.firstQuad;
                line.width = std::max(0.0f, visibleEnd);
                _layout.lines.push_back(line);
            }

            const FontAtlas &_atlas;
            const LayoutOptions &_options;
            TextLayout &_layout;
            float _letterSpacing = 0.0f;
            float _tabStop = 0.0f;
        };
    }

    TextLayout LayoutText(const SdfFont &font, const std::string_view utf8, const LayoutOptions &options)
    {
        TextLayout layout;
        if (utf8.empty())
        {
            return layout;
        }

        const IShaper &shaper = options.shaper != nullptr ? *options.shaper : GetBasicShaper();
        LineBuilder builder(font, options, layout);

        // Split into paragraphs at line breaks, shape each, and wrap it into lines
        std::u32string run;
        std::vector<std::uint32_t> clusters;
        Paragraph paragraph;
        const auto flush = [&](const std::uint32_t byteEnd)
        {
            paragraph.glyphs.clear();
            paragraph.byteEnd = byteEnd;
            shaper.Shape(font, run, clusters, paragraph.glyphs);
            for (const ShapedGlyph &glyph : paragraph.glyphs)
            {
                if (glyph.missing && std::ranges::find(layout.missingCodepoints, glyph.codepoint) == layout.missingCodepoints.end())
                {
                    layout.missingCodepoints.push_back(glyph.codepoint);
                }
            }
            builder.AddParagraph(paragraph);
            run.clear();
            clusters.clear();
        };

        std::size_t position = 0;
        while (position < utf8.size())
        {
            const auto byte = static_cast<std::uint32_t>(position);
            const char32_t c = DecodeUtf8Next(utf8, position);
            if (c == U'\n')
            {
                // A "\r" just before is dropped below as a control character, so "\r\n" is one break
                flush(byte);
                paragraph.byteBegin = static_cast<std::uint32_t>(position);
                continue;
            }
            if (IsIgnoredControl(c))
            {
                continue;
            }
            run.push_back(c);
            clusters.push_back(byte);
        }
        flush(static_cast<std::uint32_t>(utf8.size()));

        if (!layout.missingCodepoints.empty())
        {
            font.ReportMissingGlyph(layout.missingCodepoints.front());
        }

        // Vertical placement: font metrics scaled to layout units
        const FontMetrics &metrics = font.GetMetrics();
        const float scale = options.fontSize / static_cast<float>(std::max(1, metrics.unitsPerEm));
        const float ascent = static_cast<float>(metrics.ascent) * scale;
        const float descent = static_cast<float>(metrics.descent) * scale;
        layout.lineHeight = static_cast<float>(metrics.ascent - metrics.descent + metrics.lineGap) * scale * options.lineSpacing;

        const float blockHeight = (ascent - descent) + static_cast<float>(layout.lines.size() - 1) * layout.lineHeight;
        float top = 0.0f;
        switch (options.verticalAlign)
        {
        case VerticalAlign::Top:
            top = 0.0f;
            break;
        case VerticalAlign::Middle:
            top = blockHeight * 0.5f;
            break;
        case VerticalAlign::Bottom:
            top = blockHeight;
            break;
        case VerticalAlign::Baseline:
            top = ascent;
            break;
        }

        float minX = 0.0f;
        float maxX = 0.0f;
        for (std::size_t i = 0; i < layout.lines.size(); ++i)
        {
            LineInfo &line = layout.lines[i];
            switch (options.horizontalAlign)
            {
            case HorizontalAlign::Left:
                line.x = 0.0f;
                break;
            case HorizontalAlign::Center:
                line.x = -line.width * 0.5f;
                break;
            case HorizontalAlign::Right:
                line.x = -line.width;
                break;
            }
            line.baseline = top - ascent - static_cast<float>(i) * layout.lineHeight;

            for (std::size_t q = line.firstQuad; q < line.firstQuad + line.quadCount; ++q)
            {
                Rect &rect = layout.quads[q].position;
                rect.minX += line.x;
                rect.maxX += line.x;
                rect.minY += line.baseline;
                rect.maxY += line.baseline;
            }

            if (i == 0)
            {
                minX = line.x;
                maxX = line.x + line.width;
            }
            else
            {
                minX = std::min(minX, line.x);
                maxX = std::max(maxX, line.x + line.width);
            }
        }

        layout.bounds = Rect{minX, top - blockHeight, maxX, top};
        return layout;
    }
}
