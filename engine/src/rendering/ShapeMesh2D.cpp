#include "engine/rendering/ShapeMesh2D.hpp"

#include <algorithm>
#include <cstddef>
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
            const int sides = desc.shape == Shape2D::RegularPolygon ? desc.sides : 0;
            add(&sides, 4);
            add(&desc.outline, 4);
            for (const float f : {desc.regionOffset.x, desc.regionOffset.y, desc.regionSize.x, desc.regionSize.y, desc.pivot.x, desc.pivot.y})
            {
                add(&f, 4);
            }
            if (desc.shape == Shape2D::Polygon)
            {
                for (const Vector2 &p : desc.vertices)
                {
                    add(&p.x, 4);
                    add(&p.y, 4);
                }
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

        // The ring's inner edge. The exact inner boundary is the intersection of the sides moved inward by the width (Sutherland-
        // Hodgman clipping of the shape by each moved side). Each corner's mitre point is kept when it lies inside that boundary and
        // moved to the nearest point on it otherwise, so a sharp corner or a short side narrows the ring there instead of folding it.
        // Only an outline so wide that nothing is left inside fills the shape.
        std::vector<Vector2> inner;
        if (desc.outline > 0.0f && std::isfinite(desc.outline))
        {
            const auto inwardNormal = [](const Vector2 &p, const Vector2 &q)
            {
                const float dx = q.x - p.x, dy = q.y - p.y;
                const float length = std::sqrt(dx * dx + dy * dy);
                return Vector2(-dy / length, dx / length); // left of a counter-clockwise edge is inside
            };
            std::vector<Vector2> core = outer;
            for (std::size_t i = 0; i < count && !core.empty(); ++i)
            {
                const Vector2 &p = outer[i];
                const Vector2 n = inwardNormal(p, outer[(i + 1) % count]);
                const float limit = n.x * p.x + n.y * p.y + desc.outline;
                const auto side = [&](const Vector2 &v) { return n.x * v.x + n.y * v.y - limit; };
                std::vector<Vector2> clipped;
                for (std::size_t j = 0; j < core.size(); ++j)
                {
                    const Vector2 &u = core[j];
                    const Vector2 &v = core[(j + 1) % core.size()];
                    const float su = side(u), sv = side(v);
                    if (su >= 0.0f)
                    {
                        clipped.push_back(u);
                    }
                    if ((su >= 0.0f) != (sv >= 0.0f))
                    {
                        const float t = su / (su - sv);
                        clipped.emplace_back(u.x + (v.x - u.x) * t, u.y + (v.y - u.y) * t);
                    }
                }
                core = std::move(clipped);
            }
            float coreArea = 0.0f;
            for (std::size_t j = 0; j < core.size(); ++j)
            {
                const Vector2 &u = core[j], &v = core[(j + 1) % core.size()];
                coreArea += u.x * v.y - v.x * u.y;
            }
            if (core.size() >= 3 && coreArea > 1e-8f)
            {
                const auto inside = [&core](const Vector2 &point)
                {
                    for (std::size_t j = 0; j < core.size(); ++j)
                    {
                        if (Cross(core[j], core[(j + 1) % core.size()], point) < 0.0f)
                        {
                            return false;
                        }
                    }
                    return true;
                };
                const auto nearestOnCore = [&core](const Vector2 &point)
                {
                    Vector2 best = core[0];
                    float bestDistance = -1.0f;
                    for (std::size_t j = 0; j < core.size(); ++j)
                    {
                        const Vector2 &u = core[j], &v = core[(j + 1) % core.size()];
                        const float ex = v.x - u.x, ey = v.y - u.y;
                        const float lengthSquared = ex * ex + ey * ey;
                        float t = lengthSquared > 0.0f ? ((point.x - u.x) * ex + (point.y - u.y) * ey) / lengthSquared : 0.0f;
                        t = std::clamp(t, 0.0f, 1.0f);
                        const Vector2 candidate(u.x + ex * t, u.y + ey * t);
                        const float dx = point.x - candidate.x, dy = point.y - candidate.y;
                        const float distance = dx * dx + dy * dy;
                        if (bestDistance < 0.0f || distance < bestDistance)
                        {
                            bestDistance = distance;
                            best = candidate;
                        }
                    }
                    return best;
                };
                inner.resize(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    const Vector2 &prev = outer[(i + count - 1) % count];
                    const Vector2 &here = outer[i];
                    const Vector2 &next = outer[(i + 1) % count];
                    const Vector2 n1 = inwardNormal(prev, here);
                    const Vector2 n2 = inwardNormal(here, next);
                    const float denominator = 1.0f + n1.x * n2.x + n1.y * n2.y;
                    Vector2 mitre = here;
                    if (denominator > 1e-4f)
                    {
                        const float scale = desc.outline / denominator;
                        mitre = Vector2(here.x + (n1.x + n2.x) * scale, here.y + (n1.y + n2.y) * scale);
                    }
                    inner[i] = inside(mitre) ? mitre : nearestOnCore(mitre);
                }
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
        static std::size_t sweepAt = 256;
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
            // One warning per reason, so a size animated through bad values logs once, not every frame
            if (warned.insert(error).second)
            {
                Logger::Warn("Sprite2D: " + error + "; it draws nothing");
            }
            return nullptr;
        }
        std::shared_ptr<Mesh> mesh = Mesh::Create(std::move(data));
        if (cache.size() > sweepAt)
        {
            std::erase_if(cache, [](const auto &entry) { return entry.second.expired(); });
            sweepAt = std::max<std::size_t>(256, cache.size() * 2); // amortised: a full sweep at most once per doubling
        }
        cache[key] = mesh;
        return mesh;
    }
}
