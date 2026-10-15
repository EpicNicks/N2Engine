#include "engine/physics/physx/PhysXDeformableDriver.hpp"

#if N2ENGINE_PHYSX_GPU_PROBE

#include <algorithm>
#include <format>
#include <utility>

#include <PxPhysicsAPI.h>
#include <cudamanager/PxCudaContext.h>
#include <cudamanager/PxCudaContextManager.h>
#include <extensions/PxCudaHelpersExt.h>
#include <extensions/PxDeformableVolumeExt.h>

using namespace physx;

// Every PhysX deformable-volume call in the engine, and every CUDA call made for soft bodies, is in this file. None
// of it runs without a CUDA context, so it is compiled in CI (on the Windows legs, where the GPU library's headers
// and import library are) but only run by the GPU golden test (tests/engine/physics/PhysXGpuSoftBodiesTests.cpp) on a
// machine with an NVIDIA GPU. The calls follow PhysX 5.6.1's SnippetDeformableVolume; see docs/soft-bodies.html for
// what is unverified.

namespace N2Engine::Physics
{
    namespace
    {
        using SoftBodies::DeformableVec4;

        static_assert(sizeof(DeformableVec4) == sizeof(PxVec4), "the host mirror is uploaded as PxVec4");

        CUdeviceptr Device(const void *pointer)
        {
            return reinterpret_cast<CUdeviceptr>(pointer);
        }

        // The ratio of the heaviest to the lightest node PhysX may give a body (SnippetDeformableVolume uses 50)
        constexpr PxReal kMaxInvMassRatio = 50.0f;

        PxDeformableVolumeMaterialModel::Enum ToPhysX(const SoftMaterialModel model)
        {
            return model == SoftMaterialModel::NeoHookean ? PxDeformableVolumeMaterialModel::eNEO_HOOKEAN
                                                          : PxDeformableVolumeMaterialModel::eCO_ROTATIONAL;
        }

        // Frees one pinned host array (the helper takes the pointer by reference and nulls it)
        void FreePinned(PxCudaContextManager &cuda, DeformableVec4 *&array)
        {
            PxVec4 *pinned = reinterpret_cast<PxVec4 *>(array);
            Ext::PxCudaHelpersExt::freePinnedHostBuffer<PxVec4>(cuda, pinned);
            array = nullptr;
        }
    }

    PhysXDeformableDriver::PhysXDeformableDriver(PxPhysics &physics, PxScene &scene, PxCudaContextManager &cuda)
        : _physics(physics), _scene(scene), _cuda(cuda)
    {
    }

    PhysXDeformableDriver::~PhysXDeformableDriver()
    {
        for (auto &[id, volume] : _volumes)
        {
            static_cast<void>(id);
            Release(volume);
        }
        _volumes.clear();
    }

    std::string PhysXDeformableDriver::GetDeviceName() const
    {
        const char *name = _cuda.getDeviceName();
        return name ? name : "unknown device";
    }

    void PhysXDeformableDriver::Release(Volume &volume)
    {
        // The actor leaves the scene and is released (it releases the buffers it allocated on the device and its
        // reference to the shape), then the shape, the material and the cooked mesh, and last the pinned memory the
        // device arrays were filled from
        if (volume.volume)
        {
            if (volume.inScene)
            {
                _scene.removeActor(*volume.volume);
            }
            volume.volume->release();
            volume.volume = nullptr;
        }
        volume.inScene = false;
        if (volume.shape)
        {
            volume.shape->release();
            volume.shape = nullptr;
        }
        if (volume.material)
        {
            volume.material->release();
            volume.material = nullptr;
        }
        volume.cooked.mesh.reset();
        FreePinned(_cuda, volume.simPositions);
        FreePinned(_cuda, volume.simVelocities);
        FreePinned(_cuda, volume.collisionPositions);
        FreePinned(_cuda, volume.restPositions);
        volume.simCount = volume.collisionCount = 0;
    }

    void PhysXDeformableDriver::Apply(Volume &volume, const SoftBodies::DeformableMaterialParams &material)
    {
        if (volume.material)
        {
            volume.material->setYoungsModulus(material.youngsModulus);
            volume.material->setPoissons(material.poisson);
            volume.material->setDynamicFriction(material.dynamicFriction);
            volume.material->setElasticityDamping(material.elasticityDamping);
            volume.material->setMaterialModel(ToPhysX(material.materialModel));
        }
        if (volume.volume)
        {
            volume.volume->setLinearDamping(material.linearDamping);
            volume.volume->setSolverIterationCounts(std::clamp<PxU32>(material.solverIterations, 1u, 255u));
            volume.volume->setDeformableBodyFlag(PxDeformableBodyFlag::eDISABLE_SELF_COLLISION,
                                                 !material.selfCollision);
        }
    }

