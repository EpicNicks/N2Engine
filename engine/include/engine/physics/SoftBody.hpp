#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/physics/ISoftBodyBackend.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/softbody/SoftBodyBackendSelection.hpp"

namespace N2Engine::Physics
{
    class CpuSoftBodyBackend;

    /// The material model of the PhysX soft-body backend (the CPU backend ignores it)
    enum class SoftMaterialModel
    {
        /// PhysX's default: fast, needs well-shaped tetrahedra
        CoRotational,
        /// Robust to bad tetrahedra, slower
        NeoHookean
    };

    N2_SERIALIZE_ENUM(SoftMaterialModel, {
                      { SoftMaterialModel::CoRotational, "CoRotational" },
                      { SoftMaterialModel::NeoHookean, "NeoHookean" }
                      })

    /**
     * Makes the MeshRenderer on the same GameObject a soft body: it squashes and wobbles under gravity. See
     * docs/soft-bodies.html.
     *
     * Needs a MeshRenderer (with a mesh) on the same object and refuses a Rigidbody there; with either wrong it
     * does nothing and logs one warning when it starts. The body is tetrahedralised from the mesh's surface when the
     * component attaches (play starts): SoftBodies::Tetrahedralise voxelises it at `resolution` voxels along its
     * longest axis and embeds every render vertex in a tetrahedron, cached in memory per mesh version and resolution
     * (SoftBodies::TetCache), so a large mesh can stall entering play.
     *
     * Simulation: a SoftBody gets its body simulated by an ISoftBodyBackend, chosen by `backend` when the component
     * attaches (Auto, PhysX or CPU, see SoftBodies::SelectSoftBodyBackend, which mirrors the particles' selection).
     * That is the CPU backend (Physics::CpuSoftBodyBackend, XPBD) unless the physics backend has a PhysX GPU
     * soft-body backend, which none does yet: Auto then uses the GPU one and otherwise the CPU one quietly, PhysX the
     * GPU one and otherwise the CPU one with one warning. A body on the GPU backend whose backend is replaced or
     * recreated (its handle's generation no longer matches) restarts on the CPU with one warning; with the physics
     * backend removed entirely it is inert. The CPU backend is stepped from OnFixedUpdate, once per fixed step, so
     * the body moves the same whatever the frame rate. The simulation is in world space: moving the object in play
     * does not drag the body (Reset puts it back at the object's current pose).
     *
     * Play mode, as the ParticleSystem: the body exists only while the scene is playing (nothing is simulated or
     * changed in a scene opened for editing, which shows the undeformed mesh), the deformation is never saved, and
     * leaving play mode destroys it with the scene. Pausing (time scale 0) freezes it: no fixed step runs.
     *
     * Rendering: the component owns a runtime copy of the mesh and hands it to the MeshRenderer through its
     * runtime-only override (MeshRenderer::SetDeformedMesh, never serialized, cleared when the SoftBody is destroyed).
     * After each step the copy gets the skinned vertex positions and recomputed area-weighted normals (Mesh::SetData,
     * which bumps the version so the GPU copy is re-uploaded in place); tangents, texture coordinates, colours,
     * submeshes and materials stay the rest ones. The renderer is unchanged: whatever draws a mesh draws it.
     */
    class SoftBody final : public SerializableComponent
    {
    public:
        explicit SoftBody(GameObject &gameObject);
        ~SoftBody() override;

        [[nodiscard]] std::string GetTypeName() const override { return "SoftBody"; }

        // Lifecycle
        void OnAttach() override;
        void OnFixedUpdate() override;
        void OnDestroy() override;

