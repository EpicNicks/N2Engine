#pragma once

#include "engine/physics/physx/PhysXGpuSupport.hpp"

#if N2ENGINE_PHYSX_GPU_PROBE

#include <cstdint>
#include <string>
#include <unordered_map>

#include "engine/particles/PbdDriver.hpp"

namespace physx
{
    class PxCudaContextManager;
    class PxParticleBuffer;
    class PxPBDMaterial;
    class PxPBDParticleSystem;
    class PxPhysics;
    class PxScene;
}

namespace N2Engine::Physics
{
    /**
     * The real PBD driver: the only place PhysX's particle and CUDA calls are made for particles. One PBD material,
     * one PBD particle system (added to the scene as an actor) and one particle buffer per system, with pinned
     * host arrays as the host mirror. It exists only in builds where the GPU library does (Windows, N2ENGINE_PHYSX_GPU_PROBE);
     * everything it does needs a CUDA context, so nothing in CI runs it (the GPU golden test does, on a GPU PC).
     *
     * It uses the scene, the physics SDK and the CUDA context manager of the PhysXBackend that made it, and must be
     * destroyed before they are.
     */
    class PhysXPbdDriver final : public Particles::IPbdDriver
    {
    public:
        PhysXPbdDriver(physx::PxPhysics &physics, physx::PxScene &scene, physx::PxCudaContextManager &cuda);
        ~PhysXPbdDriver() override;

        PhysXPbdDriver(const PhysXPbdDriver &) = delete;
        PhysXPbdDriver &operator=(const PhysXPbdDriver &) = delete;

        [[nodiscard]] std::string GetDeviceName() const override;
        [[nodiscard]] Math::Vector3 GetSceneGravity() const override;

        [[nodiscard]] Particles::PbdSystemId CreateSystem(const Particles::PbdSystemDesc &desc,
                                                          std::string &error) override;
        void DestroySystem(Particles::PbdSystemId id) override;

        [[nodiscard]] Particles::PbdStaging GetStaging(Particles::PbdSystemId id) override;
        [[nodiscard]] std::uint32_t GetPhase(Particles::PbdSystemId id) const override;
        void SetMaterial(Particles::PbdSystemId id, const Particles::PbdMaterialParams &material) override;

        [[nodiscard]] bool Upload(Particles::PbdSystemId id, std::uint32_t activeCount, std::uint32_t dirty,
                                  std::string &error) override;
        [[nodiscard]] bool ReadBack(Particles::PbdSystemId id, std::uint32_t count, std::string &error) override;

    private:
        struct System
        {
            physx::PxPBDParticleSystem *system = nullptr;
            physx::PxPBDMaterial *material = nullptr;
            physx::PxParticleBuffer *buffer = nullptr;
            Particles::PbdVec4 *positions = nullptr;  // pinned host memory
            Particles::PbdVec4 *velocities = nullptr; // pinned host memory
            std::uint32_t *phases = nullptr;          // pinned host memory
            std::uint32_t maxParticles = 0;
            std::uint32_t phase = 0;
            bool inScene = false;
            bool bufferAdded = false;
        };

        void Release(System &system);

        physx::PxPhysics &_physics;
        physx::PxScene &_scene;
        physx::PxCudaContextManager &_cuda;
        std::unordered_map<Particles::PbdSystemId, System> _systems;
        Particles::PbdSystemId _nextId = 1;
    };
}

#endif
