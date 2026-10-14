#include "engine/physics/physx/PhysXPbdDriver.hpp"

#if N2ENGINE_PHYSX_GPU_PROBE

#include <algorithm>
#include <cstring>
#include <format>

#include <PxPhysicsAPI.h>
#include <cudamanager/PxCudaContext.h>
#include <cudamanager/PxCudaContextManager.h>
#include <extensions/PxCudaHelpersExt.h>
#include <extensions/PxParticleExt.h>

using namespace physx;

// Every PhysX GPU particle call in the engine is in this file. None of it runs without a CUDA context, so it is
// compiled in CI (on the Windows legs, where the GPU library's headers and import library are) but only run by the
// GPU golden test (tests/engine/physics/PhysXGpuParticlesTests.cpp) on a machine with an NVIDIA GPU.

namespace N2Engine::Physics
{
    namespace
    {
        static_assert(sizeof(Particles::PbdVec4) == sizeof(PxVec4), "the host mirror is uploaded as PxVec4");

        CUdeviceptr Device(const void *pointer)
        {
            return reinterpret_cast<CUdeviceptr>(pointer);
        }
    }

    PhysXPbdDriver::PhysXPbdDriver(PxPhysics &physics, PxScene &scene, PxCudaContextManager &cuda)
        : _physics(physics), _scene(scene), _cuda(cuda)
    {
    }

    PhysXPbdDriver::~PhysXPbdDriver()
    {
        for (auto &[id, system] : _systems)
        {
            static_cast<void>(id);
            Release(system);
        }
        _systems.clear();
    }

    std::string PhysXPbdDriver::GetDeviceName() const
    {
        const char *name = _cuda.getDeviceName();
        return name ? name : "unknown device";
    }

    Math::Vector3 PhysXPbdDriver::GetSceneGravity() const
    {
        const PxVec3 gravity = _scene.getGravity();
        return Math::Vector3(gravity.x, gravity.y, gravity.z);
    }

    void PhysXPbdDriver::Release(System &system)
    {
        // The buffer leaves the particle system, the system leaves the scene, then everything is released and the
        // pinned memory (which the buffer was populated from) is freed
        if (system.system && system.buffer && system.bufferAdded)
        {
            system.system->removeParticleBuffer(system.buffer);
        }
        system.bufferAdded = false;
        if (system.buffer)
        {
            system.buffer->release();
            system.buffer = nullptr;
        }
        if (system.system)
        {
            if (system.inScene)
            {
                _scene.removeActor(*system.system);
            }
            system.system->release();
            system.system = nullptr;
        }
        system.inScene = false;
        if (system.material)
        {
            system.material->release();
            system.material = nullptr;
        }
        Ext::PxCudaHelpersExt::freePinnedHostBuffer<Particles::PbdVec4>(_cuda, system.positions);
        Ext::PxCudaHelpersExt::freePinnedHostBuffer<Particles::PbdVec4>(_cuda, system.velocities);
        Ext::PxCudaHelpersExt::freePinnedHostBuffer<std::uint32_t>(_cuda, system.phases);
    }

