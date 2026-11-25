#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <text/RichText.hpp>
#include <text/TextLayout.hpp>

#include "engine/rendering/TextDrawing.hpp"
#include "engine/serialization/FieldInfo.hpp"
#include "engine/text/Font.hpp"
#include "engine/text/TextEffects.hpp"
#include "engine/text/TextJson.hpp" // the alignments' JSON names: every user must see the same serializer
#include "engine/ui/UIGraphic.hpp"

namespace N2Engine::UI
{
    /// What a UIText does with text that doesn't fit its rect (issue #445)
    enum class TextOverflow : std::uint8_t
    {
        /// It runs past the rect (the default)
        Overflow,
        /// Cut at the rect's edges, partly through a glyph if need be
        Clip,
        /// Whole characters and lines that don't fit are left out
        Truncate,
        /// As Truncate, with an ellipsis where text was left out
        Ellipsis
    };

    N2_SERIALIZE_ENUM(TextOverflow, {
                      { TextOverflow::Overflow, "Overflow" },
                      { TextOverflow::Clip, "Clip" },
                      { TextOverflow::Truncate, "Truncate" },
                      { TextOverflow::Ellipsis, "Ellipsis" }
                      })

    /**
     * A block of UTF-8 text drawn inside its object's rect in the UI pass, with an SDF font: a label, a
     * score, a button caption. The UI counterpart of Rendering::TextRenderer, sharing its layout, mesh and
     * atlas code (Rendering::TextDrawing).
     *
     * - fontSize is in canvas pixels per em (a line is about 1.36 em high in the default font).
     * - With wrap on (the default) lines wrap at the rect's width; with it off they run past the rect.
     *   Text that doesn't fit the rect overflows it by default; the overflow setting (issue #445, TextOverflow) can cut
     *   it at the edges (Clip), leave out what doesn't fit (Truncate) or mark that with an ellipsis (Ellipsis). A
     *   RectMask above it clips it too.
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
     * shader is null (a renderer or a build without it) nothing is drawn and one warning is logged per process.
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
        /// What happens to text that doesn't fit the rect (issue #445; default Overflow)
        void SetOverflow(const TextOverflow overflow) { _overflow = overflow; }
        [[nodiscard]] TextOverflow GetOverflow() const { return _overflow; }
        /**
         * Rich text (issue #445; default off, so a "<" in existing text stays text): the tags of Text::ParseRichText,
         * <b>, <i>, <color=...>, <size=...> and <noparse>, style parts of the text; anything else is shown as written.
         * Sizes are canvas pixels (<size=30>) or percentages of fontSize. A <color> replaces the text's colour (a
         * Button's tint still multiplies it) and its alpha multiplies the text's. A font asset is one face, so bold is
         * drawn twice, a little apart, and italic is slanted. The per-character offsets, scales and colours count the
         * characters that show, not the tags.
         */
        void SetRichText(const bool richText) { _richText = richText; }
        [[nodiscard]] bool GetRichText() const { return _richText; }
        /// The characters that show: GetText() without its tags when rich text is on
        [[nodiscard]] std::string GetPlainText() const;
        /// The whole text laid out wrapped at the width of a rect `rectWidth` wide (0, or wrap off: not wrapped),
        /// whatever the overflow setting: what a ContentSizeFitter or a layout group measures. Not cached
        [[nodiscard]] Text::TextLayout Measure(float rectWidth) const;

        /// How far bold text's second drawing is moved, and by how much a bold character's advance grows, in its ems
        static constexpr float BoldOffsetEms = 0.04f;
        /// How far italic text leans: x moves by this much per unit above the baseline
        static constexpr float ItalicSlant = 0.2f;

        /// Whether the text doesn't fit its current rect (GetCurrentRect): a line wider than it, or more lines than its
        /// height holds. With Truncate or Ellipsis, whether something is left out.
        [[nodiscard]] bool IsOverflowing() const;

        /// A text cut to fit: `text`, and for each of its characters (code points) the index of the character of the
        /// original it is, or NoCharacter for one that was added (an ellipsis, a line break put where a line wrapped)
        struct FittedText
        {
            std::string text;
            std::vector<std::uint32_t> source;
        };
        static constexpr std::uint32_t NoCharacter = ~std::uint32_t{0};

        /**
         * `text` cut to fit `width` by `height` layout units, laid out with `options` (issue #445; Truncate and Ellipsis;
         * nothing for the other modes). Nothing when it already fits. Otherwise the result has the lines that fit the
         * height, broken where the layout broke them; a line wider than the width is cut after its last character that
         * fits; with Ellipsis, a cut line, and the last line when lines were left out, ends with an ellipsis (U+2026, or
         * "..." in a font without it), trailing spaces before it removed. A width too small even for the ellipsis leaves
         * that line empty, and a height too small for one line leaves nothing. options.metrics, when given, are those of
         * `text`'s characters (the ellipsis takes the one before it).
         */
        [[nodiscard]] static std::optional<FittedText> FitText(const Text::Font &font, const std::string &text,
                                                               const Text::LayoutOptions &options, float width,
                                                               float height, TextOverflow overflow);

