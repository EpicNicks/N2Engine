#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <vector>

#include <math/Vector3.hpp>

#include "SoftBodyTestSupport.hpp"
#include "engine/physics/CpuSoftBodyBackend.hpp"
#include "engine/physics/ISoftBodyBackend.hpp"
#include "engine/physics/TetMeshData.hpp"

// The CPU XPBD soft-body backend on its own (no component, no scene): motion under gravity, the ground plane,
// volume, determinism, AddVelocity and SetParams, the caps and the generation-counted handles.

using namespace N2Engine;
using namespace N2Engine::Physics;
using TestSupport::Centroid;
using TestSupport::MakeBoxDesc;

namespace
{
    constexpr float kDt = 0.02f;

    void StepN(CpuSoftBodyBackend &backend, const int steps, const float dt = kDt)
    {
        for (int i = 0; i < steps; ++i)
        {
            backend.Step(dt);
        }
    }

    Math::Vector3 NodeCentroid(const CpuSoftBodyBackend &backend, const SoftBodyHandle handle)
    {
        std::vector<Math::Vector3> nodes;
        backend.CopyNodePositions(handle, nodes);
        return Centroid(nodes);
    }

    SoftBodyDesc StiffBox(const Math::Vector3 &position, const int resolution = 4)
    {
        SoftBodyDesc desc = MakeBoxDesc(resolution, position);
        desc.params.stiffness = 1.0e6f;
        return desc;
    }

    /// A tetrahedral mesh of `tetCount` copies of one tetrahedron, for the caps
    std::shared_ptr<const TetMeshData> RepeatedTet(const std::size_t tetCount)
    {
        auto mesh = std::make_shared<TetMeshData>();
        mesh->positions = {Math::Vector3(0, 0, 0), Math::Vector3(1, 0, 0), Math::Vector3(0, 1, 0), Math::Vector3(0, 0, 1)};
        mesh->tets.assign(tetCount, std::array<std::uint32_t, 4>{0, 1, 2, 3});
        mesh->surfaceEmbedding.push_back({});
        return mesh;
    }
}

TEST(CpuSoftBodyBackendTest, IsNamedCpuAndTakesItsGenerationFromTheCounterOrTheWrapper)
{
    CpuSoftBodyBackend backend;
    EXPECT_EQ(backend.GetName(), "CPU");
    EXPECT_NE(backend.GetGeneration(), 0u);
    CpuSoftBodyBackend other;
    EXPECT_NE(backend.GetGeneration(), other.GetGeneration());

    CpuSoftBodyBackend wrapped(77u);
    EXPECT_EQ(wrapped.GetGeneration(), 77u);
    const SoftBodyHandle handle = wrapped.CreateBody(MakeBoxDesc(3));
    ASSERT_NE(handle, kInvalidSoftBody);
    EXPECT_EQ(SoftBodyHandleGeneration(handle), 77u);
    EXPECT_TRUE(wrapped.OwnsHandle(handle));
}

TEST(CpuSoftBodyBackendTest, ABodyStartsAtItsRestShapeAtTheGivenPose)
{
    CpuSoftBodyBackend backend;
    const Renderer::Common::MeshData box = TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f);
    const SoftBodyHandle handle = backend.CreateBody(MakeBoxDesc(4, Math::Vector3(1.0f, 2.0f, 3.0f)));
    ASSERT_NE(handle, kInvalidSoftBody);
    EXPECT_EQ(backend.GetBodyCount(), 1u);
    EXPECT_EQ(backend.GetTetCount(handle), 5u * 64);
    EXPECT_NEAR(backend.GetRestVolume(handle), 1.0f, 1e-4f);
    EXPECT_NEAR(backend.GetVolume(handle), 1.0f, 1e-4f);

    std::vector<Math::Vector3> surface;
    backend.CopySurfacePositions(handle, surface);
    ASSERT_EQ(surface.size(), box.vertices.size());
    for (std::size_t i = 0; i < surface.size(); ++i)
    {
        EXPECT_NEAR(surface[i].x, box.vertices[i].position[0] + 1.0f, 1e-4f);
        EXPECT_NEAR(surface[i].y, box.vertices[i].position[1] + 2.0f, 1e-4f);
        EXPECT_NEAR(surface[i].z, box.vertices[i].position[2] + 3.0f, 1e-4f);
    }
}

