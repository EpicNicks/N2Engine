#include "engine/rendering/TrailRenderer.hpp"

#include <algorithm>
#include <cmath>

#include "engine/GameObject.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Rendering
{
    TrailRenderer::TrailRenderer(GameObject &gameObject) : GeometryRenderer2D(gameObject)
    {
        RegisterMember(NAMEOF(_time), _time).Range(0.0, 600.0);
        RegisterMember(NAMEOF(_minVertexDistance), _minVertexDistance).Range(0.0, 1000.0);
        RegisterMember(NAMEOF(_emitting), _emitting);
        RegisterMember(NAMEOF(_startWidth), _startWidth).Range(0.0, 1000.0);
        RegisterMember(NAMEOF(_endWidth), _endWidth).Range(0.0, 1000.0);
        RegisterMember(NAMEOF(_startColor), _startColor);
        RegisterMember(NAMEOF(_endColor), _endColor);
        RegisterMember(NAMEOF(_join), _join);
        RegisterMember(NAMEOF(_miterLimit), _miterLimit).Range(1.0, 100.0);
    }

    std::string TrailRenderer::GetTypeName() const
    {
        return NAMEOF(TrailRenderer);
    }

    void TrailRenderer::SetWidth(const float width)
    {
        _startWidth = width;
        _endWidth = width;
    }

    void TrailRenderer::SetColor(const Common::Color &color)
    {
        _startColor = color;
        _endColor = color;
    }

    void TrailRenderer::Clear()
    {
        _points.clear();
        _head.reset();
    }

    void TrailRenderer::Step(const Math::Vector3 &head, const float deltaTime)
    {
        const bool finiteHead = std::isfinite(head.x) && std::isfinite(head.y) && std::isfinite(head.z);
        if (finiteHead)
        {
            _head = head;
        }
        if (deltaTime > 0.0f && std::isfinite(deltaTime))
        {
            for (Point &point : _points)
            {
                point.age += deltaTime;
            }
        }
        // A time of 0 or below (or not a number) keeps nothing
        while (!_points.empty() && !(_points.front().age < _time))
        {
            _points.pop_front();
        }
        if (!_emitting || !finiteHead || !(_time > 0.0f))
        {
            return;
        }
        const float spacing = std::isfinite(_minVertexDistance) ? std::max(_minVertexDistance, 0.0f) : 0.0f;
        bool add = _points.empty();
        if (!add)
        {
            const float distance = (head - _points.back().position).Length();
            add = distance >= spacing && distance > 0.0f;
        }
        if (add)
        {
            _points.push_back(Point{head, 0.0f});
            while (_points.size() > kMaxTrailPoints)
            {
                _points.pop_front();
            }
        }
    }

    std::vector<LinePoint> TrailRenderer::GetLinePoints() const
    {
        std::vector<LinePoint> line;
        line.reserve(_points.size() + 1);
        const auto at = [this](const Math::Vector3 &position, const float age)
        {
            const float t = _time > 0.0f ? std::clamp(age / _time, 0.0f, 1.0f) : 1.0f;
            LinePoint point;
            point.position = position;
            point.width = _startWidth + (_endWidth - _startWidth) * t;
            point.color = Common::Color(_startColor.r + (_endColor.r - _startColor.r) * t, _startColor.g + (_endColor.g - _startColor.g) * t,
                                        _startColor.b + (_endColor.b - _startColor.b) * t, _startColor.a + (_endColor.a - _startColor.a) * t);
            return point;
        };
        for (const Point &point : _points)
        {
            line.push_back(at(point.position, point.age));
        }
        if (_emitting && _head)
        {
            line.push_back(at(*_head, 0.0f));
        }
        return line;
    }

    std::shared_ptr<Mesh> TrailRenderer::GetMesh()
    {
        LineShape shape;
        shape.join = _join;
        shape.miterLimit = _miterLimit;
        const std::vector<LinePoint> line = GetLinePoints();
        if (line.size() < 2)
        {
            _mesh.reset();
            return _mesh;
        }
        UpdateGeometryMesh(_mesh, MakeLineMesh(line, shape));
        return _mesh;
    }

    void TrailRenderer::OnLateUpdate()
    {
        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            return;
        }
        Step(positionable->GetPosition(), Time::GetDeltaTime());
    }

    std::optional<BoundingBox> TrailRenderer::GetWorldBounds() const
    {
        // GetMesh only caches: it does not change what the trail is
        const std::shared_ptr<Mesh> mesh = const_cast<TrailRenderer *>(this)->GetMesh();
        if (!mesh)
        {
            return std::nullopt;
        }
        return mesh->GetBounds();
    }

    void TrailRenderer::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                      const RenderQueue queue)
    {
        if (!renderer || queue != RenderQueue::Transparent)
        {
            return;
        }
        const std::shared_ptr<Mesh> mesh = GetMesh();
        if (!mesh)
        {
            return;
        }
        _draw.Draw(renderer, mesh, Math::Matrix<float, 4, 4>::identity(), _blendMode, nullptr, Common::Color(1.0f, 1.0f, 1.0f, 1.0f),
                   queue, state);
    }
}
