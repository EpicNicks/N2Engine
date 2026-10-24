#include <gtest/gtest.h>

#ifdef N2ENGINE_PHYSX_ENABLED

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <PxPhysicsAPI.h>
#include <extensions/PxDefaultAllocator.h>
#include <extensions/PxDefaultErrorCallback.h>

#include <math/Vector3.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "../softbody/SoftBodyTestSupport.hpp"
#include "engine/physics/TetMeshData.hpp"
#include "engine/physics/physx/DeformableCooking.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/softbody/DeformableDriver.hpp"
#include "engine/softbody/TetSkinning.hpp"
#include "engine/softbody/Tetrahedraliser.hpp"

// Cooking the engine's own tetrahedra for a PhysX deformable volume (CookDeformableMesh). Cooking is CPU code, so
// this runs on every leg that links PhysX (Windows PhysX ON, Linux x86_64, Linux arm64), without a GPU: a box, a
// sphere and a seamed cube from the tetrahedraliser, under identity, non-uniform and mirrored transforms, must cook
// with the GPU data, keep the node mapping and keep every tetrahedron positive.

using namespace N2Engine;
using namespace N2Engine::Physics;
using Rendering::Mesh;

namespace
{
    /// The PhysX foundation cooking needs: made here unless one already exists (another test's physics backend)
    class DeformableCookingTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if (!PxIsFoundationValid())
            {
                _foundation = PxCreateFoundation(PX_PHYSICS_VERSION, _allocator, _errorCallback);
            }
            ASSERT_TRUE(PxIsFoundationValid() != nullptr) << "no PhysX foundation to cook with";
        }

        void TearDown() override
        {
            if (_foundation)
            {
                _foundation->release();
                _foundation = nullptr;
            }
        }

    private:
        physx::PxDefaultAllocator _allocator;
        physx::PxDefaultErrorCallback _errorCallback;
        physx::PxFoundation *_foundation = nullptr;
    };

    struct Source
    {
        const char *name;
        Renderer::Common::MeshData data;
        int resolution;
    };

    struct Transform
    {
        const char *name;
        float scale[3];
        bool mirrored;
    };

    std::vector<Source> Sources()
    {
        return {{"box", TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f), 4},
                {"sphere", Mesh::MakeSphere(), 6},
                {"seamed cube", Mesh::MakeCube(), 5}};
    }

    const Transform kTransforms[] = {{"identity", {1.0f, 1.0f, 1.0f}, false},
                                     {"non-uniform", {2.0f, 0.5f, 1.0f}, false},
                                     {"mirrored x", {-1.0f, 1.0f, 1.0f}, true},
                                     {"mirrored y", {1.0f, -1.0f, 1.0f}, true},
                                     {"mirrored xyz", {-1.0f, -1.0f, -1.0f}, true},
                                     {"mirrored non-uniform", {-2.0f, 1.0f, 0.5f}, true},
                                     {"two negative axes", {-1.0f, 1.0f, -2.0f}, false}};

    void MakeMatrix(const Transform &transform, float (&m)[12])
    {
        const float values[12] = {transform.scale[0], 0.0f, 0.0f, 1.0f, 0.0f, transform.scale[1],
                                  0.0f, 2.0f, 0.0f, 0.0f, transform.scale[2], 3.0f};
        std::copy(std::begin(values), std::end(values), m);
    }

    struct Prepared
    {
        std::shared_ptr<const TetMeshData> mesh;
        std::vector<Math::Vector3> rest;
        std::vector<std::array<std::uint32_t, 4>> tets;
        std::vector<std::uint8_t> flipped;
    };

    Prepared Prepare(const Source &source, const Transform &transform)
    {
        Prepared prepared;
        SoftBodies::TetSettings settings;
        settings.resolution = source.resolution;
        prepared.mesh = SoftBodies::Tetrahedralise(source.data.vertices, source.data.indices, settings).mesh;
        float m[12];
        MakeMatrix(transform, m);
        if (prepared.mesh)
        {
            SoftBodies::OrientTets(*prepared.mesh, m, prepared.rest, prepared.tets, prepared.flipped);
        }
        return prepared;
    }
}

TEST_F(DeformableCookingTest, BoxSphereAndSeamedCubeCookWithGpuDataUnderEveryTransform)
{
    for (const Source &source : Sources())
    {
        for (const Transform &transform : kTransforms)
        {
            SCOPED_TRACE(std::string(source.name) + ", " + transform.name);
            const Prepared prepared = Prepare(source, transform);
            ASSERT_NE(prepared.mesh, nullptr);

            const CookedDeformableMesh cooked = CookDeformableMesh(prepared.rest, prepared.tets);
            ASSERT_TRUE(cooked.Ok()) << cooked.error;
            EXPECT_TRUE(cooked.error.empty());
            EXPECT_EQ(cooked.GetCollisionVertexCount(), prepared.rest.size());
            EXPECT_EQ(cooked.GetSimulationVertexCount(), prepared.rest.size());
            EXPECT_EQ(cooked.GetCollisionTetCount(), prepared.tets.size());

            // Mirrored transforms were turned positive by OrientTets, and cooking kept every tetrahedron positive
            if (transform.mirrored)
            {
                EXPECT_TRUE(std::ranges::any_of(prepared.flipped, [](const std::uint8_t f) { return f != 0; }))
                    << "a mirrored transform turns tetrahedra inside out";
            }
            EXPECT_GT(cooked.GetMinTetVolume6(), 0.0f) << "every cooked tetrahedron has a positive volume";
        }
    }
}