TEST(CpuSoftBodyBackendTest, TheTransformScalesAndAMirroredOneKeepsEveryVolumePositive)
{
    CpuSoftBodyBackend backend;
    SoftBodyDesc scaled = MakeBoxDesc(3);
    scaled.localToWorld[0] = scaled.localToWorld[5] = scaled.localToWorld[10] = 2.0f;
    const SoftBodyHandle big = backend.CreateBody(scaled);
    ASSERT_NE(big, kInvalidSoftBody);
    EXPECT_NEAR(backend.GetRestVolume(big), 8.0f, 1e-3f);

    SoftBodyDesc mirrored = MakeBoxDesc(3);
    mirrored.localToWorld[0] = -1.0f; // x flipped: every tetrahedron turns inside out
    const SoftBodyHandle flipped = backend.CreateBody(mirrored);
    ASSERT_NE(flipped, kInvalidSoftBody);
    EXPECT_NEAR(backend.GetRestVolume(flipped), 1.0f, 1e-4f);
    EXPECT_NEAR(backend.GetVolume(flipped), 1.0f, 1e-4f) << "the corners were reordered, so the volume is positive";
    // And it still behaves: it keeps its volume while falling
    StepN(backend, 20);
    EXPECT_NEAR(backend.GetVolume(flipped), 1.0f, 0.05f);
}

TEST(CpuSoftBodyBackendTest, MirroredTransformsStillSkinTheRenderVerticesAtTheirTransformedRestPositions)
{
    const float scales[2][3] = {{-1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, -2.0f}};
    for (const auto &scale : scales)
    {
        CpuSoftBodyBackend backend;
        SoftBodyDesc desc = MakeBoxDesc(3);
        desc.localToWorld[0] = scale[0];
        desc.localToWorld[5] = scale[1];
        desc.localToWorld[10] = scale[2];
        desc.localToWorld[3] = 1.0f;
        desc.localToWorld[7] = 2.0f;
        desc.localToWorld[11] = 3.0f;
        const SoftBodyHandle handle = backend.CreateBody(desc);
        ASSERT_NE(handle, kInvalidSoftBody);

        std::vector<Math::Vector3> surface;
        backend.CopySurfacePositions(handle, surface);
        const TetMeshData &mesh = *desc.tetMesh;
        ASSERT_EQ(surface.size(), mesh.surfaceEmbedding.size());
        ASSERT_FALSE(surface.empty());
        for (std::size_t i = 0; i < surface.size(); ++i)
        {
            const TetEmbedding &e = mesh.surfaceEmbedding[i];
            Math::Vector3 local(0.0f, 0.0f, 0.0f);
            for (int c = 0; c < 4; ++c)
            {
                const Math::Vector3 &p = mesh.positions[mesh.tets[e.tet][c]];
                local.x += e.weights[c] * p.x;
                local.y += e.weights[c] * p.y;
                local.z += e.weights[c] * p.z;
            }
            EXPECT_NEAR(surface[i].x, scale[0] * local.x + 1.0f, 1e-4f) << "vertex " << i;
            EXPECT_NEAR(surface[i].y, scale[1] * local.y + 2.0f, 1e-4f) << "vertex " << i;
            EXPECT_NEAR(surface[i].z, scale[2] * local.z + 3.0f, 1e-4f) << "vertex " << i;
        }
    }
}

TEST(CpuSoftBodyBackendTest, FallsUnderGravity)
{
    CpuSoftBodyBackend backend;
    const SoftBodyHandle handle = backend.CreateBody(StiffBox(Math::Vector3(0.0f, 5.0f, 0.0f)));
    ASSERT_NE(handle, kInvalidSoftBody);
    const Math::Vector3 before = NodeCentroid(backend, handle);

    StepN(backend, 25); // half a second: about 1.2 m
    const Math::Vector3 after = NodeCentroid(backend, handle);
    EXPECT_GT(before.y - after.y, 1.0f);
    EXPECT_LT(before.y - after.y, 1.5f);
    EXPECT_NEAR(after.x, before.x, 1e-3f);
    EXPECT_NEAR(after.z, before.z, 1e-3f);

    // It falls as one body, keeping its shape and volume
    EXPECT_NEAR(backend.GetVolume(handle), 1.0f, 0.02f);
}

