#include "engine/ui/UIText.hpp"

#include <algorithm>
#include <cmath>

#include <text/Utf8.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/ui/QuadClip.hpp"
#include "engine/ui/RectTransform.hpp"

namespace N2Engine::UI
{
    UIText::UIText(GameObject &gameObject) : UIGraphic(gameObject)
    {
        // A label lets the pointer through by default (see the class comment)
        _raycastTarget = false;

        RegisterMember("text", _text);
        RegisterAssetRef("font", _font);
        RegisterMember("fontSize", _fontSize);
        RegisterMember("horizontalAlign", _horizontalAlign);
        RegisterMember("verticalAlign", _verticalAlign);
        RegisterMember("wrap", _wrap);
        RegisterMember("lineSpacing", _lineSpacing);
        RegisterMember("letterSpacing", _letterSpacing);
        RegisterMember("overflow", _overflow).Tooltip("What happens to text that doesn't fit the rect");
        RegisterMember("richText", _richText).Tooltip("Read <b>, <i>, <color> and <size> tags in the text");
        // Effects, one key each (a scene saved before effects existed loads with them all off)
        RegisterMember("outlineWidth", _effects.outlineWidth);
        RegisterMember("outlineColor", _effects.outlineColor);
        RegisterMember("shadowOffset", _effects.shadowOffset);
        RegisterMember("shadowColor", _effects.shadowColor);
        RegisterMember("shadowSoftness", _effects.shadowSoftness);
        RegisterMember("softness", _effects.softness);
        // The extra passes, a list of {color, offset, width, softness, order} (TextJson.hpp); a scene saved
        // before they existed has none
        RegisterMember("effectPasses", _effects.passes);
    }

    UIText::~UIText()
    {
        // Normally OnDestroy has released everything already. A component freed without it (its GameObject
        // never joined a scene) may outlive its renderer, so the renderer isn't called.
        _resources.Release(false);
    }

    std::shared_ptr<Text::Font> UIText::GetEffectiveFont() const
    {
        return _font ? _font : Text::Font::GetDefault();
    }

    Text::LayoutOptions UIText::GetLayoutOptions(const float rectWidth) const
    {
        Text::LayoutOptions options;
        options.fontSize = _fontSize;
        // A rect without width wraps nothing rather than breaking after every character
        options.maxWidth = _wrap && rectWidth > 0.0f ? rectWidth : 0.0f;
        options.horizontalAlign = _horizontalAlign;
        options.verticalAlign = _verticalAlign;
        options.lineSpacing = _lineSpacing;
        options.letterSpacing = _letterSpacing;
        return options;
    }

    const Text::TextLayout &UIText::GetLayout(const Rect &rect) const
    {
        const Shown *shown = ShownIn(rect);
        Text::LayoutOptions options = GetLayoutOptions(rect.width);
        if (shown)
        {
            options.metrics = shown->metrics;
        }
        return _layoutCache.Get(shown ? shown->text : _text, GetEffectiveFont(), options);
    }

    namespace
    {
        /// The per-byte metrics of `text` whose characters have these styles
        std::vector<Text::CharacterMetrics> MetricsFor(const std::string &text, const std::vector<Text::RichStyle> &styles)
        {
            std::vector<Text::CharacterMetrics> metrics(text.size());
            std::size_t character = 0;
            for (std::size_t position = 0; position < text.size() && character < styles.size(); ++character)
            {
                const std::size_t begin = position;
                static_cast<void>(Text::DecodeUtf8Next(text, position));
                const Text::RichStyle &style = styles[character];
                for (std::size_t byte = begin; byte < position; ++byte)
                {
                    metrics[byte] = Text::CharacterMetrics{style.scale, style.bold ? UIText::BoldOffsetEms : 0.0f};
                }
            }
            return metrics;
        }
    }

    UIText::Shown UIText::Parse() const
    {
        Text::RichText rich = Text::ParseRichText(_text, _fontSize);
        Shown parsed;
        parsed.source.resize(rich.styles.size());
        for (std::size_t i = 0; i < parsed.source.size(); ++i)
        {
            parsed.source[i] = static_cast<std::uint32_t>(i);
        }
        parsed.text = std::move(rich.text);
        parsed.styles = std::move(rich.styles);
        parsed.metrics = MetricsFor(parsed.text, parsed.styles);
        return parsed;
    }

    std::string UIText::GetPlainText() const
    {
        return _richText ? Text::ParseRichText(_text, _fontSize).text : _text;
    }