        // Control
        /// Adds `velocity` (world units per second) to every node of the body; does nothing before it started
        void AddVelocity(const Math::Vector3 &velocity);
        /// Puts the body back to its rest shape at the object's current pose, at rest (restarting it, so the
        /// backend is chosen again). Does nothing before the component attached.
        void Reset();
        /// "CPU" or "PhysX": the backend simulating this body; empty when it is inert or before it started
        [[nodiscard]] std::string_view GetActiveBackendName() const;
        /// The tetrahedral mesh in use (nullptr when inert), and the runtime mesh the MeshRenderer draws
        [[nodiscard]] const std::shared_ptr<const TetMeshData> &GetTetMesh() const { return _tetMesh; }
        [[nodiscard]] const std::shared_ptr<Rendering::Mesh> &GetRuntimeMesh() const { return _runtimeMesh; }

        /// The simulation settings the current fields give (the physics backend's gravity, else 0, -9.81, 0)
        [[nodiscard]] SoftBodyParams BuildParams() const;

        static constexpr bool IsSingleton = false;

        // Settings, all saved with the scene (an inspector edits them; most apply at once, backend and resolution
        // when the body starts)

        /// Which simulation to ask for; see SoftBodies::SoftBodyBackend
        SoftBodies::SoftBodyBackend backend = SoftBodies::SoftBodyBackend::Auto;
        /// Voxels along the longest axis of the mesh the body is built from (2 to 32); more is finer and slower. Read
        /// when the body starts. Lowered if the body would have more than kMaxTetsPerBody tetrahedra.
        int resolution = 10;
        /// Mass per unit volume, kg/m^3 (water is 1000)
        float density = 1000.0f;
        /// Young's modulus in pascals: how hard the body resists being deformed (rubber 1e6, a soft gel 1e4). The
        /// CPU backend maps it to its constraint compliances (see CpuSoftBodyBackend).
        float stiffness = 1.0e5f;
        /// Poisson's ratio, 0 to 0.49: how much the body keeps its volume when squashed (0.49 is nearly
        /// incompressible, 0 shrinks freely)
        float poisson = 0.3f;
        /// Velocity decay in 1/s: how quickly the wobble dies out (0 never)
        float damping = 0.1f;
        /// Friction against the ground, 0 to 1 (the CPU backend's Coulomb coefficient)
        float friction = 0.5f;
        /// Solver iterations in each of the CPU backend's 4 sub-steps; more is stiffer and slower
        int solverIterations = 2;
        /// The body rests on the plane y = groundHeight (world space). The only collision the CPU backend does.
        bool collideWithGround = false;
        float groundHeight = 0.0f;
        /// Used only by the PhysX backend (a later stage); the CPU backend ignores both
        SoftMaterialModel materialModel = SoftMaterialModel::CoRotational;
        bool selfCollision = false;

    private:
        /// Builds the body: true when it runs. Warns once and leaves the component inert when it can't.
        bool Start();
        void ReleaseSim(bool touchRenderer);
        /// A PhysX-backed body whose physics backend was replaced drops its stale handle and restarts on the CPU
        /// backend with one warning; true when it did
        bool RecoverFromReplacedBackend();
        /// The backend simulating this body: the CPU one this component owns, or the physics backend's GPU one,
        /// asked for each time (it goes away with the physics backend); nullptr when there is none
        [[nodiscard]] ISoftBodyBackend *Backend() const;
        [[nodiscard]] SoftBodyDesc BuildDesc() const;
        /// Writes the backend's positions and recomputed normals into the runtime mesh
        void UpdateRuntimeMesh();

        std::unique_ptr<CpuSoftBodyBackend> _cpuBackend; // owned when the CPU backend is the one in use
        SoftBodyHandle _handle = kInvalidSoftBody;
        bool _usingGpu = false;
        bool _attached = false;
        std::shared_ptr<const TetMeshData> _tetMesh;

        // The picture: the runtime mesh, its vertices (rest data with the current positions and normals) and
        // submeshes, and the scratch vectors that keep their capacity between steps
        std::shared_ptr<Rendering::Mesh> _runtimeMesh;
        Renderer::Common::MeshData _meshData;
        std::vector<Rendering::Submesh> _submeshes;
        std::vector<Math::Vector3> _worldPositions;
        std::vector<float> _normalSums;
    };
}
