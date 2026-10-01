#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <text/TextLayout.hpp>

#include "engine/IRenderable.hpp"
#include "engine/common/Color.hpp"
#include "engine/text/Font.hpp"

namespace N2Engine::Rendering
{
    /**
     * Draws a block of UTF-8 text in the world, on the GameObject's x/y plane facing +Z, with an SDF font.
     *
     * The text is laid out with Font::Layout (see docs/text.html#layout): fontSize is world units per em,
     * maxWidth (world units, 0 = no wrapping) wraps it, and the alignments place the block relative to
     * the object's origin. It draws in the Transparent render queue, blended and without writing depth.
     *
     * Rendering:
     * - The layout and mesh are rebuilt only when the text, font, size, alignment, wrap width or spacing
     *   changes; the colour is a material uniform and never rebuilds anything. A changed mesh is updated
     *   in place (IRenderer::UpdateMesh), or recreated where the backend can't update meshes.
     * - Each font's atlas becomes one texture per renderer, shared by every TextRenderer using that font
     *   there, and destroyed when the last of them releases it.
     * - It needs IRenderer::GetStandardTextShader. Where that is null (the software renderer until #1 P3,
     *   Vulkan until #43) it draws nothing and logs one warning per process.
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
        /// Draws with RenderState::Transparent(), as Scene::Render would
        void Render(Renderer::Common::IRenderer *renderer) override;
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state) override;
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

        /// The layout options the current settings give
        [[nodiscard]] Text::LayoutOptions GetLayoutOptions() const;
        /// The current layout, in the object's local space. Laid out again only if a setting changed since
        /// the last call (or draw); needs no renderer.
        [[nodiscard]] const Text::TextLayout &GetLayout() const;

        /**
         * The mesh for a layout: one quad (4 vertices, 6 indices) per GlyphQuad, in order, at z = 0 with
         * normal +Z and white vertex colour. Corners are bottom-left, bottom-right, top-right, top-left
         * (counter-clockwise seen from +Z), with the glyph's atlas uvs as given (y-down, so the top corners
         * get uv.minY).
         */
        [[nodiscard]] static Renderer::Common::MeshData BuildMesh(const Text::TextLayout &layout);

        static constexpr bool IsSingleton = false;

    private:
        // Everything that changes the layout. The colour isn't here: it doesn't.
        struct LayoutInputs
        {
            std::string text;
            const Text::Font *font = nullptr;
            float fontSize = 0.0f;
            float maxWidth = 0.0f;
            float lineSpacing = 0.0f;
            float letterSpacing = 0.0f;
            Text::HorizontalAlign horizontalAlign = Text::HorizontalAlign::Left;
            Text::VerticalAlign verticalAlign = Text::VerticalAlign::Top;

            [[nodiscard]] bool Matches(const LayoutInputs &other) const;
        };

        /// Releases every GPU resource. With callRenderer false (the destructor, when the renderer may be
        /// gone) nothing is destroyed on the renderer; it frees them itself when it shuts down.
        void ReleaseRenderResources(bool callRenderer);
        bool EnsureAtlasTexture(const std::shared_ptr<Text::Font> &font);
        bool EnsureMesh(const Text::TextLayout &layout);

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

        // Layout cache. _layoutFont keeps the font the layout came from alive, so its address (in the
        // inputs) can't be reused while the cache refers to it.
        mutable std::optional<LayoutInputs> _layoutInputs;
        mutable Text::TextLayout _layout;
        mutable std::shared_ptr<Text::Font> _layoutFont;
        mutable std::uint64_t _layoutVersion = 0;

        // GPU resources, all on _renderer
        Renderer::Common::IRenderer *_renderer = nullptr;
        Renderer::Common::IMesh *_mesh = nullptr;
        Renderer::Common::IMaterial *_material = nullptr;
        Renderer::Common::ITexture *_atlasTexture = nullptr; // shared per (renderer, font); not ours to destroy
        const Text::Font *_atlasFont = nullptr;
        std::uint64_t _meshVersion = 0; // the _layoutVersion _mesh was built from
    };
}
