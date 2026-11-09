#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/GpuCompute.hpp>

#include "engine/physics/ISoftBodyBackend.hpp"

namespace N2Engine::SoftBodies
{
    /**
     * Soft bodies simulated on the renderer's GPU through WebGPU compute (Renderer::Common::IGpuCompute; the WGSL is
     * renderer/shaders/webgpu/compute/softbody.wgsl). What a SoftBody uses for the WebGPU backend, and for Auto in a
     * browser whose renderer is WebGPU (see SelectSoftBodyBackend).
     *
     * The same model as the CPU backend (Physics::CpuSoftBodyBackend): XPBD with one distance constraint per edge and one
     * volume constraint per tetrahedron, the same compliances from Young's modulus and Poisson's ratio, the same
     * kSubSteps sub-steps, damping, masses, ground plane and friction, so a body looks and moves much as it does on the
     * CPU. What differs is the order the constraints are solved in: they are coloured on the CPU when the body is made
     * (no two constraints of a colour share a node), and one workgroup per body solves a colour in parallel before the
     * next, all sub-steps in one dispatch per Step. Not bitwise the CPU's result, and not deterministic across GPUs.
     *
     * Positions come back asynchronously, as for WebGpuParticleBackend: each Step asks for a copy of the node positions
     * when none is in flight, and CopySurfacePositions skins the render vertices on the latest copy that has arrived (the
     * rest shape until the first does). A copy with a non-finite position (the body blew up) puts the body back at rest,
     * as the CPU backend does.
     *
     * The backend lives as long as the renderer that made its compute (the `lifetime` token): once that expires, or the
     * device is lost, or a GPU call fails, IsHealthy is false, it simulates nothing more and the owner restarts its body
     * on the CPU. Caps as the CPU backend: kMaxTetsPerBody tetrahedra, solverIterations in [1, kMaxIterations].
     */
    class WebGpuSoftBodyBackend final : public Physics::ISoftBodyBackend
    {
    public:
        static constexpr int kSubSteps = 4;
        static constexpr std::uint32_t kMaxIterations = 32;

        /// The most steps the GPU may run ahead of the positions the CPU has: while the copy in flight was asked for this
        /// many steps ago or more, Step skips its GPU work (and, for particles, the lifetimes it counts down), so a slow
        /// GPU slows the simulation down instead of piling work up in its queue. 0: no limit (tests that count steps)
        static constexpr std::uint32_t kMaxStepsInFlight = 3;
        void SetMaxStepsInFlight(std::uint32_t steps) { _maxStepsInFlight = steps; }
        [[nodiscard]] std::uint32_t GetMaxStepsInFlight() const { return _maxStepsInFlight; }
        /// Steps whose GPU work was skipped because the GPU was behind (since the backend was made)
        [[nodiscard]] std::uint64_t GetSkippedStepCount() const { return _skippedSteps; }

        WebGpuSoftBodyBackend(Renderer::Common::IGpuCompute *compute, std::weak_ptr<const void> lifetime);
        ~WebGpuSoftBodyBackend() override;
        WebGpuSoftBodyBackend(const WebGpuSoftBodyBackend &) = delete;
        WebGpuSoftBodyBackend &operator=(const WebGpuSoftBodyBackend &) = delete;

        [[nodiscard]] std::string_view GetName() const override { return "WebGPU"; }

        /// kInvalidSoftBody when the backend is not healthy, for what the CPU backend refuses (a missing or malformed
        /// mesh, too many tetrahedra, a non-finite transform), or when the GPU refuses a buffer
        [[nodiscard]] Physics::SoftBodyHandle CreateBody(const Physics::SoftBodyDesc &desc) override;
        void Destroy(Physics::SoftBodyHandle handle) override;
        void SetParams(Physics::SoftBodyHandle handle, const Physics::SoftBodyParams &params) override;
        /// Applied to every node with mass at the start of the next Step
        void AddVelocity(Physics::SoftBodyHandle handle, const Math::Vector3 &velocity) override;
        void Step(float dt) override;
        void CopySurfacePositions(Physics::SoftBodyHandle handle, std::vector<Math::Vector3> &out) const override;

        /// True while the renderer lives, its device runs and every GPU call so far succeeded
        [[nodiscard]] bool IsHealthy() const;

        // For tests
        /// NATIVE ONLY: waits for the GPU and lands a fresh copy of every body's nodes, so the copies are as of the last
        /// Step. False in a browser or when not healthy
        bool SyncForTest();
        [[nodiscard]] std::size_t GetBodyCount() const { return _bodies.size(); }
        /// The node positions of the last copy that landed (the rest shape before the first); empty for an unknown handle
        void CopyNodePositions(Physics::SoftBodyHandle handle, std::vector<Math::Vector3> &out) const;
        /// How many colours the body's edge and volume constraints were split into (0, 0 for an unknown handle)
        [[nodiscard]] std::array<std::uint32_t, 2> GetColourCounts(Physics::SoftBodyHandle handle) const;

    private:
        struct GpuParams; // the WGSL Params block (WebGpuSoftBodyBackend.cpp)

        struct Body
        {
            Physics::SoftBodyParams params;
            std::shared_ptr<const Physics::TetMeshData> mesh;
            std::vector<std::array<std::uint32_t, 4>> tets; // oriented (TetSkinning OrientTets), in the mesh's order
            std::vector<std::uint8_t> flipped;
            std::vector<float> restNodes; // x, y, z, inverse mass per node: what the body is reset to
            float massDensity = 1000.0f;  // the density the inverse masses were made with
            std::uint32_t nodeCount = 0;
            std::uint32_t edgeCount = 0;
            std::uint32_t tetCount = 0;
            std::uint32_t edgeColours = 0;
            std::uint32_t tetColours = 0;
            Math::Vector3 push{};

            Renderer::Common::GpuBufferId uniform = 0;
            Renderer::Common::GpuBufferId positions = 0;
            Renderer::Common::GpuBufferId previous = 0;
            Renderer::Common::GpuBufferId velocities = 0;
            Renderer::Common::GpuBufferId edges = 0;
            Renderer::Common::GpuBufferId tetBuffer = 0;
            Renderer::Common::GpuBufferId ranges = 0;
            Renderer::Common::GpuBufferId lambdas = 0;

            Renderer::Common::GpuReadTicket ticket = 0;
            std::uint64_t steps = 0;      // Steps submitted to the GPU
            std::uint64_t ticketStep = 0; // `steps` when the copy in flight was asked for
            std::vector<Math::Vector3> nodes; // the last copy that landed (the rest shape before the first)
            std::vector<std::uint8_t> readBytes;
        };

        [[nodiscard]] bool Usable() const;
        [[nodiscard]] Body *Find(Physics::SoftBodyHandle handle);
        [[nodiscard]] const Body *Find(Physics::SoftBodyHandle handle) const;
        void ReleaseBuffers(Body &body);
        /// Lands the body's read in flight if it has completed (const callers too: the copy is a cache)
        void PollRead(Body &body) const;
        void ResetToRest(Body &body);
        void StepBody(Body &body, float dt);

        Renderer::Common::IGpuCompute *_compute;
        std::weak_ptr<const void> _lifetime;
        bool _broken = false;
        Renderer::Common::GpuKernelId _simulate = 0;
        std::uint32_t _maxStepsInFlight = kMaxStepsInFlight;
        std::uint64_t _skippedSteps = 0;
        mutable std::map<std::uint32_t, Body> _bodies; // by handle index, so bodies step in creation order
        std::uint32_t _nextIndex = 1;
    };
}
