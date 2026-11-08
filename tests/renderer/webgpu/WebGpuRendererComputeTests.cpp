#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <renderer/common/GpuCompute.hpp>
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "engine/particles/CpuParticleBackend.hpp"
#include "engine/particles/WebGpuParticleBackend.hpp"
#include "engine/physics/CpuSoftBodyBackend.hpp"
#include "engine/physics/TetMeshData.hpp"
#include "engine/softbody/WebGpuSoftBodyBackend.hpp"

#include "WebGpuTestFixture.hpp"

// General-purpose compute on the WebGPU renderer (Renderer::Common::IGpuCompute, renderer/src/webgpu/WebGpuCompute.cpp) and
// the engine's two simulations on it: Particles::WebGpuParticleBackend (renderer/shaders/webgpu/compute/particles.wgsl) and
// SoftBodies::WebGpuSoftBodyBackend (softbody.wgsl). On Dawn's Vulkan backend (Mesa lavapipe in CI), with the fixture failing a
// test on any device error and on any WebGPU object left referenced after shutdown. The reads are asynchronous, as in a
// browser; natively WaitIdleForTest (and the backends' SyncForTest) lets a test wait for them.

using namespace N2Engine;
using namespace WebGpuTesting;
using Renderer::Common::GpuBindingKind;
using Renderer::Common::GpuBufferUsage;
using Renderer::Common::GpuReadStatus;
using Renderer::Common::IGpuCompute;

