#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <renderer/common/RenderTypes.hpp>

#include "SoftBodyTestSupport.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/softbody/Tetrahedraliser.hpp"

// The voxel tetrahedraliser and its cache: pure engine code, no PhysX, so these run on every build.

using namespace N2Engine;
using namespace N2Engine::SoftBodies;
using Physics::TetMeshData;
using Rendering::Mesh;

namespace
{
    TetResult Run(const Renderer::Common::MeshData &data, const int resolution)
    {
        TetSettings settings;
        settings.resolution = resolution;
        return Tetrahedralise(data.vertices, data.indices, settings);
    }

    /// Checks the properties every tetrahedral body must have: positive volumes, valid indices, one embedding per
    /// source vertex with weights that sum to 1, all inside the tetrahedron, and that reproduce the vertex
    void ExpectWellFormed(const TetMeshData &mesh, const Renderer::Common::MeshData &source)
    {
        ASSERT_FALSE(mesh.tets.empty());
        for (std::size_t t = 0; t < mesh.tets.size(); ++t)
        {
            for (const std::uint32_t node : mesh.tets[t])
            {
                ASSERT_LT(node, mesh.positions.size());
            }
            EXPECT_GT(TetVolume6(mesh, t), 0.0) << "tetrahedron " << t << " is inside out or flat";
        }
        ASSERT_EQ(mesh.surfaceEmbedding.size(), source.vertices.size());
        for (std::size_t i = 0; i < source.vertices.size(); ++i)
        {
            const Physics::TetEmbedding &e = mesh.surfaceEmbedding[i];
            ASSERT_LT(e.tet, mesh.tets.size());
            double sum = 0.0;
            double reproduced[3] = {0.0, 0.0, 0.0};
            for (int c = 0; c < 4; ++c)
            {
                sum += e.weights[c];
                EXPECT_GE(e.weights[c], -1e-4f) << "vertex " << i << " lies outside its tetrahedron";
                EXPECT_LE(e.weights[c], 1.0f + 1e-4f);
                const auto &p = mesh.positions[mesh.tets[e.tet][c]];
                reproduced[0] += e.weights[c] * p.x;
                reproduced[1] += e.weights[c] * p.y;
                reproduced[2] += e.weights[c] * p.z;
            }
            EXPECT_NEAR(sum, 1.0, 1e-5) << "vertex " << i;
            for (int a = 0; a < 3; ++a)
            {
                EXPECT_NEAR(reproduced[a], source.vertices[i].position[a], 1e-4) << "vertex " << i << " axis " << a;
            }
        }
    }

    /// Every triangular face of every tetrahedron, counted by the nodes it joins
    std::map<std::array<std::uint32_t, 3>, int> FaceCounts(const TetMeshData &mesh)
    {
        std::map<std::array<std::uint32_t, 3>, int> counts;
        for (const auto &tet : mesh.tets)
        {
            for (int skip = 0; skip < 4; ++skip)
            {
                std::array<std::uint32_t, 3> face{};
                int n = 0;
                for (int c = 0; c < 4; ++c)
                {
                    if (c != skip)
                    {
                        face[n++] = tet[c];
                    }
                }
                std::ranges::sort(face);
                ++counts[face];
            }
        }
        return counts;
    }
}

TEST(TetrahedraliserTest, ABoxGetsFiveTetrahedraPerVoxelAndTheirVolumeIsTheBoxes)
{
    const Renderer::Common::MeshData box = TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f);
    const TetResult result = Run(box, 4);
    ASSERT_TRUE(result.Ok()) << result.error;
    EXPECT_TRUE(result.error.empty());
    EXPECT_FALSE(result.cacheHit);
    EXPECT_EQ(result.mesh->resolution, 4);
    EXPECT_EQ(result.mesh->tets.size(), 5u * 4 * 4 * 4);
    EXPECT_EQ(result.mesh->positions.size(), 5u * 5 * 5) << "one node per grid corner";
    EXPECT_NEAR(result.mesh->voxelSize, 0.25f, 1e-6f);
    EXPECT_EQ(result.mesh->weldedVertexCount, 8u);
    ExpectWellFormed(*result.mesh, box);
    EXPECT_NEAR(TestSupport::TotalVolume(*result.mesh), 1.0, 1e-4);
}

