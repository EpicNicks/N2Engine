#include "engine/ui/QuadClip.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace N2Engine::UI
{
    namespace
    {
        using Renderer::Common::Vertex;

        /// Twice the signed area of (from, to, vertex): positive when the vertex is left of the edge from -> to
        float Side(const Math::Vector2 &from, const Math::Vector2 &to, const Vertex &vertex)
        {
            return (to.x - from.x) * (vertex.position[1] - from.y) - (to.y - from.y) * (vertex.position[0] - from.x);
        }

        /// The vertex a share `t` of the way from `a` to `b`
        Vertex Between(const Vertex &a, const Vertex &b, const float t)
        {
            Vertex result = a;
            const auto mix = [t](float *out, const float *to, const std::size_t count)
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    out[i] += (to[i] - out[i]) * t;
                }
            };
            mix(result.position, b.position, 3);
            mix(result.texCoord, b.texCoord, 2);
            mix(result.color, b.color, 4);
            return result;
        }
    }

    Renderer::Common::MeshData ClipQuadMesh(const Renderer::Common::MeshData &quads, const ConvexPolygon &clip)
    {
        Renderer::Common::MeshData result;
        if (clip.size() < 3)
        {
            return result;
        }
        std::vector<Vertex> polygon;
        std::vector<Vertex> input;
        for (std::size_t first = 0; first + 4 <= quads.vertices.size(); first += 4)
        {
            polygon.assign(quads.vertices.begin() + static_cast<std::ptrdiff_t>(first),
                           quads.vertices.begin() + static_cast<std::ptrdiff_t>(first + 4));
            for (std::size_t edge = 0; edge < clip.size() && !polygon.empty(); ++edge)
            {
                const Math::Vector2 &from = clip[edge];
                const Math::Vector2 &to = clip[(edge + 1) % clip.size()];
                input.swap(polygon);
                polygon.clear();
                for (std::size_t i = 0; i < input.size(); ++i)
                {
                    const Vertex &current = input[i];
                    const Vertex &next = input[(i + 1) % input.size()];
                    const float currentSide = Side(from, to, current);
                    const float nextSide = Side(from, to, next);
                    if (currentSide >= 0.0f)
                    {
                        polygon.push_back(current);
                    }
                    if ((currentSide > 0.0f && nextSide < 0.0f) || (currentSide < 0.0f && nextSide > 0.0f))
                    {
                        polygon.push_back(Between(current, next, currentSide / (currentSide - nextSide)));
                    }
                }
            }
            if (polygon.size() < 3)
            {
                continue;
            }
            const auto base = static_cast<std::uint32_t>(result.vertices.size());
            result.vertices.insert(result.vertices.end(), polygon.begin(), polygon.end());
            for (std::uint32_t i = 1; i + 1 < polygon.size(); ++i)
            {
                result.indices.insert(result.indices.end(), {base, base + i, base + i + 1});
            }
        }
        return result;
    }
}
