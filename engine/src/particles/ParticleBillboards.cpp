#include "engine/particles/ParticleBillboards.hpp"

#include <algorithm>
#include <cstdint>

namespace N2Engine::Particles
{
    namespace
    {
        float Lerp(const float a, const float b, const float t)
        {
            return a + (b - a) * t;
        }
    }

    void BuildBillboards(Renderer::Common::MeshData &mesh, const std::span<const Math::Vector4> particles,
                         const Rendering::CameraBasis &camera, const BillboardStyle &style)
    {
        using Renderer::Common::Vertex;

        const std::size_t count = particles.size();
        mesh.vertices.resize(count * 4);

        const std::size_t oldQuads = mesh.indices.size() / 6;
        mesh.indices.resize(count * 6);
        for (std::size_t q = oldQuads; q < count; ++q)
        {
            const auto base = static_cast<std::uint32_t>(q * 4);
            std::uint32_t *i = &mesh.indices[q * 6];
            i[0] = base;
            i[1] = base + 1;
            i[2] = base + 2;
            i[3] = base;
            i[4] = base + 2;
            i[5] = base + 3;
        }

        // The corners as multiples of (right, up), in the order bottom-left, bottom-right, top-right, top-left
        static constexpr float kCornerX[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
        static constexpr float kCornerY[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        static constexpr float kU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        static constexpr float kV[4] = {0.0f, 0.0f, 1.0f, 1.0f};

        const float nx = -camera.forward.x;
        const float ny = -camera.forward.y;
        const float nz = -camera.forward.z;

        for (std::size_t p = 0; p < count; ++p)
        {
            const Math::Vector4 &particle = particles[p];
            const float age = std::clamp(particle.w, 0.0f, 1.0f);
            const float half = 0.5f * std::max(0.0f, Lerp(style.startSize, style.endSize, age));
            const float r = Lerp(style.startColor.r, style.endColor.r, age);
            const float g = Lerp(style.startColor.g, style.endColor.g, age);
            const float b = Lerp(style.startColor.b, style.endColor.b, age);
            const float a = Lerp(style.startColor.a, style.endColor.a, age);

            for (int c = 0; c < 4; ++c)
            {
                Vertex &vertex = mesh.vertices[p * 4 + static_cast<std::size_t>(c)];
                const float sx = kCornerX[c] * half;
                const float sy = kCornerY[c] * half;
                vertex.position[0] = particle.x + camera.right.x * sx + camera.up.x * sy;
                vertex.position[1] = particle.y + camera.right.y * sx + camera.up.y * sy;
                vertex.position[2] = particle.z + camera.right.z * sx + camera.up.z * sy;
                vertex.normal[0] = nx;
                vertex.normal[1] = ny;
                vertex.normal[2] = nz;
                vertex.texCoord[0] = kU[c];
                vertex.texCoord[1] = kV[c];
                vertex.color[0] = r;
                vertex.color[1] = g;
                vertex.color[2] = b;
                vertex.color[3] = a;
                vertex.tangent[0] = 0.0f;
                vertex.tangent[1] = 0.0f;
                vertex.tangent[2] = 0.0f;
                vertex.tangent[3] = 0.0f;
            }
        }
    }
}