TEST(TetrahedraliserTest, NeighbouringVoxelsMeetOnMatchingFaces)
{
    const TetResult result = Run(TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f), 4);
    ASSERT_TRUE(result.Ok()) << result.error;
    int boundary = 0;
    for (const auto &[face, count] : FaceCounts(*result.mesh))
    {
        EXPECT_LE(count, 2) << "a face shared by more than two tetrahedra";
        if (count == 1)
        {
            ++boundary;
        }
    }
    // A conforming mesh has exactly the surface triangles on its boundary: 6 sides of 4 x 4 voxels, 2 triangles each.
    // A diagonal that disagrees between two voxels would leave an extra face of each on the boundary.
    EXPECT_EQ(boundary, 6 * 4 * 4 * 2);
}

TEST(TetrahedraliserTest, ABoxThatIsNotACubeGetsCubicVoxelsAlongItsLongestAxis)
{
    const Renderer::Common::MeshData box = TestSupport::MakeWeldedBox(2.0f, 1.0f, 0.5f);
    const TetResult result = Run(box, 8);
    ASSERT_TRUE(result.Ok()) << result.error;
    EXPECT_NEAR(result.mesh->voxelSize, 0.25f, 1e-6f) << "2 units over 8 voxels";
    EXPECT_EQ(result.mesh->tets.size(), 5u * 8 * 4 * 2);
    ExpectWellFormed(*result.mesh, box);
    EXPECT_NEAR(TestSupport::TotalVolume(*result.mesh), 1.0, 1e-4);
}

TEST(TetrahedraliserTest, ASphereIsAnApproximationThatStillEmbedsEveryVertex)
{
    Renderer::Common::MeshData sphere = Mesh::MakeSphere();
    const TetResult result = Run(sphere, 10);
    ASSERT_TRUE(result.Ok()) << result.error;
    ExpectWellFormed(*result.mesh, sphere);
    const double volume = TestSupport::TotalVolume(*result.mesh);
    // A sphere of diameter 1 has 0.5236; the voxel body is blocky, and holds the shell voxels too
    EXPECT_GT(volume, 0.4);
    EXPECT_LT(volume, 1.0);
    // Welding collapsed the seam and pole duplicates: 33 vertices in each of 17 rings, poles and the u seam shared
    EXPECT_LT(result.mesh->weldedVertexCount, sphere.vertices.size());
    EXPECT_EQ(result.mesh->weldedVertexCount, 2u + 15u * 32u);
}

TEST(TetrahedraliserTest, ASeamedCubeMovesItsDuplicatesTogether)
{
    // The built-in cube has 24 vertices: 4 per face, so every corner is 3 vertices with different normals
    const Renderer::Common::MeshData cube = Mesh::MakeCube();
    ASSERT_EQ(cube.vertices.size(), 24u);
    const TetResult result = Run(cube, 5);
    ASSERT_TRUE(result.Ok()) << result.error;
    EXPECT_EQ(result.mesh->weldedVertexCount, 8u);
    ExpectWellFormed(*result.mesh, cube);

    // Vertices at the same position share one embedding, exactly
    int groups = 0;
    for (std::size_t i = 0; i < cube.vertices.size(); ++i)
    {
        for (std::size_t j = i + 1; j < cube.vertices.size(); ++j)
        {
            const auto &a = cube.vertices[i].position;
            const auto &b = cube.vertices[j].position;
            if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2])
            {
                ++groups;
                const auto &ea = result.mesh->surfaceEmbedding[i];
                const auto &eb = result.mesh->surfaceEmbedding[j];
                EXPECT_EQ(ea.tet, eb.tet);
                for (int c = 0; c < 4; ++c)
                {
                    EXPECT_EQ(ea.weights[c], eb.weights[c]);
                }
            }
        }
    }
    EXPECT_EQ(groups, 8 * 3) << "8 corners, 3 pairs among each one's 3 vertices";
    EXPECT_NEAR(TestSupport::TotalVolume(*result.mesh), 1.0, 1e-4);
}

TEST(TetrahedraliserTest, TheResolutionChangesTheTetrahedronCount)
{
    const Renderer::Common::MeshData box = TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f);
    const TetResult coarse = Run(box, 4);
    const TetResult fine = Run(box, 8);
    ASSERT_TRUE(coarse.Ok() && fine.Ok());
    EXPECT_EQ(coarse.mesh->tets.size(), 5u * 64);
    EXPECT_EQ(fine.mesh->tets.size(), 5u * 512);
    EXPECT_GT(fine.mesh->tets.size(), coarse.mesh->tets.size());

    const Renderer::Common::MeshData sphere = Mesh::MakeSphere();
    EXPECT_GT(Run(sphere, 12).mesh->tets.size(), Run(sphere, 6).mesh->tets.size());
}