    SoftBodies::DeformableVolumeId PhysXDeformableDriver::CreateVolume(const SoftBodies::DeformableVolumeDesc &desc,
                                                                       std::string &error)
    {
        if (!_scene.getFlags().isSet(PxSceneFlag::eENABLE_GPU_DYNAMICS))
        {
            error = "the physics scene does not run on the GPU";
            return SoftBodies::kInvalidDeformableVolume;
        }
        if (_volumes.size() >= PX_MAX_NB_DEFORMABLE_VOLUME)
        {
            error = std::format("the scene already holds {} deformable volumes, the most PhysX supports",
                                static_cast<unsigned>(PX_MAX_NB_DEFORMABLE_VOLUME));
            return SoftBodies::kInvalidDeformableVolume;
        }

        Volume volume;
        const auto fail = [&](const std::string &reason)
        {
            error = reason;
            Release(volume);
            return SoftBodies::kInvalidDeformableVolume;
        };

        // 1. Cook the engine's tetrahedra (CPU) with the GPU data a GPU scene needs
        volume.cooked = CookDeformableMesh(desc.restPositions, desc.tets, &_physics.getPhysicsInsertionCallback());
        if (!volume.cooked.Ok())
        {
            return fail(volume.cooked.error);
        }
        PxDeformableVolumeMesh &cookedMesh = *volume.cooked.mesh;

        // 2. The material, the volume, its shape and simulation mesh
        const SoftBodies::DeformableMaterialParams &m = desc.material;
        volume.material = _physics.createDeformableVolumeMaterial(m.youngsModulus, m.poisson, m.dynamicFriction,
                                                                  m.elasticityDamping);
        if (!volume.material)
        {
            return fail("PhysX could not create the deformable volume material");
        }
        // NULL when the PhysX build has no GPU support (PX_SUPPORT_GPU_PHYSX is 0) or the GPU library is missing
        volume.volume = _physics.createDeformableVolume(_cuda);
        if (!volume.volume)
        {
            return fail("PhysX could not create a deformable volume (is PhysXGpu_64.dll next to the executable?)");
        }
        volume.shape = _physics.createShape(PxTetrahedronMeshGeometry(cookedMesh.getCollisionMesh()),
                                            *volume.material, true);
        if (!volume.shape)
        {
            return fail("PhysX could not create the deformable volume's shape");
        }
        if (!volume.volume->attachShape(*volume.shape))
        {
            return fail("PhysX could not attach the shape to the deformable volume");
        }
        if (!volume.volume->attachSimulationMesh(*cookedMesh.getSimulationMesh(),
                                                 *cookedMesh.getDeformableVolumeAuxData()))
        {
            return fail("PhysX could not attach the simulation mesh to the deformable volume");
        }
        Apply(volume, m);
        if (!_scene.addActor(*volume.volume))
        {
            return fail("the scene refused the deformable volume (it must be a GPU scene)");
        }
        volume.inScene = true;

        // 3. The host mirror (pinned), initialised from the cooked meshes (the body is already at its rest shape in
        // world space: the engine cooked the world-space mesh, so no transform is applied here), then the mass from
        // the density and the whole state up to the device
        volume.collisionCount = cookedMesh.getCollisionMesh()->getNbVertices();
        volume.simCount = cookedMesh.getSimulationMesh()->getNbVertices();
        PxVec4 *simPositions = nullptr;
        PxVec4 *simVelocities = nullptr;
        PxVec4 *collisionPositions = nullptr;
        PxVec4 *restPositions = nullptr;
        PxDeformableVolumeExt::allocateAndInitializeHostMirror(*volume.volume, &_cuda, simPositions, simVelocities,
                                                               collisionPositions, restPositions);
        volume.simPositions = reinterpret_cast<DeformableVec4 *>(simPositions);
        volume.simVelocities = reinterpret_cast<DeformableVec4 *>(simVelocities);
        volume.collisionPositions = reinterpret_cast<DeformableVec4 *>(collisionPositions);
        volume.restPositions = reinterpret_cast<DeformableVec4 *>(restPositions);
        if (!simPositions || !simVelocities || !collisionPositions || !restPositions)
        {
            return fail("CUDA could not allocate the pinned deformable volume arrays");
        }
        PxDeformableVolumeExt::updateMass(*volume.volume, desc.density, kMaxInvMassRatio, simPositions);
        PxDeformableVolumeExt::copyToDevice(*volume.volume, PxDeformableVolumeDataFlag::eALL, simPositions,
                                            simVelocities, collisionPositions, restPositions);

        const SoftBodies::DeformableVolumeId id = _nextId++;
        _volumes.emplace(id, std::move(volume));
        return id;
    }

    void PhysXDeformableDriver::DestroyVolume(const SoftBodies::DeformableVolumeId id)
    {
        const auto it = _volumes.find(id);
        if (it == _volumes.end())
        {
            return;
        }
        Release(it->second);
        _volumes.erase(it);
    }

