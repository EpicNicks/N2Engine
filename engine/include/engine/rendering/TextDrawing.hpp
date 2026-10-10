#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include <text/TextLayout.hpp>

#include "engine/common/Color.hpp"
#include "engine/rendering/GpuCache.hpp"
#include "engine/text/Font.hpp"
#include "engine/text/TextEffects.hpp"

// The drawing code every text component shares (TextRenderer in the world, UI::UIText in the UI pass): the
// layout cache, the glyph mesh, and each font's atlas texture, shared per renderer through GpuCache. Internal
// to the engine's text components; game code uses the components.
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
     * The text shader's effect uniforms for one draw, in the units the shaders use. With every effect off
     * they are all zero, which the shaders treat as "no effect" (the same maths as text without effects).
     *
     * - uOutline (float): the outline width as a distance below the edge value 0.5 (the atlas stores 0.5
     *   per spreadPx pixels), so the outline's outer edge is at 0.5 - uOutline. 0 = no outline.
     * - uOutlineColor (vec4): the outline's colour.
     * - uSoftness (float): the half width of the face's (and outline's) edge ramp, in the same distance
     *   units; the shader uses the larger of it and its one-pixel antialiasing ramp. 0 = crisp.
     * - uShadowColor (vec4): the shadow's colour; alpha 0 = no shadow (every shadow uniform is then 0).
     * - uShadowOffset (vec2): the shadow's displacement in atlas uv (y-down); the shadow at a pixel is the
     *   shape sampled at uv - uShadowOffset.
     * - uShadowSoftness (float): the shadow edge ramp's half width, like uSoftness.
     */
    struct EffectUniforms
    {
        float outline = 0.0f;
        Common::Color outlineColor{0.0f, 0.0f, 0.0f, 0.0f};
        float softness = 0.0f;
        Common::Color shadowColor{0.0f, 0.0f, 0.0f, 0.0f};
        float shadowOffsetU = 0.0f;
        float shadowOffsetV = 0.0f;
        float shadowSoftness = 0.0f;
        /// Whether a setting was reduced to fit the atlas spread (see Text::TextEffects)
        bool clamped = false;
    };

    /// The share of the SDF spread effects may use: the rest keeps the glyph quads' edges (where the
    /// distance reaches 0) out of every edge ramp
    inline constexpr float kUsableSpread = 0.9f;

    /// The largest outline (+ softness / 2, + shadow offset and shadowSoftness / 2 for a shadow), in ems,
    /// for an atlas built with these settings: kUsableSpread * spreadPx / basePx
    [[nodiscard]] float MaxEffectEms(const Text::AtlasSettings &settings);

    /**
     * The uniforms for `effects` drawn from an atlas built with `settings`, atlasWidth x atlasHeight
     * pixels. Lengths are clamped to the spread (Text::TextEffects), negative and non-finite lengths count
     * as 0, and an effect that is off gives zeros. Shadow offsets are clamped per axis.
     */
    [[nodiscard]] EffectUniforms ResolveEffects(const Text::TextEffects &effects, const Text::AtlasSettings &settings,
                                                int atlasWidth, int atlasHeight);

    /**
     * One effect pass in the units the text shader uses (a Text::TextPass, or the legacy shadow or outline
     * as one): the shape's colour, its edge as a distance below 0.5 (the atlas value; 0 = the glyph edge),
     * the edge ramp's half width in the same units, and the displacement in atlas uv (y-down).
     */
    struct PassUniforms
    {
        Common::Color color{0.0f, 0.0f, 0.0f, 0.0f};
        float width = 0.0f;
        float softness = 0.0f;
        float offsetU = 0.0f;
        float offsetV = 0.0f;
    };

    /// The passes of a Text::TextEffects, resolved for one atlas: the effect passes back to front, and the
    /// face's own edge softness (the fill pass is always last)
    struct ResolvedPasses
    {
        std::vector<PassUniforms> effectPasses;
        float faceSoftness = 0.0f;
        /// Whether a setting was reduced to fit the atlas spread
        bool clamped = false;
    };

    /**
     * The full pass list for `effects`: the shadow (when on), the outline (when on and visible), then each
     * visible entry of effects.passes in order, all clamped to the spread as ResolveEffects does. The same
     * shapes ResolveEffects gives the single-draw path, so the two draw the same image.
     */
    [[nodiscard]] ResolvedPasses ResolvePasses(const Text::TextEffects &effects, const Text::AtlasSettings &settings,
                                               int atlasWidth, int atlasHeight);

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
     * texture. The atlas is one texture per (renderer, font) in the GpuCache, shared by every text component
     * drawing that font there (TextRenderer and UIText alike), and destroyed when its last user releases it.
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
         * uAlbedo, the effects as its effect uniforms (ResolveEffects; set on every draw, so the shared
         * text program never keeps another material's values), and the given model matrix (row-major)
         * and state. Effects reduced to fit the font's spread log one warning per process, naming
         * `componentName`. Without extra passes (Text::TextEffects::passes) that is one draw of the mesh;
         * with them, one draw per effect pass in order (the material's colour alpha 0, so only the pass's
         * shape draws, through the shadow uniforms) and then the face, so only the last draw writes depth on
         * the software renderer. Creates or updates the mesh, material
         * and atlas share as needed: the mesh is updated in place (IRenderer::UpdateMesh), or recreated
         * where the backend can't.
         *
         * Draws nothing (false) without a bound renderer, for a null or unloaded font or an empty layout,
         * or if the renderer has no text shader; the last logs one warning per process, naming
         * `componentName`.
         */
        bool Draw(const std::shared_ptr<Text::Font> &font, const Text::TextLayout &layout,
                  std::uint64_t layoutVersion, const float *modelMatrix, const Common::Color &color,
                  const Text::TextEffects &effects, const Renderer::Common::RenderState &state,
                  std::string_view componentName);

    private:
        bool EnsureAtlasTexture(const std::shared_ptr<Text::Font> &font);
        bool EnsureMesh(const Text::TextLayout &layout, std::uint64_t layoutVersion);
        /// The draws of a text with extra passes: each effect pass, then the face (see Draw)
        bool DrawPasses(const Text::TextEffects &effects, const Text::AtlasSettings &settings,
                        const Text::FontAtlas &atlas, const Common::Color &color, const float *modelMatrix,
                        const Renderer::Common::RenderState &state, std::string_view componentName);
        static void WarnClamped(std::string_view componentName, const Text::AtlasSettings &settings);

        Renderer::Common::IRenderer *_renderer = nullptr;
        std::weak_ptr<const void> _rendererLifetime; // expired once _renderer is destroyed
        Renderer::Common::IMesh *_mesh = nullptr;
        Renderer::Common::IMaterial *_material = nullptr;
        GpuCache::Handle _atlas; // the font's atlas, shared per (renderer, font); its source is the font
        std::uint64_t _meshVersion = 0; // the layout version _mesh was built from
    };
}