namespace
{
    constexpr std::string_view kDoubleWgsl = R"(
@group(0) @binding(0) var<storage, read_write> data: array<f32>;

@compute @workgroup_size(64) fn double_all(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x < arrayLength(&data)) {
        data[id.x] = data[id.x] * 2.0;
    }
}
)";

    constexpr std::array<GpuBindingKind, 1> kOneStorage = {GpuBindingKind::Storage};

    /// Reads `bytes` bytes of `buffer` back as floats, waiting natively
    std::vector<float> ReadFloats(IGpuCompute &compute, const Renderer::Common::GpuBufferId buffer, const std::uint64_t bytes)
    {
        const Renderer::Common::GpuReadTicket ticket = compute.RequestRead(buffer, 0, bytes);
        EXPECT_NE(ticket, 0u);
        EXPECT_TRUE(compute.WaitIdleForTest());
        std::vector<std::uint8_t> raw;
        EXPECT_EQ(compute.PollRead(ticket, raw), GpuReadStatus::Done);
        std::vector<float> values(raw.size() / sizeof(float));
        std::memcpy(values.data(), raw.data(), values.size() * sizeof(float));
        return values;
    }

    /// A unit cube of 8 nodes and 6 tetrahedra (around its main diagonal, every one positively oriented), standing on
    /// y = `bottom`, every corner a render vertex riding on one tetrahedron
    std::shared_ptr<Physics::TetMeshData> Cube(const float bottom)
    {
        auto mesh = std::make_shared<Physics::TetMeshData>();
        for (int i = 0; i < 8; ++i)
        {
            mesh->positions.emplace_back(static_cast<float>(i & 1), bottom + static_cast<float>((i >> 1) & 1),
                                         static_cast<float>((i >> 2) & 1));
        }
        const std::array<std::array<int, 3>, 6> axes = {
            {{1, 2, 4}, {1, 4, 2}, {2, 1, 4}, {2, 4, 1}, {4, 1, 2}, {4, 2, 1}}};
        for (const auto &order : axes)
        {
            std::array<std::uint32_t, 4> tet = {0u, static_cast<std::uint32_t>(order[0]),
                                                static_cast<std::uint32_t>(order[0] + order[1]), 7u};
            const auto &p = mesh->positions;
            const Math::Vector3 a = p[tet[0]], b = p[tet[1]], c = p[tet[2]], d = p[tet[3]];
            const float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
            const float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
            const float e3x = d.x - a.x, e3y = d.y - a.y, e3z = d.z - a.z;
            const float volume6 =
                (e1y * e2z - e1z * e2y) * e3x + (e1z * e2x - e1x * e2z) * e3y + (e1x * e2y - e1y * e2x) * e3z;
            if (volume6 < 0.0f)
            {
                std::swap(tet[2], tet[3]);
            }
            mesh->tets.push_back(tet);
        }
        for (std::uint32_t node = 0; node < 8; ++node)
        {
            for (std::uint32_t t = 0; t < mesh->tets.size(); ++t)
            {
                const auto &tet = mesh->tets[t];
                const auto corner = std::ranges::find(tet, node);
                if (corner != tet.end())
                {
                    Physics::TetEmbedding embedding;
                    embedding.tet = t;
                    std::fill(std::begin(embedding.weights), std::end(embedding.weights), 0.0f);
                    embedding.weights[corner - tet.begin()] = 1.0f;
                    mesh->surfaceEmbedding.push_back(embedding);
                    break;
                }
            }
        }
        mesh->weldedVertexCount = 8;
        mesh->resolution = 1;
        mesh->voxelSize = 1.0f;
        return mesh;
    }

    Physics::SoftBodyDesc CubeDesc(const float bottom)
    {
        Physics::SoftBodyDesc desc;
        desc.tetMesh = Cube(bottom);
        desc.params.stiffness = 1.0e6f; // stiff enough that the sag (and the solvers' difference in it) is small
        desc.params.density = 1000.0f;
        desc.params.damping = 1.0f;
        desc.params.solverIterations = 8;
        desc.params.groundEnabled = true;
        desc.params.groundHeight = 0.0f;
        return desc;
    }

    float NearestDistance(const std::vector<Math::Vector4> &points, const std::size_t i)
    {
        float best = std::numeric_limits<float>::max();
        for (std::size_t j = 0; j < points.size(); ++j)
        {
            if (j == i)
            {
                continue;
            }
            const float dx = points[i].x - points[j].x, dy = points[i].y - points[j].y, dz = points[i].z - points[j].z;
            best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        return best;
    }

    /// `count` particles at rest in a small jittered clump around `centre` (distinct positions, far closer than at rest)
    void Clump(const std::size_t count, const Math::Vector3 &centre, const float size, std::vector<Math::Vector3> &positions,
               std::vector<Math::Vector3> &velocities)
    {
        positions.clear();
        velocities.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            const float fx = static_cast<float>(i % 4) / 3.0f - 0.5f;
            const float fy = static_cast<float>((i / 4) % 4) / 3.0f - 0.5f;
            const float fz = static_cast<float>(i / 16) / 3.0f - 0.5f;
            positions.emplace_back(centre.x + fx * size, centre.y + fy * size, centre.z + fz * size);
            velocities.emplace_back(0.0f, 0.0f, 0.0f);
        }
    }
}

class WebGpuComputeTest : public WebGpuTest
{
};

// ---------------------------------------------------------------- the compute interface

TEST_F(WebGpuComputeTest, ComputeIsOfferedOnlyWhileTheRendererIsInitialized)
{
    ASSERT_NE(R().GetCompute(), nullptr);
    EXPECT_TRUE(R().GetCompute()->IsUsable());
    EXPECT_EQ(R().GetCompute()->GetName(), "WebGPU");
    EXPECT_GT(R().GetCompute()->GetMaxBufferBytes(), 0u);

    WebGpuRenderer &fresh = NewRenderer();
    EXPECT_EQ(fresh.GetCompute(), nullptr) << "not initialized";

    IGpuCompute *compute = R().GetCompute();
    R().Shutdown();
    EXPECT_EQ(R().GetCompute(), nullptr);
    EXPECT_FALSE(compute->IsUsable()) << "the object outlives the device and says so";
    EXPECT_EQ(compute->CreateBuffer(GpuBufferUsage::Storage, 16), 0u);
}