TEST(TetrahedraliserTest, TheResolutionIsClampedToItsRange)
{
    const Renderer::Common::MeshData box = TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f);
    const TetResult tiny = Run(box, -5);
    ASSERT_TRUE(tiny.Ok());
    EXPECT_EQ(tiny.mesh->resolution, Physics::kMinTetResolution);
    EXPECT_EQ(tiny.mesh->tets.size(), 5u * 2 * 2 * 2);
}

TEST(TetrahedraliserTest, ABodyOverTheTetrahedronCapGetsALowerResolutionNotAnError)
{
    const Renderer::Common::MeshData box = TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f);
    // 32 voxels along each side would be 163840 tetrahedra
    const TetResult capped = Run(box, Physics::kMaxTetResolution);
    ASSERT_TRUE(capped.Ok()) << capped.error;
    EXPECT_LE(capped.mesh->tets.size(), Physics::kMaxTetsPerBody);
    EXPECT_LT(capped.mesh->resolution, Physics::kMaxTetResolution);
    EXPECT_GE(capped.mesh->resolution, Physics::kMinTetResolution);
    ExpectWellFormed(*capped.mesh, box);

    TetSettings small;
    small.resolution = 10;
    small.maxTets = 100;
    const TetResult limited = Tetrahedralise(box.vertices, box.indices, small);
    ASSERT_TRUE(limited.Ok()) << limited.error;
    EXPECT_LE(limited.mesh->tets.size(), 100u);

    // Even the lowest resolution (2 x 2 x 2 voxels = 40 tetrahedra) doesn't fit in 10
    small.maxTets = 10;
    const TetResult impossible = Tetrahedralise(box.vertices, box.indices, small);
    EXPECT_FALSE(impossible.Ok());
    EXPECT_NE(impossible.error.find("limit"), std::string::npos) << impossible.error;
}

TEST(TetrahedraliserTest, ASmallHoleInTheSurfaceDoesNotLoseTheBody)
{
    Renderer::Common::MeshData box = TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f);
    const TetResult closed = Run(box, 6);
    ASSERT_TRUE(closed.Ok());

    // Delete the two triangles of one face: the +x ray of an inside voxel still crosses the opposite face, the +y and
    // +z rays cross theirs, and two of three still say inside
    ASSERT_EQ(box.indices.size(), 36u);
    box.indices.erase(box.indices.begin() + 6, box.indices.begin() + 12);
    const TetResult holed = Run(box, 6);
    ASSERT_TRUE(holed.Ok()) << holed.error;
    EXPECT_EQ(holed.mesh->tets.size(), closed.mesh->tets.size());
    ExpectWellFormed(*holed.mesh, box);
}

TEST(TetrahedraliserTest, DegenerateInputFailsCleanly)
{
    const auto expectError = [](const TetResult &result, const char *text)
    {
        EXPECT_FALSE(result.Ok());
        EXPECT_EQ(result.mesh, nullptr);
        EXPECT_NE(result.error.find(text), std::string::npos) << result.error;
    };

    // Nothing at all
    expectError(Tetrahedralise(std::span<const Renderer::Common::Vertex>{}, std::span<const std::uint32_t>{}),
                "no triangles");

    // Vertices but no triangles, and an index list that isn't whole triangles
    Renderer::Common::MeshData box = TestSupport::MakeWeldedBox(1.0f, 1.0f, 1.0f);
    expectError(Tetrahedralise(box.vertices, std::span<const std::uint32_t>{}), "no triangles");
    expectError(Tetrahedralise(box.vertices, std::span<const std::uint32_t>(box.indices.data(), 4)), "no triangles");

    // Every triangle collapses to a line (two corners at the same position)
    Renderer::Common::MeshData collapsed = box;
    collapsed.indices = {0, 0, 1, 1, 2, 2};
    expectError(Tetrahedralise(collapsed.vertices, collapsed.indices), "no triangles");

    // Flat: a quad has no thickness
    const Renderer::Common::MeshData quad = Mesh::MakeQuad();
    expectError(Tetrahedralise(quad.vertices, quad.indices), "flat");

    // A single point
    Renderer::Common::MeshData point;
    point.vertices.assign(3, Renderer::Common::Vertex{});
    point.indices = {0, 1, 2};
    expectError(Tetrahedralise(point.vertices, point.indices), "point");

    // A bad index and a non-finite position
    Renderer::Common::MeshData outOfRange = box;
    outOfRange.indices[3] = 99;
    expectError(Tetrahedralise(outOfRange.vertices, outOfRange.indices), "index");
    Renderer::Common::MeshData notANumber = box;
    notANumber.vertices[2].position[1] = std::numeric_limits<float>::quiet_NaN();
    expectError(Tetrahedralise(notANumber.vertices, notANumber.indices), "finite");
}

