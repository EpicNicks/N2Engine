#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <math/Vector3.hpp>

#include "engine/Positionable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/Geometry2D.hpp"

namespace N2Engine::Rendering
{
    /**
     * A polyline drawn as one mesh (docs/meshes.html#lines): its points in the object's space (or in world space with useWorldSpace),
     * a width and a colour that go from start to end along its length, open or closed (loop), with miter or bevel corners and butt or
     * square ends. Flat in the x, y plane of its space (each point keeps its z), seen from both sides. Draws in the Transparent queue by
     * sortingOrder, as Sprite2D does, blended Alpha or Additive (Geometry2DDraw says what the software renderer does). The mesh is made
     * again only after a change.
     */
    class LineRenderer final : public GeometryRenderer2D
    {
    public:
        explicit LineRenderer(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override;
        [[nodiscard]] std::optional<BoundingBox> GetWorldBounds() const override;
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;

        /// False, changing nothing, for over kMaxLinePoints points
        bool SetPositions(std::span<const Math::Vector3> positions);
        [[nodiscard]] const std::vector<Math::Vector3> &GetPositions() const { return _positions; }
        /// Point `index` (from 0); false, changing nothing, for an index past the last point
        bool SetPosition(std::size_t index, const Math::Vector3 &position);
        /// Fewer points drops the last ones, more adds points at the origin; false, changing nothing, over kMaxLinePoints
        bool SetPositionCount(std::size_t count);
        [[nodiscard]] std::size_t GetPositionCount() const { return _positions.size(); }

        /// Both widths
        void SetWidth(float width);
        void SetStartWidth(float width) { _startWidth = width; }
        [[nodiscard]] float GetStartWidth() const { return _startWidth; }
        void SetEndWidth(float width) { _endWidth = width; }
        [[nodiscard]] float GetEndWidth() const { return _endWidth; }
        /// Both colours
        void SetColor(const Common::Color &color);
        void SetStartColor(const Common::Color &color) { _startColor = color; }
        [[nodiscard]] const Common::Color &GetStartColor() const { return _startColor; }
        void SetEndColor(const Common::Color &color) { _endColor = color; }
        [[nodiscard]] const Common::Color &GetEndColor() const { return _endColor; }

        void SetLoop(bool loop) { _loop = loop; }
        [[nodiscard]] bool GetLoop() const { return _loop; }
        /// True: the points are world positions, and the object's transform does not move them
        void SetUseWorldSpace(bool world) { _useWorldSpace = world; }
        [[nodiscard]] bool GetUseWorldSpace() const { return _useWorldSpace; }
        void SetJoin(LineJoin join) { _join = join; }
        [[nodiscard]] LineJoin GetJoin() const { return _join; }
        void SetMiterLimit(float limit) { _miterLimit = limit; }
        [[nodiscard]] float GetMiterLimit() const { return _miterLimit; }
        void SetCap(LineCap cap) { _cap = cap; }
        [[nodiscard]] LineCap GetCap() const { return _cap; }

        /// The mesh drawn now (made again if anything changed), or null when the line cannot be drawn (under two distinct points)
        [[nodiscard]] std::shared_ptr<Mesh> GetMesh();
        /// The matrix the mesh draws with: the object's local-to-world, or the identity in world space; nullopt without a transform
        [[nodiscard]] std::optional<Positionable::Matrix4> GetDrawMatrix() const;

        static constexpr bool IsSingleton = false;

    private:
        std::vector<Math::Vector3> _positions;
        float _startWidth = 0.1f;
        float _endWidth = 0.1f;
        Common::Color _startColor{1.0f, 1.0f, 1.0f, 1.0f};
        Common::Color _endColor{1.0f, 1.0f, 1.0f, 1.0f};
        bool _loop = false;
        bool _useWorldSpace = false;
        LineJoin _join = LineJoin::Miter;
        float _miterLimit = 4.0f;
        LineCap _cap = LineCap::Butt;

        std::shared_ptr<Mesh> _mesh;
        /// What _mesh was made from: the fields above, compared on every draw
        struct Made
        {
            std::vector<Math::Vector3> positions;
            float values[11]{};
            int options[4]{};
        };
        [[nodiscard]] Made Describe() const;
        Made _made;
        bool _madeOnce = false;
    };
}
