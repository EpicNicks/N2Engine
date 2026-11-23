#include "engine/rendering/LineRenderer.hpp"

#include <algorithm>
#include <utility>

#include "engine/GameObject.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Rendering
{
    LineRenderer::LineRenderer(GameObject &gameObject) : GeometryRenderer2D(gameObject)
    {
        RegisterMember(NAMEOF(_positions), _positions);
        RegisterMember(NAMEOF(_startWidth), _startWidth).Range(0.0, 1000.0);
        RegisterMember(NAMEOF(_endWidth), _endWidth).Range(0.0, 1000.0);
        RegisterMember(NAMEOF(_startColor), _startColor);
        RegisterMember(NAMEOF(_endColor), _endColor);
        RegisterMember(NAMEOF(_loop), _loop);
        RegisterMember(NAMEOF(_useWorldSpace), _useWorldSpace);
        RegisterMember(NAMEOF(_join), _join);
        RegisterMember(NAMEOF(_miterLimit), _miterLimit).Range(1.0, 100.0);
        RegisterMember(NAMEOF(_cap), _cap);
    }

    std::string LineRenderer::GetTypeName() const
    {
        return NAMEOF(LineRenderer);
    }

    bool LineRenderer::SetPositions(const std::span<const Math::Vector3> positions)
    {
        if (positions.size() > kMaxLinePoints)
        {
            return false;
        }
        _positions.assign(positions.begin(), positions.end());
        return true;
    }

    bool LineRenderer::SetPosition(const std::size_t index, const Math::Vector3 &position)
    {
        if (index >= _positions.size())
        {
            return false;
        }
        _positions[index] = position;
        return true;
    }

    bool LineRenderer::SetPositionCount(const std::size_t count)
    {
        if (count > kMaxLinePoints)
        {
            return false;
        }
        _positions.resize(count, Math::Vector3(0.0f, 0.0f, 0.0f));
        return true;
    }

    void LineRenderer::SetWidth(const float width)
    {
        _startWidth = width;
        _endWidth = width;
    }

    void LineRenderer::SetColor(const Common::Color &color)
    {
        _startColor = color;
        _endColor = color;
    }

    LineRenderer::Made LineRenderer::Describe() const
    {
        Made made;
        made.positions = _positions;
        const float values[11] = {_startWidth, _endWidth, _startColor.r, _startColor.g, _startColor.b, _startColor.a,
                                  _endColor.r, _endColor.g, _endColor.b, _endColor.a, _miterLimit};
        std::copy(std::begin(values), std::end(values), std::begin(made.values));
        made.options[0] = _loop ? 1 : 0;
        made.options[1] = static_cast<int>(_join);
        made.options[2] = static_cast<int>(_cap);
        return made;
    }

    std::shared_ptr<Mesh> LineRenderer::GetMesh()
    {
        Made now = Describe();
        const auto samePoint = [](const Math::Vector3 &a, const Math::Vector3 &b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
        const bool unchanged = _madeOnce && std::ranges::equal(now.positions, _made.positions, samePoint) &&
                               std::ranges::equal(now.values, _made.values) && std::ranges::equal(now.options, _made.options);
        if (unchanged)
        {
            return _mesh;
        }
        const std::vector<LinePoint> points = LinePointsAlongLength(_positions, _loop, _startWidth, _endWidth, _startColor, _endColor);
        LineShape shape;
        shape.loop = _loop;
        shape.join = _join;
        shape.miterLimit = _miterLimit;
        shape.cap = _cap;
        UpdateGeometryMesh(_mesh, MakeLineMesh(points, shape));
        _made = std::move(now);
        _madeOnce = true;
        return _mesh;
    }

    std::optional<Positionable::Matrix4> LineRenderer::GetDrawMatrix() const
    {
        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            return std::nullopt;
        }
        if (_useWorldSpace)
        {
            return Positionable::Matrix4::identity();
        }
        return positionable->GetLocalToWorldMatrix();
    }

    std::optional<BoundingBox> LineRenderer::GetWorldBounds() const
    {
        const std::optional<Positionable::Matrix4> world = GetDrawMatrix();
        if (!world)
        {
            return std::nullopt;
        }
        // GetMesh only caches: it does not change what the line is
        const std::shared_ptr<Mesh> mesh = const_cast<LineRenderer *>(this)->GetMesh();
        if (!mesh)
        {
            return std::nullopt;
        }
        return mesh->GetBounds().Transformed(*world);
    }

    void LineRenderer::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                     const RenderQueue queue)
    {
        if (!renderer || queue != RenderQueue::Transparent)
        {
            return;
        }
        const std::optional<Positionable::Matrix4> world = GetDrawMatrix();
        const std::shared_ptr<Mesh> mesh = world ? GetMesh() : nullptr;
        if (!mesh)
        {
            return;
        }
        _draw.Draw(renderer, mesh, *world, _blendMode, nullptr, Common::Color(1.0f, 1.0f, 1.0f, 1.0f), queue, state);
    }
}