TEST_F(WebGpuComputeTest, ANewBufferIsZeroAndAWriteReadsBack)
{
    IGpuCompute &compute = *R().GetCompute();
    const auto buffer = compute.CreateBuffer(GpuBufferUsage::Storage, 64);
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(ReadFloats(compute, buffer, 64), std::vector<float>(16, 0.0f));

    std::vector<float> values(16);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        values[i] = static_cast<float>(i) * 1.5f - 3.0f;
    }
    ASSERT_TRUE(compute.WriteBuffer(buffer, 0, values.data(), 64));
    EXPECT_EQ(ReadFloats(compute, buffer, 64), values);

    const float patch[2] = {100.0f, 200.0f};
    ASSERT_TRUE(compute.WriteBuffer(buffer, 8, patch, sizeof(patch)));
    const auto ticket = compute.RequestRead(buffer, 8, 8);
    ASSERT_TRUE(compute.WaitIdleForTest());
    std::vector<std::uint8_t> raw;
    ASSERT_EQ(compute.PollRead(ticket, raw), GpuReadStatus::Done);
    ASSERT_EQ(raw.size(), 8u);
    float back[2];
    std::memcpy(back, raw.data(), 8);
    EXPECT_EQ(back[0], 100.0f);
    EXPECT_EQ(back[1], 200.0f);
    EXPECT_EQ(compute.PollRead(ticket, raw), GpuReadStatus::Failed) << "a finished ticket is unknown";
    compute.DestroyBuffer(buffer);
}

TEST_F(WebGpuComputeTest, DispatchesRunInOrderAndAWriteAfterADispatchComesAfterIt)
{
    IGpuCompute &compute = *R().GetCompute();
    const auto kernel = compute.CreateKernel("double", kDoubleWgsl, "double_all", kOneStorage);
    ASSERT_NE(kernel, 0u);
    EXPECT_EQ(compute.CreateKernel("double again", kDoubleWgsl, "double_all", kOneStorage), kernel) << "cached";

    constexpr std::size_t kCount = 100; // two workgroups, the second partly used
    const auto buffer = compute.CreateBuffer(GpuBufferUsage::Storage, kCount * sizeof(float));
    ASSERT_NE(buffer, 0u);
    std::vector<float> values(kCount);
    for (std::size_t i = 0; i < kCount; ++i)
    {
        values[i] = static_cast<float>(i);
    }
    ASSERT_TRUE(compute.WriteBuffer(buffer, 0, values.data(), kCount * sizeof(float)));
    const std::array<Renderer::Common::GpuBufferId, 1> bindings = {buffer};
    ASSERT_TRUE(compute.Dispatch(kernel, bindings, 2));
    ASSERT_TRUE(compute.Dispatch(kernel, bindings, 2));
    // Recorded before this write: they run on the values above (x4), and the write lands after them
    const float one = 1.0f;
    ASSERT_TRUE(compute.WriteBuffer(buffer, 0, &one, sizeof(one)));
    ASSERT_TRUE(compute.Dispatch(kernel, bindings, 2));
    ASSERT_TRUE(compute.Submit());

    const std::vector<float> result = ReadFloats(compute, buffer, kCount * sizeof(float));
    ASSERT_EQ(result.size(), kCount);
    EXPECT_EQ(result[0], 2.0f) << "the write came after the first two dispatches and before the third";
    for (std::size_t i = 1; i < kCount; ++i)
    {
        EXPECT_EQ(result[i], static_cast<float>(i) * 8.0f) << "element " << i;
    }
    compute.DestroyBuffer(buffer);
}

