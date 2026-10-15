#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/physics/ISoftBodyBackend.hpp"
#include "engine/softbody/DeformableDriver.hpp"

namespace N2Engine::SoftBodies
{
    /**
     * The PhysX GPU soft-body backend's logic, on top of an IDeformableDriver (the only thing that touches PhysX and
     * CUDA). With the real driver it is what the physics backend returns from GetSoftBodyBackend when the GPU scene
     * is up; the tests run it against a fake driver on every CI leg. It mirrors Particles::PbdParticleBackend.
     *
     * One deformable volume per SoftBody, made by the driver from the engine's own tetrahedral mesh
     * (Physics::TetMeshData) under the body's transform; the render vertices ride on it with the SAME embedding
     * the CPU backend uses, so the picture is skinned from the volume's node positions by the same code
     * (SoftBodies::SkinSurface):
     *
     *  - CreateBody puts the mesh under the transform (OrientTets: rest positions in world space, a mirrored
     *    transform flips the affected tetrahedra), asks the driver for a volume and remembers the driver's node map
     *    (cooking may order the vertices differently). The body starts at its rest shape.
     *  - OnSceneStepped(dt) is called by the physics backend after every scene step (the scene simulates the
     *    volumes; Step does nothing). It reads every live body's collision positions back from the GPU once and
     *    caches its nodes in the order of the engine's mesh.
     *  - CopySurfacePositions skins from that cache: no GPU call and no allocation once `out` has grown.
     *  - AddVelocity reads the simulation velocities back, adds the push to every node and uploads them.
     *  - SetParams maps the settings to a material and solver settings and gives the driver the ones that changed.
     *    The density is read when the body is created (changing a live body's mass would mean reading its whole
     *    state back); the ground plane is the CPU backend's only (the GPU body collides with the scene's own
     *    colliders), so groundEnabled and groundHeight are ignored; gravity is the scene's.
     *
     * Failure is soft: when the driver refuses a body or a transfer, the backend remembers why (HasFailed), logs
     * once and does nothing more with the GPU; its owner then stops offering it, and the SoftBodies that used it
     * restart on the CPU backend.
     *
     * Handles carry this backend's generation (ISoftBodyBackend), so a handle of a replaced or recreated backend is
     * ignored here, even if the new backend sits at the same address. Indexes are never reused, so a stale handle of
     * a destroyed body names nothing; the room a destroyed body held (the driver's volume limit) is free again.
     */
    class DeformableSoftBodyBackend final : public Physics::ISoftBodyBackend
    {
    public:
        explicit DeformableSoftBodyBackend(std::unique_ptr<IDeformableDriver> driver);
        ~DeformableSoftBodyBackend() override;

        [[nodiscard]] std::string_view GetName() const override { return "PhysX"; }

        /// kInvalidSoftBody for a missing or malformed tetrahedral mesh, one above kMaxTetsPerBody tetrahedra or a
        /// non-finite transform (the body is not made and the backend does not fail: the CPU backend refuses these
        /// too), and, after recording the failure, when the driver refuses the volume
        [[nodiscard]] Physics::SoftBodyHandle CreateBody(const Physics::SoftBodyDesc &desc) override;
        void Destroy(Physics::SoftBodyHandle handle) override;
        void SetParams(Physics::SoftBodyHandle handle, const Physics::SoftBodyParams &params) override;
        void AddVelocity(Physics::SoftBodyHandle handle, const Math::Vector3 &velocity) override;
        /// Nothing: the physics scene steps the volumes (see OnSceneStepped)
        void Step(float dt) override { static_cast<void>(dt); }
        void CopySurfacePositions(Physics::SoftBodyHandle handle, std::vector<Math::Vector3> &out) const override;

        /// After the scene stepped by `dt` seconds (and fetchResults returned): refreshes every body's nodes from
        /// the GPU
        void OnSceneStepped(float dt);

        /// The driver (or a transfer) failed; the backend does nothing with the GPU from then on
        [[nodiscard]] bool HasFailed() const { return _failed; }
        [[nodiscard]] const std::string &GetFailureReason() const { return _failureReason; }
        /// Destroys every volume (the driver's, and the backend's records of them)
        void ReleaseAll();

        [[nodiscard]] std::string GetDeviceName() const { return _driver->GetDeviceName(); }
        [[nodiscard]] std::size_t GetBodyCount() const { return _bodies.size(); }

        /// The material and solver settings a SoftBodyParams gives the driver (sanitised)
        [[nodiscard]] static DeformableMaterialParams MakeMaterial(const Physics::SoftBodyParams &params);
        /// The solver's position iterations are the CPU backend's per-sub-step count times its sub-steps, so a
        /// body does about the same work per fixed step on either backend
        static constexpr std::uint32_t kIterationsPerSolverIteration = 4;

        // For tests and diagnostics
        /// Replaces `out` with the body's node positions in the order of the engine's tetrahedral mesh, as of the
        /// last read-back (empty for an unknown handle)
        void CopyNodePositions(Physics::SoftBodyHandle handle, std::vector<Math::Vector3> &out) const;

    private:
        struct Body
        {
            DeformableVolumeId driverId = kInvalidDeformableVolume;
            std::shared_ptr<const Physics::TetMeshData> mesh;
            std::vector<std::array<std::uint32_t, 4>> tets; // positively oriented in world space
            std::vector<std::uint8_t> flipped;              // per tetrahedron: corners 2 and 3 were swapped
            std::vector<DeformableVec4> nodes;              // world positions, in the engine mesh's node order
            DeformableMaterialParams material;              // what the driver has
            DeformableStaging staging;
            std::span<const std::uint32_t> nodeMap;         // node -> vertex of staging.collisionPositions
        };

        [[nodiscard]] Body *Find(Physics::SoftBodyHandle handle);
        [[nodiscard]] const Body *Find(Physics::SoftBodyHandle handle) const;
        void Fail(const std::string &reason);
        /// Reads the nodes out of the staging's collision positions (after a read-back); false when one is not finite
        static bool RefreshNodes(Body &body);

        std::unique_ptr<IDeformableDriver> _driver;
        std::map<std::uint32_t, Body> _bodies; // by handle index, so bodies read back in creation order
        std::uint32_t _nextIndex = 1;
        bool _failed = false;
        std::string _failureReason;
    };
}