TEST(CpuSoftBodyBackendTest, ZeroGravityLeavesTheBodyWhereItIs)
{
    CpuSoftBodyBackend backend;
    SoftBodyDesc desc = StiffBox(Math::Vector3(0.0f, 1.0f, 0.0f));
    desc.params.gravity = Math::Vector3(0.0f, 0.0f, 0.0f);
    const SoftBodyHandle handle = backend.CreateBody(desc);
    const Math::Vector3 before = NodeCentroid(backend, handle);
    StepN(backend, 30);
    const Math::Vector3 after = NodeCentroid(backend, handle);
    EXPECT_NEAR(after.x, before.x, 1e-5f);
    EXPECT_NEAR(after.y, before.y, 1e-5f);
    EXPECT_NEAR(after.z, before.z, 1e-5f);
}

TEST(CpuSoftBodyBackendTest, ComesToRestOnTheGroundWithoutSinkingBelowIt)
{
    CpuSoftBodyBackend backend;
    SoftBodyDesc desc = StiffBox(Math::Vector3(0.0f, 0.6f, 0.0f)); // its bottom is 0.1 above the ground
    desc.params.groundEnabled = true;
    desc.params.groundHeight = 0.0f;
    const SoftBodyHandle handle = backend.CreateBody(desc);
    ASSERT_NE(handle, kInvalidSoftBody);

    float lowest = std::numeric_limits<float>::max();
    std::vector<Math::Vector3> nodes;
    for (int step = 0; step < 400; ++step)
    {
        backend.Step(kDt);
        backend.CopyNodePositions(handle, nodes);
        for (const auto &node : nodes)
        {
            lowest = std::min(lowest, node.y);
        }
    }
    EXPECT_GE(lowest, -1e-5f) << "no node ever went below the ground plane";

    // At rest on it: the bottom nodes on the plane, the body about as high as it started, hardly moving
    backend.CopyNodePositions(handle, nodes);
    float bottom = std::numeric_limits<float>::max();
    for (const auto &node : nodes)
    {
        bottom = std::min(bottom, node.y);
    }
    EXPECT_NEAR(bottom, 0.0f, 1e-3f);
    const Math::Vector3 centre = Centroid(nodes);
    EXPECT_GT(centre.y, 0.3f);
    EXPECT_LT(centre.y, 0.55f);
    EXPECT_LT(std::abs(centre.x), 0.05f);
    EXPECT_LT(std::abs(centre.z), 0.05f);

    std::vector<Math::Vector3> velocities;
    backend.CopyNodeVelocities(handle, velocities);
    const Math::Vector3 meanVelocity = Centroid(velocities);
    EXPECT_LT(std::abs(meanVelocity.y), 0.2f) << "it has stopped";

    // Squashed a little under its own weight, but it keeps its volume within a tolerance
    EXPECT_NEAR(backend.GetVolume(handle), backend.GetRestVolume(handle), 0.2f * backend.GetRestVolume(handle));

    // The render vertices (embedded in the tetrahedra) are above the plane too
    std::vector<Math::Vector3> surface;
    backend.CopySurfacePositions(handle, surface);
    for (const auto &p : surface)
    {
        EXPECT_GE(p.y, -1e-4f);
    }
}

TEST(CpuSoftBodyBackendTest, FrictionSlowsASlidingBody)
{
    const auto slide = [](const float friction)
    {
        CpuSoftBodyBackend backend;
        SoftBodyDesc desc = StiffBox(Math::Vector3(0.0f, 0.5f, 0.0f));
        desc.params.groundEnabled = true;
        desc.params.friction = friction;
        const SoftBodyHandle handle = backend.CreateBody(desc);
        StepN(backend, 10); // settle
        backend.AddVelocity(handle, Math::Vector3(2.0f, 0.0f, 0.0f));
        StepN(backend, 50);
        return NodeCentroid(backend, handle).x;
    };
    const float slippery = slide(0.0f);
    const float grippy = slide(1.0f);
    EXPECT_GT(slippery, grippy + 0.3f) << "slippery " << slippery << ", grippy " << grippy;
    EXPECT_GT(slippery, 1.0f);
}

