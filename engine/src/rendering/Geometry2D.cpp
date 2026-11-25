#include "engine/rendering/Geometry2D.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

#include <renderer/common/BlendCutOut.hpp>

#include "engine/GameObject.hpp"
#include "engine/common/ScriptUtils.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        using Math::Vector2;
        using Math::Vector3;
        using Renderer::Common::MeshData;
        using Renderer::Common::Vertex;

        constexpr float kSamePoint = 1e-6f;

        bool Fail(std::string *error, std::string reason)
        {
            if (error)
            {
                *error = std::move(reason);
            }
            return false;
        }

        bool Finite(const Vector3 &p)
        {
            return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        }

        Vertex MakeVertex(const float x, const float y, const float z, const float u, const float v, const Common::Color &color)
        {
            Vertex vertex{};
            vertex.position[0] = x;
            vertex.position[1] = y;
            vertex.position[2] = z;
            vertex.normal[2] = 1.0f;
            vertex.texCoord[0] = u;
            vertex.texCoord[1] = v;
            vertex.color[0] = color.r;
            vertex.color[1] = color.g;
            vertex.color[2] = color.b;
            vertex.color[3] = color.a;
            vertex.tangent[0] = 1.0f;
            vertex.tangent[3] = 1.0f;
            return vertex;
        }

        float SignedArea2(const Vertex &a, const Vertex &b, const Vertex &c)
        {
            return (b.position[0] - a.position[0]) * (c.position[1] - a.position[1]) -
                   (b.position[1] - a.position[1]) * (c.position[0] - a.position[0]);
        }

        /// Adds triangle a, b, c counter-clockwise (swapping two corners when it winds the other way)
        void AddTriangle(MeshData &data, const std::uint32_t a, std::uint32_t b, std::uint32_t c)
        {
            if (SignedArea2(data.vertices[a], data.vertices[b], data.vertices[c]) < 0.0f)
            {
                std::swap(b, c);
            }
            data.indices.insert(data.indices.end(), {a, b, c});
        }

        Common::Color LerpColor(const Common::Color &a, const Common::Color &b, const float t)
        {
            return Common::Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
        }

        float Cross2(const Vector2 &a, const Vector2 &b, const Vector2 &c)
        {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        }
    }

    // ===== Lines =====

    MeshData MakeLineMesh(const std::span<const LinePoint> input, const LineShape &shape, std::string *error)
    {
        MeshData data;
        std::vector<LinePoint> points;
        points.reserve(input.size());
        for (const LinePoint &point : input)
        {
            if (!Finite(point.position) || !std::isfinite(point.width))
            {
                Fail(error, "a point of the line is not finite");
                return data;
            }
            if (!points.empty())
            {
                const float dx = point.position.x - points.back().position.x;
                const float dy = point.position.y - points.back().position.y;
                if (dx * dx + dy * dy <= kSamePoint * kSamePoint)
                {
                    continue;
                }
            }
            points.push_back(point);
        }
        bool loop = shape.loop;
        if (loop && points.size() > 2)
        {
            const float dx = points.front().position.x - points.back().position.x;
            const float dy = points.front().position.y - points.back().position.y;
            if (dx * dx + dy * dy <= kSamePoint * kSamePoint)
            {
                points.pop_back();
            }
        }
        const std::size_t count = points.size();
        if (count < 2)
        {
            Fail(error, "a line needs at least two distinct points");
            return data;
        }
        if (count < 3)
        {
            loop = false;
        }
        const float miterLimit = std::isfinite(shape.miterLimit) ? std::max(shape.miterLimit, 1.0f) : 1.0f;

        // Each segment's direction and length, and the length along the line to each point (for u)
        const std::size_t segments = loop ? count : count - 1;
        std::vector<Vector2> direction(segments);
        std::vector<float> along(count + 1, 0.0f);
        for (std::size_t s = 0; s < segments; ++s)
        {
            const Vector3 &a = points[s].position;
            const Vector3 &b = points[(s + 1) % count].position;
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            direction[s] = Vector2(dx / length, dy / length);
            along[s + 1] = along[s] + length;
        }
        const float total = along[segments] > 0.0f ? along[segments] : 1.0f;

        // Every point has an "in" pair (where the segment before it ends) and an "out" pair (where the segment after it starts), left
        // then right. They are the same place except at a bevel, where the outer corner splits in two
        struct Pair
        {
            std::uint32_t in[2]{};
            std::uint32_t out[2]{};
        };
        std::vector<Pair> pairs(count);
        data.vertices.reserve(count * 4);
        const auto add = [&](const Vector2 &at, const LinePoint &point, const float u, const float v)
        {
            data.vertices.push_back(MakeVertex(at.x, at.y, point.position.z, u, v, point.color));
            return static_cast<std::uint32_t>(data.vertices.size() - 1);
        };
        for (std::size_t i = 0; i < count; ++i)
        {
            const LinePoint &point = points[i];
            const Vector2 p(point.position.x, point.position.y);
            const float half = 0.5f * std::max(point.width, 0.0f);
            const float uOut = along[i] / total;
            const float uIn = loop && i == 0 ? 1.0f : uOut;
            const bool hasBefore = loop || i > 0;
            const bool hasAfter = loop || i + 1 < count;
            Pair &pair = pairs[i];

            const auto straight = [&](const Vector2 &centre, const Vector2 &offset)
            {
                const Vector2 left(centre.x + offset.x, centre.y + offset.y);
                const Vector2 right(centre.x - offset.x, centre.y - offset.y);
                pair.in[0] = add(left, point, uIn, 1.0f);
                pair.in[1] = add(right, point, uIn, 0.0f);
                pair.out[0] = add(left, point, uOut, 1.0f);
                pair.out[1] = add(right, point, uOut, 0.0f);
            };

            if (!hasBefore || !hasAfter)
            {
                // An end: square to its segment, moved out by half the width for a square cap
                const Vector2 d = hasAfter ? direction[0] : direction[segments - 1];
                const float push = shape.cap == LineCap::Square ? (hasAfter ? -half : half) : 0.0f;
                straight(Vector2(p.x + d.x * push, p.y + d.y * push), Vector2(-d.y * half, d.x * half));
                continue;
            }

            const Vector2 before = direction[(i + segments - 1) % segments];
            const Vector2 after = direction[i];
            const Vector2 n1(-before.y, before.x);
            const Vector2 n2(-after.y, after.x);
            const float turn = before.x * after.y - before.y * after.x; // above 0: a left turn
            const float facing = n1.x * n2.x + n1.y * n2.y;
            if (facing > 1.0f - 1e-6f)
            {
                straight(p, Vector2(n1.x * half, n1.y * half));
                continue;
            }
            Vector2 miter(n1.x + n2.x, n1.y + n2.y);
            const float miterLength = std::sqrt(miter.x * miter.x + miter.y * miter.y);
            // How far the corner point is, in half widths: 1 / cos(half the turn). Infinite for a turn straight back
            float scale = std::numeric_limits<float>::infinity();
            if (miterLength > 1e-6f)
            {
                miter = Vector2(miter.x / miterLength, miter.y / miterLength);
                const float cosine = miter.x * n1.x + miter.y * n1.y;
                if (cosine > 1e-6f)
                {
                    scale = 1.0f / cosine;
                }
            }
            if (shape.join == LineJoin::Miter && scale <= miterLimit)
            {
                straight(p, Vector2(miter.x * half * scale, miter.y * half * scale));
                continue;
            }

            // A bevel: the outer side (right on a left turn) has a corner for each segment, the inner side one point on the miter,
            // no further than the miter limit (at the line's centre for a turn straight back)
            const float side = turn > 0.0f ? -1.0f : 1.0f; // +1: the outer side is the left
            const float innerScale = std::isfinite(scale) ? std::min(scale, miterLimit) : 0.0f;
            const Vector2 inner(p.x - side * miter.x * half * innerScale, p.y - side * miter.y * half * innerScale);
            const Vector2 outerIn(p.x + side * n1.x * half, p.y + side * n1.y * half);
            const Vector2 outerOut(p.x + side * n2.x * half, p.y + side * n2.y * half);
            const int outer = side > 0.0f ? 0 : 1;
            const int innerSlot = 1 - outer;
            pair.in[outer] = add(outerIn, point, uIn, outer == 0 ? 1.0f : 0.0f);
            pair.in[innerSlot] = add(inner, point, uIn, innerSlot == 0 ? 1.0f : 0.0f);
            pair.out[outer] = add(outerOut, point, uOut, outer == 0 ? 1.0f : 0.0f);
            pair.out[innerSlot] = add(inner, point, uOut, innerSlot == 0 ? 1.0f : 0.0f);
            AddTriangle(data, pair.in[outer], pair.out[outer], pair.in[innerSlot]);
        }

        data.indices.reserve(data.indices.size() + segments * 6);
        for (std::size_t s = 0; s < segments; ++s)
        {
            const Pair &a = pairs[s];
            const Pair &b = pairs[(s + 1) % count];
            // Right then left at each end: counter-clockwise for a segment drawn left to right
            AddTriangle(data, a.out[1], b.in[1], b.in[0]);
            AddTriangle(data, a.out[1], b.in[0], a.out[0]);
        }
        return data;
    }

    std::vector<LinePoint> LinePointsAlongLength(const std::span<const Math::Vector3> positions, const bool loop,
                                                 const float startWidth, const float endWidth, const Common::Color &startColor,
                                                 const Common::Color &endColor)
    {
        const std::size_t count = positions.size();
        std::vector<float> along(count, 0.0f);
        for (std::size_t i = 1; i < count; ++i)
        {
            along[i] = along[i - 1] + (positions[i] - positions[i - 1]).Length();
        }
        float total = count > 0 ? along[count - 1] : 0.0f;
        if (loop && count > 2)
        {
            total += (positions[0] - positions[count - 1]).Length();
        }
        std::vector<LinePoint> points(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            float t = 0.0f;
            if (total > 0.0f && std::isfinite(total))
            {
                t = along[i] / total;
            }
            else if (count > 1)
            {
                t = static_cast<float>(i) / static_cast<float>(count - 1);
            }
            points[i].position = positions[i];
            points[i].width = startWidth + (endWidth - startWidth) * t;
            points[i].color = LerpColor(startColor, endColor, t);
        }
        return points;
    }

    // ===== Polygons =====

    std::vector<std::uint32_t> TriangulatePolygon2D(const std::span<const Math::Vector2> points, std::string *error)
    {
        std::vector<std::uint32_t> triangles;
        const std::size_t count = points.size();
        if (count < 3 || count > kMaxTriangulatePoints)
        {
            Fail(error, std::format("a polygon needs 3 to {} points, not {}", kMaxTriangulatePoints, count));
            return triangles;
        }
        double area = 0.0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const Vector2 &a = points[i];
            const Vector2 &b = points[(i + 1) % count];
            if (!std::isfinite(a.x) || !std::isfinite(a.y))
            {
                Fail(error, "a point of the polygon is not finite");
                return triangles;
            }
            area += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
        }
        if (!(std::abs(area) > 1e-12))
        {
            Fail(error, "the polygon has no area");
            return triangles;
        }
        // Worked on counter-clockwise; the indices still name the caller's points
        std::vector<std::uint32_t> ring(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            ring[i] = static_cast<std::uint32_t>(area > 0.0 ? i : count - 1 - i);
        }
        const auto inside = [&](const Vector2 &p, const Vector2 &a, const Vector2 &b, const Vector2 &c)
        {
            return Cross2(a, b, p) >= 0.0f && Cross2(b, c, p) >= 0.0f && Cross2(c, a, p) >= 0.0f;
        };
        const auto same = [](const Vector2 &a, const Vector2 &b) { return a.x == b.x && a.y == b.y; };
        triangles.reserve((count - 2) * 3);
        while (ring.size() > 3)
        {
            const std::size_t n = ring.size();
            bool clipped = false;
            for (std::size_t k = 0; k < n && !clipped; ++k)
            {
                const std::uint32_t i0 = ring[(k + n - 1) % n], i1 = ring[k], i2 = ring[(k + 1) % n];
                const Vector2 &a = points[i0], &b = points[i1], &c = points[i2];
                if (Cross2(a, b, c) <= 0.0f)
                {
                    continue; // a reflex corner, or three in a line
                }
                bool ear = true;
                for (std::size_t j = 0; j < n && ear; ++j)
                {
                    const std::uint32_t other = ring[j];
                    if (other == i0 || other == i1 || other == i2)
                    {
                        continue;
                    }
                    const Vector2 &p = points[other];
                    if (same(p, a) || same(p, b) || same(p, c))
                    {
                        continue;
                    }
                    ear = !inside(p, a, b, c);
                }
                if (ear)
                {
                    triangles.insert(triangles.end(), {i0, i1, i2});
                    ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(k));
                    clipped = true;
                }
            }
            if (clipped)
            {
                continue;
            }
            // No ear: a corner with no area (points in a line, or a point repeated) is dropped; anything else crosses itself
            bool dropped = false;
            for (std::size_t k = 0; k < n && !dropped; ++k)
            {
                const Vector2 &a = points[ring[(k + n - 1) % n]], &b = points[ring[k]], &c = points[ring[(k + 1) % n]];
                if (std::abs(Cross2(a, b, c)) <= 1e-12f)
                {
                    ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(k));
                    dropped = true;
                }
            }
            if (!dropped)
            {
                Fail(error, "the polygon crosses itself");
                return {};
            }
        }
        if (Cross2(points[ring[0]], points[ring[1]], points[ring[2]]) > 0.0f)
        {
            triangles.insert(triangles.end(), {ring[0], ring[1], ring[2]});
        }
        if (triangles.empty())
        {
            Fail(error, "the polygon has no area");
        }
        return triangles;
    }

    // ===== Script meshes =====

    std::optional<MeshData> MakeScriptMeshData(const ScriptMeshDesc &desc, std::string *error)
    {
        const std::size_t vertexCount = desc.positions.size();
        if (vertexCount == 0 || vertexCount > kMaxScriptMeshVertices)
        {
            Fail(error, std::format("a mesh needs 1 to {} vertices, not {}", kMaxScriptMeshVertices, vertexCount));
            return std::nullopt;
        }
        if (!desc.colors.empty() && desc.colors.size() != vertexCount)
        {
            Fail(error, std::format("{} colours for {} vertices: give one per vertex, or none", desc.colors.size(), vertexCount));
            return std::nullopt;
        }
        if (!desc.uvs.empty() && desc.uvs.size() != vertexCount)
        {
            Fail(error, std::format("{} uvs for {} vertices: give one per vertex, or none", desc.uvs.size(), vertexCount));
            return std::nullopt;
        }
        const std::size_t indexCount = desc.indices.size();
        if (indexCount == 0 || indexCount > kMaxScriptMeshIndices || indexCount % 3 != 0)
        {
            Fail(error, std::format("a mesh needs whole triangles, 3 to {} indices, not {}", kMaxScriptMeshIndices, indexCount));
            return std::nullopt;
        }
        MeshData data;
        data.vertices.reserve(vertexCount);
        const Common::Color white(1.0f, 1.0f, 1.0f, 1.0f);
        for (std::size_t i = 0; i < vertexCount; ++i)
        {
            const Vector3 &p = desc.positions[i];
            if (!Finite(p))
            {
                Fail(error, std::format("vertex {} is not finite", i + static_cast<std::size_t>(desc.indexBase)));
                return std::nullopt;
            }
            const Vector2 uv = desc.uvs.empty() ? Vector2(0.0f, 0.0f) : desc.uvs[i];
            data.vertices.push_back(MakeVertex(p.x, p.y, p.z, uv.x, uv.y, desc.colors.empty() ? white : desc.colors[i]));
        }
        data.indices.reserve(indexCount);
        const std::int64_t first = desc.indexBase;
        const std::int64_t last = desc.indexBase + static_cast<std::int64_t>(vertexCount) - 1;
        for (std::size_t i = 0; i < indexCount; ++i)
        {
            const std::int64_t index = desc.indices[i];
            if (index < first || index > last)
            {
                Fail(error, std::format("index {} is {}, outside the vertices {} to {}", i + static_cast<std::size_t>(desc.indexBase),
                                        index, first, last));
                return std::nullopt;
            }
            data.indices.push_back(static_cast<std::uint32_t>(index - first));
        }
        return data;
    }

    // ===== Drawing =====

    void Geometry2DDraw::Draw(Renderer::Common::IRenderer *renderer, const std::shared_ptr<const Mesh> &mesh,
                              const Math::Matrix<float, 4, 4> &world, const Blend2D blend, const std::shared_ptr<Texture> &texture,
                              const Common::Color &tint, const RenderQueue queue, const Renderer::Common::RenderState &state)
    {
        if (!renderer || !mesh)
        {
            return;
        }
        if (!_material)
        {
            _material = Material::Create(ShadingModel::Unlit);
            _material->SetAlphaMode(AlphaMode::Blend);
            _material->SetDoubleSided(true);
        }
        const BlendMode mode = blend == Blend2D::Additive ? BlendMode::Additive : BlendMode::Alpha;
        if (_material->GetBlendMode() != mode)
        {
            _material->SetBlendMode(mode);
        }
        if (_material->GetBaseColorTexture() != texture)
        {
            _material->SetBaseColorTexture(texture);
        }
        // Textured or not, the vertex colours carry alpha (a fading trail): on a renderer that cannot blend, cut out what is under half
        const float discard = Renderer::Common::BlendCutOutBelow(true, renderer->SupportsBlending(), tint.a);
        if (_material->GetBlendDiscardBelow() != discard)
        {
            _material->SetBlendDiscardBelow(discard);
        }
        // Every submesh draws with the one material (a mesh asset may have several)
        _slots.assign(std::max<std::size_t>(mesh->GetSubmeshCount(), 1), _material);
        _resources.Bind(renderer);
        _resources.Draw(mesh, _slots, Material::GetDefaultUnlit(), world, tint, queue, state);
    }

    void UpdateGeometryMesh(std::shared_ptr<Mesh> &mesh, Renderer::Common::MeshData data)
    {
        if (data.vertices.empty() || data.indices.empty())
        {
            mesh.reset();
            return;
        }
        if (mesh && mesh->SetData(std::move(data)))
        {
            return;
        }
        if (!mesh)
        {
            mesh = Mesh::Create(std::move(data));
        }
    }

    // ===== GeometryRenderer2D =====

    GeometryRenderer2D::GeometryRenderer2D(GameObject &gameObject) : IRenderable(gameObject)
    {
        _gameObject.CreatePositionable();
        RegisterMember(NAMEOF(_blendMode), _blendMode);
        RegisterMember(NAMEOF(_sortingOrder), _sortingOrder);
    }

    GeometryRenderer2D::~GeometryRenderer2D()
    {
        // A component freed without OnDestroy (its object never joined a scene) may outlive its renderer
        _draw.Release(false);
    }

    RenderQueueKey GeometryRenderer2D::GetRenderQueue() const
    {
        return {RenderQueue::Transparent, _sortingOrder};
    }

    void GeometryRenderer2D::Render(Renderer::Common::IRenderer *renderer)
    {
        RenderInQueue(renderer, Renderer::Common::RenderState::Transparent(), RenderQueue::Transparent);
    }

    void GeometryRenderer2D::InitializeRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (renderer)
        {
            _draw.Bind(renderer);
        }
    }

    void GeometryRenderer2D::CleanupRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (!renderer || renderer != _draw.GetRenderer())
        {
            return;
        }
        _draw.Release(true);
    }

    void GeometryRenderer2D::OnDestroy()
    {
        CleanupRenderResources(_draw.GetRenderer());
    }
}
