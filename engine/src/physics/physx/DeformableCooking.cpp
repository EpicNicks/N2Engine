#include "engine/physics/physx/DeformableCooking.hpp"

#ifdef N2ENGINE_PHYSX_ENABLED

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

#include <PxPhysicsAPI.h>
#include <cooking/PxCooking.h>
#include <cooking/PxTetrahedronMeshDesc.h>
#include <geometry/PxTetrahedronMesh.h>

using namespace physx;

// PhysX cooking of a soft body's tetrahedra. Cooking is CPU code (no PX_SUPPORT_GPU_PHYSX guard, no CUDA context),
// so this file is compiled and unit-tested on every leg that links PhysX; only creating a volume from the result
// (PhysXDeformableDriver) needs a GPU.

namespace N2Engine::Physics
{
    void DeformableMeshReleaser::operator()(PxDeformableVolumeMesh *mesh) const
    {
        if (mesh)
        {
            mesh->release();
        }
    }

    std::uint32_t CookedDeformableMesh::GetCollisionVertexCount() const
    {
        return mesh ? mesh->getCollisionMesh()->getNbVertices() : 0;
    }

    std::uint32_t CookedDeformableMesh::GetCollisionTetCount() const
    {
        return mesh ? mesh->getCollisionMesh()->getNbTetrahedrons() : 0;
    }

    std::uint32_t CookedDeformableMesh::GetSimulationVertexCount() const
    {
        return mesh ? mesh->getSimulationMesh()->getNbVertices() : 0;
    }

    void CookedDeformableMesh::CopyCollisionVertices(std::vector<Math::Vector3> &out) const
    {
        out.clear();
        if (!mesh)
        {
            return;
        }
        const PxTetrahedronMesh *collision = mesh->getCollisionMesh();
        const PxVec3 *vertices = collision->getVertices();
        for (PxU32 i = 0; i < collision->getNbVertices(); ++i)
        {
            out.emplace_back(vertices[i].x, vertices[i].y, vertices[i].z);
        }
    }

    float CookedDeformableMesh::GetMinTetVolume6() const
    {
        if (!mesh)
        {
            return 0.0f;
        }
        const PxTetrahedronMesh *collision = mesh->getCollisionMesh();
        const PxVec3 *v = collision->getVertices();
        const void *indices = collision->getTetrahedrons();
        const bool small = collision->getTetrahedronMeshFlags().isSet(PxTetrahedronMeshFlag::e16_BIT_INDICES);
        float smallest = std::numeric_limits<float>::max();
        for (PxU32 t = 0; t < collision->getNbTetrahedrons(); ++t)
        {
            PxU32 i[4];
            for (PxU32 c = 0; c < 4; ++c)
            {
                i[c] = small ? static_cast<const PxU16 *>(indices)[t * 4 + c]
                             : static_cast<const PxU32 *>(indices)[t * 4 + c];
            }
            const PxVec3 e1 = v[i[1]] - v[i[0]];
            const PxVec3 e2 = v[i[2]] - v[i[0]];
            const PxVec3 e3 = v[i[3]] - v[i[0]];
            smallest = std::min(smallest, e1.cross(e2).dot(e3));
        }
        return smallest;
    }

    namespace
    {
        using PositionKey = std::array<std::uint32_t, 3>;

        // The exact bit pattern of a position (cooking copies vertices, so equal positions stay bit-equal)
        PositionKey KeyOf(const float x, const float y, const float z)
        {
            PositionKey key;
            std::memcpy(&key[0], &x, sizeof(float));
            std::memcpy(&key[1], &y, sizeof(float));
            std::memcpy(&key[2], &z, sizeof(float));
            return key;
        }
    }