    SoftBodies::DeformableStaging PhysXDeformableDriver::GetStaging(const SoftBodies::DeformableVolumeId id)
    {
        const auto it = _volumes.find(id);
        if (it == _volumes.end())
        {
            return {};
        }
        Volume &volume = it->second;
        return {.collisionPositions = {volume.collisionPositions, volume.collisionCount},
                .simPositions = {volume.simPositions, volume.simCount},
                .simVelocities = {volume.simVelocities, volume.simCount}};
    }

    std::span<const std::uint32_t> PhysXDeformableDriver::GetNodeMap(const SoftBodies::DeformableVolumeId id) const
    {
        const auto it = _volumes.find(id);
        return it == _volumes.end() ? std::span<const std::uint32_t>{} : std::span<const std::uint32_t>(it->second.cooked.nodeToVertex);
    }

    void PhysXDeformableDriver::SetMaterial(const SoftBodies::DeformableVolumeId id,
                                            const SoftBodies::DeformableMaterialParams &material)
    {
        if (const auto it = _volumes.find(id); it != _volumes.end())
        {
            Apply(it->second, material);
        }
    }

    bool PhysXDeformableDriver::Upload(const SoftBodies::DeformableVolumeId id, const std::uint32_t dirty,
                                       std::string &error)
    {
        const auto it = _volumes.find(id);
        if (it == _volumes.end())
        {
            error = "unknown deformable volume";
            return false;
        }
        Volume &volume = it->second;
        if (dirty == SoftBodies::kDeformNothing)
        {
            return true;
        }

        // The device arrays are copied over from the host mirror under the CUDA lock, then PhysX is told which
        // arrays changed (it is forbidden between simulate and fetchResults; the backend never calls this then)
        PxScopedCudaLock lock(_cuda);
        PxCudaContext *context = _cuda.getCudaContext();
        if (!context)
        {
            error = "the CUDA context is gone";
            return false;
        }
        PxDeformableVolumeDataFlags flags(PxDeformableVolumeDataFlag::eNONE);
        if (dirty & SoftBodies::kDeformSimVelocities)
        {
            if (context->memcpyHtoD(Device(volume.volume->getSimVelocityBufferD()), volume.simVelocities,
                                    sizeof(PxVec4) * volume.simCount) != 0)
            {
                error = "the soft body velocities could not be copied to the GPU";
                return false;
            }
            flags |= PxDeformableVolumeDataFlag::eSIM_VELOCITY;
        }
        if (dirty & SoftBodies::kDeformSimPositions)
        {
            if (context->memcpyHtoD(Device(volume.volume->getSimPositionInvMassBufferD()), volume.simPositions,
                                    sizeof(PxVec4) * volume.simCount) != 0)
            {
                error = "the soft body simulation positions could not be copied to the GPU";
                return false;
            }
            flags |= PxDeformableVolumeDataFlag::eSIM_POSITION_INVMASS;
        }
        if (dirty & SoftBodies::kDeformCollisionPositions)
        {
            if (context->memcpyHtoD(Device(volume.volume->getPositionInvMassBufferD()), volume.collisionPositions,
                                    sizeof(PxVec4) * volume.collisionCount) != 0)
            {
                error = "the soft body positions could not be copied to the GPU";
                return false;
            }
            flags |= PxDeformableVolumeDataFlag::ePOSITION_INVMASS;
        }
        volume.volume->markDirty(flags);
        return true;
    }

    bool PhysXDeformableDriver::ReadBack(const SoftBodies::DeformableVolumeId id, const bool simVelocities,
                                         std::string &error)
    {
        const auto it = _volumes.find(id);
        if (it == _volumes.end())
        {
            error = "unknown deformable volume";
            return false;
        }
        Volume &volume = it->second;

        // After fetchResults the simulation is done; the device arrays are copied to the host under the CUDA lock
        // (as SnippetDeformableVolume does). Whether fetchResults alone makes them safe to read is unverified.
        PxScopedCudaLock lock(_cuda);
        PxCudaContext *context = _cuda.getCudaContext();
        if (!context)
        {
            error = "the CUDA context is gone";
            return false;
        }
        if (context->memcpyDtoH(volume.collisionPositions, Device(volume.volume->getPositionInvMassBufferD()),
                                sizeof(PxVec4) * volume.collisionCount) != 0)
        {
            error = "the soft body positions could not be read back from the GPU";
            return false;
        }
        if (simVelocities && context->memcpyDtoH(volume.simVelocities, Device(volume.volume->getSimVelocityBufferD()),
                                                 sizeof(PxVec4) * volume.simCount) != 0)
        {
            error = "the soft body velocities could not be read back from the GPU";
            return false;
        }
        return true;
    }
}

#endif