TEST(CpuSoftBodyBackendTest, TheSameBodiesSteppedTheSameWayGiveBitwiseTheSamePositions)
{
    const auto run = [](std::vector<Math::Vector3> &nodes, std::vector<Math::Vector3> &surface)
    {
        CpuSoftBodyBackend backend;
        SoftBodyDesc desc = StiffBox(Math::Vector3(0.0f, 0.4f, 0.0f), 5);
        desc.params.groundEnabled = true;
        const SoftBodyHandle a = backend.CreateBody(desc);
        const SoftBodyHandle b = backend.CreateBody(StiffBox(Math::Vector3(3.0f, 2.0f, 0.0f), 3));
        for (int step = 0; step < 60; ++step)
        {
            if (step == 20)
            {
                backend.AddVelocity(a, Math::Vector3(1.0f, 0.5f, -0.25f));
            }
            backend.Step(kDt);
        }
        backend.CopyNodePositions(a, nodes);
        backend.CopySurfacePositions(b, surface);
    };
    std::vector<Math::Vector3> nodesA, surfaceA, nodesB, surfaceB;
    run(nodesA, surfaceA);
    run(nodesB, surfaceB);
    ASSERT_FALSE(nodesA.empty());
    ASSERT_EQ(nodesA.size(), nodesB.size());
    ASSERT_EQ(surfaceA.size(), surfaceB.size());
    for (std::size_t i = 0; i < nodesA.size(); ++i)
    {
        EXPECT_EQ(nodesA[i].x, nodesB[i].x);
        EXPECT_EQ(nodesA[i].y, nodesB[i].y);
        EXPECT_EQ(nodesA[i].z, nodesB[i].z);
    }
    for (std::size_t i = 0; i < surfaceA.size(); ++i)
    {
        EXPECT_EQ(surfaceA[i].x, surfaceB[i].x);
        EXPECT_EQ(surfaceA[i].y, surfaceB[i].y);
        EXPECT_EQ(surfaceA[i].z, surfaceB[i].z);
    }
}

TEST(CpuSoftBodyBackendTest, AddVelocityMovesTheWholeBody)
{
    CpuSoftBodyBackend backend;
    SoftBodyDesc desc = StiffBox(Math::Vector3(0.0f, 0.0f, 0.0f));
    desc.params.gravity = Math::Vector3(0.0f, 0.0f, 0.0f);
    const SoftBodyHandle handle = backend.CreateBody(desc);
    backend.AddVelocity(handle, Math::Vector3(1.0f, 0.0f, 0.0f));
    StepN(backend, 10); // 0.2 s at 1 m/s
    const Math::Vector3 centre = NodeCentroid(backend, handle);
    EXPECT_GT(centre.x, 0.17f);
    EXPECT_LT(centre.x, 0.21f);
    EXPECT_NEAR(centre.y, 0.0f, 1e-3f);

    // It adds: another push on top
    backend.AddVelocity(handle, Math::Vector3(1.0f, 0.0f, 0.0f));
    const float x = centre.x;
    StepN(backend, 10);
    EXPECT_GT(NodeCentroid(backend, handle).x - x, 0.35f);

    // Non-finite velocities and unknown handles are ignored
    backend.AddVelocity(handle, Math::Vector3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f));
    backend.AddVelocity(kInvalidSoftBody, Math::Vector3(1.0f, 0.0f, 0.0f));
    StepN(backend, 2);
    EXPECT_TRUE(std::isfinite(NodeCentroid(backend, handle).x));
}

