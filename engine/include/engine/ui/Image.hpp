#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Matrix.hpp>
#include <math/Vector2.hpp>
#include <math/Vector4.hpp>
#include <renderer/common/ColorEffects.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>

#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/rendering/UserShaderSlot.hpp"
#include "engine/ui/UIGraphic.hpp"

namespace N2Engine::UI
{
    /**
     * A filled rectangle: its colour, multiplied by an optional texture, over its object's rect (a panel, a
     * background, an icon). Drawn with the renderer's standard unlit shader on a unit quad.
     *
     * The texture comes from one of two places:
     * - The sprite (SetSprite): a Rendering::Texture asset, serialized as its asset UUID ("sprite"). Each
     *   renderer gets one GPU texture per sprite asset, shared by every Image showing it (Rendering::GpuCache),
     *   made on first draw and released when the last Image using it lets go. Imported images are stored
     *   bottom row first (their flipY setting, on by default), so they show upright.
     * - A raw texture (SetTexture), for textures made at runtime with IRenderer::CreateTexture. It is not owned
     *   or serialized: whoever created it keeps it alive while the Image uses it, and clears it
     *   (SetTexture(nullptr)) before destroying it. When set, it is drawn instead of the sprite.
     *
     * The first row of the texture's pixel data (v = 0) is drawn at the bottom of the rect, so raw image data
     * stored top row first shows upside down unless it is flipped before CreateTexture.
     */
    class Image final : public UIGraphic
    {
    public:
        explicit Image(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "Image"; }

        using UIGraphic::RenderUI; // the overlay overload, with canvasToWorld identity
        void RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                      const Renderer::Common::RenderState &state, const Matrix4 &canvasToWorld) override;

        void OnDestroy() override;

        /// The raw texture (nullptr when none is set). It wins over the sprite.
        [[nodiscard]] Renderer::Common::ITexture* GetTexture() const { return _texture; }
        void SetTexture(Renderer::Common::ITexture *texture) { _texture = texture; }

        /// The sprite texture asset (nullptr when none is set)
        [[nodiscard]] const std::shared_ptr<Rendering::Texture>& GetSprite() const { return _sprite; }
        /// Sets the sprite; nullptr clears it. A previous sprite's share of its GPU texture is released now.
        void SetSprite(std::shared_ptr<Rendering::Texture> sprite);

        /// The colour effects, as on Sprite2D (serialized as "saturation" and "flash"): 1 draws the colour as it is, 0 grey,
        /// in between mixes, above 1 strengthens it. Stored clamped to 0..4; a non-finite value is 1
        void SetSaturation(float saturation) { _saturation = Renderer::Common::ColorEffects::ClampSaturation(saturation); }
        [[nodiscard]] float GetSaturation() const { return _saturation; }
        /// A colour mixed over the image by its alpha, after the saturation; transparent (the default) is none. Stored with
        /// each component clamped to 0..1 (a non-finite one is 0)
        void SetFlash(const Common::Color &flash) { _flash = Renderer::Common::ColorEffects::ClampFlash(flash); }
        [[nodiscard]] const Common::Color &GetFlash() const { return _flash; }

        /// A user shader (`.n2shader`, a Shader asset) to draw the image with instead of the standard unlit shader; null goes back (and
        /// forgets the parameter values). The image keeps a Material of its own for it (Rendering::UserShaderSlot): the sprite reaches the
        /// shader as the reserved baseColorTexture, the colour (with a Button's tint) as baseColor, and the cut-out on a renderer that
        /// cannot blend stays; the saturation and flash are the standard shader's and are not applied. Only the sprite is shown through a
        /// shader, not a raw SetTexture texture (the built-in path draws that, as before). Saved as `shaderState` when set (UserShaderSlot::Save)
        void SetShader(std::shared_ptr<Rendering::Shader> shader) { _shaderSlot.SetShader(std::move(shader)); }
        [[nodiscard]] const std::shared_ptr<Rendering::Shader> &GetShader() const { return _shaderSlot.GetShader(); }
        /// Sets a parameter the shader declares; false (with one warning) without a shader, for an unknown name or a wrong type
        template <typename Value>
        bool SetShaderParam(std::string_view name, Value &&value) { return _shaderSlot.SetParam(name, std::forward<Value>(value)); }
        bool SetShaderInt(std::string_view name, int value) { return _shaderSlot.SetParamInt(name, value); }
        bool SetShaderVec4(std::string_view name, float x, float y, float z, float w) { return _shaderSlot.SetParamVec4(name, x, y, z, w); }
        /// The image's private Material when it has a shader (Material::GetParam reads the values), else null
        [[nodiscard]] const std::shared_ptr<Rendering::Material> &GetShaderMaterial() const { return _shaderSlot.GetMaterial(); }

