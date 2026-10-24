#pragma once

#ifdef N2ENGINE_PHYSX_ENABLED

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <math/Vector3.hpp>

namespace physx
{
    class PxDeformableVolumeMesh;
    class PxInsertionCallback;
}

namespace N2Engine::Physics
{
    /// Releases a cooked PhysX deformable volume mesh (defined in the .cpp, so this header needs no PhysX include)
    struct DeformableMeshReleaser
    {
        void operator()(physx::PxDeformableVolumeMesh *mesh) const;
    };

    /// A tetrahedral mesh cooked for a PhysX deformable volume
    struct CookedDeformableMesh
    {
        /// The cooked collision + simulation mesh (the same tetrahedra for both); null when cooking failed
        std::unique_ptr<physx::PxDeformableVolumeMesh, DeformableMeshReleaser> mesh;
        /// For each input node, its vertex index in the cooked collision mesh. Cooking may order the vertices
        /// differently from the input; this is what lets the host read the input's nodes out of the volume.
        std::vector<std::uint32_t> nodeToVertex;
        /// Why cooking failed (empty when it did not)
        std::string error;

        [[nodiscard]] bool Ok() const { return mesh != nullptr; }
        /// The cooked collision mesh's vertex and tetrahedron counts; the simulation mesh's vertex count
        [[nodiscard]] std::uint32_t GetCollisionVertexCount() const;
        [[nodiscard]] std::uint32_t GetCollisionTetCount() const;
        [[nodiscard]] std::uint32_t GetSimulationVertexCount() const;
        /// Replaces `out` with the cooked collision mesh's vertices, in the cooked order
        void CopyCollisionVertices(std::vector<Math::Vector3> &out) const;
        /// The smallest signed volume (times 6) among the cooked collision mesh's tetrahedra, from the cooked
        /// vertices and indices as stored; positive when every tetrahedron kept its orientation
        [[nodiscard]] float GetMinTetVolume6() const;
    };

    /**
     * Cooks a tetrahedral mesh for a PhysX deformable volume with the GPU data a GPU scene needs
     * (PxCookingParams::buildGPUData). The same tetrahedra serve as the collision and the simulation mesh (as
     * PxDeformableVolumeExt::createDeformableVolumeMeshNoVoxels does), so a render mesh embedded in the engine's
     * tetrahedra stays valid. Every tetrahedron must be positively oriented (swap two corners of a mirrored one
     * first; SoftBodies::OrientTets does). Cooking is CPU code: this needs no GPU and no CUDA context, and runs
     * wherever PhysX is linked. `insertion` is the physics SDK's insertion callback; null uses the standalone one
     * (which needs the PhysX foundation to exist).
     *
     * On failure the result is not Ok() and `error` says why.
     */
    [[nodiscard]] CookedDeformableMesh CookDeformableMesh(std::span<const Math::Vector3> positions,
                                                          std::span<const std::array<std::uint32_t, 4>> tets,
                                                          physx::PxInsertionCallback *insertion = nullptr);
}

#endif
