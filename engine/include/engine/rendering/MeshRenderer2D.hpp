#pragma once

#include <memory>
#include <optional>

#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Geometry2D.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Texture.hpp"

namespace N2Engine::Rendering
{
    /**
     * Draws a mesh the way Sprite2D draws its shapes (docs/meshes.html#mesh-renderer-2d): unlit, both faces, blended Alpha or Additive,
     * in the Transparent queue by sortingOrder, in the vertex colours times `color`, textured by the vertices' uvs when a texture is set.
     * Meant for a mesh a script builds (Mesh.Create), such as a concave shape, but takes any mesh (every submesh draws with the same
     * material). A mesh asset is saved as a reference; a mesh made at runtime has no
     * asset to point at, so a saved scene has none.
     */
    class MeshRenderer2D final : public GeometryRenderer2D
    {
    public:
        explicit MeshRenderer2D(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override;
        [[nodiscard]] std::optional<BoundingBox> GetWorldBounds() const override;
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;

        void SetMesh(std::shared_ptr<Mesh> mesh) { _mesh = std::move(mesh); }
        [[nodiscard]] const std::shared_ptr<Mesh> &GetMesh() const { return _mesh; }
        void SetColor(const Common::Color &color) { _color = color; }
        [[nodiscard]] const Common::Color &GetColor() const { return _color; }
        /// An image asset; null for the vertex colours alone
        void SetTexture(std::shared_ptr<Texture> texture) { _texture = std::move(texture); }
        [[nodiscard]] const std::shared_ptr<Texture> &GetTexture() const { return _texture; }

        static constexpr bool IsSingleton = false;

    private:
        std::shared_ptr<Mesh> _mesh;
        Common::Color _color{1.0f, 1.0f, 1.0f, 1.0f};
        std::shared_ptr<Texture> _texture;
    };
}
