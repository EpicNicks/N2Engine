#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "engine/physics/ISoftBodyBackend.hpp"

namespace N2Engine::Physics
{
    /**
     * The soft-body simulation that needs nothing but the CPU: it runs on every machine, headless hosts and CI
     * included, and is what a SoftBody uses when the PhysX GPU backend isn't there. Position-based dynamics in its
     * extended form (XPBD, Macklin, Muller and Chentanez 2016) on the body's tetrahedra.
     *
     * Constraints, per tetrahedron and edge of the tetrahedral mesh:
     * - one distance constraint per unique edge (C = |xi - xj| - L0), which resists shear and stretch, and
     * - one volume constraint per tetrahedron (C = V - V0), which resists compression.
     * Each has a compliance (the inverse of a stiffness, m/N for the edges and m^5/N for the volume) derived from the
     * body's Young's modulus E (stiffness, Pa) and Poisson ratio nu, the way a linear elastic solid would give them:
     *   shear modulus   G = E / (2 (1 + nu))     edge compliance    alpha = 1 / (G * L0)
     *   bulk modulus    K = E / (3 (1 - 2 nu))   volume compliance  alpha = V0 / K
     * (nu is clamped to [0, 0.49], so K stays finite). The mapping is a heuristic, not an FEM discretisation: a
     * stiffer E gives a stiffer body and a higher nu a more incompressible one, but the numbers are not PhysX's, so a
     * body does not behave exactly as the GPU backend would make it.
     *
     * Each Step(dt) runs kSubSteps fixed sub-steps of dt / kSubSteps. A sub-step integrates gravity and velocity
     * damping (v /= 1 + damping * h), moves the nodes, runs `solverIterations` Gauss-Seidel iterations over the
     * constraints (all edges, then all tetrahedra, in index order), then keeps every node above the ground plane
     * (the position is clamped; the tangential motion that sub-step is reduced by friction * penetration depth, as
     * Coulomb friction in PBD) and derives the velocities from the position change. The mass of a node is its share
     * (a quarter) of the density times the volume of every tetrahedron around it.
     *
     * Deterministic: the arithmetic is plain float code in a fixed order (no threads, no hash-ordered iteration, no
     * time or address dependence), so the same bodies stepped with the same dt give bitwise the same positions in
     * one build (not across compilers or architectures).
     *
     * Collision: only the optional ground plane (SoftBodyParams::groundEnabled). No colliders, no self-collision, no
     * pushing of rigid bodies (it never queries the physics world). Single threaded.
     *
     * Caps: a body of more than kMaxTetsPerBody tetrahedra is refused (kInvalidSoftBody); solverIterations is
     * clamped to [1, kMaxIterations].
     */
    class CpuSoftBodyBackend final : public ISoftBodyBackend
    {
    public:
        /// Fixed sub-steps in each Step
        static constexpr int kSubSteps = 4;
        static constexpr std::uint32_t kMaxIterations = 32;

        CpuSoftBodyBackend() = default;
        /// A backend whose handles carry `generation` (for a wrapper that is itself the backend; see ISoftBodyBackend)
        explicit CpuSoftBodyBackend(const std::uint32_t generation) : ISoftBodyBackend(generation) {}

        [[nodiscard]] std::string_view GetName() const override { return "CPU"; }

        /// kInvalidSoftBody for a missing or malformed tetrahedral mesh, one above kMaxTetsPerBody tetrahedra, or a
        /// non-finite transform
        [[nodiscard]] SoftBodyHandle CreateBody(const SoftBodyDesc &desc) override;
        void Destroy(SoftBodyHandle handle) override;
        void SetParams(SoftBodyHandle handle, const SoftBodyParams &params) override;
        void AddVelocity(SoftBodyHandle handle, const Math::Vector3 &velocity) override;
        void Step(float dt) override;
        void CopySurfacePositions(SoftBodyHandle handle, std::vector<Math::Vector3> &out) const override;

        // For tests and diagnostics
        /// How many bodies exist
        [[nodiscard]] std::size_t GetBodyCount() const { return _bodies.size(); }
        /// Replaces `out` with the body's node positions in world space (empty for an unknown handle)
        void CopyNodePositions(SoftBodyHandle handle, std::vector<Math::Vector3> &out) const;
        /// Replaces `out` with the body's node velocities (empty for an unknown handle)
        void CopyNodeVelocities(SoftBodyHandle handle, std::vector<Math::Vector3> &out) const;
        /// The summed volume of the body's tetrahedra now, and at rest (0 for an unknown handle)
        [[nodiscard]] float GetVolume(SoftBodyHandle handle) const;
        [[nodiscard]] float GetRestVolume(SoftBodyHandle handle) const;
        [[nodiscard]] std::size_t GetTetCount(SoftBodyHandle handle) const;

    private:
        struct V3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        struct Edge
        {
            std::uint32_t a = 0;
            std::uint32_t b = 0;
            float restLength = 0.0f;
        };

        struct Body
        {
            SoftBodyParams params;
            std::shared_ptr<const TetMeshData> mesh;
            std::vector<std::array<std::uint32_t, 4>> tets; // positively oriented in world space
            std::vector<float> restVolume;                  // per tetrahedron; 0 for a degenerate one (no constraint)
            std::vector<Edge> edges;                        // unique, sorted by node pair
            std::vector<V3> rest;                           // the world-space rest shape
            std::vector<V3> position;
            std::vector<V3> previous;
            std::vector<V3> velocity;
            std::vector<float> invMass;
            std::vector<float> edgeLambda;
            std::vector<float> volumeLambda;
        };

        [[nodiscard]] Body *Find(SoftBodyHandle handle);
        [[nodiscard]] const Body *Find(SoftBodyHandle handle) const;
        static SoftBodyParams Sanitized(SoftBodyParams params);
        static void ComputeMasses(Body &body);
        static void Simulate(Body &body, float dt);
        static void ResetToRest(Body &body);

        std::map<std::uint32_t, Body> _bodies; // by handle index, so bodies step in creation order
        std::uint32_t _nextIndex = 1;
    };
}