        // Effects (see Text::TextEffects): lengths in ems, all off by default. Material uniforms, like the
        // colour: changing them never lays the text out again.
        void SetEffects(const Text::TextEffects &effects) { _effects = effects; }
        [[nodiscard]] const Text::TextEffects &GetEffects() const { return _effects; }
        /// An outline `width` ems wide (0 turns it off)
        void SetOutline(const float width, const Common::Color &color)
        {
            _effects.outlineWidth = width;
            _effects.outlineColor = color;
        }
        /// A shadow `offset` ems away (+y up), fading over `softness` ems; a colour with alpha 0 turns it off
        void SetShadow(const Math::Vector2 &offset, const Common::Color &color, const float softness = 0.0f)
        {
            _effects.shadowOffset = offset;
            _effects.shadowColor = color;
            _effects.shadowSoftness = softness;
        }
        /// How far the outer edges fade, in ems (0 = crisp)
        void SetSoftness(const float softness) { _effects.softness = softness; }
        /// Adds an extra effect pass behind the face, placed by its order (Text::TextPass::order; the default 0
        /// is in front of the shadow and the outline settings and of the passes added before it). Saved with
        /// the scene.
        void AddEffectPass(const Text::TextPass &pass) { _effects.passes.push_back(pass); }
        /// Removes every extra pass (the shadow, outline and softness settings stay)
        void ClearEffectPasses() { _effects.passes.clear(); }

        /**
         * Moves single characters away from where the layout put them, by `offset` in canvas units (+y up), as
         * TextMeshPro scripts do to vertices for waving, shaking or bouncing text. `index` counts characters (code
         * points) of GetText() from 0, spaces and line breaks included; a character with no glyph (a space) has
         * nothing to move. Offsets are not saved with the scene and survive SetText: a script that changes the
         * text sets them again. Changing them rebuilds the text's mesh on the next draw, not its layout. An index
         * above MaxCharacterIndex is ignored, and characters at the end that are back to no offset are dropped from
         * GetCharacterOffsets (the same rules for the colours and scales below).
         */
        void SetCharacterOffset(std::size_t index, const Math::Vector2 &offset);
        /// Every character back where the layout puts it
        void ClearCharacterOffsets();
        /// The largest character index SetCharacterOffset, SetCharacterColor and SetCharacterScale take: a larger one
        /// (a script's mistake, which no text reaches) is ignored instead of growing a list to that size
        static constexpr std::size_t MaxCharacterIndex = 65535;
        [[nodiscard]] const std::vector<Math::Vector2> &GetCharacterOffsets() const { return _characterOffsets; }
        /// `layout` (of `text`) with `offsets` added to the quads of the characters they index (see
        /// SetCharacterOffset); used by the draw, public for tests
        [[nodiscard]] static Text::TextLayout ApplyCharacterOffsets(const Text::TextLayout &layout, const std::string &text,
                                                                    const std::vector<Math::Vector2> &offsets);

        /**
         * Colours single characters, with the same index, lifetime and bounds rules as SetCharacterOffset: not
         * saved, kept across SetText, cleared with ClearCharacterColors, and a default value (white) past the end
         * adds nothing. The colour multiplies the text's colour (UIGraphic) on the character's face; its alpha
         * also fades the character's outline, shadow and extra effect passes (their colours are their own), so a
         * character at alpha 0 is gone with its outline and shadow. Each component is clamped to 0..1; a
         * non-finite one counts as 1. Changing it rebuilds the text's mesh (the colour is in its vertex colours)
         * on the next draw. The software renderer cannot blend: there a character is drawn solid until its alpha
         * falls below the text's minimum, then not at all.
         */
        void SetCharacterColor(std::size_t index, const Common::Color &color);
        /// Every character back to the text's own colour
        void ClearCharacterColors();
        [[nodiscard]] const std::vector<Common::Color> &GetCharacterColors() const { return _characterColors; }
        /// The vertex colour of each quad of `layout` (of `text`) for these per-character `colors` (white for a
        /// character with none); empty when `colors` is. Used by the draw, public for tests
        [[nodiscard]] static std::vector<Common::Color> CharacterQuadColors(const Text::TextLayout &layout,
                                                                           const std::string &text,
                                                                           const std::vector<Common::Color> &colors);

        /**
         * Scales single characters about the centre of their glyph quad (1 is the layout's size, 2 twice as
         * big, 0 hides the character), with the same index, lifetime and bounds rules as SetCharacterOffset.
         * The layout is not changed: neighbours do not move apart, and GetBoundsIn ignores it, as it ignores
         * the offsets. Applied before the offset. A negative scale counts as 0, a non-finite one as 1.
         */
        void SetCharacterScale(std::size_t index, float scale);
        /// Every character back to the layout's size
        void ClearCharacterScales();
        [[nodiscard]] const std::vector<float> &GetCharacterScales() const { return _characterScales; }
        /// `layout` (of `text`) with the quads of the characters `scales` index scaled about their centres (see
        /// SetCharacterScale); used by the draw, public for tests
        [[nodiscard]] static Text::TextLayout ApplyCharacterScales(const Text::TextLayout &layout, const std::string &text,
                                                                   const std::vector<float> &scales);