TEST(CpuSoftBodyBackendTest, SetParamsTakesEffectAtOnce)
{
    CpuSoftBodyBackend backend;
    SoftBodyDesc desc = StiffBox(Math::Vector3(0.0f, 5.0f, 0.0f));
    const SoftBodyHandle handle = backend.CreateBody(desc);
    const float start = NodeCentroid(backend, handle).y;
    StepN(backend, 5);
    EXPECT_LT(NodeCentroid(backend, handle).y, start);

    // Gravity reversed: the body decelerates and rises above where it started
    SoftBodyParams up = desc.params;
    up.gravity = Math::Vector3(0.0f, 9.81f, 0.0f);
    backend.SetParams(handle, up);
    StepN(backend, 25);
    EXPECT_GT(NodeCentroid(backend, handle).y, start);

    // The ground is a parameter too
    SoftBodyParams ground = up;
    ground.gravity = Math::Vector3(0.0f, -9.81f, 0.0f);
    ground.groundEnabled = true;
    ground.groundHeight = 10.0f; // above the body: it is pushed up onto the plane at once
    backend.SetParams(handle, ground);
    StepN(backend, 1);
    std::vector<Math::Vector3> nodes;
    backend.CopyNodePositions(handle, nodes);
    for (const auto &node : nodes)
    {
        EXPECT_GE(node.y, 10.0f - 1e-4f);
    }

    // Unknown handles are ignored
    backend.SetParams(kInvalidSoftBody, ground);
}

TEST(CpuSoftBodyBackendTest, ADensityChangeChangesTheMassAndNotTheFreeFall)
{
    // Free fall doesn't depend on mass; what does is how hard a pushed body moves under the same constraint
    // forces. Here: it just must keep working after the change.
    CpuSoftBodyBackend backend;
    const SoftBodyHandle handle = backend.CreateBody(StiffBox(Math::Vector3(0.0f, 5.0f, 0.0f)));
    SoftBodyParams params = StiffBox({}).params;
    params.density = 250.0f;
    backend.SetParams(handle, params);
    StepN(backend, 10);
    EXPECT_LT(NodeCentroid(backend, handle).y, 5.0f);
    EXPECT_NEAR(backend.GetVolume(handle), 1.0f, 0.02f);
}

TEST(CpuSoftBodyBackendTest, NonFiniteParametersAreReplacedNotPassedOn)
{
    CpuSoftBodyBackend backend;
    SoftBodyDesc desc = StiffBox(Math::Vector3(0.0f, 1.0f, 0.0f));
    const SoftBodyHandle handle = backend.CreateBody(desc);
    SoftBodyParams bad = desc.params;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    bad.gravity = Math::Vector3(nan, nan, nan);
    bad.density = nan;
    bad.stiffness = std::numeric_limits<float>::infinity();
    bad.poisson = nan;
    bad.damping = nan;
    bad.groundHeight = nan;
    bad.solverIterations = 0;
    backend.SetParams(handle, bad);
    StepN(backend, 20);
    std::vector<Math::Vector3> nodes;
    backend.CopyNodePositions(handle, nodes);
    ASSERT_FALSE(nodes.empty());
    for (const auto &node : nodes)
    {
        EXPECT_TRUE(std::isfinite(node.x) && std::isfinite(node.y) && std::isfinite(node.z));
    }
    // A step with a bad dt does nothing
    const Math::Vector3 before = NodeCentroid(backend, handle);
    backend.Step(0.0f);
    backend.Step(-1.0f);
    backend.Step(nan);
    const Math::Vector3 after = NodeCentroid(backend, handle);
    EXPECT_EQ(before.x, after.x);
    EXPECT_EQ(before.y, after.y);
    EXPECT_EQ(before.z, after.z);
}

TEST(CpuSoftBodyBackendTest, AnExtremeBodyIsKeptFiniteByTheSolver)
{
    // A huge stiffness, a tiny density and a large step: the solver must not leave NaNs for the renderer
    CpuSoftBodyBackend backend;
    SoftBodyDesc desc = MakeBoxDesc(4, Math::Vector3(0.0f, 3.0f, 0.0f));
    desc.params.stiffness = 1.0e11f;
    desc.params.density = 0.001f;
    desc.params.solverIterations = 32;
    desc.params.groundEnabled = true;
    const SoftBodyHandle handle = backend.CreateBody(desc);
    StepN(backend, 30, 0.1f);
    std::vector<Math::Vector3> surface;
    backend.CopySurfacePositions(handle, surface);
    for (const auto &p : surface)
    {
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    }
}

