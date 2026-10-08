#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <text/TextLayout.hpp>

#include "engine/IRenderable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/TextDrawing.hpp"
#include "engine/text/Font.hpp"
#include "engine/text/TextEffects.hpp"
#include "engine/text/TextJson.hpp" // the alignments' JSON names: every user must see the same serializer

namespace N2Engine::Rendering
{
    /**
     * Draws a block of UTF-8 text in the world, on the GameObject's x/y plane facing +Z, with an SDF font.
     *
     * The text is laid out with Font::Layout (see docs/text.html#layout): fontSize is world units per em,
     * maxWidth (world units, 0 = no wrapping) wraps it, and the alignments place the block relative to
     * the object's origin. It draws in the Transparent render queue with RenderState::Transparent(): on
     * OpenGL blended and without writing depth; on the software renderer, which can't blend, the text
     * shader alpha-tests the glyphs instead and writes their depth (see IRenderer::GetStandardTextShader).
     *
     * Rendering:
     * - The layout and mesh are rebuilt only when the text, font, size, alignment, wrap width or spacing
     *   changes; the colour and the effects (outline, shadow, softness: Text::TextEffects) are material
     *   uniforms and never rebuild anything. A changed mesh is updated
     *   in place (IRenderer::UpdateMesh), or recreated where the backend can't update meshes.
     * - Each font's atlas becomes one texture per renderer, shared by every TextRenderer (and UI::UIText)
     *   using that font there, and destroyed when the last of them releases it. The drawing code is
     *   shared with UI::UIText (TextDrawing.hpp).
     * - It needs IRenderer::GetStandardTextShader, which OpenGL and the software renderer have. Where it
     *   is null (Vulkan until #43) it draws nothing and logs one warning per process.
     * - Empty text, or text with only spaces, draws nothing.
     */
    class TextRenderer final : public IRenderable
    {
    public:
        explicit TextRenderer(GameObject &gameObject);
        ~TextRenderer() override;

        [[nodiscard]] std::string GetTypeName() const override { return "TextRenderer"; }

        // IRenderable
        [[nodiscard]] RenderQueueKey GetRenderQueue() const override { return {RenderQueue::Transparent, 0}; }
        /// The laid-out text block (the layout's bounds, a rectangle on the object's x/y plane) moved by the
        /// object's world transform; nullopt for empty text or without a transform
        [[nodiscard]] std::optional<BoundingBox> GetWorldBounds() const override;
        /// Draws with RenderState::Transparent(), as Scene::Render would
        void Render(Renderer::Common::IRenderer *renderer) override;
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;
        /// Resources are created on first draw; this only binds the component to the renderer (releasing
        /// what it held on another one)
        void InitializeRenderResources(Renderer::Common::IRenderer *renderer) override;
        /// Releases the mesh, material and atlas texture this component holds on `renderer`. Does nothing
        /// for a renderer it holds nothing on.
        void CleanupRenderResources(Renderer::Common::IRenderer *renderer) override;
        void OnDestroy() override;

        // Text and font
        void SetText(const std::string &text) { _text = text; }
        [[nodiscard]] const std::string &GetText() const { return _text; }
        /// nullptr draws with Font::GetDefault()
        void SetFont(std::shared_ptr<Text::Font> font) { _font = std::move(font); }
        /// The font that was set: nullptr when the default font is used
        [[nodiscard]] std::shared_ptr<Text::Font> GetFont() const { return _font; }
        /// The font the text is drawn with: the one set, or the default font
        [[nodiscard]] std::shared_ptr<Text::Font> GetEffectiveFont() const;

        // Appearance
        /// World units per em
        void SetFontSize(float fontSize) { _fontSize = fontSize; }
        [[nodiscard]] float GetFontSize() const { return _fontSize; }
        void SetColor(const Common::Color &color) { _color = color; }
        [[nodiscard]] const Common::Color &GetColor() const { return _color; }
        void SetHorizontalAlign(Text::HorizontalAlign align) { _horizontalAlign = align; }
        [[nodiscard]] Text::HorizontalAlign GetHorizontalAlign() const { return _horizontalAlign; }
        void SetVerticalAlign(Text::VerticalAlign align) { _verticalAlign = align; }
        [[nodiscard]] Text::VerticalAlign GetVerticalAlign() const { return _verticalAlign; }
        /// Wrap width in world units; 0 or less never wraps
        void SetMaxWidth(float maxWidth) { _maxWidth = maxWidth; }
        [[nodiscard]] float GetMaxWidth() const { return _maxWidth; }
        /// Multiplies the font's line height
        void SetLineSpacing(float lineSpacing) { _lineSpacing = lineSpacing; }
        [[nodiscard]] float GetLineSpacing() const { return _lineSpacing; }
        /// Extra space between glyphs, in ems (negative tightens)
        void SetLetterSpacing(float letterSpacing) { _letterSpacing = letterSpacing; }
        [[nodiscard]] float GetLetterSpacing() const { return _letterSpacing; }

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

        /// The layout options the current settings give
        [[nodiscard]] Text::LayoutOptions GetLayoutOptions() const;
        /// The current layout, in the object's local space. Laid out again only if a setting changed since
        /// the last call (or draw); needs no renderer.
        [[nodiscard]] const Text::TextLayout &GetLayout() const;

        /**
         * The mesh for a layout: one quad (4 vertices, 6 indices) per GlyphQuad, in order, at z = 0 with
         * normal +Z and white vertex colour. Corners are bottom-left, bottom-right, top-right, top-left
         * (counter-clockwise seen from +Z), with the glyph's atlas uvs as given (y-down, so the top corners
         * get uv.minY). The same as TextDrawing::BuildMesh.
         */
        [[nodiscard]] static Renderer::Common::MeshData BuildMesh(const Text::TextLayout &layout);

        static constexpr bool IsSingleton = false;

    private:
        // Serialized settings
        std::string _text;
        std::shared_ptr<Text::Font> _font;
        float _fontSize = 1.0f;
        Common::Color _color{Common::Color::White};
        Text::HorizontalAlign _horizontalAlign = Text::HorizontalAlign::Left;
        Text::VerticalAlign _verticalAlign = Text::VerticalAlign::Top;
        float _maxWidth = 0.0f;
        float _lineSpacing = 1.0f;
        float _letterSpacing = 0.0f;
        Text::TextEffects _effects;

        // Layout cache and GPU resources, shared code with UI::UIText (see TextDrawing.hpp)
        mutable TextDrawing::LayoutCache _layoutCache;
        TextDrawing::DrawResources _resources;
    };
}
