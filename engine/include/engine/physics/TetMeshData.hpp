#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <math/Vector3.hpp>

namespace N2Engine::Physics
{
    /// The most tetrahedra one soft body may have. The tetrahedraliser lowers the resolution until the body fits,
    /// and the CPU backend refuses a body above it.
    inline constexpr std::size_t kMaxTetsPerBody = 20000;
    /// The voxel resolution range (voxels along the longest axis) the tetrahedraliser accepts; a request outside it is
    /// clamped to it
    inline constexpr int kMinTetResolution = 2;
    inline constexpr int kMaxTetResolution = 32;

    /// Where one render vertex sits inside the tetrahedral mesh: a tetrahedron and the barycentric weights of its
    /// four corners. The weights sum to 1; a vertex inside the tetrahedron has all four in [0, 1]. They reproduce the
    /// vertex's rest position exactly (a weight may be slightly negative for a vertex a little outside).
    struct TetEmbedding
    {
        std::uint32_t tet = 0;
        float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    };

    /**
     * A tetrahedral mesh of a body and how its render vertices ride on it (no PhysX types: both soft-body backends
     * take it). Built from a render mesh by SoftBodies::Tetrahedralise; the positions are in the mesh's own (local)
     * space, at rest.
     */
    struct TetMeshData
    {
        std::vector<Math::Vector3> positions;
        /// Four node indices each; every tetrahedron has a positive volume (a right-handed corner order)
        std::vector<std::array<std::uint32_t, 4>> tets;
        /// One entry per ORIGINAL render vertex of the source mesh (seam duplicates included, which share one entry's
        /// value, so they stay together)
        std::vector<TetEmbedding> surfaceEmbedding;
        /// How many distinct positions the render vertices had after welding (the seam duplicates collapse)
        std::uint32_t weldedVertexCount = 0;
        /// The voxel resolution actually used (the request, clamped and lowered to fit kMaxTetsPerBody)
        int resolution = 0;
        /// Edge length of a voxel in the mesh's space
        float voxelSize = 0.0f;
    };
}