// ---------------------------------------------------------------- caps and refusals

TEST(CpuSoftBodyBackendTest, ABodyOverTheTetrahedronCapIsRefusedAndOneAtTheCapIsAccepted)
{
    CpuSoftBodyBackend backend;
    SoftBodyDesc desc;
    desc.tetMesh = RepeatedTet(kMaxTetsPerBody + 1);
    EXPECT_EQ(backend.CreateBody(desc), kInvalidSoftBody);
    EXPECT_EQ(backend.GetBodyCount(), 0u);

    desc.tetMesh = RepeatedTet(kMaxTetsPerBody);
    const SoftBodyHandle handle = backend.CreateBody(desc);
    ASSERT_NE(handle, kInvalidSoftBody);
    EXPECT_EQ(backend.GetTetCount(handle), kMaxTetsPerBody);
}

TEST(CpuSoftBodyBackendTest, AMissingOrMalformedMeshIsRefused)
{
    CpuSoftBodyBackend backend;
    SoftBodyDesc none;
    EXPECT_EQ(backend.CreateBody(none), kInvalidSoftBody) << "no tetrahedral mesh";

    SoftBodyDesc empty;
    empty.tetMesh = std::make_shared<TetMeshData>();
    EXPECT_EQ(backend.CreateBody(empty), kInvalidSoftBody);

    auto badNode = std::make_shared<TetMeshData>(*RepeatedTet(2));
    badNode->tets[1] = {0, 1, 2, 9};
    SoftBodyDesc nodePastTheEnd;
    nodePastTheEnd.tetMesh = badNode;
    EXPECT_EQ(backend.CreateBody(nodePastTheEnd), kInvalidSoftBody);

    auto badEmbedding = std::make_shared<TetMeshData>(*RepeatedTet(2));
    badEmbedding->surfaceEmbedding[0].tet = 5;
    SoftBodyDesc embeddedPastTheEnd;
    embeddedPastTheEnd.tetMesh = badEmbedding;
    EXPECT_EQ(backend.CreateBody(embeddedPastTheEnd), kInvalidSoftBody);

    SoftBodyDesc notFinite = MakeBoxDesc(3);
    notFinite.localToWorld[3] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(backend.CreateBody(notFinite), kInvalidSoftBody);

    EXPECT_EQ(backend.GetBodyCount(), 0u) << "a refused body leaves nothing behind";
}

// ---------------------------------------------------------------- handles and generations

TEST(CpuSoftBodyBackendTest, UnknownHandlesAreIgnored)
{
    CpuSoftBodyBackend backend;
    const SoftBodyHandle handle = backend.CreateBody(MakeBoxDesc(3));
    ASSERT_NE(handle, kInvalidSoftBody);

    std::vector<Math::Vector3> out(5);
    backend.CopySurfacePositions(kInvalidSoftBody, out);
    EXPECT_TRUE(out.empty());
    out.assign(5, {});
    backend.CopySurfacePositions(MakeSoftBodyHandle(backend.GetGeneration(), 999u), out);
    EXPECT_TRUE(out.empty()) << "right generation, no such body";
    backend.Destroy(kInvalidSoftBody);
    backend.Destroy(MakeSoftBodyHandle(backend.GetGeneration(), 999u));
    EXPECT_EQ(backend.GetBodyCount(), 1u);
    EXPECT_EQ(backend.GetVolume(kInvalidSoftBody), 0.0f);
    EXPECT_EQ(backend.GetTetCount(kInvalidSoftBody), 0u);
}

TEST(CpuSoftBodyBackendTest, ADestroyedBodyIsGoneAndDestroyingTwiceIsHarmless)
{
    CpuSoftBodyBackend backend;
    const SoftBodyHandle first = backend.CreateBody(MakeBoxDesc(3));
    const SoftBodyHandle second = backend.CreateBody(MakeBoxDesc(3));
    ASSERT_NE(first, second);
    backend.Destroy(first);
    EXPECT_EQ(backend.GetBodyCount(), 1u);
    std::vector<Math::Vector3> out;
    backend.CopySurfacePositions(first, out);
    EXPECT_TRUE(out.empty());
    backend.CopySurfacePositions(second, out);
    EXPECT_FALSE(out.empty());
    backend.Destroy(first);
    EXPECT_EQ(backend.GetBodyCount(), 1u);
    // A handle is never reused by a later body
    const SoftBodyHandle third = backend.CreateBody(MakeBoxDesc(3));
    EXPECT_NE(third, first);
}

