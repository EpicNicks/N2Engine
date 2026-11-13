#include "engine/ui/UIText.hpp"

#include <algorithm>
#include <cmath>

#include "engine/GameObjectScene.hpp"
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
        return _layoutCache.Get(_text, GetEffectiveFont(), GetLayoutOptions(rect.width));
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
        constexpr std::uint32_t NoCharacter = ~std::uint32_t{0};

        /// The character index (code points of `text`, from 0) of each quad of `layout`; NoCharacter for a quad
        /// whose cluster is outside the text
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
                result.push_back(quad.cluster < text.size() ? characterOf[quad.cluster] : NoCharacter);
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

    void UIText::RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                          const Renderer::Common::RenderState &state, const Matrix4 &canvasToWorld)
    {
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
        const bool layoutChanged = _layoutCache.GetVersion() != _drawnLayoutVersion;
        if (layoutChanged || _charactersChanged)
        {
            _drawnLayoutVersion = _layoutCache.GetVersion();
            ++_meshVersion;
            const bool recolor = !_characterColors.empty() && (layoutChanged || _colorsChanged);
            const std::vector<std::uint32_t> characterOf =
                moved || recolor ? CharacterOfQuads(layout, _text) : std::vector<std::uint32_t>{};
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
        _resources.Draw(_layoutCache.GetFont(), drawn, _meshVersion, model.Data(), GetDrawColor(), _effects, state,
                        "UIText", _quadColors);
    }

    void UIText::OnDestroy()
    {
        _resources.Release(true);
    }
}