    Particles::PbdSystemId PhysXPbdDriver::CreateSystem(const Particles::PbdSystemDesc &desc, std::string &error)
    {
        if (desc.maxParticles == 0)
        {
            error = "a PBD particle system needs room for at least one particle";
            return Particles::kInvalidPbdSystem;
        }
        if (!_scene.getFlags().isSet(PxSceneFlag::eENABLE_GPU_DYNAMICS))
        {
            error = "the physics scene does not run on the GPU";
            return Particles::kInvalidPbdSystem;
        }

        System system;
        system.maxParticles = desc.maxParticles;
        const Particles::PbdMaterialParams &m = desc.material;

        const auto fail = [&](const char *reason)
        {
            error = reason;
            Release(system);
            return Particles::kInvalidPbdSystem;
        };

        system.material = _physics.createPBDMaterial(m.friction, m.damping, m.adhesion, m.viscosity,
                                                     m.vorticityConfinement, m.surfaceTension, m.cohesion, m.lift,
                                                     m.drag, m.cflCoefficient, m.gravityScale);
        if (!system.material)
        {
            return fail("PhysX could not create the PBD material");
        }

        // NULL when the PhysX build has no GPU support (PX_SUPPORT_GPU_PHYSX is 0) or the GPU library is missing
        system.system = _physics.createPBDParticleSystem(_cuda, 96);
        if (!system.system)
        {
            return fail("PhysX could not create a PBD particle system (is PhysXGpu_64.dll next to the executable?)");
        }

        // The offsets follow SnippetPBF: the rest offset is half the particle spacing over the fluid/solid ratio
        const float spacing = std::max(desc.preset.particleSpacing, 1e-3f);
        const float restOffset = 0.5f * spacing / 0.6f;
        const float fluidRestOffset = restOffset * 0.6f;
        PxPBDParticleSystem &ps = *system.system;
        ps.setRestOffset(restOffset);
        ps.setContactOffset(restOffset + 0.01f);
        ps.setParticleContactOffset(fluidRestOffset / 0.6f);
        ps.setSolidRestOffset(restOffset);
        ps.setFluidRestOffset(fluidRestOffset);
        ps.setMaxVelocity(std::max(restOffset * 100.0f, 20.0f));

        PxParticlePhaseFlags phaseFlags(PxParticlePhaseFlag::eParticlePhaseSelfCollide);
        if (desc.preset.fluid)
        {
            phaseFlags |= PxParticlePhaseFlag::eParticlePhaseFluid;
        }
        system.phase = ps.createPhase(system.material, phaseFlags);

        if (!_scene.addActor(ps))
        {
            return fail("the scene refused the PBD particle system (it must be a GPU scene)");
        }
        system.inScene = true;

        // Pinned host memory is the mirror, and the buffer is populated from it (empty: no active particles yet)
        system.positions = Ext::PxCudaHelpersExt::allocPinnedHostBuffer<Particles::PbdVec4>(_cuda, desc.maxParticles);
        system.velocities = Ext::PxCudaHelpersExt::allocPinnedHostBuffer<Particles::PbdVec4>(_cuda, desc.maxParticles);
        system.phases = Ext::PxCudaHelpersExt::allocPinnedHostBuffer<std::uint32_t>(_cuda, desc.maxParticles);
        if (!system.positions || !system.velocities || !system.phases)
        {
            return fail("CUDA could not allocate the pinned particle arrays");
        }
        std::memset(system.positions, 0, sizeof(Particles::PbdVec4) * desc.maxParticles);
        std::memset(system.velocities, 0, sizeof(Particles::PbdVec4) * desc.maxParticles);
        std::memset(system.phases, 0, sizeof(std::uint32_t) * desc.maxParticles);

        ExtGpu::PxParticleBufferDesc bufferDesc;
        bufferDesc.positions = reinterpret_cast<PxVec4 *>(system.positions);
        bufferDesc.velocities = reinterpret_cast<PxVec4 *>(system.velocities);
        bufferDesc.phases = system.phases;
        bufferDesc.maxParticles = desc.maxParticles;
        bufferDesc.numActiveParticles = 0;
        system.buffer = ExtGpu::PxCreateAndPopulateParticleBuffer(bufferDesc, &_cuda);
        if (!system.buffer)
        {
            return fail("PhysX could not create the particle buffer (out of GPU memory?)");
        }
        ps.addParticleBuffer(system.buffer);
        system.bufferAdded = true;

        const Particles::PbdSystemId id = _nextId++;
        _systems.emplace(id, system);
        return id;
    }

    void PhysXPbdDriver::DestroySystem(const Particles::PbdSystemId id)
    {
        const auto it = _systems.find(id);
        if (it == _systems.end())
        {
            return;
        }
        Release(it->second);
        _systems.erase(it);
    }

    Particles::PbdStaging PhysXPbdDriver::GetStaging(const Particles::PbdSystemId id)
    {
        const auto it = _systems.find(id);
        if (it == _systems.end())
        {
            return {};
        }
        System &system = it->second;
        return {.positions = {system.positions, system.maxParticles},
                .velocities = {system.velocities, system.maxParticles},
                .phases = {system.phases, system.maxParticles}};
    }

    std::uint32_t PhysXPbdDriver::GetPhase(const Particles::PbdSystemId id) const
    {
        const auto it = _systems.find(id);
        return it == _systems.end() ? 0u : it->second.phase;
    }