TEST(CpuSoftBodyBackendTest, AHandleOfAnotherBackendIsIgnored)
{
    CpuSoftBodyBackend a;
    CpuSoftBodyBackend b;
    const SoftBodyHandle fromA = a.CreateBody(MakeBoxDesc(3));
    const SoftBodyHandle fromB = b.CreateBody(MakeBoxDesc(3));
    EXPECT_EQ(SoftBodyHandleIndex(fromA), SoftBodyHandleIndex(fromB)) << "the same index, as the test needs";
    EXPECT_NE(fromA, fromB);
    EXPECT_FALSE(b.OwnsHandle(fromA));

    std::vector<Math::Vector3> out;
    b.CopySurfacePositions(fromA, out);
    EXPECT_TRUE(out.empty());
    b.AddVelocity(fromA, Math::Vector3(5.0f, 0.0f, 0.0f));
    b.SetParams(fromA, SoftBodyParams{});
    b.Destroy(fromA);
    EXPECT_EQ(a.GetBodyCount(), 1u) << "B never touched A's body";
    EXPECT_EQ(b.GetBodyCount(), 1u);
    a.CopySurfacePositions(fromA, out);
    EXPECT_FALSE(out.empty());
}

TEST(CpuSoftBodyBackendTest, ABackendRecreatedAtTheSameAddressIgnoresTheOldBackendsHandles)
{
    alignas(CpuSoftBodyBackend) unsigned char storage[sizeof(CpuSoftBodyBackend)];
    auto *backend = new (storage) CpuSoftBodyBackend();
    const SoftBodyHandle old = backend->CreateBody(MakeBoxDesc(3));
    ASSERT_NE(old, kInvalidSoftBody);
    const std::uint32_t generation = backend->GetGeneration();

    // The very same address now holds a different backend (an allocator can do this when a backend is replaced)
    backend->~CpuSoftBodyBackend();
    backend = new (storage) CpuSoftBodyBackend();
    EXPECT_NE(backend->GetGeneration(), generation);

    const SoftBodyHandle fresh = backend->CreateBody(MakeBoxDesc(3));
    EXPECT_EQ(SoftBodyHandleIndex(fresh), SoftBodyHandleIndex(old)) << "the new backend's first body has the old index";
    EXPECT_NE(fresh, old);

    std::vector<Math::Vector3> out;
    backend->CopySurfacePositions(old, out);
    EXPECT_TRUE(out.empty()) << "the old handle does not name the new backend's body";
    backend->Destroy(old);
    EXPECT_EQ(backend->GetBodyCount(), 1u);
    backend->CopySurfacePositions(fresh, out);
    EXPECT_FALSE(out.empty());

    backend->~CpuSoftBodyBackend();
}

TEST(CpuSoftBodyBackendTest, ManyBodiesStepIndependently)
{
    CpuSoftBodyBackend backend;
    SoftBodyDesc falling = StiffBox(Math::Vector3(0.0f, 5.0f, 0.0f));
    SoftBodyDesc floating = StiffBox(Math::Vector3(4.0f, 5.0f, 0.0f));
    floating.params.gravity = Math::Vector3(0.0f, 0.0f, 0.0f);
    const SoftBodyHandle a = backend.CreateBody(falling);
    const SoftBodyHandle b = backend.CreateBody(floating);
    StepN(backend, 20);
    EXPECT_LT(NodeCentroid(backend, a).y, 4.5f);
    EXPECT_NEAR(NodeCentroid(backend, b).y, 5.0f, 1e-4f);
    backend.Destroy(a);
    StepN(backend, 5);
    EXPECT_NEAR(NodeCentroid(backend, b).y, 5.0f, 1e-4f);
}
