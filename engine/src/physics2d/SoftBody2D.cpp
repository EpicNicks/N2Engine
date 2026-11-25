#include "engine/physics2d/SoftBody2D.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include <math/DeterministicMath.hpp>

#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Physics2D
{
    namespace
    {
        bool Finite(const Math::Vector2 &v) { return std::isfinite(v.x) && std::isfinite(v.y); }

        /// Twice the signed area: positive for a counter-clockwise outline
        float TwiceArea(const std::vector<Math::Vector2> &points)
        {
            float twice = 0.0f;
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                const Math::Vector2 &a = points[i];
                const Math::Vector2 &b = points[(i + 1) % points.size()];
                twice += a.x * b.y - b.x * a.y;
            }
            return twice;
        }

        int ClampNodes(const int count, const int minimum = SoftBody2D::MinNodes)
        {
            return std::clamp(count, minimum, SoftBody2D::MaxNodes);
        }
    }

    SoftBody2D::SoftBody2D(GameObject &gameObject) : GeometryRenderer2D(gameObject)
    {
        RegisterMember(NAMEOF(_shape), _shape);
        RegisterMember(NAMEOF(_radius), _radius).VisibleWhen(NAMEOF(_shape), {"Circle"});
        RegisterMember(NAMEOF(_size), _size).VisibleWhen(NAMEOF(_shape), {"Box"});
        RegisterMember(NAMEOF(_nodeCount), _nodeCount)
            .Range(MinNodes, MaxNodes)
            .VisibleWhen(NAMEOF(_shape), {"Circle", "Box"})
            .Tooltip("Nodes around the outline: more is smoother and costs more");
        RegisterMember(NAMEOF(_outline), _outline)
            .VisibleWhen(NAMEOF(_shape), {"Outline"})
            .Tooltip("3 to 64 points around the object's origin, in local units");
        RegisterMember(NAMEOF(_pinned), _pinned).Tooltip("Node indices (from 0) held where they are");
        RegisterMember(NAMEOF(_nodeRadius), _nodeRadius).Tooltip("The radius of each node's collision circle, in metres");
        RegisterMember(NAMEOF(_density), _density);
        RegisterMember(NAMEOF(_friction), _friction);
        RegisterMember(NAMEOF(_bounciness), _bounciness);
        RegisterMember(NAMEOF(_edgeFrequency), _edgeFrequency).Tooltip("Hertz: how stiff the outline is");
        RegisterMember(NAMEOF(_edgeDamping), _edgeDamping).Tooltip("Damping ratio of the outline's springs");
        RegisterMember(NAMEOF(_shapeFrequency), _shapeFrequency).Tooltip("Hertz: how strongly it keeps its shape");
        RegisterMember(NAMEOF(_shapeDamping), _shapeDamping).Tooltip("Damping ratio of the shape springs");
        RegisterMember(NAMEOF(_gravityScale), _gravityScale);
        RegisterMember(NAMEOF(_linearDamping), _linearDamping);
        RegisterMember(NAMEOF(_color), _color);
        RegisterAssetRef(NAMEOF(_texture), _texture);
    }

    // ===== Shape =====

    std::vector<Math::Vector2> SoftBody2D::RestOutline() const
    {
        std::vector<Math::Vector2> points;
        switch (_shape)
        {
        case SoftBodyShape2D::Circle:
        {
            if (!(_radius > 0.0f) || !std::isfinite(_radius))
            {
                break;
            }
            const int count = ClampNodes(_nodeCount);
            for (int i = 0; i < count; ++i)
            {
                const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(count);
                points.emplace_back(_radius * Math::Cos(angle), _radius * Math::Sin(angle));
            }
            break;
        }
        case SoftBodyShape2D::Box:
        {
            if (!Finite(_size) || !(_size.x > 0.0f) || !(_size.y > 0.0f))
            {
                break;
            }
            const int count = ClampNodes(_nodeCount, 4);
            const float hx = _size.x * 0.5f;
            const float hy = _size.y * 0.5f;
            // Counter-clockwise from the bottom-left corner. Each side starts at its corner; the other nodes go, one at a
            // time, to the side whose spacing is then the widest
            const Math::Vector2 corners[4] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
            const float lengths[4] = {_size.x, _size.y, _size.x, _size.y};
            int parts[4] = {1, 1, 1, 1};
            for (int extra = 4; extra < count; ++extra)
            {
                int widest = 0;
                for (int side = 1; side < 4; ++side)
                {
                    if (lengths[side] / static_cast<float>(parts[side]) > lengths[widest] / static_cast<float>(parts[widest]))
                    {
                        widest = side;
                    }
                }
                ++parts[widest];
            }
            for (int side = 0; side < 4; ++side)
            {
                const Math::Vector2 &from = corners[side];
                const Math::Vector2 &to = corners[(side + 1) % 4];
                for (int i = 0; i < parts[side]; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(parts[side]);
                    points.emplace_back(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
                }
            }
            break;
        }
        case SoftBodyShape2D::Outline:
        {
            if (_outline.size() < MinNodes || _outline.size() > MaxNodes || !std::ranges::all_of(_outline, Finite))
            {
                break;
            }
            points = _outline;
            if (TwiceArea(points) < 0.0f)
            {
                std::ranges::reverse(points);
            }
            break;
        }
        }
        return points;
    }

    void SoftBody2D::SetCircle(const float radius, const int nodeCount)
    {
        if (!(radius > 0.0f) || !std::isfinite(radius))
        {
            Logger::Warn("SoftBody2D: the radius must be a positive number; the shape was not changed");
            return;
        }
        _shape = SoftBodyShape2D::Circle;
        _radius = radius;
        _nodeCount = ClampNodes(nodeCount);
        Rebuild();
    }

    void SoftBody2D::SetBox(const Math::Vector2 &size, const int nodeCount)
    {
        if (!Finite(size) || !(size.x > 0.0f) || !(size.y > 0.0f))
        {
            Logger::Warn("SoftBody2D: the size must be positive numbers; the shape was not changed");
            return;
        }
        _shape = SoftBodyShape2D::Box;
        _size = size;
        _nodeCount = ClampNodes(nodeCount, 4);
        Rebuild();
    }

    bool SoftBody2D::SetOutline(const std::vector<Math::Vector2> &points)
    {
        if (points.size() < MinNodes || points.size() > MaxNodes || !std::ranges::all_of(points, Finite) ||
            TwiceArea(points) == 0.0f)
        {
            Logger::Warn("SoftBody2D: an outline is 3 to 64 finite points around an area; the shape was not changed");
            return false;
        }
        _shape = SoftBodyShape2D::Outline;
        _outline = points;
        Rebuild();
        return true;
    }

    // ===== Settings =====

    namespace
    {
        /// Sets `member` to a finite value of at least `minimum`; warns and returns false otherwise
        bool Take(float &member, const float value, const float minimum, const char *what)
        {
            if (!std::isfinite(value) || value < minimum)
            {
                Logger::Warn(std::format("SoftBody2D: {} must be a number of at least {}; it was not changed", what, minimum));
                return false;
            }
            member = value;
            return true;
        }
    }

    void SoftBody2D::SetNodeRadius(const float radius)
    {
        if (!(radius > 0.0f) || !std::isfinite(radius))
        {
            Logger::Warn("SoftBody2D: the node radius must be a positive number; it was not changed");
            return;
        }
        _nodeRadius = radius;
        Rebuild();
    }

    void SoftBody2D::SetDensity(const float density)
    {
        if (Take(_density, density, 0.0f, "the density"))
        {
            Rebuild();
        }
    }

    void SoftBody2D::SetFriction(const float friction)
    {
        if (Take(_friction, friction, 0.0f, "the friction"))
        {
            Rebuild();
        }
    }

    void SoftBody2D::SetBounciness(const float bounciness)
    {
        if (Take(_bounciness, bounciness, 0.0f, "the bounciness"))
        {
            Rebuild();
        }
    }

    void SoftBody2D::SetStiffness(const float edgeFrequency, const float edgeDamping, const float shapeFrequency,
                                  const float shapeDamping)
    {
        for (const float value : {edgeFrequency, edgeDamping, shapeFrequency, shapeDamping})
        {
            if (!std::isfinite(value) || value < 0.0f)
            {
                Logger::Warn("SoftBody2D: frequencies and damping ratios must be finite and not negative; the stiffness was not "
                             "changed");
                return;
            }
        }
        _edgeFrequency = edgeFrequency;
        _edgeDamping = edgeDamping;
        _shapeFrequency = shapeFrequency;
        _shapeDamping = shapeDamping;
        Rebuild();
    }

    void SoftBody2D::SetGravityScale(const float scale)
    {
        if (!std::isfinite(scale))
        {
            Logger::Warn("SoftBody2D: the gravity scale must be finite; it was not changed");
            return;
        }
        _gravityScale = scale;
        Rebuild();
    }

    void SoftBody2D::SetLinearDamping(const float damping)
    {
        if (Take(_linearDamping, damping, 0.0f, "the linear damping"))
        {
            Rebuild();
        }
    }

    // ===== Runtime =====

    Physics2DWorld *SoftBody2D::Live() const
    {
        const auto world = _world.lock();
        return world && _handle.IsValid() && world->SoftBodyHolds(_handle, &_gameObject) ? world.get() : nullptr;
    }

    bool SoftBody2D::HasBody() const
    {
        return Live() != nullptr;
    }

    Math::Vector2 SoftBody2D::Position() const
    {
        const Positionable *positionable = _gameObject.GetPositionable();
        if (!positionable)
        {
            return Math::Vector2{0.0f, 0.0f};
        }
        const Math::Vector3 position = positionable->GetPosition();
        return Math::Vector2{position.x, position.y};
    }

    std::vector<Math::Vector2> SoftBody2D::PlacedOutline() const
    {
        std::vector<Math::Vector2> points = RestOutline();
        const Positionable *positionable = _gameObject.GetPositionable();
        if (!positionable)
        {
            return points;
        }
        const auto matrix = positionable->GetLocalToWorldMatrix();
        for (Math::Vector2 &point : points)
        {
            const Math::Vector3 world = matrix.TransformPoint(Math::Vector3{point.x, point.y, 0.0f});
            point = Math::Vector2{world.x, world.y};
        }
        if (TwiceArea(points) < 0.0f)
        {
            std::ranges::reverse(points); // a mirroring scale
        }
        return points;
    }

    std::size_t SoftBody2D::GetNodeCount() const
    {
        return RestOutline().size();
    }

    std::vector<Math::Vector2> SoftBody2D::GetNodePositions() const
    {
        if (const Physics2DWorld *world = Live())
        {
            std::vector<Math::Vector2> ring;
            Math::Vector2 centre;
            if (world->ReadSoftBody(_handle, ring, centre))
            {
                return ring;
            }
        }
        return PlacedOutline();
    }

    Math::Vector2 SoftBody2D::GetCentre() const
    {
        if (const Physics2DWorld *world = Live())
        {
            std::vector<Math::Vector2> ring;
            Math::Vector2 centre;
            if (world->ReadSoftBody(_handle, ring, centre))
            {
                return centre;
            }
        }
        return Position();
    }

    Math::Vector2 SoftBody2D::GetVelocity() const
    {
        const Physics2DWorld *world = Live();
        return world ? world->GetLinearVelocity(_handle) : Math::Vector2{0.0f, 0.0f};
    }

    bool SoftBody2D::IsNodePinned(const std::size_t node) const
    {
        return std::ranges::find(_pinned, static_cast<int>(node)) != _pinned.end();
    }

    void SoftBody2D::SetNodePinned(const std::size_t node, const bool pinned)
    {
        if (node >= GetNodeCount() || IsNodePinned(node) == pinned)
        {
            return;
        }
        if (pinned)
        {
            _pinned.push_back(static_cast<int>(node));
            std::ranges::sort(_pinned);
        }
        else
        {
            std::erase(_pinned, static_cast<int>(node));
        }
        if (Physics2DWorld *world = Live())
        {
            world->SetSoftNodePinned(_handle, node, pinned);
        }
    }

    void SoftBody2D::AddImpulse(const Math::Vector2 &impulse)
    {
        if (Physics2DWorld *world = Live())
        {
            world->AddSoftBodyImpulse(_handle, impulse);
        }
    }

    void SoftBody2D::Rebuild()
    {
        DestroyBody();
        _warned = false;
        _meshStale = true;
    }

    void SoftBody2D::DestroyBody()
    {
        // Only a body that is still this component's: after a restore the handle may name nothing, or another's
        if (Physics2DWorld *world = Live())
        {
            world->DestroyBody(_handle);
        }
        _handle = Physics::INVALID_PHYSICS_HANDLE;
    }

    void SoftBody2D::Refresh()
    {
        if (!_attached || IsDestroyed() || !IsActive() || Live())
        {
            return;
        }
        Scene *scene = _gameObject.GetScene();
        const std::shared_ptr<Physics2DWorld> world = scene ? scene->GetOrCreatePhysics2DWorld() : nullptr;
        if (!world)
        {
            return; // a build without Box2D
        }
        SoftBodyDesc2D desc;
        desc.owner = &_gameObject;
        desc.ring = PlacedOutline();
        if (desc.ring.size() < MinNodes)
        {
            if (!std::exchange(_warned, true))
            {
                Logger::Warn(std::format("SoftBody2D on '{}': the shape's settings give no outline, so there is no body",
                                         _gameObject.GetName()));
            }
            return;
        }
        desc.centre = Position();
        desc.pinned.assign(desc.ring.size(), 0);
        for (const int node : _pinned)
        {
            if (node >= 0 && static_cast<std::size_t>(node) < desc.pinned.size())
            {
                desc.pinned[static_cast<std::size_t>(node)] = 1;
            }
        }
        desc.nodeRadius = _nodeRadius;
        desc.density = _density;
        desc.material.dynamicFriction = _friction;
        desc.material.staticFriction = _friction;
        desc.material.restitution = _bounciness;
        desc.layer = _gameObject.GetLayer();
        desc.edgeFrequency = _edgeFrequency;
        desc.edgeDamping = _edgeDamping;
        desc.shapeFrequency = _shapeFrequency;
        desc.shapeDamping = _shapeDamping;
        desc.gravityScale = _gravityScale;
        desc.linearDamping = _linearDamping;
        _handle = world->CreateSoftBody(desc);
        _world = world;
    }

    void SoftBody2D::OnAttach()
    {
        _attached = true; // made in the first FixedUpdate
    }

    void SoftBody2D::OnDestroy()
    {
        DestroyBody();
        _attached = false;
        GeometryRenderer2D::OnDestroy();
    }

    void SoftBody2D::OnEnable()
    {
        if (Physics2DWorld *world = Live())
        {
            world->SetBodyEnabled(_handle, IsActive());
        }
    }

    void SoftBody2D::OnDisable()
    {
        if (Physics2DWorld *world = Live())
        {
            world->SetBodyEnabled(_handle, false);
        }
    }

    void SoftBody2D::OnActiveFlagChanged()
    {
        if (Physics2DWorld *world = Live())
        {
            world->SetBodyEnabled(_handle, IsActive());
        }
    }

    void SoftBody2D::OnFixedUpdate()
    {
        Refresh();
    }

    void SoftBody2D::OnEditorFieldsChanged(const std::span<const std::string> changed)
    {
        const bool physical = std::ranges::any_of(changed, [](const std::string &name)
        {
            return name != "_color" && name != "_texture" && name != "_blendMode" && name != "_sortingOrder" &&
                   name != "color" && name != "texture" && name != "blendMode" && name != "sortingOrder" &&
                   name != "isActive";
        });
        _meshStale = true;
        if (physical)
        {
            Rebuild();
        }
    }

    void SoftBody2D::OnTransformChanged() const
    {
        if (Physics2DWorld *world = Live(); world && !world->IsSyncingTransforms(_handle))
        {
            world->MarkTransformDirty(_handle);
        }
    }

    // ===== Drawing =====

    Renderer::Common::MeshData SoftBody2D::BuildMesh(const std::vector<Math::Vector2> &nodes, const Math::Vector2 &centre,
                                                     const float z) const
    {
        Renderer::Common::MeshData data;
        const std::vector<Math::Vector2> rest = RestOutline();
        if (nodes.size() < MinNodes || nodes.size() != rest.size())
        {
            return data;
        }
        // The texture covers the rest outline's box
        Math::Vector2 low = rest.front();
        Math::Vector2 high = rest.front();
        for (const Math::Vector2 &point : rest)
        {
            low = Math::Vector2{std::min(low.x, point.x), std::min(low.y, point.y)};
            high = Math::Vector2{std::max(high.x, point.x), std::max(high.y, point.y)};
        }
        const auto uv = [&low, &high](const Math::Vector2 &point)
        {
            return Math::Vector2{high.x > low.x ? (point.x - low.x) / (high.x - low.x) : 0.0f,
                                 high.y > low.y ? (point.y - low.y) / (high.y - low.y) : 0.0f};
        };
        const auto add = [&data, z](const Math::Vector2 &position, const Math::Vector2 &texture)
        {
            data.vertices.push_back({{position.x, position.y, z}, {0.0f, 0.0f, 1.0f}, {texture.x, texture.y},
                                     {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}});
        };

        // Each node pushed out from the centre by its radius, so the drawing covers what collides
        std::vector<Math::Vector2> outer;
        outer.reserve(nodes.size());
        for (const Math::Vector2 &node : nodes)
        {
            const float dx = node.x - centre.x;
            const float dy = node.y - centre.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            const float out = length > 1e-6f ? _nodeRadius / length : 0.0f;
            outer.emplace_back(node.x + dx * out, node.y + dy * out);
        }

        // A circle or a box fans from its centre. An outline may be concave: it is triangulated as a polygon, and
        // fanned only while it is folded over itself
        std::vector<std::uint32_t> triangles;
        if (_shape == SoftBodyShape2D::Outline)
        {
            triangles = Rendering::TriangulatePolygon2D(outer);
        }
        if (!triangles.empty())
        {
            for (std::size_t i = 0; i < outer.size(); ++i)
            {
                add(outer[i], uv(rest[i]));
            }
            data.indices = std::move(triangles);
        }
        else
        {
            add(centre, uv(Math::Vector2{0.0f, 0.0f}));
            for (std::size_t i = 0; i < outer.size(); ++i)
            {
                add(outer[i], uv(rest[i]));
            }
            const auto count = static_cast<std::uint32_t>(outer.size());
            for (std::uint32_t i = 0; i < count; ++i)
            {
                data.indices.insert(data.indices.end(), {0u, 1u + i, 1u + (i + 1u) % count});
            }
        }
        return data;
    }

    std::optional<BoundingBox> SoftBody2D::GetWorldBounds() const
    {
        const Positionable *positionable = _gameObject.GetPositionable();
        const std::vector<Math::Vector2> nodes = GetNodePositions();
        if (!positionable || nodes.empty())
        {
            return std::nullopt;
        }
        const float z = positionable->GetPosition().z;
        // Twice the node radius: the drawn outline is pushed out by one
        const float margin = 2.0f * _nodeRadius;
        Math::Vector3 low{nodes.front().x, nodes.front().y, z};
        Math::Vector3 high = low;
        for (const Math::Vector2 &node : nodes)
        {
            low = Math::Vector3{std::min(low.x, node.x), std::min(low.y, node.y), z};
            high = Math::Vector3{std::max(high.x, node.x), std::max(high.y, node.y), z};
        }
        return BoundingBox{Math::Vector3{low.x - margin, low.y - margin, z},
                           Math::Vector3{high.x + margin, high.y + margin, z}};
    }

    void SoftBody2D::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                   const RenderQueue queue)
    {
        const Positionable *positionable = _gameObject.GetPositionable();
        if (!renderer || queue != RenderQueue::Transparent || !positionable)
        {
            return;
        }
        const std::vector<Math::Vector2> nodes = GetNodePositions();
        const Math::Vector2 centre = GetCentre();
        const float z = positionable->GetPosition().z;
        const bool moved = _meshStale || z != _meshZ || centre.x != _meshCentre.x || centre.y != _meshCentre.y ||
                           !std::ranges::equal(nodes, _meshNodes, [](const Math::Vector2 &a, const Math::Vector2 &b)
                                               { return a.x == b.x && a.y == b.y; });
        if (moved)
        {
            Rendering::UpdateGeometryMesh(_mesh, BuildMesh(nodes, centre, z));
            _meshNodes = nodes;
            _meshCentre = centre;
            _meshZ = z;
            _meshStale = false;
        }
        if (!_mesh)
        {
            return;
        }
        _draw.Draw(renderer, _mesh, Math::Matrix<float, 4, 4>::identity(), _blendMode, _texture, _color, queue, state);
    }
}
