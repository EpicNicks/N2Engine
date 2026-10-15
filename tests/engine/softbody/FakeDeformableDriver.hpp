#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <math/Vector3.hpp>

#include "engine/softbody/DeformableDriver.hpp"

namespace N2Engine::TestSupport
{
    /**
     * A stand-in for the PhysX deformable driver, with a fake "device": the host mirror is what the backend reads
     * and writes, the device arrays are what Upload copies to (only the arrays flagged dirty, as the real one does),
     * and Simulate moves the device nodes under gravity as a scene step would. ReadBack copies the device back into
     * the mirror. It counts every call, records what it was given, and can be told to fail, to hold only a few
     * volumes, or to order the vertices of its meshes differently from the input (as cooking is free to).
     */
    class FakeDeformableDriver final : public SoftBodies::IDeformableDriver
    {
    public:
        struct Volume
        {
            // What CreateVolume was given (copied: the spans only live during the call)
            std::vector<Math::Vector3> rest;
            std::vector<std::array<std::uint32_t, 4>> tets;
            float density = 0.0f;
            SoftBodies::DeformableMaterialParams material;

            std::vector<std::uint32_t> nodeMap; // input node -> vertex
            std::vector<SoftBodies::DeformableVec4> hostCollision, hostSimPositions, hostSimVelocities;
            std::vector<SoftBodies::DeformableVec4> deviceCollision, deviceSimPositions, deviceSimVelocities;
            int uploads = 0;
            int readBacks = 0;
            int velocityReadBacks = 0;
            int materialChanges = 0;
            std::uint32_t lastDirty = 0;
            std::uint32_t allDirty = 0; // every flag any Upload had
        };

        /// Events in the order they happened ("create 1", "destroy 1", "driver destroyed"): for release-order tests
        std::vector<std::string> *log = nullptr;

        ~FakeDeformableDriver() override
        {
            if (log)
            {
                log->push_back("driver destroyed");
            }
        }

        [[nodiscard]] std::string GetDeviceName() const override { return "Fake GPU"; }

        [[nodiscard]] SoftBodies::DeformableVolumeId CreateVolume(const SoftBodies::DeformableVolumeDesc &desc,
                                                                  std::string &error) override
        {
            ++createCalls;
            if (failCreate)
            {
                error = "fake: out of GPU memory";
                return SoftBodies::kInvalidDeformableVolume;
            }
            if (volumes.size() >= maxVolumes)
            {
                error = "fake: the scene holds the most deformable volumes";
                return SoftBodies::kInvalidDeformableVolume;
            }
            const SoftBodies::DeformableVolumeId id = _nextId++;
            Volume &volume = volumes[id];
            volume.rest.assign(desc.restPositions.begin(), desc.restPositions.end());
            volume.tets.assign(desc.tets.begin(), desc.tets.end());
            volume.density = desc.density;
            volume.material = desc.material;

            const std::size_t n = volume.rest.size();
            volume.nodeMap.resize(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                // Reversed when asked to: the vertex of node i is then n - 1 - i
                volume.nodeMap[i] = static_cast<std::uint32_t>(reorderVertices ? n - 1 - i : i);
            }
            volume.hostCollision.assign(n, {});
            volume.hostSimPositions.assign(n, {});
            volume.hostSimVelocities.assign(n, {});
            for (std::size_t i = 0; i < n; ++i)
            {
                const SoftBodies::DeformableVec4 p{volume.rest[i].x, volume.rest[i].y, volume.rest[i].z, 1.0f};
                volume.hostCollision[volume.nodeMap[i]] = p;
                volume.hostSimPositions[volume.nodeMap[i]] = p;
            }
            volume.deviceCollision = volume.hostCollision;
            volume.deviceSimPositions = volume.hostSimPositions;
            volume.deviceSimVelocities = volume.hostSimVelocities;
            if (log)
            {
                log->push_back("create " + std::to_string(id));
            }
            return id;
        }

        void DestroyVolume(const SoftBodies::DeformableVolumeId id) override
        {
            if (volumes.erase(id) > 0)
            {
                ++destroyCalls;
                if (log)
                {
                    log->push_back("destroy " + std::to_string(id));
                }
            }
        }

        [[nodiscard]] SoftBodies::DeformableStaging GetStaging(const SoftBodies::DeformableVolumeId id) override
        {
            const auto it = volumes.find(id);
            if (it == volumes.end())
            {
                return {};
            }
            Volume &volume = it->second;
            return {.collisionPositions = volume.hostCollision,
                    .simPositions = volume.hostSimPositions,
                    .simVelocities = volume.hostSimVelocities};
        }

