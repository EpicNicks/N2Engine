#pragma once

#include <memory>
#include <string>
#include <utility>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <text/TextLayout.hpp>

#include "engine/rendering/TextDrawing.hpp"
#include "engine/text/Font.hpp"
#include "engine/text/TextJson.hpp" // the alignments' JSON names: every user must see the same serializer
#include "engine/ui/UIGraphic.hpp"

namespace N2Engine::UI
{
    /**
     * A block of UTF-8 text drawn inside its object's rect in the UI pass, with an SDF font: a label, a
     * score, a button caption. The UI counterpart of Rendering::TextRenderer, sharing its layout, mesh and
     * atlas code (Rendering::TextDrawing).
     *
     * - fontSize is in canvas pixels per em (a line is about 1.36 em high in the default font).
     * - With wrap on (the default) lines wrap at the rect's width; with it off they run past the rect.
     *   Nothing is clipped: text that doesn't fit in the rect's height overflows it.
     * - The alignments place the block inside the rect. Left, Center and Right align the lines to the
     *   rect's left edge, centre and right edge. Top puts the first line's ascent on the top edge, Middle
     *   centres the block vertically, Bottom puts the last line's descent on the bottom edge, and Baseline
     *   puts the first line's baseline on the rect's vertical centre (as TextMesh Pro does).
     * - The colour (UIGraphic) is a material uniform. raycastTarget is off by default, unlike Image: a
     *   label lets the pointer through to what is underneath (a button's Image) unless it is turned on.
     *
     * Rendering: drawn with IRenderer::GetStandardTextShader and the UI pass's state (no depth, no culling,
     * blended; the software renderer alpha-tests instead). The layout and mesh are built relative to the
     * alignment point in the rect (GetAnchor), and the model matrix moves them there, so moving the rect
     * changes only the model matrix. The mesh is rebuilt (updated in place) only when the text, font, size,
     * alignment, spacing, wrapping or, with wrap on, the rect's width changes. Each font's atlas is one
     * texture per renderer, shared with every TextRenderer and UIText using that font there. Where the text
     * shader is null (Vulkan until #43) nothing is drawn and one warning is logged per process.
     */
    class UIText final : public UIGraphic
    {
    public:
        explicit UIText(GameObject &gameObject);
        ~UIText() override;

        [[nodiscard]] std::string GetTypeName() const override { return "UIText"; }

        using UIGraphic::RenderUI; // the overlay overload, with canvasToWorld identity
        void RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                      const Renderer::Common::RenderState &state, const Matrix4 &canvasToWorld) override;

        void OnDestroy() override;

        // Text and font
        void SetText(const std::string &text) { _text = text; }
        [[nodiscard]] const std::string &GetText() const { return _text; }
        /// nullptr draws with Text::Font::GetDefault()
        void SetFont(std::shared_ptr<Text::Font> font) { _font = std::move(font); }
        /// The font that was set: nullptr when the default font is used
        [[nodiscard]] std::shared_ptr<Text::Font> GetFont() const { return _font; }
        /// The font the text is drawn with: the one set, or the default font
        [[nodiscard]] std::shared_ptr<Text::Font> GetEffectiveFont() const;

        // Appearance (the colour is UIGraphic's)
        /// Canvas pixels per em (default 24)
        void SetFontSize(const float fontSize) { _fontSize = fontSize; }
        [[nodiscard]] float GetFontSize() const { return _fontSize; }
        void SetHorizontalAlign(const Text::HorizontalAlign align) { _horizontalAlign = align; }
        [[nodiscard]] Text::HorizontalAlign GetHorizontalAlign() const { return _horizontalAlign; }
        void SetVerticalAlign(const Text::VerticalAlign align) { _verticalAlign = align; }
        [[nodiscard]] Text::VerticalAlign GetVerticalAlign() const { return _verticalAlign; }
        /// Whether lines wrap at the rect's width (default true)
        void SetWrap(const bool wrap) { _wrap = wrap; }
        [[nodiscard]] bool GetWrap() const { return _wrap; }
        /// Multiplies the font's line height
        void SetLineSpacing(const float lineSpacing) { _lineSpacing = lineSpacing; }
        [[nodiscard]] float GetLineSpacing() const { return _lineSpacing; }
        /// Extra space between glyphs, in ems (negative tightens)
        void SetLetterSpacing(const float letterSpacing) { _letterSpacing = letterSpacing; }
        [[nodiscard]] float GetLetterSpacing() const { return _letterSpacing; }

        /// The layout options the current settings give in a rect of this width (it is the wrap width
        /// with wrap on)
        [[nodiscard]] Text::LayoutOptions GetLayoutOptions(float rectWidth) const;
        /// The layout in `rect`, relative to the alignment point GetAnchor(rect): add the anchor to get
        /// canvas space. Laid out again only if a setting (or, with wrap on, the rect's width) changed
        /// since the last call or draw; needs no renderer.
        [[nodiscard]] const Text::TextLayout &GetLayout(const Rect &rect) const;
        /// The point of `rect` the layout origin goes to for these alignments (canvas space)
        [[nodiscard]] static Math::Vector2 AnchorFor(const Rect &rect, Text::HorizontalAlign horizontal,
                                                     Text::VerticalAlign vertical);
        /// AnchorFor with this text's alignments
        [[nodiscard]] Math::Vector2 GetAnchor(const Rect &rect) const;
        /// The laid-out block in canvas space (Text::TextLayout::bounds moved to the anchor); all zero
        /// for empty text
        [[nodiscard]] Text::Rect GetBoundsIn(const Rect &rect) const;
        /// The rect the text is laid out in: its RectTransform's last resolved rect, or for an object
        /// without one, the rect it was last drawn in (zero before either)
        [[nodiscard]] Rect GetCurrentRect() const;

        /// The model matrix (row-major, as the renderers take it) that moves the layout to `anchor`
        [[nodiscard]] static Math::Matrix<float, 4, 4> ModelMatrixFor(const Math::Vector2 &anchor);

        static constexpr bool IsSingleton = false;

    private:
        // Serialized settings
        std::string _text;
        std::shared_ptr<Text::Font> _font;
        float _fontSize = 24.0f;
        Text::HorizontalAlign _horizontalAlign = Text::HorizontalAlign::Left;
        Text::VerticalAlign _verticalAlign = Text::VerticalAlign::Top;
        bool _wrap = true;
        float _lineSpacing = 1.0f;
        float _letterSpacing = 0.0f;

        Rect _lastDrawnRect{};

        // Layout cache and GPU resources, the code TextRenderer uses
        mutable Rendering::TextDrawing::LayoutCache _layoutCache;
        Rendering::TextDrawing::DrawResources _resources;
    };
}
