#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <math/Constants.hpp>
#include <math/UUID.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/rendering/Mesh.hpp"

#include "MeshTestSupport.hpp"

// The Mesh asset: validation, submeshes and bounds, versions, and the built-in meshes, which must be exactly the
// geometry CubeRenderer, SphereRenderer and QuadRenderer built for themselves before #3 P2

using namespace N2Engine;
using Rendering::BuiltinMesh;
using Rendering::Mesh;
using Rendering::Submesh;
using Renderer::Common::MeshData;
using Renderer::Common::Vertex;
using MeshTestSupport::WarningCapture;

namespace
{
    // ===== The pre-P2 generators, copied verbatim from the shape renderers' CreateMesh =====

    MeshData OldCube()
    {
        Renderer::Common::MeshData cubeData;

        constexpr float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        constexpr float h = 0.5f; // Half size for unit cube

        cubeData.vertices = {
            {{-h, -h,  h}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h, -h,  h}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h,  h}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h,  h}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            {{ h, -h, -h}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h, -h, -h}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h, -h}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h, -h}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            {{ h, -h,  h}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h, -h, -h}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h, -h}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h,  h}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            {{-h, -h, -h}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h, -h,  h}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h,  h}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h, -h}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            {{-h,  h,  h}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h,  h}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h,  h, -h}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h,  h, -h}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},

            {{-h, -h, -h}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h, -h, -h}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{ h, -h,  h}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{-h, -h,  h}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}}
        };

        cubeData.indices = {
            0, 1, 2, 0, 2, 3,
            4, 5, 6, 4, 6, 7,
            8, 9, 10, 8, 10, 11,
            12, 13, 14, 12, 14, 15,
            16, 17, 18, 16, 18, 19,
            20, 21, 22, 20, 22, 23
        };
        return cubeData;
    }

    MeshData OldSphere(const uint32_t latitudeSegments, const uint32_t longitudeSegments)
    {
        Renderer::Common::MeshData sphereData;
        for (uint32_t lat = 0; lat <= latitudeSegments; ++lat)
        {
            constexpr float PI = Math::Constants::PI_F;
            const float theta = static_cast<float>(lat) * PI / static_cast<float>(latitudeSegments);
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);

            for (uint32_t lon = 0; lon <= longitudeSegments; ++lon)
            {
                constexpr float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                constexpr float radius = 0.5f;
                const float phi = static_cast<float>(lon) * 2.0f * PI / static_cast<float>(longitudeSegments);
                const float sinPhi = std::sin(phi);
                const float cosPhi = std::cos(phi);

                const float x = cosPhi * sinTheta;
                const float y = cosTheta;
                const float z = sinPhi * sinTheta;

                const float u = static_cast<float>(lon) / static_cast<float>(longitudeSegments);
                const float v = static_cast<float>(lat) / static_cast<float>(latitudeSegments);

                Renderer::Common::Vertex vertex;
                vertex.position[0] = x * radius;
                vertex.position[1] = y * radius;
                vertex.position[2] = z * radius;
                vertex.normal[0] = x;
                vertex.normal[1] = y;
                vertex.normal[2] = z;
                vertex.texCoord[0] = u;
                vertex.texCoord[1] = v;
                vertex.color[0] = white[0];
                vertex.color[1] = white[1];
                vertex.color[2] = white[2];
                vertex.color[3] = white[3];
                sphereData.vertices.push_back(vertex);
            }
        }
        for (uint32_t lat = 0; lat < latitudeSegments; ++lat)
        {
            for (uint32_t lon = 0; lon < longitudeSegments; ++lon)
            {
                const uint32_t first = lat * (longitudeSegments + 1) + lon;
                const uint32_t second = first + longitudeSegments + 1;
                sphereData.indices.push_back(first);
                sphereData.indices.push_back(second);
                sphereData.indices.push_back(first + 1);
                sphereData.indices.push_back(second);
                sphereData.indices.push_back(second + 1);
                sphereData.indices.push_back(first + 1);
            }
        }
        return sphereData;
    }

    MeshData OldQuad()
    {
        Renderer::Common::MeshData quadData;
        constexpr float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        quadData.vertices = {
            {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {white[0], white[1], white[2], white[3]}},
            {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {white[0], white[1], white[2], white[3]}},
            {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {white[0], white[1], white[2], white[3]}}
        };
        quadData.indices = {0, 1, 2, 0, 2, 3};
        return quadData;
    }

    /// Byte for byte: positions, normals, uvs, colours and indices
    void ExpectSameGeometry(const MeshData &actual, const MeshData &expected)
    {
        ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
        ASSERT_EQ(actual.indices, expected.indices);
        static_assert(sizeof(Vertex) == 48, "Vertex stays 48 bytes until normal maps (#3 P4)");
        EXPECT_EQ(std::memcmp(actual.vertices.data(), expected.vertices.data(), actual.vertices.size() * sizeof(Vertex)), 0);
    }

    Vertex At(const float x, const float y, const float z)
    {
        return Vertex{{x, y, z}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}};
    }

    /// Two triangles side by side (0..2 and 3..5), the second further along x and higher
    MeshData TwoTriangles()
    {
        MeshData data;
        data.vertices = {At(0, 0, 0), At(1, 0, 0), At(0, 1, 0), At(5, 2, -1), At(6, 2, -1), At(5, 4, 3)};
        data.indices = {0, 1, 2, 3, 4, 5};
        return data;
    }

    void ExpectVector(const Math::Vector3 &actual, const float x, const float y, const float z)
    {
        EXPECT_FLOAT_EQ(actual.x, x);
        EXPECT_FLOAT_EQ(actual.y, y);
        EXPECT_FLOAT_EQ(actual.z, z);
    }
}