TEST(TetrahedraliserTest, TheSameMeshGivesTheSameBodyEveryTime)
{
    const Renderer::Common::MeshData sphere = Mesh::MakeSphere();
    const TetResult a = Run(sphere, 8);
    const TetResult b = Run(sphere, 8);
    ASSERT_TRUE(a.Ok() && b.Ok());
    ASSERT_EQ(a.mesh->tets.size(), b.mesh->tets.size());
    EXPECT_EQ(a.mesh->tets, b.mesh->tets);
    ASSERT_EQ(a.mesh->positions.size(), b.mesh->positions.size());
    for (std::size_t i = 0; i < a.mesh->positions.size(); ++i)
    {
        EXPECT_EQ(a.mesh->positions[i].x, b.mesh->positions[i].x);
        EXPECT_EQ(a.mesh->positions[i].y, b.mesh->positions[i].y);
        EXPECT_EQ(a.mesh->positions[i].z, b.mesh->positions[i].z);
    }
    for (std::size_t i = 0; i < a.mesh->surfaceEmbedding.size(); ++i)
    {
        EXPECT_EQ(a.mesh->surfaceEmbedding[i].tet, b.mesh->surfaceEmbedding[i].tet);
    }
}

TEST(TetrahedraliserTest, ATetrahedraliserOnAMeshObjectReadsItsCpuData)
{
    const std::shared_ptr<Mesh> mesh = TestSupport::MakeBoxMesh();
    ASSERT_NE(mesh, nullptr);
    const TetResult result = Tetrahedralise(*mesh);
    ASSERT_TRUE(result.Ok()) << result.error;
    EXPECT_EQ(result.mesh->surfaceEmbedding.size(), mesh->GetVertices().size());
    EXPECT_EQ(result.mesh->resolution, 10) << "the default resolution";
}

// ---------------------------------------------------------------- the cache

class TetCacheTest : public ::testing::Test
{
protected:
    void SetUp() override { TetCache::Instance().Clear(); }
    void TearDown() override { TetCache::Instance().Clear(); }
};

TEST_F(TetCacheTest, TheSameMeshVersionAndResolutionIsABuiltOnceAndThenAHit)
{
    TetCache &cache = TetCache::Instance();
    const std::shared_ptr<Mesh> mesh = TestSupport::MakeBoxMesh();
    const std::size_t hits = cache.GetHitCount();
    const std::size_t misses = cache.GetMissCount();

    const TetResult first = cache.GetOrBuild(mesh, 5);
    ASSERT_TRUE(first.Ok()) << first.error;
    EXPECT_FALSE(first.cacheHit);
    EXPECT_EQ(cache.Size(), 1u);

    const TetResult second = cache.GetOrBuild(mesh, 5);
    ASSERT_TRUE(second.Ok());
    EXPECT_TRUE(second.cacheHit);
    EXPECT_EQ(second.mesh, first.mesh) << "the very same body, not a copy";
    EXPECT_EQ(cache.GetMissCount(), misses + 1);
    EXPECT_EQ(cache.GetHitCount(), hits + 1);
    EXPECT_EQ(cache.Size(), 1u);
}

TEST_F(TetCacheTest, ASetDataOrAnotherResolutionMisses)
{
    TetCache &cache = TetCache::Instance();
    const std::shared_ptr<Mesh> mesh = TestSupport::MakeBoxMesh();
    const TetResult first = cache.GetOrBuild(mesh, 4);
    ASSERT_TRUE(first.Ok());

    const TetResult otherResolution = cache.GetOrBuild(mesh, 6);
    ASSERT_TRUE(otherResolution.Ok());
    EXPECT_FALSE(otherResolution.cacheHit);
    EXPECT_NE(otherResolution.mesh, first.mesh);
    EXPECT_EQ(otherResolution.mesh->resolution, 6);

    // The mesh changes (its version goes up): the body of the old geometry is not served
    const std::uint64_t version = mesh->GetVersion();
    ASSERT_TRUE(mesh->SetData(TestSupport::MakeWeldedBox(2.0f, 1.0f, 1.0f)));
    EXPECT_GT(mesh->GetVersion(), version);
    const TetResult afterSetData = cache.GetOrBuild(mesh, 4);
    ASSERT_TRUE(afterSetData.Ok());
    EXPECT_FALSE(afterSetData.cacheHit);
    EXPECT_NE(afterSetData.mesh, first.mesh);
    EXPECT_NEAR(TestSupport::TotalVolume(*afterSetData.mesh), 2.0, 1e-3);

    // ... and the new version is cached in turn
    EXPECT_TRUE(cache.GetOrBuild(mesh, 4).cacheHit);
}

