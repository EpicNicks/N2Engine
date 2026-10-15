#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <renderer/common/RenderTypes.hpp>

#include "engine/physics/TetMeshData.hpp"

namespace N2Engine::Rendering
{
    class Mesh;
}

namespace N2Engine::SoftBodies
{
    struct TetSettings
    {
        /// Voxels along the longest axis of the mesh's bounding box; clamped to [Physics::kMinTetResolution,
        /// Physics::kMaxTetResolution]
        int resolution = 10;
        /// The body may not have more tetrahedra than this: the resolution is lowered until it fits
        std::size_t maxTets = Physics::kMaxTetsPerBody;
    };

    /// What Tetrahedralise returns: the mesh, or an error (and no mesh)
    struct TetResult
    {
        std::shared_ptr<const Physics::TetMeshData> mesh;
        /// Why there is no mesh (no triangles, flat, nothing inside, ...); empty on success
        std::string error;
        /// A TetCache answered without building (always false from Tetrahedralise itself)
        bool cacheHit = false;

        [[nodiscard]] bool Ok() const { return mesh != nullptr; }
    };

    /**
     * The engine's voxel tetrahedraliser: turns a render mesh (a triangle soup that need not be watertight or
     * manifold) into a tetrahedral mesh a soft-body backend can simulate. Pure engine code, no PhysX, the same in
     * every build.
     *
     * 1. Weld: render vertices are split at UV and normal seams, so vertices closer than 1e-5 of the bounding box
     *    diagonal are welded into one position, and triangles that collapse are dropped.
     * 2. Voxelise: a grid of cubic voxels, `resolution` along the longest axis of the bounding box, centred on the box.
     *    A voxel is inside when at least two of three ray-parity tests agree (a ray from the voxel centre along +x,
     *    +y and +z crosses the surface an odd number of times; the rays are offset slightly so they never run along
     *    an edge shared by two triangles). The majority vote makes a small hole or a crack in one direction harmless,
     *    which is why parity is used rather than a single ray. A voxel that holds a welded vertex is inside too, so
     *    every vertex lies in a tetrahedron (a thin fin narrower than a voxel still gets one).
     * 3. Tetrahedra: 5 per inside voxel, one central tetrahedron and four at the corners, in a pattern that
     *    alternates with the parity of the voxel's (i + j + k), so neighbouring voxels meet on matching face
     *    diagonals and the mesh conforms. Every tetrahedron has a positive volume.
     * 4. Embedding: every ORIGINAL (unwelded) render vertex gets a tetrahedron and barycentric weights, computed
     *    once per welded position and copied to the duplicates, so seam duplicates move together.
     *
     * Fails (no mesh, an error) for no usable triangles, a flat mesh (an extent under 1e-5 of the longest) or nothing
     * inside. The resolution is clamped to its range and lowered until the body has at most settings.maxTets
     * tetrahedra (TetMeshData::resolution says what was used); it fails only when even the lowest resolution is too
     * big.
     */
    [[nodiscard]] TetResult Tetrahedralise(std::span<const Renderer::Common::Vertex> vertices,
                                           std::span<const std::uint32_t> indices, const TetSettings &settings = {});
    /// The same for a Mesh's CPU data (Mesh::GetVertices and GetIndices)
    [[nodiscard]] TetResult Tetrahedralise(const Rendering::Mesh &mesh, const TetSettings &settings = {});

    /**
     * Keeps the tetrahedral meshes Tetrahedralise made, so a soft body that starts again on the same mesh (play mode
     * entered twice, many bodies on one mesh) doesn't build it again. Keyed by the Mesh object, its
     * Mesh::GetVersion() and the requested resolution; SetData or another resolution misses. A process-wide cache
     * owned by the soft-body code, NOT Mesh::SetCpuCache (that slot holds the picking BVH, and a second type would
     * evict it). It holds the meshes weakly (an entry whose Mesh is gone never matches and is dropped), keeps at most
     * kMaxEntries entries (the oldest goes first) and is main-thread only. Failures are not cached.
     */
    class TetCache
    {
    public:
        static constexpr std::size_t kMaxEntries = 32;

        [[nodiscard]] static TetCache &Instance();

        /// The cached mesh for `mesh` at this version and resolution, or one built now (and kept)
        [[nodiscard]] TetResult GetOrBuild(const std::shared_ptr<const Rendering::Mesh> &mesh, int resolution);

        void Clear();
        [[nodiscard]] std::size_t Size() const { return _entries.size(); }
        /// How many GetOrBuild calls were answered from the cache and how many had to build, since the process started
        [[nodiscard]] std::size_t GetHitCount() const { return _hits; }
        [[nodiscard]] std::size_t GetMissCount() const { return _misses; }

    private:
        struct Entry
        {
            std::weak_ptr<const Rendering::Mesh> mesh;
            std::uint64_t version = 0;
            int resolution = 0;
            std::shared_ptr<const Physics::TetMeshData> data;
        };

        std::vector<Entry> _entries; // oldest first
        std::size_t _hits = 0;
        std::size_t _misses = 0;
    };
}