TEST_F(DeformableCookingTest, TheNodeMapRoundTripsTheInputVertices)
{
    for (const Source &source : Sources())
    {
        for (const Transform &transform : kTransforms)
        {
            SCOPED_TRACE(std::string(source.name) + ", " + transform.name);
            const Prepared prepared = Prepare(source, transform);
            ASSERT_NE(prepared.mesh, nullptr);
            const CookedDeformableMesh cooked = CookDeformableMesh(prepared.rest, prepared.tets);
            ASSERT_TRUE(cooked.Ok()) << cooked.error;

            // A permutation of the cooked vertices: every input node has its own vertex
            ASSERT_EQ(cooked.nodeToVertex.size(), prepared.rest.size());
            std::set<std::uint32_t> seen;
            for (const std::uint32_t vertex : cooked.nodeToVertex)
            {
                ASSERT_LT(vertex, cooked.GetCollisionVertexCount());
                seen.insert(vertex);
            }
            EXPECT_EQ(seen.size(), prepared.rest.size());

            // And it reads each node back: the cooked vertex of node i is node i
            std::vector<Math::Vector3> vertices;
            cooked.CopyCollisionVertices(vertices);
            ASSERT_EQ(vertices.size(), prepared.rest.size());
            for (std::size_t i = 0; i < prepared.rest.size(); ++i)
            {
                const Math::Vector3 &got = vertices[cooked.nodeToVertex[i]];
                EXPECT_EQ(got.x, prepared.rest[i].x) << "node " << i;
                EXPECT_EQ(got.y, prepared.rest[i].y) << "node " << i;
                EXPECT_EQ(got.z, prepared.rest[i].z) << "node " << i;
            }
        }
    }
}

TEST_F(DeformableCookingTest, TheEmbeddingStillSkinsTheRenderVerticesThroughTheNodeMap)
{
    // The point of cooking the engine's own tetrahedra: the render vertices, embedded in them once, are skinned from
    // the cooked vertices read through the node map, and land at their transformed rest positions
    for (const Source &source : Sources())
    {
        for (const Transform &transform : kTransforms)
        {
            SCOPED_TRACE(std::string(source.name) + ", " + transform.name);
            const Prepared prepared = Prepare(source, transform);
            ASSERT_NE(prepared.mesh, nullptr);
            const CookedDeformableMesh cooked = CookDeformableMesh(prepared.rest, prepared.tets);
            ASSERT_TRUE(cooked.Ok()) << cooked.error;

            std::vector<Math::Vector3> vertices;
            cooked.CopyCollisionVertices(vertices);
            std::vector<SoftBodies::DeformableVec4> nodes(prepared.rest.size());
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                const Math::Vector3 &v = vertices[cooked.nodeToVertex[i]];
                nodes[i] = {v.x, v.y, v.z, 0.0f};
            }
            std::vector<Math::Vector3> surface;
            SoftBodies::SkinSurface<SoftBodies::DeformableVec4>(*prepared.mesh, prepared.tets, prepared.flipped, nodes,
                                                                surface);
            ASSERT_EQ(surface.size(), source.data.vertices.size());

            float m[12];
            MakeMatrix(transform, m);
            for (std::size_t i = 0; i < surface.size(); ++i)
            {
                const auto &p = source.data.vertices[i].position;
                const Math::Vector3 expected = SoftBodies::TransformPoint(m, Math::Vector3(p[0], p[1], p[2]));
                EXPECT_NEAR(surface[i].x, expected.x, 2e-3f) << "vertex " << i;
                EXPECT_NEAR(surface[i].y, expected.y, 2e-3f) << "vertex " << i;
                EXPECT_NEAR(surface[i].z, expected.z, 2e-3f) << "vertex " << i;
            }
        }
    }
}

TEST_F(DeformableCookingTest, RefusesMeshesPhysXCouldNotUse)
{
    const Prepared good = Prepare(Sources().front(), kTransforms[0]);
    ASSERT_NE(good.mesh, nullptr);

    const CookedDeformableMesh none = CookDeformableMesh({}, {});
    EXPECT_FALSE(none.Ok());
    EXPECT_FALSE(none.error.empty());

    std::vector<std::array<std::uint32_t, 4>> badIndex = good.tets;
    badIndex[0][1] = static_cast<std::uint32_t>(good.rest.size());
    const CookedDeformableMesh outOfRange = CookDeformableMesh(good.rest, badIndex);
    EXPECT_FALSE(outOfRange.Ok());
    EXPECT_FALSE(outOfRange.error.empty());

    std::vector<Math::Vector3> nan = good.rest;
    nan[0].x = std::numeric_limits<float>::quiet_NaN();
    const CookedDeformableMesh notFinite = CookDeformableMesh(nan, good.tets);
    EXPECT_FALSE(notFinite.Ok());
    EXPECT_FALSE(notFinite.error.empty());

    const CookedDeformableMesh noTets = CookDeformableMesh(good.rest, {});
    EXPECT_FALSE(noTets.Ok());
    EXPECT_FALSE(noTets.nodeToVertex.size() > 0);
}

TEST_F(DeformableCookingTest, ACookedMeshReleasesCleanlyAndCooksAgain)
{
    const Prepared prepared = Prepare(Sources().front(), kTransforms[0]);
    ASSERT_NE(prepared.mesh, nullptr);
    for (int i = 0; i < 3; ++i)
    {
        CookedDeformableMesh cooked = CookDeformableMesh(prepared.rest, prepared.tets);
        ASSERT_TRUE(cooked.Ok()) << cooked.error;
        cooked.mesh.reset();
        EXPECT_FALSE(cooked.Ok());
        EXPECT_EQ(cooked.GetCollisionVertexCount(), 0u);
    }
}

#endif
