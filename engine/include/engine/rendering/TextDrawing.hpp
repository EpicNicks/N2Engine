#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <text/TextLayout.hpp>

#include "engine/common/Color.hpp"
#include "engine/text/Font.hpp"

// The drawing code every text component shares (TextRenderer in the world, UI::UIText in the UI pass): the
// layout cache, the glyph mesh, and the per-(renderer, font) atlas texture cache. Internal to the engine's
// text components; game code uses the components.
namespace N2Engine::Rendering::TextDrawing
{
    /**
     * The mesh for a layout: one quad (4 vertices, 6 indices) per GlyphQuad, in order, at z = 0 with
     * normal +Z and white vertex colour. Corners are bottom-left, bottom-right, top-right, top-left
     * (counter-clockwise seen from +Z), with the glyph's atlas uvs as given (y-down, so the top corners
     * get uv.minY).
     */
    [[nodiscard]] Renderer::Common::MeshData BuildMesh(const Text::TextLayout &layout);

    /**
     * A text block's layout, laid out again only when an input changes. The text is compared in place and
     * copied only when the layout changes, so an unchanged block costs a string compare per frame.
     */
    class LayoutCache
    {
    public:
        /// The layout of `text` in `font` (an empty layout for a null font) with `options`. Every option
        /// is an input; a NaN compares equal to a NaN, so it doesn't lay the text out every call.
        const Text::TextLayout &Get(const std::string &text, std::shared_ptr<Text::Font> font,
                                    const Text::LayoutOptions &options);

        /// The last layout Get returned (empty before the first call)
        [[nodiscard]] const Text::TextLayout &GetLayout() const { return _layout; }
        /// The font the current layout came from. Kept alive while it's cached, so its address (an input)
        /// can't be reused by another font meanwhile.
        [[nodiscard]] const std::shared_ptr<Text::Font> &GetFont() const { return _font; }
        /// Goes up by one each time the text is laid out (0 before the first layout)
        [[nodiscard]] std::uint64_t GetVersion() const { return _version; }

    private:
        struct Inputs
        {
            std::string text;
            const Text::Font *font = nullptr;
            Text::LayoutOptions options;
        };

        std::optional<Inputs> _inputs;
        Text::TextLayout _layout;
        std::shared_ptr<Text::Font> _font;
        std::uint64_t _version = 0;
    };

    /**
     * One text block's GPU resources on one renderer: its mesh and material, and a share of the font's atlas
     * texture. The atlas is one texture per (renderer, font), shared by every text component drawing that
     * font there (TextRenderer and UIText alike), and destroyed when its last user releases it.
     *
     * Like the scene renderables, it holds the renderer's lifetime token (IRenderer::GetLifetimeToken): a
     * renderer that was destroyed, or replaced by a new one at the same address, is never called.
     */
    class DrawResources
    {
    public:
        DrawResources() = default;
        /// Releases everything without calling the renderer, which may be gone by now
        ~DrawResources();
        DrawResources(const DrawResources &) = delete;
        DrawResources &operator=(const DrawResources &) = delete;

        /// The renderer resources are created on (nullptr when none is bound)
        [[nodiscard]] Renderer::Common::IRenderer *GetRenderer() const { return _renderer; }
        /// Whether the resources held are on this renderer: the same one, still alive
        [[nodiscard]] bool Holds(const Renderer::Common::IRenderer *renderer) const;

        /// Makes `renderer` (not null) the one resources are created on, releasing any held on another
        void Bind(Renderer::Common::IRenderer *renderer);

        /// Releases every resource. With callRenderer false (a destructor, when the renderer may be gone)
        /// nothing is destroyed on the renderer; it frees them itself when it shuts down. A renderer that no
        /// longer exists is never called either way.
        void Release(bool callRenderer);

        /**
         * Draws `layout` (laid out with `font`; layoutVersion tells a new layout from the one the mesh was
         * built from) on the bound renderer with the standard text shader, the colour as the material's
         * uAlbedo, and the given model matrix (row-major) and state. Creates or updates the mesh, material
         * and atlas share as needed: the mesh is updated in place (IRenderer::UpdateMesh), or recreated
         * where the backend can't.
         *
         * Draws nothing (false) without a bound renderer, for a null or unloaded font or an empty layout,
         * or if the renderer has no text shader; the last logs one warning per process, naming
         * `componentName`.
         */
        bool Draw(const std::shared_ptr<Text::Font> &font, const Text::TextLayout &layout,
                  std::uint64_t layoutVersion, const float *modelMatrix, const Common::Color &color,
                  const Renderer::Common::RenderState &state, std::string_view componentName);

    private:
        bool EnsureAtlasTexture(const std::shared_ptr<Text::Font> &font);
        bool EnsureMesh(const Text::TextLayout &layout, std::uint64_t layoutVersion);

        Renderer::Common::IRenderer *_renderer = nullptr;
        std::weak_ptr<const void> _rendererLifetime; // expired once _renderer is destroyed
        Renderer::Common::IMesh *_mesh = nullptr;
        Renderer::Common::IMaterial *_material = nullptr;
        Renderer::Common::ITexture *_atlasTexture = nullptr; // shared per (renderer, font); not ours to destroy
        const Text::Font *_atlasFont = nullptr;
        std::uint64_t _meshVersion = 0; // the layout version _mesh was built from
    };
}