    Text::TextLayout UIText::Measure(const float rectWidth) const
    {
        const std::shared_ptr<Text::Font> font = GetEffectiveFont();
        if (!font)
        {
            return {};
        }
        Text::LayoutOptions options = GetLayoutOptions(rectWidth);
        if (!_richText)
        {
            return font->Layout(_text, options);
        }
        const Shown parsed = Parse();
        options.metrics = parsed.metrics;
        return font->Layout(parsed.text, options);
    }

    const UIText::Shown *UIText::ShownIn(const Rect &rect) const
    {
        const bool fits = _overflow == TextOverflow::Truncate || _overflow == TextOverflow::Ellipsis;
        if (!fits && !_richText)
        {
            return nullptr;
        }
        const std::shared_ptr<Text::Font> font = GetEffectiveFont();
        if (!font)
        {
            return nullptr;
        }
        Text::LayoutOptions options = GetLayoutOptions(rect.width);
        // The rect matters only to the modes that cut the text to it
        const float width = fits ? rect.width : 0.0f;
        const float height = fits ? rect.height : 0.0f;
        const float maxWidth = fits ? options.maxWidth : 0.0f;
        if (!_shown || _shown->font != font || _shown->richText != _richText || _shown->fontSize != options.fontSize ||
            _shown->letterSpacing != options.letterSpacing || _shown->lineSpacing != options.lineSpacing ||
            _shown->maxWidth != maxWidth || _shown->width != width || _shown->height != height ||
            _shown->overflow != _overflow || _shown->text != _text)
        {
            std::optional<Shown> result;
            if (_richText)
            {
                result = Parse();
            }
            if (fits)
            {
                if (result)
                {
                    options.metrics = result->metrics;
                }
                if (std::optional<FittedText> fitted =
                        FitText(*font, result ? result->text : _text, options, rect.width, rect.height, _overflow))
                {
                    Shown cut;
                    if (result)
                    {
                        // Each character keeps its style; an added one takes the style before it
                        Text::RichStyle last = result->styles.empty() ? Text::RichStyle{} : result->styles.front();
                        cut.styles.reserve(fitted->source.size());
                        for (const std::uint32_t source : fitted->source)
                        {
                            if (source < result->styles.size())
                            {
                                last = result->styles[source];
                            }
                            cut.styles.push_back(last);
                        }
                        cut.metrics = MetricsFor(fitted->text, cut.styles);
                    }
                    cut.text = std::move(fitted->text);
                    cut.source = std::move(fitted->source);
                    result = std::move(cut);
                }
            }
            _shown = ShownCache{_text,    font,  _richText, options.fontSize, options.letterSpacing,
                                options.lineSpacing, maxWidth, width, height, _overflow, std::move(result)};
            ++_shownVersion;
        }
        return _shown->result ? &*_shown->result : nullptr;
    }

    namespace
    {
        /// How many lines of `layout`, from the first, end within `height` of the block's top
        std::size_t LinesThatFit(const Text::TextLayout &layout, const float height, const float tolerance)
        {
            std::size_t count = 0;
            while (count < layout.lines.size() && layout.bounds.maxY - layout.lines[count].bottom <= height + tolerance)
            {
                ++count;
            }
            return count;
        }

        /// Rounding slack for "does it fit": a text sized exactly to its content fits
        float FitTolerance(const Text::LayoutOptions &options) { return 1e-3f * std::max(1.0f, options.fontSize); }
    }

