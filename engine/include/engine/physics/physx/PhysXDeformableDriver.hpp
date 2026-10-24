#pragma once

#include "engine/physics/physx/PhysXGpuSupport.hpp"

#if N2ENGINE_PHYSX_GPU_PROBE

#include <cstdint>
#include <string>
#include <unordered_map>

#include "engine/physics/physx/DeformableCooking.hpp"
#include "engine/softbody/DeformableDriver.hpp"

namespace physx
{
    class PxCudaContextManager;
    class PxDeformableVolume;
    class PxDeformableVolumeMaterial;
    class PxPhysics;
    class PxScene;
    class PxShape;
}

namespace N2Engine::Physics
{
    /**
     * The real deformable-volume driver: the only place PhysX's deformable and CUDA calls are made for soft bodies.
     * One cooked tetrahedral mesh (CookDeformableMesh, the engine's own tetrahedra as both collision and simulation
     * mesh), one deformable volume material, one shape and one PxDeformableVolume (added to the scene as an actor)
     * per volume, with pinned host arrays as the host mirror. It exists only in builds where the GPU library does
     * (Windows, N2ENGINE_PHYSX_GPU_PROBE); creating a volume needs a CUDA context, so nothing in CI runs it (the GPU
     * golden test does, on a GPU PC). Cooking itself is CPU code and is tested everywhere (DeformableCooking).
     *
     * It uses the scene, the physics SDK and the CUDA context manager of the PhysXBackend that made it, and must be
     * destroyed before they are.
     */
    class PhysXDeformableDriver final : public SoftBodies::IDeformableDriver
    {
    public:
        PhysXDeformableDriver(physx::PxPhysics &physics, physx::PxScene &scene, physx::PxCudaContextManager &cuda);
        ~PhysXDeformableDriver() override;

        PhysXDeformableDriver(const PhysXDeformableDriver &) = delete;
        PhysXDeformableDriver &operator=(const PhysXDeformableDriver &) = delete;

        [[nodiscard]] std::string GetDeviceName() const override;

        [[nodiscard]] SoftBodies::DeformableVolumeId CreateVolume(const SoftBodies::DeformableVolumeDesc &desc,
                                                                  std::string &error) override;
        void DestroyVolume(SoftBodies::DeformableVolumeId id) override;

        [[nodiscard]] SoftBodies::DeformableStaging GetStaging(SoftBodies::DeformableVolumeId id) override;
        [[nodiscard]] std::span<const std::uint32_t> GetNodeMap(SoftBodies::DeformableVolumeId id) const override;
        void SetMaterial(SoftBodies::DeformableVolumeId id, const SoftBodies::DeformableMaterialParams &material) override;

        [[nodiscard]] bool Upload(SoftBodies::DeformableVolumeId id, std::uint32_t dirty, std::string &error) override;
        [[nodiscard]] bool ReadBack(SoftBodies::DeformableVolumeId id, bool simVelocities, std::string &error) override;

    private:
        struct Volume
        {
            CookedDeformableMesh cooked;
            physx::PxDeformableVolumeMaterial *material = nullptr;
            physx::PxShape *shape = nullptr;
            physx::PxDeformableVolume *volume = nullptr;
            bool inScene = false;
            // Pinned host memory (the host mirror)
            SoftBodies::DeformableVec4 *simPositions = nullptr;
            SoftBodies::DeformableVec4 *simVelocities = nullptr;
            SoftBodies::DeformableVec4 *collisionPositions = nullptr;
            SoftBodies::DeformableVec4 *restPositions = nullptr;
            std::uint32_t simCount = 0;
            std::uint32_t collisionCount = 0;
        };

        void Release(Volume &volume);
        static void Apply(Volume &volume, const SoftBodies::DeformableMaterialParams &material);

        physx::PxPhysics &_physics;
        physx::PxScene &_scene;
        physx::PxCudaContextManager &_cuda;
        std::unordered_map<SoftBodies::DeformableVolumeId, Volume> _volumes;
        SoftBodies::DeformableVolumeId _nextId = 1;
    };
}

#endif