        /**
         * 9-slicing (issue #445): the texture's border widths in texture pixels, x left, y bottom, z right, w top (all 0, the
         * default, draws the texture stretched over the rect as before). With any border above 0 and a texture, the corners
         * keep their size (border x sliceScale canvas units), the edges stretch along one axis and the centre both ways, so
         * a frame or a rounded panel scales without stretching its corners. When the rect is smaller than two borders, the
         * borders shrink in proportion. Saved as "sliceBorder", "sliceScale" and "fillCenter".
         */
        [[nodiscard]] const Math::Vector4 &GetSliceBorder() const { return _sliceBorder; }
        void SetSliceBorder(const Math::Vector4 &border) { _sliceBorder = border; }
        /// The same by side (Math::Vector4's constructor takes w first: this avoids getting it wrong)
        void SetSliceBorder(const float left, const float bottom, const float right, const float top)
        {
            _sliceBorder.x = left;
            _sliceBorder.y = bottom;
            _sliceBorder.z = right;
            _sliceBorder.w = top;
        }
        /// Canvas units per texture pixel of the borders (1 by default)
        [[nodiscard]] float GetSliceScale() const { return _sliceScale; }
        void SetSliceScale(float scale) { _sliceScale = scale; }
        /// Whether a sliced image draws its centre (true by default; false leaves a frame)
        [[nodiscard]] bool GetFillCenter() const { return _fillCenter; }
        void SetFillCenter(bool fill) { _fillCenter = fill; }
        /// Whether it is drawn 9-sliced: a border above 0
        [[nodiscard]] bool IsSliced() const;

        /// The model matrix (row-major, as the renderers take it) that maps the unit square onto the rect
        [[nodiscard]] static Math::Matrix<float, 4, 4> ModelMatrixFor(const Rect &rect);

        /// One quad of an image's geometry: where it goes (canvas space) and the part of the texture on it (uv.x, uv.y the
        /// bottom-left texture coordinate, v = 0 at the bottom of the rect; width and height the extent)
        struct Quad
        {
            Rect position;
            Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
            friend constexpr bool operator==(const Quad &, const Quad &) = default;
        };

        /**
         * `quad` cut to `clip` (issue #445, RectMask): the part of it inside the clip, with its texture coordinates cut in
         * the same proportion, so the texture stays where it was on screen. nullopt when no area of it is inside.
         */
        [[nodiscard]] static std::optional<Quad> ClipQuad(const Quad &quad, const Rect &clip);

        /**
         * The quads this image draws over `rect` with `clip` (GetRenderClip during a UI draw): empty when it draws its plain
         * unit quad (no clip cuts it), else the visible quads, in canvas space. A clip that leaves nothing gives one quad
         * without area, which draws nothing.
         */
        [[nodiscard]] std::vector<Quad> BuildQuads(const Rect &rect, const std::optional<Rect> &clip,
                                                   const Math::Vector2 &textureSize = Math::Vector2{0.0f, 0.0f}) const;
        /// The nine quads (fewer where one has no area, no centre without fillCenter) of a sliced image over `rect`, for a
        /// texture `textureSize` pixels; empty without a texture size
        [[nodiscard]] std::vector<Quad> SliceQuads(const Rect &rect, const Math::Vector2 &textureSize) const;

    private:
        Renderer::Common::ITexture *_texture = nullptr;
        std::shared_ptr<Rendering::Texture> _sprite; // serialized as "sprite"
        float _saturation = 1.0f;
        Math::Vector4 _sliceBorder{0.0f, 0.0f, 0.0f, 0.0f};
        float _sliceScale = 1.0f;
        bool _fillCenter = true;
        Common::Color _flash{Common::Color::Transparent};
        Rendering::GpuCache::Handle _spriteTexture;  // _sprite's texture on _renderer, once drawn
        // The last sprite a renderer failed to make a texture for, so it isn't retried every frame
        std::weak_ptr<Rendering::Texture> _failedSprite;
        std::weak_ptr<const void> _failedRenderer;

        // Created on first draw with the renderer that drew it, released in OnDestroy (as the scene renderables
        // do: by the time a leftover component is freed, the renderer may be gone)
        Renderer::Common::IRenderer *_renderer = nullptr;
        std::weak_ptr<const void> _rendererLifetime; // expired once _renderer is destroyed
        Renderer::Common::IMesh *_mesh = nullptr;
        Renderer::Common::IMaterial *_material = nullptr;
        // The mesh of a clipped image (BuildQuads), relative to its rect's corner, and the quads (and, under a turned
        // mask, the clip polygon) it was built from
        Renderer::Common::IMesh *_quadMesh = nullptr;
        std::vector<Quad> _quadMeshQuads;
        ConvexPolygon _quadMeshPolygon;
        Rendering::UserShaderSlot _shaderSlot; // the user shader and its Material, when one is set

        /// Creates the quad and material on first use; false if the renderer couldn't
        bool EnsureResources(Renderer::Common::IRenderer *renderer);
        /// The mesh of `quads` (relative to `origin`), cut to `polygon` when it has corners, updated in place when they
        /// change; nullptr if the renderer can't, or if the polygon leaves nothing
        Renderer::Common::IMesh *EnsureQuadMesh(Renderer::Common::IRenderer *renderer, const std::vector<Quad> &quads,
                                                const Math::Vector2 &origin, const ConvexPolygon &polygon = {});
        /// The sprite's texture on `renderer` (nullptr for no sprite, or one the renderer can't upload)
        Renderer::Common::ITexture *EnsureSpriteTexture(Renderer::Common::IRenderer *renderer);
        void ReleaseResources();
    };
}