    std::optional<UIText::FittedText> UIText::FitText(const Text::Font &font, const std::string &text,
                                                      const Text::LayoutOptions &options, const float width,
                                                      const float height, const TextOverflow overflow)
    {
        if ((overflow != TextOverflow::Truncate && overflow != TextOverflow::Ellipsis) || text.empty() || !font.IsLoaded())
        {
            return std::nullopt;
        }
        const Text::TextLayout full = font.Layout(text, options);
        if (full.lines.empty())
        {
            return std::nullopt;
        }
        const float tolerance = FitTolerance(options);
        const std::size_t count = full.lines.size();
        const std::size_t kept = std::min(count, LinesThatFit(full, height, tolerance));
        const auto tooWide = [&](const std::size_t line) { return full.lines[line].width > width + tolerance; };
        bool cuts = kept < count;
        for (std::size_t i = 0; i < kept; ++i)
        {
            cuts = cuts || tooWide(i);
        }
        if (!cuts)
        {
            return std::nullopt;
        }

        const bool ellipsis = overflow == TextOverflow::Ellipsis;
        const std::string mark = font.GetSdfFont().GetAtlas().FindCodepoint(U'\u2026') ? "\xE2\x80\xA6" : "...";
        Text::LayoutOptions oneLine = options;
        oneLine.maxWidth = 0.0f;
        // The width of a candidate for a line: `prefix`, the bytes of `text` from `begin`, then `suffix`, styled as the
        // byte before it
        std::vector<Text::CharacterMetrics> candidateMetrics;
        const auto widthOf = [&](const std::size_t begin, const std::string &prefix, const std::string &suffix)
        {
            const std::string candidate = prefix + suffix;
            if (!options.metrics.empty())
            {
                candidateMetrics.clear();
                for (std::size_t i = 0; i < candidate.size(); ++i)
                {
                    const std::size_t within = i < prefix.size() ? i : prefix.empty() ? 0 : prefix.size() - 1;
                    const std::size_t byte = begin + within;
                    candidateMetrics.push_back(byte < options.metrics.size() ? options.metrics[byte]
                                                                             : Text::CharacterMetrics{});
                }
                oneLine.metrics = candidateMetrics;
            }
            const Text::TextLayout layout = font.Layout(candidate, oneLine);
            return layout.lines.empty() ? 0.0f : layout.lines.front().width;
        };

        FittedText result;
        // The character index of each line's first byte, counted along as the lines go
        std::size_t countedBytes = 0;
        std::uint32_t countedCharacters = 0;
        const auto characterAt = [&](const std::size_t byte)
        {
            while (countedBytes < byte && countedBytes < text.size())
            {
                static_cast<void>(Text::DecodeUtf8Next(text, countedBytes));
                ++countedCharacters;
            }
            return countedCharacters;
        };

        for (std::size_t i = 0; i < kept; ++i)
        {
            const Text::LineInfo &line = full.lines[i];
            std::string_view content{text.data() + line.byteBegin, line.byteEnd - line.byteBegin};
            if (!content.empty() && content.back() == '\r')
            {
                content.remove_suffix(1);
            }
            // Where each character of the line ends, in bytes
            std::vector<std::size_t> ends{0};
            for (std::size_t position = 0; position < content.size();)
            {
                static_cast<void>(Text::DecodeUtf8Next(content, position));
                ends.push_back(position);
            }
            const std::size_t characters = ends.size() - 1;
            const bool wide = tooWide(i);
            const bool marked = ellipsis && (wide || (i + 1 == kept && kept < count));

            std::size_t keep = characters;
            std::string suffix = marked ? mark : std::string{};
            if (wide || marked)
            {
                const auto candidate = [&](const std::size_t n)
                {
                    std::string prefix(content.substr(0, ends[n]));
                    if (marked)
                    {
                        while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == '\t'))
                        {
                            prefix.pop_back();
                        }
                    }
                    return prefix;
                };
                if (widthOf(line.byteBegin, candidate(characters), suffix) > width + tolerance)
                {
                    // The most characters that fit, by halving (the width grows with the characters)
                    std::size_t low = 0;
                    std::size_t high = characters;
                    while (high - low > 1)
                    {
                        const std::size_t middle = low + (high - low) / 2;
                        (widthOf(line.byteBegin, candidate(middle), suffix) <= width + tolerance ? low : high) = middle;
                    }
                    keep = low;
                    if (keep == 0 && !suffix.empty() &&
                        widthOf(line.byteBegin, std::string{}, suffix) > width + tolerance)
                    {
                        suffix.clear(); // no room even for the mark
                    }
                }
            }

