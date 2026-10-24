#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <math/Vector3.hpp>

#include "engine/physics/TetMeshData.hpp"

namespace N2Engine::SoftBodies
{
    /// A point of the mesh's local space under `m`, the top three rows of a row-major 4x4 matrix (the layout of
    /// SoftBodyDesc::localToWorld)
    [[nodiscard]] inline Math::Vector3 TransformPoint(const float *m, const Math::Vector3 &p)
    {
        return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
    }

    /// Six times the signed volume of the tetrahedron a, b, c, d
    [[nodiscard]] inline float TetVolume6(const Math::Vector3 &a, const Math::Vector3 &b, const Math::Vector3 &c,
                                          const Math::Vector3 &d)
    {
        const float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
        const float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
        const float e3x = d.x - a.x, e3y = d.y - a.y, e3z = d.z - a.z;
        return (e1y * e2z - e1z * e2y) * e3x + (e1z * e2x - e1x * e2z) * e3y + (e1x * e2y - e1y * e2x) * e3z;
    }

    /**
     * Puts the mesh under `localToWorld` for a backend that needs every tetrahedron positively oriented in world
     * space: `rest` gets the transformed node positions, `tets` the mesh's tetrahedra with corners 2 and 3 swapped
     * where the transform turned one inside out (a mirrored transform, a negative determinant), and `flipped` is 1
     * for each swapped tetrahedron. The embedding's weights stay in the mesh's original corner order, so SkinSurface
     * swaps the weights of corners 2 and 3 of a flipped tetrahedron to match. (The CPU backend does the same
     * orientation inside its own setup, which also needs the rest volumes.)
     */
    inline void OrientTets(const Physics::TetMeshData &mesh, const float *localToWorld,
                           std::vector<Math::Vector3> &rest, std::vector<std::array<std::uint32_t, 4>> &tets,
                           std::vector<std::uint8_t> &flipped)
    {
        rest.resize(mesh.positions.size());
        for (std::size_t i = 0; i < mesh.positions.size(); ++i)
        {
            rest[i] = TransformPoint(localToWorld, mesh.positions[i]);
        }
        tets = mesh.tets;
        flipped.assign(tets.size(), 0);
        for (std::size_t t = 0; t < tets.size(); ++t)
        {
            auto &tet = tets[t];
            if (TetVolume6(rest[tet[0]], rest[tet[1]], rest[tet[2]], rest[tet[3]]) < 0.0f)
            {
                std::swap(tet[2], tet[3]);
                flipped[t] = 1;
            }
        }
    }

    /**
     * The render-vertex positions of a body: every vertex's embedding (a tetrahedron and four barycentric weights)
     * applied to the current node positions. Shared by both soft-body backends. `Node` is any type with float x, y
     * and z members; `tets` and `flipped` are what OrientTets (or the CPU backend's setup) made. `out` is replaced
     * and keeps its capacity.
     */
    template <typename Node>
    void SkinSurface(const Physics::TetMeshData &mesh, const std::span<const std::array<std::uint32_t, 4>> tets,
                     const std::span<const std::uint8_t> flipped, const std::span<const Node> nodes,
                     std::vector<Math::Vector3> &out)
    {
        out.clear();
        out.reserve(mesh.surfaceEmbedding.size());
        for (const Physics::TetEmbedding &e : mesh.surfaceEmbedding)
        {
            const auto &tet = tets[e.tet];
            float x = 0.0f, y = 0.0f, z = 0.0f;
            const bool swapped = flipped[e.tet] != 0;
            for (int c = 0; c < 4; ++c)
            {
                const Node &p = nodes[tet[c]];
                const float weight = e.weights[swapped && c >= 2 ? 5 - c : c]; // corners 2 and 3 were swapped
                x += weight * p.x;
                y += weight * p.y;
                z += weight * p.z;
            }
            out.emplace_back(x, y, z);
        }
    }
}