TEST_F(WebGpuComputeTest, BadArgumentsFailQuietlyWithoutADeviceError)
{
    IGpuCompute &compute = *R().GetCompute();
    EXPECT_EQ(compute.CreateBuffer(GpuBufferUsage::Storage, 0), 0u);
    EXPECT_EQ(compute.CreateBuffer(GpuBufferUsage::Storage, 6), 0u) << "not a multiple of 4";
    EXPECT_EQ(compute.CreateBuffer(GpuBufferUsage::Storage, compute.GetMaxBufferBytes() + 4), 0u);

    const auto storage = compute.CreateBuffer(GpuBufferUsage::Storage, 16);
    const auto uniform = compute.CreateBuffer(GpuBufferUsage::Uniform, 16);
    ASSERT_NE(storage, 0u);
    ASSERT_NE(uniform, 0u);
    const float data[4] = {};
    EXPECT_FALSE(compute.WriteBuffer(storage, 4, data, 16)) << "past the end";
    EXPECT_FALSE(compute.WriteBuffer(storage, 2, data, 4)) << "misaligned";
    EXPECT_FALSE(compute.WriteBuffer(12345, 0, data, 4)) << "unknown id";
    EXPECT_EQ(compute.RequestRead(uniform, 0, 16), 0u) << "a uniform buffer can't be read back";
    EXPECT_EQ(compute.RequestRead(storage, 0, 20), 0u) << "past the end";

    const auto kernel = compute.CreateKernel("double", kDoubleWgsl, "double_all", kOneStorage);
    ASSERT_NE(kernel, 0u);
    const std::array<Renderer::Common::GpuBufferId, 1> asUniform = {uniform};
    const std::array<Renderer::Common::GpuBufferId, 2> tooMany = {storage, storage};
    const std::array<Renderer::Common::GpuBufferId, 1> unknown = {999999};
    EXPECT_FALSE(compute.Dispatch(kernel, asUniform, 1)) << "a uniform buffer at a storage binding";
    EXPECT_FALSE(compute.Dispatch(kernel, tooMany, 1));
    EXPECT_FALSE(compute.Dispatch(kernel, unknown, 1));
    EXPECT_FALSE(compute.Dispatch(424242, unknown, 1)) << "unknown kernel";
    const std::array<Renderer::Common::GpuBufferId, 1> fine = {storage};
    EXPECT_TRUE(compute.Dispatch(kernel, fine, 0)) << "zero workgroups record nothing";
    EXPECT_TRUE(compute.Submit());

    std::vector<std::uint8_t> raw;
    EXPECT_EQ(compute.PollRead(777, raw), GpuReadStatus::Failed);
    compute.CancelRead(777);
    compute.DestroyBuffer(0);
    compute.DestroyBuffer(storage);
    compute.DestroyBuffer(uniform);
    // The fixture's teardown fails the test on any device error: none of the above reached the device
}

TEST_F(WebGpuComputeTest, IdsMadeBeforeAShutdownNameNothingAfterIt)
{
    const auto buffer = R().GetCompute()->CreateBuffer(GpuBufferUsage::Storage, 16);
    ASSERT_NE(buffer, 0u);
    const auto ticket = R().GetCompute()->RequestRead(buffer, 0, 16); // in flight across the shutdown
    ASSERT_NE(ticket, 0u);
    R().Shutdown();
    ASSERT_TRUE(R().Initialize(nullptr, 8, 8));
    IGpuCompute &compute = *R().GetCompute();
    const float data[4] = {};
    EXPECT_FALSE(compute.WriteBuffer(buffer, 0, data, 16));
    std::vector<std::uint8_t> raw;
    EXPECT_EQ(compute.PollRead(ticket, raw), GpuReadStatus::Failed);
    EXPECT_NE(compute.CreateBuffer(GpuBufferUsage::Storage, 16), buffer) << "ids are never reused";
}

// ---------------------------------------------------------------- the particle backend

namespace
{
    Physics::ParticleSystemDesc Desc(const Physics::ParticleMaterial material, const float lifetime)
    {
        Physics::ParticleSystemDesc desc;
        desc.maxParticles = 256;
        desc.seed = 7;
        desc.material = material;
        desc.params.lifetimeMin = lifetime;
        desc.params.lifetimeMax = lifetime;
        desc.params.groundEnabled = true;
        desc.params.groundHeight = 0.0f;
        desc.params.bounce = 0.0f;
        return desc;
    }
}