        /**
         * `layout`'s glyph quads cut to `clip` (issue #445, RectMask; in the layout's own space, relative to the anchor, as
         * the quads are): a quad wholly outside is dropped, one across the edge keeps the part inside with its atlas
         * coordinates cut in the same proportion, so the glyph stays where it was. `quadColors`, when given with one colour
         * per quad, is filtered the same way. The lines and bounds are kept as they were. Used by the draw, public for tests
         */
        [[nodiscard]] static Text::TextLayout ClipLayout(const Text::TextLayout &layout, const Text::Rect &clip,
                                                         std::vector<Common::Color> *quadColors = nullptr);

        /// The layout options the current settings give in a rect of this width (it is the wrap width
        /// with wrap on)
        [[nodiscard]] Text::LayoutOptions GetLayoutOptions(float rectWidth) const;
        /// The layout in `rect` of the text as it is drawn there (with Truncate or Ellipsis, what is left of it),
        /// relative to the alignment point GetAnchor(rect): add the anchor to get canvas space. Laid out again only if a setting (or, with wrap on, the rect's width) changed
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
        TextOverflow _overflow = TextOverflow::Overflow;
        Text::TextEffects _effects;

        Rect _lastDrawnRect{};

        // Per-character offsets, scales and colours (runtime only), and the layout and quad colours they gave
        // for the last draw
        std::vector<Math::Vector2> _characterOffsets;
        std::vector<float> _characterScales;
        std::vector<Common::Color> _characterColors;
        bool _charactersChanged = false; // the mesh must be rebuilt
        bool _colorsChanged = false;     // and the quad colours with it
        Text::TextLayout _offsetLayout;
        std::vector<Common::Color> _quadColors;
        std::uint64_t _drawnLayoutVersion = ~std::uint64_t{0}; // the layout version the last mesh came from
        std::uint64_t _meshVersion = 0;                        // what DrawResources sees: up on any change
        // The RectMask clip of the last draw (relative to the anchor; nullopt: none that cuts the text), and the clipped
        // layout and quad colours made for it at _clippedVersion
        std::optional<Text::Rect> _drawnClip;
        ConvexPolygon _drawnClipPolygon; // a turned mask's region that cuts the text (empty: none)
        Renderer::Common::MeshData _clippedMesh; // and the mesh cut to it
        Text::TextLayout _clippedLayout;
        std::vector<Common::Color> _clippedColors;
        std::uint64_t _clippedVersion = ~std::uint64_t{0};

        bool _richText = false;

        // The text as drawn when that is not _text itself: rich text without its tags, or what Truncate and Ellipsis
        // leave of it
        struct Shown
        {
            std::string text;
            std::vector<std::uint32_t> source;  // per character: its index among the plain text's, or NoCharacter
            std::vector<Text::RichStyle> styles; // per character; empty without rich text
            std::vector<Text::CharacterMetrics> metrics; // per byte; empty without rich text
        };
        // Kept for the inputs it was made from
        struct ShownCache
        {
            std::string text;
            std::shared_ptr<Text::Font> font;
            bool richText = false;
            float fontSize = 0.0f;
            float letterSpacing = 0.0f;
            float lineSpacing = 0.0f;
            float maxWidth = 0.0f;
            float width = 0.0f;
            float height = 0.0f;
            TextOverflow overflow = TextOverflow::Overflow;
            std::optional<Shown> result;
        };
        mutable std::optional<ShownCache> _shown;
        mutable std::uint64_t _shownVersion = 0; // up whenever _shown is computed again
        std::uint64_t _drawnShownVersion = 0;
        // Rich text drawing: the mesh with bold and italic worked in, and the draw colour the quad colours of <color>
        // text include
        Renderer::Common::MeshData _styledMesh;
        bool _styled = false;
        // The colour and tint the coloured mesh was built with: black text drawn the same under any tint, so the tint
        // is kept too
        Common::Color _styledBase{Common::Color::White};
        Common::Color _styledTint{Common::Color::White};

        /// What is drawn in `rect`, or nullptr when that is _text as it is
        [[nodiscard]] const Shown *ShownIn(const Rect &rect) const;
        /// The plain text's characters and their styles, and the per-byte metrics they lay out with
        [[nodiscard]] Shown Parse() const;

        // Layout cache and GPU resources, the code TextRenderer uses
        mutable Rendering::TextDrawing::LayoutCache _layoutCache;
        Rendering::TextDrawing::DrawResources _resources;
    };
}
