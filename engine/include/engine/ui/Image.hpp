#pragma once

#include <memory>
#include <string>

#include <math/Matrix.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>

#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Texture.hpp"
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

        /// The model matrix (row-major, as the renderers take it) that maps the unit square onto the rect
        [[nodiscard]] static Math::Matrix<float, 4, 4> ModelMatrixFor(const Rect &rect);

    private:
        Renderer::Common::ITexture *_texture = nullptr;
        std::shared_ptr<Rendering::Texture> _sprite; // serialized as "sprite"
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

        /// Creates the quad and material on first use; false if the renderer couldn't
        bool EnsureResources(Renderer::Common::IRenderer *renderer);
        /// The sprite's texture on `renderer` (nullptr for no sprite, or one the renderer can't upload)
        Renderer::Common::ITexture *EnsureSpriteTexture(Renderer::Common::IRenderer *renderer);
        void ReleaseResources();
    };
}