// ============================================================================
// Built-in meshes
// ============================================================================

TEST(BuiltinMeshTest, TheBuiltinsAreExactlyTheShapesTheRenderersUsedToBuild)
{
    ExpectSameGeometry(Mesh::GetBuiltin(BuiltinMesh::Cube)->GetMeshData(), OldCube());
    ExpectSameGeometry(Mesh::GetBuiltin(BuiltinMesh::Sphere)->GetMeshData(), OldSphere(16, 32));
    ExpectSameGeometry(Mesh::GetBuiltin(BuiltinMesh::Quad)->GetMeshData(), OldQuad());
    // ...and so is a sphere of any other subdivision
    ExpectSameGeometry(Mesh::MakeSphere(8, 12), OldSphere(8, 12));
}

TEST(BuiltinMeshTest, EachIsOneSharedMeshWithAFixedUuid)
{
    const auto cube = Mesh::GetBuiltin(BuiltinMesh::Cube);
    EXPECT_EQ(cube, Mesh::GetBuiltin(BuiltinMesh::Cube)) << "made once, shared";
    EXPECT_TRUE(cube->IsBuiltin());
    EXPECT_EQ(cube->GetResourceType(), "Mesh");
    EXPECT_EQ(cube->GetSubmeshCount(), 1u);
    EXPECT_EQ(cube->GetUUID().ToString(), "6e32656e-6d65-5348-0001-000000000001") << "saved in scene files: never change it";
    EXPECT_EQ(Mesh::GetBuiltin(BuiltinMesh::Sphere)->GetUUID().ToString(), "6e32656e-6d65-5348-0001-000000000002");
    EXPECT_EQ(Mesh::GetBuiltin(BuiltinMesh::Quad)->GetUUID().ToString(), "6e32656e-6d65-5348-0001-000000000003");

    for (const BuiltinMesh which : {BuiltinMesh::Cube, BuiltinMesh::Sphere, BuiltinMesh::Quad})
    {
        const auto mesh = Mesh::GetBuiltin(which);
        EXPECT_EQ(Mesh::GetBuiltinUUID(which), mesh->GetUUID());
        EXPECT_EQ(Mesh::FindBuiltin(mesh->GetUUID()), mesh);
        EXPECT_EQ(Mesh::ParseBuiltinName(Mesh::GetBuiltinName(which)), which);
        EXPECT_EQ(mesh->GetDebugName(), Mesh::GetBuiltinName(which));
    }
    EXPECT_EQ(Mesh::FindBuiltin(Math::UUID::Random()), nullptr);
    EXPECT_FALSE(Mesh::ParseBuiltinName("cube").has_value()) << "names are exact";
    EXPECT_FALSE(Mesh::ParseBuiltinName("Teapot").has_value());
}

TEST(BuiltinMeshTest, BuiltinsHaveTheirShapesBoundsAndCantBeChanged)
{
    const auto cube = Mesh::GetBuiltin(BuiltinMesh::Cube);
    ExpectVector(cube->GetBounds().min, -0.5f, -0.5f, -0.5f);
    ExpectVector(cube->GetBounds().max, 0.5f, 0.5f, 0.5f);
    const auto quad = Mesh::GetBuiltin(BuiltinMesh::Quad);
    ExpectVector(quad->GetBounds().min, -0.5f, -0.5f, 0.0f);
    ExpectVector(quad->GetBounds().max, 0.5f, 0.5f, 0.0f);
    const auto sphere = Mesh::GetBuiltin(BuiltinMesh::Sphere);
    EXPECT_NEAR(sphere->GetBounds().min.y, -0.5f, 1e-5f);
    EXPECT_NEAR(sphere->GetBounds().max.y, 0.5f, 1e-5f);
    EXPECT_NEAR(sphere->GetBounds().max.x, 0.5f, 1e-5f);

    const std::uint64_t version = cube->GetVersion();
    WarningCapture capture;
    EXPECT_FALSE(cube->SetData(OldQuad()));
    EXPECT_TRUE(capture.Mentions("built-in"));
    EXPECT_EQ(cube->GetVersion(), version);
    ExpectSameGeometry(cube->GetMeshData(), OldCube());
}

