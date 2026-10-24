#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include <math/Vector3.hpp>

#include "engine/physics/SoftMaterialModel.hpp"

namespace N2Engine::SoftBodies
{
    /// A driver's id for one deformable volume
    using DeformableVolumeId = std::uint32_t;
    inline constexpr DeformableVolumeId kInvalidDeformableVolume = 0;

    /// 16 bytes, laid out like PxVec4 (x, y, z, w)
    struct DeformableVec4
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 0.0f;
    };
    static_assert(sizeof(DeformableVec4) == 16);

    /// Which host arrays changed and must be sent to the GPU (bit flags)
    enum DeformableDirty : std::uint32_t
    {
        kDeformNothing = 0,
        kDeformCollisionPositions = 1u << 0,
        kDeformSimPositions = 1u << 1,
        kDeformSimVelocities = 1u << 2,
    };

    /// What a driver needs to build a volume's material and solver settings (no PhysX types: the real driver maps
    /// them to PxDeformableVolumeMaterial and the PxDeformableVolume setters)
    struct DeformableMaterialParams
    {
        /// Young's modulus, pascals
        float youngsModulus = 1.0e5f;
        float poisson = 0.3f;
        float dynamicFriction = 0.5f;
        float elasticityDamping = 0.0f;
        /// The body's linear velocity damping, 1/s
        float linearDamping = 0.05f;
        /// Position iterations of the solver, 1 to 255
        std::uint32_t solverIterations = 8;
        Physics::SoftMaterialModel materialModel = Physics::SoftMaterialModel::CoRotational;
        bool selfCollision = false;

        [[nodiscard]] bool operator==(const DeformableMaterialParams &) const = default;
    };

    /// What CreateVolume takes. The spans point into the backend's memory and are only read during the call.
    struct DeformableVolumeDesc
    {
        /// The body's rest shape in WORLD space: the engine's tetrahedral mesh under the body's transform
        std::span<const Math::Vector3> restPositions;
        /// Four node indices each into restPositions, every tetrahedron positively oriented in world space (a
        /// mirrored transform is already accounted for by swapping two corners)
        std::span<const std::array<std::uint32_t, 4>> tets;
        /// Mass per unit volume
        float density = 1000.0f;
        DeformableMaterialParams material;
    };

    /// The host mirrors of one volume's device arrays, one DeformableVec4 per vertex (PxVec4 layout)
    struct DeformableStaging
    {
        /// The collision mesh's vertices (x, y, z the position, w the inverse mass): what the scene's step moves
        /// and ReadBack refreshes
        std::span<DeformableVec4> collisionPositions;
        /// The simulation mesh's vertices and their velocities (x, y, z; w unused)
        std::span<DeformableVec4> simPositions;
        std::span<DeformableVec4> simVelocities;
    };

    /**
     * The thin layer between the PhysX soft-body backend's logic (handles, slots, parameters, velocity pushes,
     * read-back and skinning, failure) and the PhysX/CUDA calls, mirroring Particles::IPbdDriver.
     * DeformableSoftBodyBackend only ever talks to this; the real implementation (Physics::PhysXDeformableDriver) is
     * the one place a CUDA context, a PxDeformableVolume or a device pointer appears, and the tests run the backend
     * against a fake. Everything is called on the main thread, outside a scene step (never between simulate and
     * fetchResults).
     *
     * Node order: a driver may order the vertices of its volume's meshes differently from the order of
     * DeformableVolumeDesc::restPositions (cooking is free to). GetNodeMap gives, for each input node, the index of
     * its vertex in the collision mesh, so the backend can read its nodes out of the read-back positions.
     *
     * Each volume has host mirrors (DeformableStaging). The backend reads CollisionPositions after ReadBack, and
     * edits the velocities and sends them with Upload.
     */
    class IDeformableDriver
    {
    public:
        virtual ~IDeformableDriver() = default;

        /// The GPU's name, for the health row
        [[nodiscard]] virtual std::string GetDeviceName() const = 0;

        /// Cooks the mesh, makes the volume (collision and simulation mesh, material, actor in the scene), puts it
        /// at its rest shape and uploads it. kInvalidDeformableVolume with `error` set when it can't (cooking
        /// refused the mesh, the scene holds PhysX's most volumes (4095), out of GPU memory, no GPU library, ...).
        [[nodiscard]] virtual DeformableVolumeId CreateVolume(const DeformableVolumeDesc &desc, std::string &error) = 0;
        /// Releases the volume and everything it owns. An unknown id is ignored.
        virtual void DestroyVolume(DeformableVolumeId id) = 0;

        /// The host mirrors of the volume (empty spans for an unknown id)
        [[nodiscard]] virtual DeformableStaging GetStaging(DeformableVolumeId id) = 0;
        /// For each node of DeformableVolumeDesc::restPositions, its vertex index in the staging's collision
        /// positions (empty for an unknown id)
        [[nodiscard]] virtual std::span<const std::uint32_t> GetNodeMap(DeformableVolumeId id) const = 0;
        /// Changes the material and solver settings of a live volume
        virtual void SetMaterial(DeformableVolumeId id, const DeformableMaterialParams &material) = 0;

        /// Sends the `dirty` host arrays to the GPU and tells the solver what changed. False on a failure (`error`
        /// says why).
        [[nodiscard]] virtual bool Upload(DeformableVolumeId id, std::uint32_t dirty, std::string &error) = 0;
        /// Copies the collision positions (and, with `simVelocities`, the simulation velocities) from the GPU into
        /// the host mirror, after the scene step finished. False on a failure (`error` says why).
        [[nodiscard]] virtual bool ReadBack(DeformableVolumeId id, bool simVelocities, std::string &error) = 0;
    };
}
