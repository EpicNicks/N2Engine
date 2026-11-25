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
        const FittedText *fitted = Fitted(rect);
        return _layoutCache.Get(fitted ? fitted->text : _text, GetEffectiveFont(), GetLayoutOptions(rect.width));
    }

    const UIText::FittedText *UIText::Fitted(const Rect &rect) const
    {
        if (_overflow != TextOverflow::Truncate && _overflow != TextOverflow::Ellipsis)
        {
            return nullptr;
        }
        const std::shared_ptr<Text::Font> font = GetEffectiveFont();
        if (!font)
        {
            return nullptr;
        }
        const Text::LayoutOptions options = GetLayoutOptions(rect.width);
        if (!_fit || _fit->font != font || _fit->fontSize != options.fontSize ||
            _fit->letterSpacing != options.letterSpacing || _fit->lineSpacing != options.lineSpacing ||
            _fit->maxWidth != options.maxWidth || _fit->width != rect.width || _fit->height != rect.height ||
            _fit->overflow != _overflow || _fit->text != _text)
        {
            _fit = FitCache{_text,      font,        options.fontSize, options.letterSpacing, options.lineSpacing,
                            options.maxWidth, rect.width, rect.height,      _overflow,
                            FitText(*font, _text, options, rect.width, rect.height, _overflow)};
            ++_fitVersion;
        }
        return _fit->result ? &*_fit->result : nullptr;
    }

    namespace
    {
        /// How many lines of `layout` fit `height` (all of them when their spacing is 0)
        std::size_t LinesThatFit(const Text::TextLayout &layout, const float height, const float tolerance)
        {
            const std::size_t count = layout.lines.size();
            if (count == 0)
            {
                return 0;
            }
            const float oneLine = layout.bounds.Height() - static_cast<float>(count - 1) * layout.lineHeight;
            if (height + tolerance < oneLine)
            {
                return 0;
            }
            if (!(layout.lineHeight > 0.0f))
            {
                return count;
            }
            const float more = std::floor((height + tolerance - oneLine) / layout.lineHeight);
            return more >= static_cast<float>(count - 1) ? count : static_cast<std::size_t>(more) + 1;
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
        const auto widthOf = [&](const std::string &candidate)
        {
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
                if (widthOf(candidate(characters) + suffix) > width + tolerance)
                {
                    // The most characters that fit, by halving (the width grows with the characters)
                    std::size_t low = 0;
                    std::size_t high = characters;
                    while (high - low > 1)
                    {
                        const std::size_t middle = low + (high - low) / 2;
                        (widthOf(candidate(middle) + suffix) <= width + tolerance ? low : high) = middle;
                    }
                    keep = low;
                    if (keep == 0 && !suffix.empty() && widthOf(suffix) > width + tolerance)
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
        const Text::TextLayout full = font->Layout(_text, options);
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
        const bool layoutChanged = _layoutCache.GetVersion() != _drawnLayoutVersion || _fitVersion != _drawnFitVersion;
        if (layoutChanged || _charactersChanged)
        {
            _drawnLayoutVersion = _layoutCache.GetVersion();
            _drawnFitVersion = _fitVersion;
            ++_meshVersion;
            const bool recolor = !_characterColors.empty() && (layoutChanged || _colorsChanged);
            // The characters of GetText() the quads are: through the fitted text's map when some of it was left out
            std::vector<std::uint32_t> characterOf;
            if (moved || recolor)
            {
                if (const FittedText *fitted = Fitted(rect))
                {
                    characterOf = CharacterOfQuads(layout, fitted->text);
                    for (std::uint32_t &index : characterOf)
                    {
                        index = index < fitted->source.size() ? fitted->source[index] : NoCharacter;
                    }
                }
                else
                {
                    characterOf = CharacterOfQuads(layout, _text);
                }
            }
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
            if (_characterColors.empty())
            {
                _quadColors.clear();
            }
            else if (recolor)
            {
                _quadColors.clear();
                _quadColors.reserve(characterOf.size());
                for (const std::uint32_t index : characterOf)
                {
                    _quadColors.push_back(index < _characterColors.size() ? _characterColors[index] : Common::Color::White);
                }
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
            const bool cuts = std::ranges::any_of(drawn.quads, [&local](const Text::GlyphQuad &quad)
            {
                const Text::Rect &p = quad.position;
                return !ConvexContains(local, Math::Vector2{p.minX, p.minY}) ||
                       !ConvexContains(local, Math::Vector2{p.maxX, p.minY}) ||
                       !ConvexContains(local, Math::Vector2{p.maxX, p.maxY}) ||
                       !ConvexContains(local, Math::Vector2{p.minX, p.maxY});
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
            _clippedMesh = ClipQuadMesh(Rendering::TextDrawing::BuildMesh(drawn, _quadColors), polygon);
            _clippedVersion = _meshVersion;
        }
        _resources.Draw(_layoutCache.GetFont(), clip ? _clippedLayout : drawn, _meshVersion, model.Data(), GetDrawColor(),
                        _effects, state, "UIText", clip ? _clippedColors : _quadColors,
                        polygon.empty() ? nullptr : &_clippedMesh);
#endif
    }

    void UIText::OnDestroy()
    {
        _resources.Release(true);
    }
}
