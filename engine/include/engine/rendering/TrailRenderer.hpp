#pragma once

#include <deque>
#include <memory>
#include <optional>

#include <math/Vector3.hpp>

#include "engine/common/Color.hpp"
#include "engine/rendering/Geometry2D.hpp"

namespace N2Engine::Rendering
{
    /**
     * A trail behind a moving object (docs/meshes.html#trails): every LateUpdate it ages its points by the frame's scaled delta time
     * (Time::GetDeltaTime, so it freezes with the game at time scale 0 and slows in a hit-stop), drops those older than `time`, and adds
     * the object's world position when it is at least minVertexDistance from the newest point. The object's position is always the
     * trail's head. The width and colour go from the start values at the head to the end values at a point `time` old, by age, so the
     * end fades smoothly as points age. Drawn in world space as a line (LineRenderer's mesh, with its joins) in the Transparent queue by
     * sortingOrder, blended Alpha or Additive. Clear() forgets every point: call it after a teleport so no streak joins the two places.
     * Points are runtime state: nothing about them is saved. Rendering only, so it is not part of the simulation.
     */
    class TrailRenderer final : public GeometryRenderer2D
    {
    public:
        explicit TrailRenderer(GameObject &gameObject);

        [[nodiscard]] std::string GetTypeName() const override;
        [[nodiscard]] std::optional<BoundingBox> GetWorldBounds() const override;
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;
        void OnLateUpdate() override;

        /// Ages the points by `deltaTime` seconds, drops the expired ones and, while emitting, adds `head` if it is far enough from the
        /// newest point. OnLateUpdate calls it with the object's world position and Time::GetDeltaTime()
        void Step(const Math::Vector3 &head, float deltaTime);
        /// Forgets every point (after a teleport); the next step starts again from where the object is
        void Clear();

        struct Point
        {
            Math::Vector3 position;
            float age = 0.0f;
        };
        /// The stored points, oldest first (the head is not one of them)
        [[nodiscard]] const std::deque<Point> &GetPoints() const { return _points; }
        [[nodiscard]] std::size_t GetPointCount() const { return _points.size(); }
        /// The line drawn now: the points oldest first, then the head while emitting, each with its width and colour
        [[nodiscard]] std::vector<LinePoint> GetLinePoints() const;

        /// How long a point lives, in seconds; at most 0 keeps none
        void SetTime(float seconds) { _time = seconds; }
        [[nodiscard]] float GetTime() const { return _time; }
        void SetMinVertexDistance(float distance) { _minVertexDistance = distance; }
        [[nodiscard]] float GetMinVertexDistance() const { return _minVertexDistance; }
        /// False: no new points and no head; the old ones still age away
        void SetEmitting(bool emitting) { _emitting = emitting; }
        [[nodiscard]] bool IsEmitting() const { return _emitting; }

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
        void SetJoin(LineJoin join) { _join = join; }
        [[nodiscard]] LineJoin GetJoin() const { return _join; }
        void SetMiterLimit(float limit) { _miterLimit = limit; }
        [[nodiscard]] float GetMiterLimit() const { return _miterLimit; }

        /// The mesh drawn now (in world space, made again from the points), or null with fewer than two distinct points
        [[nodiscard]] std::shared_ptr<Mesh> GetMesh();

        static constexpr bool IsSingleton = false;

    private:
        float _time = 1.0f;
        float _minVertexDistance = 0.1f;
        bool _emitting = true;
        float _startWidth = 0.1f;
        float _endWidth = 0.0f;
        Common::Color _startColor{1.0f, 1.0f, 1.0f, 1.0f};
        Common::Color _endColor{1.0f, 1.0f, 1.0f, 0.0f};
        LineJoin _join = LineJoin::Miter;
        float _miterLimit = 4.0f;

        std::deque<Point> _points;
        std::optional<Math::Vector3> _head;
        std::shared_ptr<Mesh> _mesh;
    };
}
