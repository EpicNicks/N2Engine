#include "engine/rendering/ShapeMesh2D.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

#include <math/DeterministicMath.hpp>

#include "engine/Logger.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        using Math::Vector2;

        constexpr float kPi = 3.14159265358979323846f;

        float Cross(const Vector2 &a, const Vector2 &b, const Vector2 &c)
        {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        }

        bool Fail(std::string *error, std::string reason)
        {
            if (error)
            {
                *error = std::move(reason);
            }
            return false;
        }

        // Unit points around the origin, before the size and pivot. The size box is -0.5 to 0.5 on each axis.
        bool UnitPoints(const ShapeDesc2D &desc, std::vector<Vector2> &points, std::string *error)
        {
            const auto ring = [&points](const int count, const float startAngle)
            {
                for (int i = 0; i < count; ++i)
                {
                    const float angle = startAngle + 2.0f * kPi * static_cast<float>(i) / static_cast<float>(count);
                    // The engine's own sine and cosine (DeterministicMath): the same mesh on every platform
                    points.emplace_back(0.5f * Math::Cos(angle), 0.5f * Math::Sin(angle));
                }
            };
            switch (desc.shape)
            {
            case Shape2D::Rect:
                points = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
                return true;
            case Shape2D::Triangle:
                points = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.0f, 0.5f}};
                return true;
            case Shape2D::Circle:
                ring(kCircleSegments2D, 0.0f);
                return true;
            case Shape2D::RegularPolygon:
                if (desc.sides < 3 || desc.sides > kMaxSides2D)
                {
                    return Fail(error, std::format("a regular polygon needs 3 to {} sides, not {}", kMaxSides2D, desc.sides));
                }
                ring(desc.sides, 0.5f * kPi); // a corner at the top
                return true;
            case Shape2D::Polygon:
            {
                const std::size_t count = desc.vertices.size();
                if (count < 3 || count > static_cast<std::size_t>(kMaxSides2D))
                {
                    return Fail(error, std::format("a polygon needs 3 to {} points, not {}", kMaxSides2D, count));
                }
                points = desc.vertices;
                for (const Vector2 &p : points)
                {
                    if (!std::isfinite(p.x) || !std::isfinite(p.y))
                    {
                        return Fail(error, "a polygon point is not finite");
                    }
                }
                // Either winding is fine; the mesh is counter-clockwise
                float area = 0.0f;
                for (std::size_t i = 0; i < count; ++i)
                {
                    const Vector2 &a = points[i];
                    const Vector2 &b = points[(i + 1) % count];
                    area += a.x * b.y - b.x * a.y;
                }
                if (area < 0.0f)
                {
                    std::reverse(points.begin(), points.end());
                }
                for (std::size_t i = 0; i < count; ++i)
                {
                    if (Cross(points[i], points[(i + 1) % count], points[(i + 2) % count]) <= 0.0f)
                    {
                        return Fail(error, "the polygon is not convex (or has three points in a line)");
                    }
                }
                return true;
            }
            }
            return Fail(error, "unknown shape");
        }

        Renderer::Common::Vertex MakeVertex(const Vector2 &position, const Vector2 &uv)
        {
            Renderer::Common::Vertex v{};
            v.position[0] = position.x;
            v.position[1] = position.y;
            v.normal[2] = 1.0f;
            v.texCoord[0] = uv.x;
            v.texCoord[1] = uv.y;
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
            v.tangent[0] = 1.0f;
            v.tangent[3] = 1.0f;
            return v;
        }

        std::string KeyOf(const ShapeDesc2D &desc)
        {
            std::string key;
            const auto add = [&key](const void *data, const std::size_t bytes) { key.append(static_cast<const char *>(data), bytes); };
            const auto shape = static_cast<std::uint8_t>(desc.shape);
            add(&shape, 1);
            add(&desc.size.x, 4);
            add(&desc.size.y, 4);
            add(&desc.sides, 4);
            add(&desc.outline, 4);
            for (const float f : {desc.regionOffset.x, desc.regionOffset.y, desc.regionSize.x, desc.regionSize.y, desc.pivot.x, desc.pivot.y})
            {
                add(&f, 4);
            }
            for (const Vector2 &p : desc.vertices)
            {
                add(&p.x, 4);
                add(&p.y, 4);
            }
            return key;
        }
    }

    std::vector<Math::Vector2> ShapeOutline2D(const ShapeDesc2D &desc, std::string *error)
    {
        if (!(desc.size.x > 0.0f) || !(desc.size.y > 0.0f) || !std::isfinite(desc.size.x) || !std::isfinite(desc.size.y))
        {
            Fail(error, "the size must be above zero");
            return {};
        }
        if (!std::isfinite(desc.pivot.x) || !std::isfinite(desc.pivot.y))
        {
            Fail(error, "the pivot must be finite");
            return {};
        }
        std::vector<Vector2> points;
        if (!UnitPoints(desc, points, error))
        {
            return {};
        }
        const float shiftX = (0.5f - desc.pivot.x) * desc.size.x;
        const float shiftY = (0.5f - desc.pivot.y) * desc.size.y;
        for (Vector2 &p : points)
        {
            p = Vector2(p.x * desc.size.x + shiftX, p.y * desc.size.y + shiftY);
        }
        return points;
    }

    Renderer::Common::MeshData MakeShapeMesh2D(const ShapeDesc2D &desc, std::string *error)
    {
        Renderer::Common::MeshData data;
        const std::vector<Vector2> outer = ShapeOutline2D(desc, error);
        if (outer.empty())
        {
            return data;
        }
        // uv maps the size box onto the region
        const float left = (0.0f - desc.pivot.x) * desc.size.x;
        const float bottom = (0.0f - desc.pivot.y) * desc.size.y;
        const auto uvOf = [&](const Vector2 &p)
        {
            const float u = (p.x - left) / desc.size.x;
            const float v = (p.y - bottom) / desc.size.y;
            return Vector2(desc.regionOffset.x + u * desc.regionSize.x, desc.regionOffset.y + v * desc.regionSize.y);
        };
        const std::size_t count = outer.size();

        // The ring's inner edge: each side moved inward by the width, corners where neighbouring sides meet. Where a side would
        // turn round (the width reaches past the middle), the shape is filled instead.
        std::vector<Vector2> inner;
        if (desc.outline > 0.0f && std::isfinite(desc.outline))
        {
            inner.resize(count);
            bool fits = true;
            for (std::size_t i = 0; i < count && fits; ++i)
            {
                const Vector2 &prev = outer[(i + count - 1) % count];
                const Vector2 &here = outer[i];
                const Vector2 &next = outer[(i + 1) % count];
                const auto inwardNormal = [](const Vector2 &a, const Vector2 &b)
                {
                    const float dx = b.x - a.x, dy = b.y - a.y;
                    const float length = std::sqrt(dx * dx + dy * dy);
                    return Vector2(-dy / length, dx / length); // left of a counter-clockwise edge is inside
                };
                const Vector2 n1 = inwardNormal(prev, here);
                const Vector2 n2 = inwardNormal(here, next);
                const float denominator = 1.0f + n1.x * n2.x + n1.y * n2.y;
                if (!(denominator > 1e-4f))
                {
                    fits = false;
                    break;
                }
                // The mitre: the corner moves along the bisector so both sides end up `outline` away
                const float scale = desc.outline / denominator;
                inner[i] = Vector2(here.x + (n1.x + n2.x) * scale, here.y + (n1.y + n2.y) * scale);
            }
            for (std::size_t i = 0; i < count && fits; ++i)
            {
                // A side of the inner edge that points the other way than its outer side means the ring has folded over
                const Vector2 &a = outer[i], &b = outer[(i + 1) % count];
                const Vector2 &c = inner[i], &d = inner[(i + 1) % count];
                if ((b.x - a.x) * (d.x - c.x) + (b.y - a.y) * (d.y - c.y) <= 0.0f)
                {
                    fits = false;
                }
            }
            if (!fits)
            {
                inner.clear();
            }
        }

        if (inner.empty())
        {
            // A fan around the middle of the points
            Vector2 middle;
            for (const Vector2 &p : outer)
            {
                middle = Vector2(middle.x + p.x, middle.y + p.y);
            }
            middle = Vector2(middle.x / static_cast<float>(count), middle.y / static_cast<float>(count));
            data.vertices.push_back(MakeVertex(middle, uvOf(middle)));
            for (const Vector2 &p : outer)
            {
                data.vertices.push_back(MakeVertex(p, uvOf(p)));
            }
            for (std::uint32_t i = 0; i < count; ++i)
            {
                data.indices.insert(data.indices.end(), {0u, 1u + i, 1u + (i + 1) % static_cast<std::uint32_t>(count)});
            }
            return data;
        }

        for (std::size_t i = 0; i < count; ++i)
        {
            data.vertices.push_back(MakeVertex(outer[i], uvOf(outer[i])));
            data.vertices.push_back(MakeVertex(inner[i], uvOf(inner[i])));
        }
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uint32_t o0 = 2 * i, i0 = 2 * i + 1;
            const std::uint32_t o1 = 2 * ((i + 1) % static_cast<std::uint32_t>(count)), i1 = o1 + 1;
            data.indices.insert(data.indices.end(), {o0, o1, i1, o0, i1, i0});
        }
        return data;
    }

    std::shared_ptr<Mesh> GetShapeMesh2D(const ShapeDesc2D &desc)
    {
        // Weak entries: a mesh lives as long as a sprite holds it. Expired entries are swept as the map grows.
        static std::map<std::string, std::weak_ptr<Mesh>> cache;
        static std::set<std::string> warned;
        const std::string key = KeyOf(desc);
        if (const auto it = cache.find(key); it != cache.end())
        {
            if (auto mesh = it->second.lock())
            {
                return mesh;
            }
        }
        std::string error;
        Renderer::Common::MeshData data = MakeShapeMesh2D(desc, &error);
        if (data.vertices.empty())
        {
            if (warned.insert(key).second)
            {
                Logger::Warn("Sprite2D: " + error + "; it draws nothing");
            }
            return nullptr;
        }
        std::shared_ptr<Mesh> mesh = Mesh::Create(std::move(data));
        if (cache.size() > 256)
        {
            std::erase_if(cache, [](const auto &entry) { return entry.second.expired(); });
        }
        cache[key] = mesh;
        return mesh;
    }
}