            std::string shown(content.substr(0, ends[keep]));
            std::size_t shownCharacters = keep;
            if (marked)
            {
                while (!shown.empty() && (shown.back() == ' ' || shown.back() == '\t'))
                {
                    shown.pop_back();
                    --shownCharacters;
                }
            }
            const std::uint32_t first = characterAt(line.byteBegin);
            for (std::size_t c = 0; c < shownCharacters; ++c)
            {
                result.source.push_back(first + static_cast<std::uint32_t>(c));
            }
            result.text += shown;
            for (std::size_t position = 0; position < suffix.size();)
            {
                static_cast<void>(Text::DecodeUtf8Next(suffix, position));
                result.source.push_back(NoCharacter);
            }
            result.text += suffix;
            if (i + 1 < kept)
            {
                result.text += '\n';
                result.source.push_back(NoCharacter);
            }
        }
        return result;
    }

    bool UIText::IsOverflowing() const
    {
        const std::shared_ptr<Text::Font> font = GetEffectiveFont();
        if (!font || _text.empty())
        {
            return false;
        }
        const Rect rect = GetCurrentRect();
        const Text::LayoutOptions options = GetLayoutOptions(rect.width);
        const Text::TextLayout full = Measure(rect.width);
        const float tolerance = FitTolerance(options);
        if (LinesThatFit(full, rect.height, tolerance) < full.lines.size())
        {
            return true;
        }
        return std::ranges::any_of(full.lines, [&](const Text::LineInfo &line) { return line.width > rect.width + tolerance; });
    }

    Math::Vector2 UIText::AnchorFor(const Rect &rect, const Text::HorizontalAlign horizontal,
                                    const Text::VerticalAlign vertical)
    {
        // Font::Layout places lines relative to x = 0: Left starts them there, Center centres them on it,
        // Right ends them there. Vertically, Top puts the first ascent at y = 0, Middle centres the block on
        // it, Bottom puts the last descent there, and Baseline the first baseline.
        float x = rect.XMin();
        switch (horizontal)
        {
        case Text::HorizontalAlign::Left:
            x = rect.XMin();
            break;
        case Text::HorizontalAlign::Center:
            x = rect.x + rect.width * 0.5f;
            break;
        case Text::HorizontalAlign::Right:
            x = rect.XMax();
            break;
        }

        float y = rect.YMax();
        switch (vertical)
        {
        case Text::VerticalAlign::Top:
            y = rect.YMax();
            break;
        case Text::VerticalAlign::Middle:
        case Text::VerticalAlign::Baseline:
            y = rect.y + rect.height * 0.5f;
            break;
        case Text::VerticalAlign::Bottom:
            y = rect.YMin();
            break;
        }
        return Math::Vector2{x, y};
    }

    Math::Vector2 UIText::GetAnchor(const Rect &rect) const
    {
        return AnchorFor(rect, _horizontalAlign, _verticalAlign);
    }

    Text::Rect UIText::GetBoundsIn(const Rect &rect) const
    {
        const Text::Rect &bounds = GetLayout(rect).bounds;
        if (bounds == Text::Rect{})
        {
            return bounds;
        }
        const Math::Vector2 anchor = GetAnchor(rect);
        return Text::Rect{bounds.minX + anchor.x, bounds.minY + anchor.y, bounds.maxX + anchor.x,
                          bounds.maxY + anchor.y};
    }

    Rect UIText::GetCurrentRect() const
    {
        if (const RectTransform *rectTransform = GetGameObject().GetComponent<RectTransform>())
        {
            return rectTransform->GetRect();
        }
        return _lastDrawnRect;
    }

    Math::Matrix<float, 4, 4> UIText::ModelMatrixFor(const Math::Vector2 &anchor)
    {
        Math::Matrix<float, 4, 4> model = Math::Matrix<float, 4, 4>::identity();
        model(0, 3) = anchor.x;
        model(1, 3) = anchor.y;
        return model;
    }

    namespace
    {
        /// The character index (code points of `text`, from 0) of each quad of `layout`; UIText::NoCharacter for a
        /// quad whose cluster is outside the text
        std::vector<std::uint32_t> CharacterOfQuads(const Text::TextLayout &layout, const std::string &text)
        {
            // The character index of each byte: lead bytes start a character, continuation bytes (10xxxxxx) don't
            std::vector<std::uint32_t> characterOf(text.size() + 1, 0);
            std::uint32_t character = 0;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                {
                    ++character;
                }
                characterOf[i] = character;
            }
            std::vector<std::uint32_t> result;
            result.reserve(layout.quads.size());
            for (const Text::GlyphQuad &quad : layout.quads)
            {
                result.push_back(quad.cluster < text.size() ? characterOf[quad.cluster] : UIText::NoCharacter);
            }
            return result;
        }

        bool SameColor(const Common::Color &a, const Common::Color &b)
        {
            // Component by component: the Color comparison allows an epsilon, which would skip a small step of a fade
            return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
        }

        bool SameOffset(const Math::Vector2 &a, const Math::Vector2 &b) { return a.x == b.x && a.y == b.y; }

        bool SameScale(const float a, const float b) { return a == b; }

        float ClampComponent(const float value)
        {
            return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f;
        }

        /**
         * The rules every per-character list follows (offsets, scales, colours): an index above MaxCharacterIndex is
         * ignored; setting the default past the end adds nothing; a list grows with defaults to reach `index`; trailing
         * defaults are dropped, so a list whose characters are all back to the default is empty again. True when the list
         * changed.
         */
        template <typename T, typename Same>
        bool SetSlot(std::vector<T> &values, const std::size_t index, const T &value, const T &defaultValue, Same same)
        {
            if (index > UIText::MaxCharacterIndex)
            {
                return false;
            }
            if (index >= values.size())
            {
                if (same(value, defaultValue))
                {
                    return false; // already that
                }
                values.resize(index + 1, defaultValue);
            }
            if (same(values[index], value))
            {
                return false;
            }
            values[index] = value;
            while (!values.empty() && same(values.back(), defaultValue))
            {
                values.pop_back();
            }
            return true;
        }

        template <typename T>
        bool ClearSlots(std::vector<T> &values)
        {
            if (values.empty())
            {
                return false;
            }
            values.clear();
            return true;
        }

        /// What rich text does to one glyph quad of the mesh
        struct QuadStyle
        {
            float slant = 0.0f; // x moves by this per unit above the baseline (italic)
            float baseline = 0.0f;
            float doubled = 0.0f; // drawn again this far to the right (bold)
        };

        /// `quads` (TextDrawing::BuildMesh: four vertices per quad) slanted and doubled as `styles` say, still four
        /// vertices per quad
        Renderer::Common::MeshData StyleMesh(const Renderer::Common::MeshData &quads, const std::vector<QuadStyle> &styles)
        {
            Renderer::Common::MeshData mesh;
            const auto add = [&mesh](const Renderer::Common::Vertex (&corners)[4])
            {
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                mesh.vertices.insert(mesh.vertices.end(), std::begin(corners), std::end(corners));
                for (const std::uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
                {
                    mesh.indices.push_back(base + index);
                }
            };
            for (std::size_t q = 0; q < styles.size() && (q + 1) * 4 <= quads.vertices.size(); ++q)
            {
                Renderer::Common::Vertex corners[4];
                for (std::size_t i = 0; i < 4; ++i)
                {
                    corners[i] = quads.vertices[q * 4 + i];
                    corners[i].position[0] += styles[q].slant * (corners[i].position[1] - styles[q].baseline);
                }
                add(corners);
                if (styles[q].doubled > 0.0f)
                {
                    for (Renderer::Common::Vertex &corner : corners)
                    {
                        corner.position[0] += styles[q].doubled;
                    }
                    add(corners);
                }
            }
            return mesh;
        }

        void MoveQuad(Text::Rect &p, const Math::Vector2 &o)
        {
            p.minX += o.x;
            p.maxX += o.x;
            p.minY += o.y;
            p.maxY += o.y;
        }

        void ScaleQuad(Text::Rect &p, const float s)
        {
            const float cx = 0.5f * (p.minX + p.maxX);
            const float cy = 0.5f * (p.minY + p.maxY);
            const float hw = 0.5f * (p.maxX - p.minX) * s;
            const float hh = 0.5f * (p.maxY - p.minY) * s;
            p = Text::Rect{cx - hw, cy - hh, cx + hw, cy + hh};
        }
    }

    void UIText::SetCharacterOffset(const std::size_t index, const Math::Vector2 &offset)
    {
        if (SetSlot(_characterOffsets, index, offset, Math::Vector2{0.0f, 0.0f}, SameOffset))
        {
            _charactersChanged = true;
        }
    }

    void UIText::ClearCharacterOffsets()
    {
        if (ClearSlots(_characterOffsets))
        {
            _charactersChanged = true;
        }
    }

    Text::TextLayout UIText::ApplyCharacterOffsets(const Text::TextLayout &layout, const std::string &text,
                                                   const std::vector<Math::Vector2> &offsets)
    {
        Text::TextLayout result = layout;
        if (offsets.empty())
        {
            return result;
        }
        const std::vector<std::uint32_t> characterOf = CharacterOfQuads(layout, text);
        for (std::size_t q = 0; q < result.quads.size(); ++q)
        {
            if (characterOf[q] < offsets.size())
            {
                MoveQuad(result.quads[q].position, offsets[characterOf[q]]);
            }
        }
        return result;
    }

    void UIText::SetCharacterColor(const std::size_t index, const Common::Color &color)
    {
        const Common::Color clamped{ClampComponent(color.r), ClampComponent(color.g), ClampComponent(color.b),
                                    ClampComponent(color.a)};
        if (SetSlot(_characterColors, index, clamped, Common::Color::White, SameColor))
        {
            _charactersChanged = true;
            _colorsChanged = true;
        }
    }

    void UIText::ClearCharacterColors()
    {
        if (ClearSlots(_characterColors))
        {
            _charactersChanged = true;
            _colorsChanged = true;
        }
    }

    std::vector<Common::Color> UIText::CharacterQuadColors(const Text::TextLayout &layout, const std::string &text,
                                                           const std::vector<Common::Color> &colors)
    {
        std::vector<Common::Color> result;
        if (colors.empty())
        {
            return result;
        }
        const std::vector<std::uint32_t> characterOf = CharacterOfQuads(layout, text);
        result.reserve(characterOf.size());
        for (const std::uint32_t index : characterOf)
        {
            result.push_back(index < colors.size() ? colors[index] : Common::Color::White);
        }
        return result;
    }

    void UIText::SetCharacterScale(const std::size_t index, const float scale)
    {
        const float clamped = std::isfinite(scale) ? std::max(scale, 0.0f) : 1.0f;
        if (SetSlot(_characterScales, index, clamped, 1.0f, SameScale))
        {
            _charactersChanged = true;
        }
    }

    void UIText::ClearCharacterScales()
    {
        if (ClearSlots(_characterScales))
        {
            _charactersChanged = true;
        }
    }

    Text::TextLayout UIText::ApplyCharacterScales(const Text::TextLayout &layout, const std::string &text,
                                                  const std::vector<float> &scales)
    {
        Text::TextLayout result = layout;
        if (scales.empty())
        {
            return result;
        }
        const std::vector<std::uint32_t> characterOf = CharacterOfQuads(layout, text);
        for (std::size_t q = 0; q < result.quads.size(); ++q)
        {
            if (characterOf[q] < scales.size())
            {
                ScaleQuad(result.quads[q].position, scales[characterOf[q]]);
            }
        }
        return result;
    }

    Text::TextLayout UIText::ClipLayout(const Text::TextLayout &layout, const Text::Rect &clip,
                                        std::vector<Common::Color> *quadColors)
    {
        Text::TextLayout result = layout;
        result.quads.clear();
        const bool colored = quadColors && quadColors->size() == layout.quads.size();
        std::vector<Common::Color> colors;
        for (std::size_t q = 0; q < layout.quads.size(); ++q)
        {
            const Text::GlyphQuad &quad = layout.quads[q];
            const Text::Rect &p = quad.position;
            const Text::Rect cut{std::max(p.minX, clip.minX), std::max(p.minY, clip.minY), std::min(p.maxX, clip.maxX),
                                 std::min(p.maxY, clip.maxY)};
            if (cut.IsEmpty() || p.IsEmpty())
            {
                continue; // outside the clip
            }
            Text::GlyphQuad kept = quad;
            if (!(cut == p))
            {
                // uv is y-down: the quad's top edge (maxY) is at uv.minY
                const float uPerX = quad.uv.Width() / p.Width();
                const float vPerY = quad.uv.Height() / p.Height();
                kept.position = cut;
                kept.uv = Text::Rect{quad.uv.minX + (cut.minX - p.minX) * uPerX, quad.uv.minY + (p.maxY - cut.maxY) * vPerY,
                                     quad.uv.maxX - (p.maxX - cut.maxX) * uPerX, quad.uv.maxY - (cut.minY - p.minY) * vPerY};
            }
            result.quads.push_back(kept);
            if (colored)
            {
                colors.push_back((*quadColors)[q]);
            }
        }
        if (colored)
        {
            *quadColors = std::move(colors);
        }
        return result;
    }

    void UIText::RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                          const Renderer::Common::RenderState &state, const Matrix4 &canvasToWorld)
    {
#ifndef N2ENGINE_UI_ENABLED
        // A build without the UI module (N2ENGINE_USE_UI, defined on this file by engine/CMakeLists.txt) draws no UI
        static_cast<void>(renderer);
        static_cast<void>(rect);
        static_cast<void>(state);
        static_cast<void>(canvasToWorld);
#else
        if (!renderer || !rect.HasArea())
        {
            return;
        }
        _lastDrawnRect = rect;
        // Drawn by another renderer than the one holding the resources, or by a new renderer at the old
        // one's address (the window was re-created): start again with this one. The old resources are
        // destroyed only if their renderer still exists.
        _resources.Bind(renderer);

        const Text::TextLayout &layout = GetLayout(rect);
        const Matrix4 model = ComposeModel(canvasToWorld, ModelMatrixFor(GetAnchor(rect)));
        // The mesh follows the layout and the per-character scales, offsets and colours: a new version for
        // DrawResources when any changed. One pass over the quads: each one's scale (about its centre), then its offset,
        // on a copy of the layout; the colours become one vertex colour per quad, rebuilt only when they or the layout
        // changed (an animated offset leaves them as they are)
        const bool moved = !_characterOffsets.empty() || !_characterScales.empty();
        const Shown *shown = ShownIn(rect);
        // What the rich text styles ask of the mesh
        bool coloured = false;
        bool styled = false;
        if (shown)
        {
            for (const Text::RichStyle &style : shown->styles)
            {
                coloured = coloured || style.hasColor;
                styled = styled || style.bold || style.italic;
            }
        }
        // With <color> text the colour is in the vertices, the whole of it, so a change of the text's colour or tint
        // builds the mesh again; the material then carries only its alpha, which the effects don't follow
        const Common::Color drawColor = GetDrawColor();
        const bool baseChanged = coloured && (!SameColor(_styledBase, drawColor) || !SameColor(_styledTint, GetTint()));
        const bool layoutChanged =
            _layoutCache.GetVersion() != _drawnLayoutVersion || _shownVersion != _drawnShownVersion;
        if (layoutChanged || _charactersChanged || baseChanged)
        {
            _drawnLayoutVersion = _layoutCache.GetVersion();
            _drawnShownVersion = _shownVersion;
            _styledBase = drawColor;
            _styledTint = GetTint();
            ++_meshVersion;
            const bool recolor = coloured || (!_characterColors.empty() && (layoutChanged || _colorsChanged));
            // The characters the quads are: of the shown text, and through its map of GetPlainText()
            std::vector<std::uint32_t> shownCharacterOf;
            std::vector<std::uint32_t> characterOf;
            if (moved || recolor || styled)
            {
                shownCharacterOf = CharacterOfQuads(layout, shown ? shown->text : _text);
                characterOf = shownCharacterOf;
                if (shown)
                {
                    for (std::uint32_t &index : characterOf)
                    {
                        index = index < shown->source.size() ? shown->source[index] : NoCharacter;
                    }
                }
            }
            const auto styleOf = [&](const std::size_t q)
            {
                const std::uint32_t index = shownCharacterOf[q];
                return shown && index < shown->styles.size() ? shown->styles[index] : Text::RichStyle{};
            };
            if (moved)
            {
                _offsetLayout = layout;
                for (std::size_t q = 0; q < _offsetLayout.quads.size(); ++q)
                {
                    const std::uint32_t index = characterOf[q];
                    Text::Rect &position = _offsetLayout.quads[q].position;
                    if (index < _characterScales.size())
                    {
                        ScaleQuad(position, _characterScales[index]);
                    }
                    if (index < _characterOffsets.size())
                    {
                        MoveQuad(position, _characterOffsets[index]);
                    }
                }
            }
            else
            {
                _offsetLayout = Text::TextLayout{}; // nothing moved: the copy is not kept
            }
            if (_characterColors.empty() && !coloured)
            {
                _quadColors.clear();
            }
            else if (recolor)
            {
                _quadColors.clear();
                _quadColors.reserve(characterOf.size());
                const Common::Color &tint = GetTint();
                for (std::size_t q = 0; q < characterOf.size(); ++q)
                {
                    const std::uint32_t index = characterOf[q];
                    Common::Color color =
                        index < _characterColors.size() ? _characterColors[index] : Common::Color::White;
                    if (coloured)
                    {
                        const Text::RichStyle style = styleOf(q);
                        const Common::Color base =
                            style.hasColor ? Common::Color{style.colorR * tint.r, style.colorG * tint.g,
                                                           style.colorB * tint.b, style.colorA}
                                           : Common::Color{drawColor.r, drawColor.g, drawColor.b, 1.0f};
                        color = Common::Color{color.r * base.r, color.g * base.g, color.b * base.b, color.a * base.a};
                    }
                    _quadColors.push_back(color);
                }
            }
            _styled = styled;
            _styledMesh = Renderer::Common::MeshData{};
            if (styled)
            {
                const Text::TextLayout &source = moved ? _offsetLayout : layout;
                std::vector<QuadStyle> quadStyles(source.quads.size());
                for (const Text::LineInfo &line : layout.lines)
                {
                    for (std::size_t q = line.firstQuad; q < line.firstQuad + line.quadCount && q < quadStyles.size(); ++q)
                    {
                        const Text::RichStyle style = styleOf(q);
                        const std::uint32_t index = characterOf[q];
                        // A moved character leans about its own moved baseline
                        const float baseline =
                            line.baseline + (index < _characterOffsets.size() ? _characterOffsets[index].y : 0.0f);
                        quadStyles[q] = QuadStyle{style.italic ? ItalicSlant : 0.0f, baseline,
                                                  style.bold ? BoldOffsetEms * _fontSize * style.scale : 0.0f};
                    }
                }
                _styledMesh = StyleMesh(Rendering::TextDrawing::BuildMesh(source, _quadColors), quadStyles);
            }
            _charactersChanged = false;
            _colorsChanged = false;
        }
        const Text::TextLayout &drawn = moved ? _offsetLayout : layout;

        // Issue #445: a RectMask's clip, in the layout's space, only when it cuts some glyph (text inside it draws as
        // without one). A new clip is a new mesh; the clipped copy is made once per mesh version.
        // With overflow Clip the text's own rect clips it too
        std::optional<Rect> clipRect = _renderClip;
        ConvexPolygon clipPolygon = _renderClipPolygon;
        if (_overflow == TextOverflow::Clip)
        {
            if (clipPolygon.empty())
            {
                clipRect = clipRect ? Rect::Intersect(*clipRect, rect) : rect;
            }
            else
            {
                clipPolygon = IntersectConvex(clipPolygon, TransformedCorners(rect, Transform2D{}));
                if (clipPolygon.empty())
                {
                    return; // nothing of the rect is inside the mask
                }
            }
        }
        // Slanted and doubled glyphs are no longer the layout's rects, so a rect clip cuts their mesh as a polygon does
        if (_styled && clipRect && clipPolygon.empty())
        {
            if (!clipRect->HasArea())
            {
                return;
            }
            clipPolygon = TransformedCorners(*clipRect, Transform2D{});
        }
        std::optional<Text::Rect> clip;
        if (clipRect && clipPolygon.empty())
        {
            const Math::Vector2 anchor = GetAnchor(rect);
            const Text::Rect local{clipRect->XMin() - anchor.x, clipRect->YMin() - anchor.y,
                                   clipRect->XMax() - anchor.x, clipRect->YMax() - anchor.y};
            const bool cuts = std::ranges::any_of(drawn.quads, [&local](const Text::GlyphQuad &quad)
            {
                const Text::Rect &p = quad.position;
                return p.minX < local.minX || p.minY < local.minY || p.maxX > local.maxX || p.maxY > local.maxY;
            });
            if (cuts)
            {
                clip = local;
            }
        }
        // A clip that is no rect (a mask turned against the text), likewise in the layout's space and only when it cuts
        ConvexPolygon polygon;
        if (!clipPolygon.empty())
        {
            const Math::Vector2 anchor = GetAnchor(rect);
            ConvexPolygon local = std::move(clipPolygon);
            for (Math::Vector2 &point : local)
            {
                point = Math::Vector2{point.x - anchor.x, point.y - anchor.y};
            }
            const auto outside = [&local](const float x, const float y)
            {
                return !ConvexContains(local, Math::Vector2{x, y});
            };
            const bool cuts =
                _styled ? std::ranges::any_of(_styledMesh.vertices,
                                              [&outside](const Renderer::Common::Vertex &vertex)
                                              { return outside(vertex.position[0], vertex.position[1]); })
                        : std::ranges::any_of(drawn.quads, [&outside](const Text::GlyphQuad &quad)
                          {
                              const Text::Rect &p = quad.position;
                              return outside(p.minX, p.minY) || outside(p.maxX, p.minY) || outside(p.maxX, p.maxY) ||
                                     outside(p.minX, p.maxY);
                          });
            if (cuts)
            {
                polygon = std::move(local);
            }
        }
        const bool samePolygon = std::ranges::equal(polygon, _drawnClipPolygon, [](const auto &a, const auto &b)
                                                    { return a.x == b.x && a.y == b.y; });
        if (clip != _drawnClip || !samePolygon)
        {
            _drawnClip = clip;
            _drawnClipPolygon = polygon;
            ++_meshVersion;
        }
        if (clip && _clippedVersion != _meshVersion)
        {
            _clippedColors = _quadColors;
            _clippedLayout = ClipLayout(drawn, *clip, &_clippedColors);
            _clippedVersion = _meshVersion;
        }
        if (!polygon.empty() && _clippedVersion != _meshVersion)
        {
            _clippedMesh = ClipQuadMesh(_styled ? _styledMesh : Rendering::TextDrawing::BuildMesh(drawn, _quadColors),
                                        polygon);
            _clippedVersion = _meshVersion;
        }
        const Common::Color materialColor = coloured ? Common::Color{1.0f, 1.0f, 1.0f, drawColor.a} : drawColor;
        const Renderer::Common::MeshData *geometry =
            !polygon.empty() ? &_clippedMesh : _styled ? &_styledMesh : nullptr;
        _resources.Draw(_layoutCache.GetFont(), clip ? _clippedLayout : drawn, _meshVersion, model.Data(), materialColor,
                        _effects, state, "UIText", clip ? _clippedColors : _quadColors, geometry);
#endif
    }

    void UIText::OnDestroy()
    {
        _resources.Release(true);
    }
}