TEST_F(WebGpuComputeTest, ParticlesFallOntoTheGroundAndStayAboveIt)
{
    Particles::WebGpuParticleBackend backend(R().GetCompute(), R().GetLifetimeToken());
    ASSERT_TRUE(backend.IsHealthy());
    EXPECT_EQ(backend.GetName(), "WebGPU");
    const auto handle = backend.CreateSystem(Desc(Physics::ParticleMaterial::Water, 100.0f));
    ASSERT_NE(handle, Physics::kInvalidParticleSystem);

    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    for (int i = 0; i < 20; ++i)
    {
        // Spread out (no neighbours): each falls on its own from 1 m
        positions.emplace_back(static_cast<float>(i) * 0.5f, 1.0f, 0.0f);
        velocities.emplace_back(0.0f, 0.0f, 0.0f);
    }
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 20u);
    EXPECT_EQ(backend.GetAliveCount(handle), 20u);

    for (int step = 0; step < 10; ++step)
    {
        backend.Step(0.02f);
    }
    ASSERT_TRUE(backend.SyncForTest());
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 20u);
    for (const Math::Vector4 &p : out)
    {
        // Free fall for 0.2 s: 0.196 m, a little less with the preset's damping
        EXPECT_NEAR(p.y, 1.0f - 0.5f * 9.81f * 0.04f, 0.03f);
        EXPECT_GT(p.w, 0.0f);
        EXPECT_LT(p.w, 0.01f) << "0.2 s of a 100 s life";
    }

    for (int step = 0; step < 100; ++step)
    {
        backend.Step(0.02f);
    }
    ASSERT_TRUE(backend.SyncForTest());
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 20u);
    for (const Math::Vector4 &p : out)
    {
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        EXPECT_GE(p.y, -1e-4f) << "the ground plane holds";
        EXPECT_LT(p.y, 0.05f) << "it came down";
    }
}

TEST_F(WebGpuComputeTest, AFluidClumpSpreadsOutInsteadOfStayingPiledUp)
{
    Particles::WebGpuParticleBackend backend(R().GetCompute(), R().GetLifetimeToken());
    ASSERT_TRUE(backend.IsHealthy());
    const auto handle = backend.CreateSystem(Desc(Physics::ParticleMaterial::Water, 100.0f));
    ASSERT_NE(handle, Physics::kInvalidParticleSystem);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    Clump(64, Math::Vector3(0.0f, 0.1f, 0.0f), 0.03f, positions, velocities); // water's spacing is 0.1
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 64u);
    for (int step = 0; step < 100; ++step)
    {
        backend.Step(0.02f);
    }
    ASSERT_TRUE(backend.SyncForTest());
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 64u);
    float sum = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        ASSERT_TRUE(std::isfinite(out[i].x) && std::isfinite(out[i].y) && std::isfinite(out[i].z));
        EXPECT_GE(out[i].y, -1e-4f);
        sum += NearestDistance(out, i);
    }
    // Started 0.01 m apart; the density constraint pushes them out towards the rest spacing
    EXPECT_GT(sum / static_cast<float>(out.size()), 0.04f) << "mean distance to the nearest neighbour";
}

TEST_F(WebGpuComputeTest, SandPilesUpWithoutParticlesInsideEachOther)
{
    Particles::WebGpuParticleBackend backend(R().GetCompute(), R().GetLifetimeToken());
    ASSERT_TRUE(backend.IsHealthy());
    const auto handle = backend.CreateSystem(Desc(Physics::ParticleMaterial::Sand, 100.0f));
    ASSERT_NE(handle, Physics::kInvalidParticleSystem);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    Clump(64, Math::Vector3(0.0f, 0.5f, 0.0f), 0.04f, positions, velocities); // sand's spacing is 0.08
    ASSERT_EQ(backend.Spawn(handle, positions, velocities), 64u);
    for (int step = 0; step < 150; ++step)
    {
        backend.Step(0.02f);
    }
    ASSERT_TRUE(backend.SyncForTest());
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    ASSERT_EQ(out.size(), 64u);
    float lowest = std::numeric_limits<float>::max();
    float highest = std::numeric_limits<float>::lowest();
    float sum = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        ASSERT_TRUE(std::isfinite(out[i].x) && std::isfinite(out[i].y) && std::isfinite(out[i].z));
        lowest = std::min(lowest, out[i].y);
        highest = std::max(highest, out[i].y);
        sum += NearestDistance(out, i);
    }
    EXPECT_GE(lowest, -1e-4f);
    EXPECT_GT(highest, 0.02f) << "a pile, not a single layer of overlapping grains";
    EXPECT_GT(sum / static_cast<float>(out.size()), 0.04f) << "grains keep (most of) their spacing";
}

