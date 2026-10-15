#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/Logger.hpp"
#include "engine/physics/ISoftBodyBackend.hpp"
#include "engine/physics/TetMeshData.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/softbody/Tetrahedraliser.hpp"

// Shared by the soft-body tests: small meshes, tetrahedral bodies made from them, and a warning counter

namespace N2Engine::TestSupport
{
    /// A box centred on the origin, `sx` x `sy` x `sz`, as 8 welded vertices and 12 triangles (outward, counter-clockwise)
    inline Renderer::Common::MeshData MakeWeldedBox(const float sx, const float sy, const float sz)
    {
        Renderer::Common::MeshData data;
        const float half[3] = {sx * 0.5f, sy * 0.5f, sz * 0.5f};
        for (int i = 0; i < 8; ++i)
        {
            Renderer::Common::Vertex vertex{};
            for (int a = 0; a < 3; ++a)
            {
                vertex.position[a] = ((i >> a) & 1) != 0 ? half[a] : -half[a];
            }
            vertex.texCoord[0] = static_cast<float>(i & 1);
            vertex.texCoord[1] = static_cast<float>((i >> 1) & 1);
            vertex.normal[1] = 1.0f;
            vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 1.0f;
            data.vertices.push_back(vertex);
        }
        constexpr int corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int a = 0; a < 3; ++a)
        {
            const int u = (a + 1) % 3;
            const int v = (a + 2) % 3;
            for (int side = 0; side < 2; ++side)
            {
                std::uint32_t q[4];
                for (int c = 0; c < 4; ++c)
                {
                    q[c] = static_cast<std::uint32_t>((side << a) | (corner[c][0] << u) | (corner[c][1] << v));
                }
                const auto &p0 = data.vertices[q[0]].position;
                const auto &p1 = data.vertices[q[1]].position;
                const auto &p2 = data.vertices[q[2]].position;
                const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
                const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
                const float cross[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                        e1[0] * e2[1] - e1[1] * e2[0]};
                const float outward = side == 1 ? 1.0f : -1.0f;
                const bool flip = cross[a] * outward < 0.0f;
                const std::uint32_t order[6] = {q[0], flip ? q[2] : q[1], flip ? q[1] : q[2], q[0],
                                                flip ? q[3] : q[2], flip ? q[2] : q[3]};
                data.indices.insert(data.indices.end(), std::begin(order), std::end(order));
            }
        }
        return data;
    }

    inline std::shared_ptr<Rendering::Mesh> MakeBoxMesh(const float sx = 1.0f, const float sy = 1.0f,
                                                        const float sz = 1.0f)
    {
        return Rendering::Mesh::Create(MakeWeldedBox(sx, sy, sz));
    }

    /// Six times the signed volume of tetrahedron `t` (in doubles, from the stored float positions)
    inline double TetVolume6(const Physics::TetMeshData &mesh, const std::size_t t)
    {
        const auto &tet = mesh.tets[t];
        const auto &a = mesh.positions[tet[0]];
        const auto &b = mesh.positions[tet[1]];
        const auto &c = mesh.positions[tet[2]];
        const auto &d = mesh.positions[tet[3]];
        const double e1[3] = {double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z};
        const double e2[3] = {double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z};
        const double e3[3] = {double(d.x) - a.x, double(d.y) - a.y, double(d.z) - a.z};
        return (e1[1] * e2[2] - e1[2] * e2[1]) * e3[0] + (e1[2] * e2[0] - e1[0] * e2[2]) * e3[1] +
               (e1[0] * e2[1] - e1[1] * e2[0]) * e3[2];
    }

    inline double TotalVolume(const Physics::TetMeshData &mesh)
    {
        double total = 0.0;
        for (std::size_t t = 0; t < mesh.tets.size(); ++t)
        {
            total += TetVolume6(mesh, t) / 6.0;
        }
        return total;
    }

    /// A tetrahedral body of a box, made by the engine's tetrahedraliser
    inline std::shared_ptr<const Physics::TetMeshData> MakeBoxTets(const int resolution, const float sx = 1.0f,
                                                                   const float sy = 1.0f, const float sz = 1.0f)
    {
        const Renderer::Common::MeshData data = MakeWeldedBox(sx, sy, sz);
        SoftBodies::TetSettings settings;
        settings.resolution = resolution;
        return SoftBodies::Tetrahedralise(data.vertices, data.indices, settings).mesh;
    }

    /// A description of a body at `position` (a translation), with the box tetrahedra
    inline Physics::SoftBodyDesc MakeBoxDesc(const int resolution, const Math::Vector3 &position = {},
                                             const float size = 1.0f)
    {
        Physics::SoftBodyDesc desc;
        desc.tetMesh = MakeBoxTets(resolution, size, size, size);
        desc.localToWorld[3] = position.x;
        desc.localToWorld[7] = position.y;
        desc.localToWorld[11] = position.z;
        return desc;
    }

    /// The mean of a set of points
    inline Math::Vector3 Centroid(const std::vector<Math::Vector3> &points)
    {
        double x = 0.0, y = 0.0, z = 0.0;
        for (const auto &p : points)
        {
            x += p.x;
            y += p.y;
            z += p.z;
        }
        const double n = points.empty() ? 1.0 : static_cast<double>(points.size());
        return Math::Vector3(static_cast<float>(x / n), static_cast<float>(y / n), static_cast<float>(z / n));
    }

    /// Collects the warnings logged while it lives
    class SoftBodyWarningCapture
    {
    public:
        SoftBodyWarningCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn)
                {
                    warnings.emplace_back(message);
                }
            };
        }
        ~SoftBodyWarningCapture() { Logger::logEvent -= _id; }
        SoftBodyWarningCapture(const SoftBodyWarningCapture &) = delete;
        SoftBodyWarningCapture &operator=(const SoftBodyWarningCapture &) = delete;

        /// How many warnings contain `text`
        [[nodiscard]] int Count(const std::string &text) const
        {
            return static_cast<int>(std::ranges::count_if(
                warnings, [&](const std::string &w) { return w.find(text) != std::string::npos; }));
        }

        std::vector<std::string> warnings;

    private:
        std::size_t _id = 0;
    };
}