    CookedDeformableMesh CookDeformableMesh(const std::span<const Math::Vector3> positions,
                                            const std::span<const std::array<std::uint32_t, 4>> tets,
                                            PxInsertionCallback *insertion)
    {
        CookedDeformableMesh result;
        if (positions.size() < 4 || tets.empty())
        {
            result.error = "a deformable volume needs at least one tetrahedron";
            return result;
        }
        if (tets.size() > PX_MAX_NB_DEFORMABLE_VOLUME_TET)
        {
            result.error = "the tetrahedral mesh has more tetrahedra than PhysX supports in one deformable volume";
            return result;
        }

        std::vector<PxVec3> points(positions.size());
        for (std::size_t i = 0; i < positions.size(); ++i)
        {
            const Math::Vector3 &p = positions[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            {
                result.error = "the tetrahedral mesh has a non-finite position";
                return result;
            }
            points[i] = PxVec3(p.x, p.y, p.z);
        }
        std::vector<PxU32> indices;
        indices.reserve(tets.size() * 4);
        for (const auto &tet : tets)
        {
            for (const std::uint32_t node : tet)
            {
                if (node >= positions.size())
                {
                    result.error = "the tetrahedral mesh has an out-of-range node index";
                    return result;
                }
                indices.push_back(node);
            }
        }

        PxTetrahedronMeshDesc desc;
        desc.points.count = static_cast<PxU32>(points.size());
        desc.points.stride = sizeof(PxVec3);
        desc.points.data = points.data();
        desc.tetrahedrons.count = static_cast<PxU32>(tets.size());
        desc.tetrahedrons.stride = 4 * sizeof(PxU32);
        desc.tetrahedrons.data = indices.data();
        desc.tetsPerElement = 1;
        if (!desc.isValid())
        {
            result.error = "PhysX rejected the tetrahedral mesh description";
            return result;
        }

        PxCookingParams params{PxTolerancesScale()};
        params.buildGPUData = true;
        if (!insertion)
        {
            insertion = PxGetStandaloneInsertionCallback();
        }
        if (!insertion)
        {
            result.error = "PhysX has no insertion callback to cook with";
            return result;
        }

        // The collision and the simulation mesh are the same tetrahedra; the collision-to-simulation mapping is
        // computed by cooking
        const PxDeformableVolumeSimulationDataDesc simulationData;
        result.mesh.reset(PxCreateDeformableVolumeMesh(params, desc, desc, simulationData, *insertion));
        if (!result.mesh)
        {
            result.error = "PhysX could not cook the tetrahedral mesh for a deformable volume";
            return result;
        }

        const PxTetrahedronMesh *collision = result.mesh->getCollisionMesh();
        const PxTetrahedronMesh *simulation = result.mesh->getSimulationMesh();
        if (!collision || !simulation || collision->getNbVertices() != positions.size() ||
            simulation->getNbVertices() != collision->getNbVertices() ||
            collision->getNbTetrahedrons() != tets.size())
        {
            result.mesh.reset();
            result.error = "PhysX changed the number of vertices or tetrahedra while cooking";
            return result;
        }

        // Vertex order: normally untouched (only the tetrahedra are sorted); if cooking did reorder the vertices the
        // map is rebuilt by position
        const PxVec3 *cooked = collision->getVertices();
        result.nodeToVertex.resize(positions.size());
        bool identity = true;
        for (std::size_t i = 0; i < positions.size() && identity; ++i)
        {
            identity = cooked[i].x == points[i].x && cooked[i].y == points[i].y && cooked[i].z == points[i].z;
        }
        if (identity)
        {
            for (std::size_t i = 0; i < positions.size(); ++i)
            {
                result.nodeToVertex[i] = static_cast<std::uint32_t>(i);
            }
            return result;
        }

        std::map<PositionKey, std::vector<std::uint32_t>> byPosition;
        for (std::uint32_t v = collision->getNbVertices(); v-- > 0;)
        {
            byPosition[KeyOf(cooked[v].x, cooked[v].y, cooked[v].z)].push_back(v);
        }
        for (std::size_t i = 0; i < positions.size(); ++i)
        {
            const auto it = byPosition.find(KeyOf(points[i].x, points[i].y, points[i].z));
            if (it == byPosition.end() || it->second.empty())
            {
                result.mesh.reset();
                result.nodeToVertex.clear();
                result.error = "PhysX reordered the vertices of the tetrahedral mesh and they could not be matched";
                return result;
            }
            result.nodeToVertex[i] = it->second.back();
            it->second.pop_back();
        }
        return result;
    }
}

#endif