TEST_F(WebGpuComputeTest, ParticlesDieAtTheEndOfTheirLifetimeAndTheirSlotsAreReused)
{
    Particles::WebGpuParticleBackend backend(R().GetCompute(), R().GetLifetimeToken());
    ASSERT_TRUE(backend.IsHealthy());
    auto desc = Desc(Physics::ParticleMaterial::Smoke, 0.1f);
    desc.maxParticles = 8;
    const auto handle = backend.CreateSystem(desc);
    ASSERT_NE(handle, Physics::kInvalidParticleSystem);

    std::vector<Math::Vector3> positions(12, Math::Vector3(0.0f, 1.0f, 0.0f));
    std::vector<Math::Vector3> velocities(12, Math::Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(backend.Spawn(handle, positions, velocities), 8u) << "no room beyond maxParticles";
    EXPECT_EQ(backend.GetAliveCount(handle), 8u);
    for (int step = 0; step < 4; ++step)
    {
        backend.Step(0.02f);
    }
    EXPECT_EQ(backend.GetAliveCount(handle), 8u) << "0.08 s of a 0.1 s life";
    for (int step = 0; step < 2; ++step)
    {
        backend.Step(0.02f);
    }
    EXPECT_EQ(backend.GetAliveCount(handle), 0u) << "all gone after 0.12 s";
    std::vector<Math::Vector4> out;
    backend.CopyPositions(handle, out);
    EXPECT_TRUE(out.empty());

    EXPECT_EQ(backend.Spawn(handle, positions, velocities), 8u) << "the slots are free again";
    backend.Clear(handle);
    EXPECT_EQ(backend.GetAliveCount(handle), 0u);
    backend.Step(0.02f);
    ASSERT_TRUE(backend.SyncForTest());
    backend.Destroy(handle);
    EXPECT_EQ(backend.GetSystemCount(), 0u);
}

TEST_F(WebGpuComputeTest, ParticleLifetimesMatchTheCpuBackends)
{
    // The same seed draws the same lifetimes, and the CPU-side count of the WebGPU backend never waits for the GPU
    Particles::WebGpuParticleBackend gpu(R().GetCompute(), R().GetLifetimeToken());
    Particles::CpuParticleBackend cpu;
    auto desc = Desc(Physics::ParticleMaterial::Water, 0.3f);
    desc.params.lifetimeMin = 0.1f;
    desc.params.lifetimeMax = 0.5f;
    const auto gpuHandle = gpu.CreateSystem(desc);
    const auto cpuHandle = cpu.CreateSystem(desc);
    ASSERT_NE(gpuHandle, Physics::kInvalidParticleSystem);
    std::vector<Math::Vector3> positions;
    std::vector<Math::Vector3> velocities;
    for (int i = 0; i < 50; ++i)
    {
        positions.emplace_back(static_cast<float>(i), 2.0f, 0.0f);
        velocities.emplace_back(0.0f, 0.0f, 0.0f);
    }
    ASSERT_EQ(gpu.Spawn(gpuHandle, positions, velocities), 50u);
    ASSERT_EQ(cpu.Spawn(cpuHandle, positions, velocities), 50u);
    for (int step = 0; step < 30; ++step)
    {
        gpu.Step(0.02f);
        cpu.Step(0.02f);
        // The CPU counts age up by the step and the WebGPU backend counts the remaining life down by half steps: a
        // particle on the boundary may go one step apart
        const auto a = static_cast<long>(gpu.GetAliveCount(gpuHandle));
        const auto b = static_cast<long>(cpu.GetAliveCount(cpuHandle));
        EXPECT_LE(std::abs(a - b), 2) << "step " << step;
    }
    EXPECT_EQ(gpu.GetAliveCount(gpuHandle), 0u);
    EXPECT_EQ(cpu.GetAliveCount(cpuHandle), 0u);
}

TEST_F(WebGpuComputeTest, AParticleBackendStopsWhenItsRendererShutsDown)
{
    auto backend = std::make_unique<Particles::WebGpuParticleBackend>(R().GetCompute(), R().GetLifetimeToken());
    const auto handle = backend->CreateSystem(Desc(Physics::ParticleMaterial::Water, 10.0f));
    ASSERT_NE(handle, Physics::kInvalidParticleSystem);
    const std::vector<Math::Vector3> positions(4, Math::Vector3(0.0f, 1.0f, 0.0f));
    const std::vector<Math::Vector3> velocities(4, Math::Vector3(1.0f, 0.0f, 0.0f));
    ASSERT_EQ(backend->Spawn(handle, positions, velocities), 4u);
    backend->Step(0.02f);
    R().Shutdown();
    EXPECT_FALSE(backend->IsHealthy());
    backend->Step(0.02f); // nothing, and no call into the dead renderer
    EXPECT_EQ(backend->Spawn(handle, positions, velocities), 0u);
    std::vector<Math::Vector4> out;
    backend->CopyPositions(handle, out); // the bookkeeping still answers
    EXPECT_EQ(out.size(), 4u);
    backend.reset(); // its destructor must not call the renderer either
}

// ---------------------------------------------------------------- the soft-body backend

TEST_F(WebGpuComputeTest, ASoftCubeFallsAndRestsOnTheGroundAsOnTheCpu)
{
    SoftBodies::WebGpuSoftBodyBackend gpu(R().GetCompute(), R().GetLifetimeToken());
    ASSERT_TRUE(gpu.IsHealthy());
    EXPECT_EQ(gpu.GetName(), "WebGPU");
    Physics::CpuSoftBodyBackend cpu;
    const Physics::SoftBodyDesc desc = CubeDesc(0.5f);
    const auto gpuHandle = gpu.CreateBody(desc);
    const auto cpuHandle = cpu.CreateBody(desc);
    ASSERT_NE(gpuHandle, Physics::kInvalidSoftBody);
    ASSERT_NE(cpuHandle, Physics::kInvalidSoftBody);
    const auto colours = gpu.GetColourCounts(gpuHandle);
    EXPECT_GT(colours[0], 0u);
    EXPECT_GT(colours[1], 0u);

    std::vector<Math::Vector3> surface;
    gpu.CopySurfacePositions(gpuHandle, surface);
    ASSERT_EQ(surface.size(), 8u) << "the rest shape before the first copy lands";
    EXPECT_FLOAT_EQ(surface[0].y, 0.5f);

    for (int step = 0; step < 150; ++step)
    {
        gpu.Step(0.02f);
        cpu.Step(0.02f);
    }
    ASSERT_TRUE(gpu.SyncForTest());
    std::vector<Math::Vector3> gpuNodes;
    std::vector<Math::Vector3> cpuNodes;
    gpu.CopyNodePositions(gpuHandle, gpuNodes);
    cpu.CopyNodePositions(cpuHandle, cpuNodes);
    ASSERT_EQ(gpuNodes.size(), 8u);
    ASSERT_EQ(cpuNodes.size(), 8u);
    for (std::size_t i = 0; i < 8; ++i)
    {
        const Math::Vector3 &g = gpuNodes[i];
        ASSERT_TRUE(std::isfinite(g.x) && std::isfinite(g.y) && std::isfinite(g.z));
        EXPECT_GE(g.y, -1e-4f) << "node " << i << " is above the ground";
        // The same model in another constraint order: close to the CPU's resting shape, not bitwise
        EXPECT_NEAR(g.x, cpuNodes[i].x, 0.05f) << "node " << i;
        EXPECT_NEAR(g.y, cpuNodes[i].y, 0.05f) << "node " << i;
        EXPECT_NEAR(g.z, cpuNodes[i].z, 0.05f) << "node " << i;
    }
    gpu.CopySurfacePositions(gpuHandle, surface);
    ASSERT_EQ(surface.size(), 8u);
    for (std::size_t i = 0; i < 8; ++i)
    {
        EXPECT_FLOAT_EQ(surface[i].y, gpuNodes[i].y) << "each corner rides on its own node";
    }
}

TEST_F(WebGpuComputeTest, AddVelocityPushesTheSoftBodyAndDensityChangesTheMass)
{
    SoftBodies::WebGpuSoftBodyBackend gpu(R().GetCompute(), R().GetLifetimeToken());
    Physics::SoftBodyDesc desc = CubeDesc(0.0f);
    const auto handle = gpu.CreateBody(desc);
    ASSERT_NE(handle, Physics::kInvalidSoftBody);
    for (int step = 0; step < 20; ++step)
    {
        gpu.Step(0.02f);
    }
    ASSERT_TRUE(gpu.SyncForTest());
    std::vector<Math::Vector3> before;
    gpu.CopyNodePositions(handle, before);

    gpu.AddVelocity(handle, Math::Vector3(0.0f, 5.0f, 0.0f));
    desc.params.density = 500.0f; // lighter: the same model, every mass halved
    gpu.SetParams(handle, desc.params);
    gpu.Step(0.02f);
    ASSERT_TRUE(gpu.SyncForTest());
    std::vector<Math::Vector3> after;
    gpu.CopyNodePositions(handle, after);
    ASSERT_EQ(after.size(), before.size());
    float rise = 0.0f;
    for (std::size_t i = 0; i < after.size(); ++i)
    {
        rise += after[i].y - before[i].y;
    }
    EXPECT_GT(rise / static_cast<float>(after.size()), 0.05f) << "pushed up at 5 m/s for one 0.02 s step";
    gpu.Destroy(handle);
    EXPECT_EQ(gpu.GetBodyCount(), 0u);
}

TEST_F(WebGpuComputeTest, TheSoftBodyBackendRefusesWhatTheCpuBackendRefuses)
{
    SoftBodies::WebGpuSoftBodyBackend gpu(R().GetCompute(), R().GetLifetimeToken());
    Physics::SoftBodyDesc empty;
    EXPECT_EQ(gpu.CreateBody(empty), Physics::kInvalidSoftBody) << "no mesh";
    Physics::SoftBodyDesc badTransform = CubeDesc(0.0f);
    badTransform.localToWorld[3] = std::numeric_limits<float>::infinity();
    EXPECT_EQ(gpu.CreateBody(badTransform), Physics::kInvalidSoftBody);
    Physics::SoftBodyDesc badNode = CubeDesc(0.0f);
    auto mesh = std::make_shared<Physics::TetMeshData>(*badNode.tetMesh);
    mesh->tets[0][0] = 99;
    badNode.tetMesh = mesh;
    EXPECT_EQ(gpu.CreateBody(badNode), Physics::kInvalidSoftBody);
    EXPECT_EQ(gpu.GetBodyCount(), 0u);
}

TEST_F(WebGpuComputeTest, ASoftBodyBackendStopsWhenItsRendererShutsDown)
{
    auto gpu = std::make_unique<SoftBodies::WebGpuSoftBodyBackend>(R().GetCompute(), R().GetLifetimeToken());
    const auto handle = gpu->CreateBody(CubeDesc(1.0f));
    ASSERT_NE(handle, Physics::kInvalidSoftBody);
    gpu->Step(0.02f);
    R().Shutdown();
    EXPECT_FALSE(gpu->IsHealthy());
    gpu->Step(0.02f);
    std::vector<Math::Vector3> surface;
    gpu->CopySurfacePositions(handle, surface);
    EXPECT_EQ(surface.size(), 8u) << "the last known shape";
    gpu.reset();
}
