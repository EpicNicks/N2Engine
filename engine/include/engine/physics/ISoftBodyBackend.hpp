#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <math/Vector3.hpp>

#include "engine/physics/BackendHandle.hpp"
#include "engine/physics/SoftMaterialModel.hpp"
#include "engine/physics/TetMeshData.hpp"

namespace N2Engine::Physics
{
    /// One soft body inside a soft-body backend. 0 is never a valid handle. Generation-counted like
    /// ParticleSystemHandle (see BackendHandle.hpp): a handle of another backend object is ignored.
    using SoftBodyHandle = BackendHandle;
    inline constexpr SoftBodyHandle kInvalidSoftBody = kInvalidBackendHandle;

    namespace Detail
    {
        struct SoftBodyGenerationTag;
    }

    [[nodiscard]] constexpr SoftBodyHandle MakeSoftBodyHandle(const std::uint32_t generation, const std::uint32_t index)
    {
        return MakeBackendHandle(generation, index);
    }

    [[nodiscard]] constexpr std::uint32_t SoftBodyHandleGeneration(const SoftBodyHandle handle)
    {
        return BackendHandleGeneration(handle);
    }

    [[nodiscard]] constexpr std::uint32_t SoftBodyHandleIndex(const SoftBodyHandle handle)
    {
        return BackendHandleIndex(handle);
    }

    /// The material settings a backend can change at any time (SetParams)
    struct SoftBodyParams
    {
        /// Mass per unit volume
        float density = 1000.0f;
        /// Young's modulus (PhysX units; a CPU backend maps it to its own stiffness)
        float stiffness = 1.0e5f;
        float poisson = 0.3f;
        float damping = 0.01f;
        float friction = 0.5f;
        std::uint32_t solverIterations = 8;
        /// World gravity (the CPU backend applies it; a GPU backend simulates inside the physics scene, which has its own)
        Math::Vector3 gravity{0.0f, -9.81f, 0.0f};
        /// The body collides with the plane y = groundHeight (world space). The only collision the CPU backend does;
        /// the PhysX backend ignores it (its body collides with the scene's colliders instead).
        bool groundEnabled = false;
        float groundHeight = 0.0f;
        /// The PhysX backend's material model and self-collision switch; the CPU backend ignores both
        SoftMaterialModel materialModel = SoftMaterialModel::CoRotational;
        bool selfCollision = false;
    };

    /// What CreateBody takes: the tetrahedral mesh with the render-vertex embedding (shared, never changed; a
    /// backend refuses a null one), where the body sits, and its parameters
    struct SoftBodyDesc
    {
        SoftBodyParams params;
        std::shared_ptr<const TetMeshData> tetMesh;
        /// The mesh's local space to world space: the top three rows of the 4x4 matrix, row-major (translation in
        /// elements 3, 7 and 11). The body starts at its rest shape under this transform.
        float localToWorld[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    };

    /**
     * Simulates soft bodies for SoftBody components. The seam between the component and whatever runs the
     * simulation: an engine CPU backend and, later, a PhysX GPU backend (deformable volumes) that
     * IPhysicsBackend::GetSoftBodyBackend returns when the scene runs on the GPU. No PhysX or CUDA type appears
     * here. Everything is called on the main thread.
     *
     * A backend holds many bodies, each named by a handle. It gets its generation when it is constructed and
     * returns it from GetGeneration; every handle it issues carries it, and every call with a handle of another
     * generation is ignored.
     */
    class ISoftBodyBackend
    {
    public:
        virtual ~ISoftBodyBackend() = default;

        /// What the handles this backend issued carry; never 0, never the same for two backend objects
        [[nodiscard]] std::uint32_t GetGeneration() const { return _generation; }
        /// True when `handle` was issued by this backend object (its generation matches). It says nothing of
        /// whether the body still exists.
        [[nodiscard]] bool OwnsHandle(const SoftBodyHandle handle) const
        {
            return BackendHandleOwnedBy(_generation, handle);
        }

        /// "CPU" or "PhysX": what health and logs call the backend
        [[nodiscard]] virtual std::string_view GetName() const = 0;

        /// kInvalidSoftBody if the backend can't make another body
        [[nodiscard]] virtual SoftBodyHandle CreateBody(const SoftBodyDesc &desc) = 0;
        virtual void Destroy(SoftBodyHandle handle) = 0;
        virtual void SetParams(SoftBodyHandle handle, const SoftBodyParams &params) = 0;
        /// Adds `velocity` (world units per second) to every node of the body: a push for scripts. Ignored for an
        /// unknown handle. A backend that can't do it keeps the default, which does nothing.
        virtual void AddVelocity(SoftBodyHandle handle, const Math::Vector3 &velocity)
        {
            static_cast<void>(handle);
            static_cast<void>(velocity);
        }

        /**
         * Advances the simulation by `dt` seconds. A CPU backend does its work here, and its owner calls this once
         * per fixed step. A backend that simulates inside the physics scene (the scene steps it) does nothing.
         */
        virtual void Step(float dt) = 0;

        /**
         * Replaces `out` with the body's render-vertex positions in world space (empty for an unknown handle).
         * `out` keeps its capacity, so a caller that reuses it allocates nothing once it has grown.
         */
        virtual void CopySurfacePositions(SoftBodyHandle handle, std::vector<Math::Vector3> &out) const = 0;

    protected:
        ISoftBodyBackend() : _generation(NextBackendGeneration<Detail::SoftBodyGenerationTag>()) {}
        /// For a backend that wraps another and must hand out handles the wrapper's own generation owns
        explicit ISoftBodyBackend(const std::uint32_t generation) : _generation(generation) {}

    private:
        std::uint32_t _generation;
    };
}
