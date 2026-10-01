#pragma once

#include <string>

#include <math/Matrix.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/ITexture.hpp>

#include "engine/ui/UIGraphic.hpp"

namespace N2Engine::UI
{
    /**
     * A filled rectangle: its colour, multiplied by an optional texture, over its object's rect (a panel, a
     * background, an icon). Drawn with the renderer's standard unlit shader on a unit quad.
     *
     * The texture is not owned or serialized: whoever created it (IRenderer::CreateTexture) keeps it alive while
     * the Image uses it, and clears it (SetTexture(nullptr)) before destroying it. Its v = 0 row is at the
     * bottom of the rect.
     */
    class Image final : public UIGraphic
    {
    public:
        explicit Image(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override { return "Image"; }

        void RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                      const Renderer::Common::RenderState &state) override;

        void OnDestroy() override;

        [[nodiscard]] Renderer::Common::ITexture* GetTexture() const { return _texture; }
        void SetTexture(Renderer::Common::ITexture *texture) { _texture = texture; }

        /// The model matrix (row-major, as the renderers take it) that maps the unit square onto the rect
        [[nodiscard]] static Math::Matrix<float, 4, 4> ModelMatrixFor(const Rect &rect);

    private:
        Renderer::Common::ITexture *_texture = nullptr;

        // Created on first draw with the renderer that drew it, released in OnDestroy (as the scene renderables
        // do: by the time a leftover component is freed, the renderer may be gone)
        Renderer::Common::IRenderer *_renderer = nullptr;
        Renderer::Common::IMesh *_mesh = nullptr;
        Renderer::Common::IMaterial *_material = nullptr;

        /// Creates the quad and material on first use; false if the renderer couldn't
        bool EnsureResources(Renderer::Common::IRenderer *renderer);
        void ReleaseResources();
    };
}
