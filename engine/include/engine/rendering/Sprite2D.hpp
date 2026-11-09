#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <math/Vector2.hpp>

#include "engine/IRenderable.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/MeshDrawing.hpp"
#include "engine/rendering/ShapeMesh2D.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Rendering
{
    // Literals as values, so renaming a constant does not change what scenes save
    N2_SERIALIZE_ENUM(Shape2D, {
                      {Shape2D::Rect, "Rect"},
                      {Shape2D::Triangle, "Triangle"},
                      {Shape2D::Circle, "Circle"},
                      {Shape2D::RegularPolygon, "RegularPolygon"},
                      {Shape2D::Polygon, "Polygon"}
                      })

    /**
     * A flat shape in the object's x, y plane (docs/sprite2d.html): a rectangle, triangle, circle, regular polygon or convex
     * polygon, filled or outlined, in a colour, optionally textured. It always draws in the Transparent queue with its
     * `sortingOrder` as the sort key, so lower orders draw first; equal orders draw back to front by depth, then in hierarchy order.
     * Both faces draw, so a flip or a negative scale never hides it. Depth tested against 3D geometry, not written.
     *
     * On the software renderer, which cannot blend, a textured sprite's pixels with alpha under 0.5 are not drawn, so a texture's
     * transparent parts are cut out rather than drawn opaque. Solid shapes look the same everywhere.
     */
    class Sprite2D final : public IRenderable
    {
    public:
        explicit Sprite2D(GameObject &gameObject);
        ~Sprite2D() override;

        [[nodiscard]] std::string GetTypeName() const override;

        // IRenderable
        [[nodiscard]] RenderQueueKey GetRenderQueue() const override;
        [[nodiscard]] std::optional<BoundingBox> GetWorldBounds() const override;
        void Render(Renderer::Common::IRenderer *renderer) override;
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;
        void InitializeRenderResources(Renderer::Common::IRenderer *renderer) override;
        void CleanupRenderResources(Renderer::Common::IRenderer *renderer) override;
        void OnDestroy() override;

        void SetShape(Shape2D shape) { _shape = shape; }
        [[nodiscard]] Shape2D GetShape() const { return _shape; }
        void SetSize(const Math::Vector2 &size) { _size = size; }
        [[nodiscard]] const Math::Vector2 &GetSize() const { return _size; }
        /// RegularPolygon: 3 to 64
        void SetSides(int sides) { _sides = sides; }
        [[nodiscard]] int GetSides() const { return _sides; }
        /// Polygon: 3 to 64 convex points, in units of the size (-0.5 to 0.5 spans it)
        void SetVertices(const std::vector<Math::Vector2> &vertices) { _vertices = vertices; }
        [[nodiscard]] const std::vector<Math::Vector2> &GetVertices() const { return _vertices; }
        /// 0 fills; above 0, only a border this wide, in world units
        void SetOutline(float width) { _outline = width; }
        [[nodiscard]] float GetOutline() const { return _outline; }

        void SetColor(const Common::Color &color) { _color = color; }
        [[nodiscard]] const Common::Color &GetColor() const { return _color; }
        /// An image asset; null for a plain colour
        void SetTexture(std::shared_ptr<Texture> texture);
        [[nodiscard]] const std::shared_ptr<Texture> &GetTexture() const { return _texture; }
        /// The part of the texture shown, in uv with v up: its lower-left corner and its size. (0, 0) and (1, 1) is all of it.
        void SetRegion(const Math::Vector2 &offset, const Math::Vector2 &size)
        {
            _regionOffset = offset;
            _regionSize = size;
        }
        [[nodiscard]] const Math::Vector2 &GetRegionOffset() const { return _regionOffset; }
        [[nodiscard]] const Math::Vector2 &GetRegionSize() const { return _regionSize; }

        void SetFlipX(bool flip) { _flipX = flip; }
        [[nodiscard]] bool GetFlipX() const { return _flipX; }
        void SetFlipY(bool flip) { _flipY = flip; }
        [[nodiscard]] bool GetFlipY() const { return _flipY; }
        /// Where the object's position sits within the size: (0.5, 0.5) is the middle, (0, 0) the lower-left corner
        void SetPivot(const Math::Vector2 &pivot) { _pivot = pivot; }
        [[nodiscard]] const Math::Vector2 &GetPivot() const { return _pivot; }
        /// Lower draws first, so further back
        void SetSortingOrder(int order) { _sortingOrder = order; }
        [[nodiscard]] int GetSortingOrder() const { return _sortingOrder; }

        /// The mesh this sprite draws now (shared through GetShapeMesh2D), or null when it cannot be drawn
        [[nodiscard]] std::shared_ptr<Mesh> GetMesh();
        /// The object's local-to-world matrix with the flips; nullopt without a transform
        [[nodiscard]] std::optional<Positionable::Matrix4> GetModelMatrix() const;
        [[nodiscard]] ShapeDesc2D Describe() const;

        static constexpr bool IsSingleton = false;

    private:
        /// The material for this draw: blended, both faces, with the texture; cut out on a renderer that cannot blend
        const std::shared_ptr<Material> &EnsureMaterial(const Renderer::Common::IRenderer &renderer);

        Shape2D _shape = Shape2D::Rect;
        Math::Vector2 _size{1.0f, 1.0f};
        int _sides = 6;
        std::vector<Math::Vector2> _vertices;
        float _outline = 0.0f;
        Common::Color _color{Common::Color::White};
        std::shared_ptr<Texture> _texture;
        Math::Vector2 _regionOffset{0.0f, 0.0f};
        Math::Vector2 _regionSize{1.0f, 1.0f};
        bool _flipX = false;
        bool _flipY = false;
        Math::Vector2 _pivot{0.5f, 0.5f};
        int _sortingOrder = 0;

        std::shared_ptr<Mesh> _mesh;
        std::string _meshKey;
        std::shared_ptr<Material> _material;
        MeshDrawing::DrawResources _resources;
    };
}