    void PhysXPbdDriver::SetMaterial(const Particles::PbdSystemId id, const Particles::PbdMaterialParams &material)
    {
        const auto it = _systems.find(id);
        if (it == _systems.end() || !it->second.material)
        {
            return;
        }
        // Lift and drag have only deprecated setters; they stay what the material was created with
        PxPBDMaterial &m = *it->second.material;
        m.setFriction(material.friction);
        m.setDamping(material.damping);
        m.setAdhesion(material.adhesion);
        m.setViscosity(material.viscosity);
        m.setVorticityConfinement(material.vorticityConfinement);
        m.setSurfaceTension(material.surfaceTension);
        m.setCohesion(material.cohesion);
        m.setCFLCoefficient(material.cflCoefficient);
        m.setGravityScale(material.gravityScale);
    }

    bool PhysXPbdDriver::Upload(const Particles::PbdSystemId id, const std::uint32_t activeCount,
                                const std::uint32_t dirty, std::string &error)
    {
        const auto it = _systems.find(id);
        if (it == _systems.end())
        {
            error = "unknown PBD particle system";
            return false;
        }
        System &system = it->second;
        if (activeCount > system.maxParticles)
        {
            error = "more active particles than the particle buffer holds";
            return false;
        }

        if (activeCount > 0 && dirty != 0)
        {
            // The buffer's arrays are device memory: the host mirror is copied over them, then PhysX is told which
            // arrays changed
            PxScopedCudaLock lock(_cuda);
            PxCudaContext *context = _cuda.getCudaContext();
            if (!context)
            {
                error = "the CUDA context is gone";
                return false;
            }
            const std::size_t vec4Bytes = sizeof(PxVec4) * activeCount;
            if ((dirty & Particles::kPbdPositions) &&
                context->memcpyHtoD(Device(system.buffer->getPositionInvMasses()), system.positions, vec4Bytes) != 0)
            {
                error = "the particle positions could not be copied to the GPU";
                return false;
            }
            if ((dirty & Particles::kPbdVelocities) &&
                context->memcpyHtoD(Device(system.buffer->getVelocities()), system.velocities, vec4Bytes) != 0)
            {
                error = "the particle velocities could not be copied to the GPU";
                return false;
            }
            if ((dirty & Particles::kPbdPhases) &&
                context->memcpyHtoD(Device(system.buffer->getPhases()), system.phases,
                                    sizeof(std::uint32_t) * activeCount) != 0)
            {
                error = "the particle phases could not be copied to the GPU";
                return false;
            }
        }

        system.buffer->setNbActiveParticles(activeCount);
        if (activeCount > 0)
        {
            if (dirty & Particles::kPbdPositions)
            {
                system.buffer->raiseFlags(PxParticleBufferFlag::eUPDATE_POSITION);
            }
            if (dirty & Particles::kPbdVelocities)
            {
                system.buffer->raiseFlags(PxParticleBufferFlag::eUPDATE_VELOCITY);
            }
            if (dirty & Particles::kPbdPhases)
            {
                system.buffer->raiseFlags(PxParticleBufferFlag::eUPDATE_PHASE);
            }
        }
        return true;
    }

    bool PhysXPbdDriver::ReadBack(const Particles::PbdSystemId id, const std::uint32_t count, std::string &error)
    {
        const auto it = _systems.find(id);
        if (it == _systems.end())
        {
            error = "unknown PBD particle system";
            return false;
        }
        System &system = it->second;
        if (count == 0)
        {
            return true;
        }
        if (count > system.maxParticles)
        {
            error = "more particles to read back than the particle buffer holds";
            return false;
        }

        // After fetchResults the simulation is done; the device arrays are copied to the host under the CUDA lock
        PxScopedCudaLock lock(_cuda);
        PxCudaContext *context = _cuda.getCudaContext();
        if (!context)
        {
            error = "the CUDA context is gone";
            return false;
        }
        const std::size_t bytes = sizeof(PxVec4) * count;
        if (context->memcpyDtoH(system.positions, Device(system.buffer->getPositionInvMasses()), bytes) != 0 ||
            context->memcpyDtoH(system.velocities, Device(system.buffer->getVelocities()), bytes) != 0)
        {
            error = "the particle positions could not be read back from the GPU";
            return false;
        }
        return true;
    }
}

#endif