        [[nodiscard]] std::span<const std::uint32_t> GetNodeMap(const SoftBodies::DeformableVolumeId id) const override
        {
            const auto it = volumes.find(id);
            if (it == volumes.end())
            {
                return {};
            }
            const std::vector<std::uint32_t> &map = it->second.nodeMap;
            // A driver bug the backend must catch: a node map that doesn't cover every node
            return std::span<const std::uint32_t>(map.data(), shortNodeMap && !map.empty() ? map.size() - 1 : map.size());
        }

        void SetMaterial(const SoftBodies::DeformableVolumeId id,
                         const SoftBodies::DeformableMaterialParams &material) override
        {
            if (const auto it = volumes.find(id); it != volumes.end())
            {
                it->second.material = material;
                ++it->second.materialChanges;
                ++materialChanges;
            }
        }

        [[nodiscard]] bool Upload(const SoftBodies::DeformableVolumeId id, const std::uint32_t dirty,
                                  std::string &error) override
        {
            ++uploadCalls;
            const auto it = volumes.find(id);
            if (it == volumes.end() || failUpload)
            {
                error = "fake: upload failed";
                return false;
            }
            Volume &volume = it->second;
            ++volume.uploads;
            volume.lastDirty = dirty;
            volume.allDirty |= dirty;
            if (dirty & SoftBodies::kDeformCollisionPositions)
            {
                volume.deviceCollision = volume.hostCollision;
            }
            if (dirty & SoftBodies::kDeformSimPositions)
            {
                volume.deviceSimPositions = volume.hostSimPositions;
            }
            if (dirty & SoftBodies::kDeformSimVelocities)
            {
                volume.deviceSimVelocities = volume.hostSimVelocities;
            }
            return true;
        }

        [[nodiscard]] bool ReadBack(const SoftBodies::DeformableVolumeId id, const bool simVelocities,
                                    std::string &error) override
        {
            ++readBackCalls;
            const auto it = volumes.find(id);
            if (it == volumes.end() || failReadBack)
            {
                error = "fake: read-back failed";
                return false;
            }
            Volume &volume = it->second;
            ++volume.readBacks;
            volume.hostCollision = volume.deviceCollision;
            if (simVelocities)
            {
                ++volume.velocityReadBacks;
                volume.hostSimVelocities = volume.deviceSimVelocities;
            }
            return true;
        }

        /// One scene step on the "device": gravity into the velocities, then the velocities into the positions
        void Simulate(const float dt)
        {
            for (auto &[id, volume] : volumes)
            {
                static_cast<void>(id);
                for (std::size_t v = 0; v < volume.deviceCollision.size(); ++v)
                {
                    SoftBodies::DeformableVec4 &velocity = volume.deviceSimVelocities[v];
                    SoftBodies::DeformableVec4 &position = volume.deviceCollision[v];
                    velocity.x += sceneGravity.x * dt;
                    velocity.y += sceneGravity.y * dt;
                    velocity.z += sceneGravity.z * dt;
                    position.x += velocity.x * dt;
                    position.y += velocity.y * dt;
                    position.z += velocity.z * dt;
                    volume.deviceSimPositions[v] = position;
                }
            }
        }

        /// The device position of input node `node` of the volume
        [[nodiscard]] Math::Vector3 DeviceNode(const SoftBodies::DeformableVolumeId id, const std::size_t node) const
        {
            const Volume &volume = volumes.at(id);
            const SoftBodies::DeformableVec4 &p = volume.deviceCollision[volume.nodeMap[node]];
            return Math::Vector3(p.x, p.y, p.z);
        }

        std::map<SoftBodies::DeformableVolumeId, Volume> volumes;
        Math::Vector3 sceneGravity{0.0f, -10.0f, 0.0f};
        /// How many volumes the "scene" holds (PhysX: 4095)
        std::size_t maxVolumes = 4095;
        bool reorderVertices = false;
        bool shortNodeMap = false;
        bool failCreate = false;
        bool failUpload = false;
        bool failReadBack = false;
        int createCalls = 0;
        int destroyCalls = 0;
        int uploadCalls = 0;
        int readBackCalls = 0;
        int materialChanges = 0;

    private:
        SoftBodies::DeformableVolumeId _nextId = 1;
    };
}
