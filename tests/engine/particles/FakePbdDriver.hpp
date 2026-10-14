#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "engine/particles/PbdDriver.hpp"

namespace N2Engine::TestSupport
{
    /**
     * A stand-in for the PhysX PBD driver, with a fake "device": the host mirror is what the backend writes, the
     * device arrays are what Upload copies (only the arrays flagged dirty, as the real one does), and Simulate
     * moves the device particles under gravity as a scene step would. ReadBack copies the device back into the
     * mirror. It counts every call, and can be told to fail.
     */
    class FakePbdDriver final : public Particles::IPbdDriver
    {
    public:
        struct System
        {
            Particles::PbdSystemDesc desc;
            std::vector<Particles::PbdVec4> hostPositions, hostVelocities;
            std::vector<std::uint32_t> hostPhases;
            std::vector<Particles::PbdVec4> devicePositions, deviceVelocities;
            std::vector<std::uint32_t> devicePhases;
            std::uint32_t active = 0;
            std::uint32_t phase = 0;
            Particles::PbdMaterialParams material;
            int uploads = 0;
            int readBacks = 0;
            int materialChanges = 0;
            std::uint32_t lastDirty = 0;
        };

        [[nodiscard]] std::string GetDeviceName() const override { return "Fake GPU"; }
        [[nodiscard]] Math::Vector3 GetSceneGravity() const override { return sceneGravity; }

        [[nodiscard]] Particles::PbdSystemId CreateSystem(const Particles::PbdSystemDesc &desc,
                                                          std::string &error) override
        {
            ++createCalls;
            if (failCreate)
            {
                error = "fake: out of GPU memory";
                return Particles::kInvalidPbdSystem;
            }
            const Particles::PbdSystemId id = _nextId++;
            System &system = systems[id];
            system.desc = desc;
            system.material = desc.material;
            system.phase = 0x00400001u + id; // some nonzero phase that differs per system
            system.hostPositions.assign(desc.maxParticles, {});
            system.hostVelocities.assign(desc.maxParticles, {});
            system.hostPhases.assign(desc.maxParticles, 0);
            system.devicePositions.assign(desc.maxParticles, {});
            system.deviceVelocities.assign(desc.maxParticles, {});
            system.devicePhases.assign(desc.maxParticles, 0);
            return id;
        }

        void DestroySystem(const Particles::PbdSystemId id) override
        {
            if (systems.erase(id) > 0)
            {
                ++destroyCalls;
            }
        }

        [[nodiscard]] Particles::PbdStaging GetStaging(const Particles::PbdSystemId id) override
        {
            const auto it = systems.find(id);
            if (it == systems.end())
            {
                return {};
            }
            System &system = it->second;
            return {.positions = system.hostPositions,
                    .velocities = system.hostVelocities,
                    .phases = system.hostPhases};
        }

        [[nodiscard]] std::uint32_t GetPhase(const Particles::PbdSystemId id) const override
        {
            const auto it = systems.find(id);
            return it == systems.end() ? 0u : it->second.phase;
        }

        void SetMaterial(const Particles::PbdSystemId id, const Particles::PbdMaterialParams &material) override
        {
            if (const auto it = systems.find(id); it != systems.end())
            {
                it->second.material = material;
                ++it->second.materialChanges;
                ++materialChanges;
            }
        }

        [[nodiscard]] bool Upload(const Particles::PbdSystemId id, const std::uint32_t activeCount,
                                  const std::uint32_t dirty, std::string &error) override
        {
            ++uploadCalls;
            const auto it = systems.find(id);
            if (it == systems.end() || failUpload)
            {
                error = "fake: upload failed";
                return false;
            }
            System &system = it->second;
            ++system.uploads;
            system.lastDirty = dirty;
            for (std::uint32_t i = 0; i < activeCount; ++i)
            {
                if (dirty & Particles::kPbdPositions)
                {
                    system.devicePositions[i] = system.hostPositions[i];
                }
                if (dirty & Particles::kPbdVelocities)
                {
                    system.deviceVelocities[i] = system.hostVelocities[i];
                }
                if (dirty & Particles::kPbdPhases)
                {
                    system.devicePhases[i] = system.hostPhases[i];
                }
            }
            system.active = activeCount;
            return true;
        }

        [[nodiscard]] bool ReadBack(const Particles::PbdSystemId id, const std::uint32_t count,
                                    std::string &error) override
        {
            ++readBackCalls;
            const auto it = systems.find(id);
            if (it == systems.end() || failReadBack)
            {
                error = "fake: read-back failed";
                return false;
            }
            System &system = it->second;
            ++system.readBacks;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                system.hostPositions[i] = system.devicePositions[i];
                system.hostVelocities[i] = system.deviceVelocities[i];
            }
            return true;
        }

        /// One scene step on the "device": gravity, then position, for every active particle
        void Simulate(const float dt)
        {
            for (auto &[id, system] : systems)
            {
                static_cast<void>(id);
                for (std::uint32_t i = 0; i < system.active; ++i)
                {
                    Particles::PbdVec4 &p = system.devicePositions[i];
                    Particles::PbdVec4 &v = system.deviceVelocities[i];
                    v.x += sceneGravity.x * system.material.gravityScale * dt;
                    v.y += sceneGravity.y * system.material.gravityScale * dt;
                    v.z += sceneGravity.z * system.material.gravityScale * dt;
                    p.x += v.x * dt;
                    p.y += v.y * dt;
                    p.z += v.z * dt;
                }
            }
        }

        std::map<Particles::PbdSystemId, System> systems;
        Math::Vector3 sceneGravity{0.0f, -10.0f, 0.0f};
        bool failCreate = false;
        bool failUpload = false;
        bool failReadBack = false;
        int createCalls = 0;
        int destroyCalls = 0;
        int uploadCalls = 0;
        int readBackCalls = 0;
        int materialChanges = 0;

    private:
        Particles::PbdSystemId _nextId = 1;
    };
}