// ============================================================================
// Runtime meshes
// ============================================================================

TEST(MeshTest, ACreatedMeshHasOneSubmeshCoveringEveryIndexByDefault)
{
    const auto mesh = Mesh::Create(TwoTriangles());
    ASSERT_NE(mesh, nullptr);
    EXPECT_FALSE(mesh->IsBuiltin());
    EXPECT_EQ(mesh->GetVersion(), 1u);
    ASSERT_EQ(mesh->GetSubmeshCount(), 1u);
    EXPECT_EQ(mesh->GetSubmeshes()[0].firstIndex, 0u);
    EXPECT_EQ(mesh->GetSubmeshes()[0].indexCount, 6u);
    ExpectVector(mesh->GetBounds().min, 0.0f, 0.0f, -1.0f);
    ExpectVector(mesh->GetBounds().max, 6.0f, 4.0f, 3.0f);
    ExpectVector(mesh->GetSubmeshes()[0].bounds.min, 0.0f, 0.0f, -1.0f);
    ExpectVector(mesh->GetSubmeshes()[0].bounds.max, 6.0f, 4.0f, 3.0f);
}

TEST(MeshTest, EachSubmeshGetsTheBoundsOfTheVerticesItUses)
{
    const auto mesh = Mesh::Create(TwoTriangles(), {Submesh{0, 3, {}}, Submesh{3, 3, {}}});
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->GetSubmeshCount(), 2u);
    const Submesh &first = mesh->GetSubmeshes()[0];
    const Submesh &second = mesh->GetSubmeshes()[1];
    ExpectVector(first.bounds.min, 0.0f, 0.0f, 0.0f);
    ExpectVector(first.bounds.max, 1.0f, 1.0f, 0.0f);
    ExpectVector(second.bounds.min, 5.0f, 2.0f, -1.0f);
    ExpectVector(second.bounds.max, 6.0f, 4.0f, 3.0f);
    // The mesh's box is around everything
    ExpectVector(mesh->GetBounds().min, 0.0f, 0.0f, -1.0f);
    ExpectVector(mesh->GetBounds().max, 6.0f, 4.0f, 3.0f);
    EXPECT_EQ(second.firstIndex, 3u);
    EXPECT_EQ(second.indexCount, 3u);
}

TEST(MeshTest, InvalidGeometryIsRefusedWithAnError)
{
    WarningCapture capture;
    MeshData noIndices = TwoTriangles();
    noIndices.indices.clear();
    EXPECT_EQ(Mesh::Create(noIndices), nullptr);
    EXPECT_EQ(Mesh::Create(MeshData{}), nullptr);

    MeshData notTriangles = TwoTriangles();
    notTriangles.indices.pop_back();
    EXPECT_EQ(Mesh::Create(notTriangles), nullptr);

    MeshData pastTheEnd = TwoTriangles();
    pastTheEnd.indices[4] = 6;
    EXPECT_EQ(Mesh::Create(pastTheEnd), nullptr);

    // Submeshes: empty, not whole triangles, past the indices
    EXPECT_EQ(Mesh::Create(TwoTriangles(), {Submesh{0, 0, {}}}), nullptr);
    EXPECT_EQ(Mesh::Create(TwoTriangles(), {Submesh{1, 3, {}}}), nullptr);
    EXPECT_EQ(Mesh::Create(TwoTriangles(), {Submesh{0, 4, {}}}), nullptr);
    EXPECT_EQ(Mesh::Create(TwoTriangles(), {Submesh{3, 6, {}}}), nullptr);
    EXPECT_GE(capture.messages.size(), 8u) << "each refusal says why";
}

TEST(MeshTest, SetDataReplacesTheGeometryAndBumpsTheVersion)
{
    const auto mesh = Mesh::Create(OldQuad());
    ASSERT_NE(mesh, nullptr);
    const std::uint64_t first = mesh->GetVersion();

    ASSERT_TRUE(mesh->SetData(TwoTriangles(), {Submesh{0, 3, {}}, Submesh{3, 3, {}}}));
    EXPECT_GT(mesh->GetVersion(), first);
    EXPECT_EQ(mesh->GetVertices().size(), 6u);
    EXPECT_EQ(mesh->GetSubmeshCount(), 2u);

    // Invalid data changes nothing
    const std::uint64_t second = mesh->GetVersion();
    WarningCapture capture;
    EXPECT_FALSE(mesh->SetData(MeshData{}));
    EXPECT_EQ(mesh->GetVersion(), second);
    EXPECT_EQ(mesh->GetVertices().size(), 6u);
}

TEST(MeshTest, ComputeBoundsOfNothingIsAZeroBox)
{
    const BoundingBox box = Mesh::ComputeBounds({});
    ExpectVector(box.min, 0.0f, 0.0f, 0.0f);
    ExpectVector(box.max, 0.0f, 0.0f, 0.0f);
}