TEST_F(TetCacheTest, AnotherMeshWithTheSameDataIsItsOwnEntry)
{
    TetCache &cache = TetCache::Instance();
    const std::shared_ptr<Mesh> a = TestSupport::MakeBoxMesh();
    const std::shared_ptr<Mesh> b = TestSupport::MakeBoxMesh();
    const TetResult fromA = cache.GetOrBuild(a, 4);
    const TetResult fromB = cache.GetOrBuild(b, 4);
    ASSERT_TRUE(fromA.Ok() && fromB.Ok());
    EXPECT_FALSE(fromB.cacheHit);
    EXPECT_NE(fromA.mesh, fromB.mesh);
    EXPECT_EQ(cache.Size(), 2u);
}

TEST_F(TetCacheTest, TheCacheNeverTouchesTheMeshsCpuCacheSlot)
{
    TetCache &cache = TetCache::Instance();
    const std::shared_ptr<Mesh> mesh = TestSupport::MakeBoxMesh();
    // The picking BVH lives in this slot; a second type would evict it
    const auto marker = std::make_shared<const int>(42);
    mesh->SetCpuCache<int>(marker);
    ASSERT_TRUE(mesh->HasCpuCache());

    ASSERT_TRUE(cache.GetOrBuild(mesh, 5).Ok());
    ASSERT_TRUE(cache.GetOrBuild(mesh, 5).cacheHit);
    ASSERT_TRUE(cache.GetOrBuild(mesh, 7).Ok());

    EXPECT_TRUE(mesh->HasCpuCache());
    EXPECT_EQ(mesh->GetCpuCache<int>(), marker) << "still the picking structure's slot, still its object";

    // And a mesh without a CPU cache is not given one
    const std::shared_ptr<Mesh> other = TestSupport::MakeBoxMesh();
    ASSERT_TRUE(cache.GetOrBuild(other, 5).Ok());
    EXPECT_FALSE(other->HasCpuCache());
}

TEST_F(TetCacheTest, FailuresAreNotCachedAndAMissingMeshIsAnError)
{
    TetCache &cache = TetCache::Instance();
    const std::shared_ptr<Mesh> quad = Mesh::Create(Mesh::MakeQuad());
    ASSERT_NE(quad, nullptr);
    const TetResult failed = cache.GetOrBuild(quad, 8);
    EXPECT_FALSE(failed.Ok());
    EXPECT_FALSE(failed.error.empty());
    EXPECT_EQ(cache.Size(), 0u);

    const TetResult none = cache.GetOrBuild(nullptr, 8);
    EXPECT_FALSE(none.Ok());
    EXPECT_FALSE(none.error.empty());
}

TEST_F(TetCacheTest, AnEntryForAMeshThatIsGoneNeverMatchesAndIsDropped)
{
    TetCache &cache = TetCache::Instance();
    {
        const std::shared_ptr<Mesh> gone = TestSupport::MakeBoxMesh();
        ASSERT_TRUE(cache.GetOrBuild(gone, 4).Ok());
        EXPECT_EQ(cache.Size(), 1u);
    }
    const std::shared_ptr<Mesh> later = TestSupport::MakeBoxMesh();
    const TetResult result = cache.GetOrBuild(later, 4);
    ASSERT_TRUE(result.Ok());
    EXPECT_FALSE(result.cacheHit) << "a new mesh, even at the old one's address";
    EXPECT_EQ(cache.Size(), 1u) << "the dead entry was dropped";
}

TEST_F(TetCacheTest, TheCacheKeepsAtMostMaxEntriesAndDropsTheOldest)
{
    TetCache &cache = TetCache::Instance();
    std::vector<std::shared_ptr<Mesh>> meshes;
    for (std::size_t i = 0; i < TetCache::kMaxEntries + 5; ++i)
    {
        meshes.push_back(TestSupport::MakeBoxMesh());
        ASSERT_TRUE(cache.GetOrBuild(meshes.back(), 2).Ok());
    }
    EXPECT_EQ(cache.Size(), TetCache::kMaxEntries);
    EXPECT_FALSE(cache.GetOrBuild(meshes.front(), 2).cacheHit) << "the oldest was dropped";
    EXPECT_TRUE(cache.GetOrBuild(meshes.back(), 2).cacheHit);
}
