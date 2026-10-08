#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <assetimport/GltfImporter.hpp>
#include <assetimport/TangentGeneration.hpp>
#include <nlohmann/json.hpp>

#include "GltfTestBuilder.hpp"

// Tangents for normal maps (#3 P4b): GenerateTangents (MikkTSpace) on meshes built in the test, and what the glTF
// importer does with a file's TANGENT attribute, with generated ones, and with the normal texture's scale. The
// convention checked throughout: bitangent = cross(normal, tangent.xyz) * tangent.w points along increasing v, with
// v up (the engine's convention, the green-up of glTF normal maps).

using namespace N2Engine::AssetImport;
using GltfTest::Builder;

namespace
{
    using Vec3 = std::array<float, 3>;

    float Dot(const Vec3 &a, const Vec3 &b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    Vec3 Cross(const Vec3 &a, const Vec3 &b)
    {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }

    Vec3 TangentOf(const ImportedVertex &vertex)
    {
        return {vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]};
    }

    Vec3 NormalOf(const ImportedVertex &vertex)
    {
        return {vertex.normal[0], vertex.normal[1], vertex.normal[2]};
    }

    /// The bitangent the shaders build from a vertex
    Vec3 BitangentOf(const ImportedVertex &vertex)
    {
        const Vec3 b = Cross(NormalOf(vertex), TangentOf(vertex));
        return {b[0] * vertex.tangent[3], b[1] * vertex.tangent[3], b[2] * vertex.tangent[3]};
    }

    ImportedVertex MakeVertex(const float x, const float y, const float z, const float u, const float v)
    {
        ImportedVertex vertex;
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.position[2] = z;
        vertex.normal[0] = 0.0f;
        vertex.normal[1] = 0.0f;
        vertex.normal[2] = 1.0f;
        vertex.texCoord[0] = u;
        vertex.texCoord[1] = v;
        return vertex;
    }

    /// A unit quad at z = 0 facing +z, wound counter-clockwise, with the given uv for each corner (bottom-left,
    /// bottom-right, top-right, top-left)
    struct QuadMesh
    {
        std::vector<ImportedVertex> vertices;
        std::vector<std::uint32_t> indices{0, 1, 2, 0, 2, 3};
    };

    QuadMesh MakeQuad(const std::array<std::array<float, 2>, 4> &uv)
    {
        QuadMesh quad;
        quad.vertices = {MakeVertex(0, 0, 0, uv[0][0], uv[0][1]), MakeVertex(1, 0, 0, uv[1][0], uv[1][1]),
                         MakeVertex(1, 1, 0, uv[2][0], uv[2][1]), MakeVertex(0, 1, 0, uv[3][0], uv[3][1])};
        return quad;
    }

    /// u across x, v up y: the plain mapping
    QuadMesh PlainQuad()
    {
        return MakeQuad({{{0, 0}, {1, 0}, {1, 1}, {0, 1}}});
    }

    void ExpectTangent(const ImportedVertex &vertex, const Vec3 &tangent, const float w, const std::string &what)
    {
        EXPECT_NEAR(vertex.tangent[0], tangent[0], 1e-4f) << what << " x";
        EXPECT_NEAR(vertex.tangent[1], tangent[1], 1e-4f) << what << " y";
        EXPECT_NEAR(vertex.tangent[2], tangent[2], 1e-4f) << what << " z";
        EXPECT_FLOAT_EQ(vertex.tangent[3], w) << what << " w";
    }

    /// A unit, finite tangent perpendicular to the vertex normal with w = +1 or -1
    void ExpectValidTangent(const ImportedVertex &vertex, const std::string &what)
    {
        for (const float component : vertex.tangent)
        {
            ASSERT_TRUE(std::isfinite(component)) << what;
        }
        EXPECT_NEAR(Dot(TangentOf(vertex), TangentOf(vertex)), 1.0f, 1e-3f) << what << " is a unit vector";
        EXPECT_NEAR(Dot(TangentOf(vertex), NormalOf(vertex)), 0.0f, 1e-3f) << what << " is perpendicular to the normal";
        EXPECT_TRUE(vertex.tangent[3] == 1.0f || vertex.tangent[3] == -1.0f) << what << " w = " << vertex.tangent[3];
    }
}

// ============================================================================
// GenerateTangents
// ============================================================================

TEST(TangentGenerationTest, APlainQuadHasTangentsAlongUAndPositiveHandedness)
{
    QuadMesh quad = PlainQuad();
    const TangentResult result = GenerateTangents(quad.vertices, quad.indices);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.verticesAdded, 0u);
    ASSERT_EQ(quad.vertices.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        ExpectTangent(quad.vertices[i], {1, 0, 0}, 1.0f, "vertex " + std::to_string(i));
        // The bitangent points along increasing v: up the quad
        const Vec3 bitangent = BitangentOf(quad.vertices[i]);
        EXPECT_NEAR(bitangent[1], 1.0f, 1e-4f);
    }
    EXPECT_EQ(quad.indices, (std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3})) << "no vertex needed copying";
}

TEST(TangentGenerationTest, MirroringVFlipsTheHandedness)
{
    // v grows downward: the bitangent cross(N, T) * w must still point along increasing v, so w is -1
    QuadMesh quad = MakeQuad({{{0, 1}, {1, 1}, {1, 0}, {0, 0}}});
    ASSERT_TRUE(GenerateTangents(quad.vertices, quad.indices).ok);
    for (std::size_t i = 0; i < 4; ++i)
    {
        ExpectTangent(quad.vertices[i], {1, 0, 0}, -1.0f, "vertex " + std::to_string(i));
        EXPECT_NEAR(BitangentOf(quad.vertices[i])[1], -1.0f, 1e-4f) << "along increasing v, which is down";
    }
}

TEST(TangentGenerationTest, MirroringUPointsTheTangentTheOtherWayAndFlipsTheHandedness)
{
    QuadMesh quad = MakeQuad({{{1, 0}, {0, 0}, {0, 1}, {1, 1}}});
    ASSERT_TRUE(GenerateTangents(quad.vertices, quad.indices).ok);
    for (std::size_t i = 0; i < 4; ++i)
    {
        ExpectTangent(quad.vertices[i], {-1, 0, 0}, -1.0f, "vertex " + std::to_string(i));
        EXPECT_NEAR(BitangentOf(quad.vertices[i])[1], 1.0f, 1e-4f) << "v still grows up the quad";
    }
}

TEST(TangentGenerationTest, SwappedUAndVGiveATangentAlongYAndNegativeHandedness)
{
    // u = y, v = x
    QuadMesh quad = MakeQuad({{{0, 0}, {0, 1}, {1, 1}, {1, 0}}});
    ASSERT_TRUE(GenerateTangents(quad.vertices, quad.indices).ok);
    for (std::size_t i = 0; i < 4; ++i)
    {
        ExpectTangent(quad.vertices[i], {0, 1, 0}, -1.0f, "vertex " + std::to_string(i));
        EXPECT_NEAR(BitangentOf(quad.vertices[i])[0], 1.0f, 1e-4f) << "v grows along x";
    }
}

TEST(TangentGenerationTest, TheUvScaleDoesNotChangeTheTangent)
{
    QuadMesh quad = MakeQuad({{{0, 0}, {4, 0}, {4, 3}, {0, 3}}});
    ASSERT_TRUE(GenerateTangents(quad.vertices, quad.indices).ok);
    for (const ImportedVertex &vertex : quad.vertices)
    {
        ExpectTangent(vertex, {1, 0, 0}, 1.0f, "scaled uv");
    }
}

TEST(TangentGenerationTest, ASphereHasUnitTangentsPerpendicularToItsNormalsAlongTheLongitude)
{
    constexpr int latitudes = 12;
    constexpr int longitudes = 16;
    constexpr float pi = 3.14159265f;
    std::vector<ImportedVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<float> phis;
    for (int lat = 0; lat <= latitudes; ++lat)
    {
        const float theta = static_cast<float>(lat) * pi / static_cast<float>(latitudes);
        for (int lon = 0; lon <= longitudes; ++lon)
        {
            const float phi = static_cast<float>(lon) * 2.0f * pi / static_cast<float>(longitudes);
            const float x = std::cos(phi) * std::sin(theta);
            const float y = std::cos(theta);
            const float z = std::sin(phi) * std::sin(theta);
            ImportedVertex vertex = MakeVertex(x, y, z, static_cast<float>(lon) / longitudes, static_cast<float>(lat) / latitudes);
            vertex.normal[0] = x;
            vertex.normal[1] = y;
            vertex.normal[2] = z;
            vertices.push_back(vertex);
            phis.push_back(phi);
        }
    }
    for (int lat = 0; lat < latitudes; ++lat)
    {
        for (int lon = 0; lon < longitudes; ++lon)
        {
            const auto first = static_cast<std::uint32_t>(lat * (longitudes + 1) + lon);
            const auto second = first + longitudes + 1;
            indices.insert(indices.end(), {first, first + 1, second, second, first + 1, second + 1});
        }
    }
    const TangentResult result = GenerateTangents(vertices, indices);
    ASSERT_TRUE(result.ok);

    std::size_t aligned = 0;
    std::size_t away = 0;
    for (std::size_t i = 0; i < vertices.size(); ++i)
    {
        ExpectValidTangent(vertices[i], "sphere vertex " + std::to_string(i));
        // Away from the poles, the tangent follows increasing longitude (u): (-sin(phi), 0, cos(phi))
        const int lat = static_cast<int>(i) / (longitudes + 1);
        if (lat > 1 && lat < latitudes - 1)
        {
            ++away;
            const Vec3 expected = {-std::sin(phis[i]), 0.0f, std::cos(phis[i])};
            if (Dot(TangentOf(vertices[i]), expected) > 0.95f)
            {
                ++aligned;
            }
        }
    }
    EXPECT_EQ(aligned, away) << "every vertex off the poles has its tangent along the longitude";
}

TEST(TangentGenerationTest, DegenerateUvsStillGiveFiniteUnitTangents)
{
    // Every uv the same: no mapping to follow
    QuadMesh quad = MakeQuad({{{0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}}});
    const TangentResult result = GenerateTangents(quad.vertices, quad.indices);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(quad.vertices.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        ExpectValidTangent(quad.vertices[i], "degenerate vertex " + std::to_string(i));
    }
}

TEST(TangentGenerationTest, AZeroNormalAndZeroAreaTrianglesNeverProduceNotANumber)
{
    QuadMesh quad = PlainQuad();
    for (ImportedVertex &vertex : quad.vertices)
    {
        vertex.normal[0] = vertex.normal[1] = vertex.normal[2] = 0.0f;
    }
    // A zero-area triangle too (three equal positions)
    quad.vertices.push_back(MakeVertex(5, 5, 5, 0, 0));
    quad.indices.insert(quad.indices.end(), {4, 4, 4});
    ASSERT_TRUE(GenerateTangents(quad.vertices, quad.indices).ok);
    for (const ImportedVertex &vertex : quad.vertices)
    {
        for (const float component : vertex.tangent)
        {
            EXPECT_TRUE(std::isfinite(component));
        }
        EXPECT_TRUE(vertex.tangent[3] == 1.0f || vertex.tangent[3] == -1.0f);
    }
}

TEST(TangentGenerationTest, MissingKeepsTheFilesTangentsAndAllReplacesThem)
{
    QuadMesh quad = PlainQuad();
    quad.vertices[0].tangent[0] = 0.0f;
    quad.vertices[0].tangent[1] = 1.0f;
    quad.vertices[0].tangent[2] = 0.0f;
    quad.vertices[0].tangent[3] = -1.0f; // from a file: wrong on purpose

    QuadMesh missing = quad;
    ASSERT_TRUE(GenerateTangents(missing.vertices, missing.indices, TangentSelect::Missing).ok);
    ExpectTangent(missing.vertices[0], {0, 1, 0}, -1.0f, "kept");
    ExpectTangent(missing.vertices[1], {1, 0, 0}, 1.0f, "generated");
    ExpectTangent(missing.vertices[3], {1, 0, 0}, 1.0f, "generated");

    QuadMesh all = quad;
    ASSERT_TRUE(GenerateTangents(all.vertices, all.indices, TangentSelect::All).ok);
    ExpectTangent(all.vertices[0], {1, 0, 0}, 1.0f, "replaced");

    // Nothing missing: nothing touched, not even MikkTSpace run
    QuadMesh complete = PlainQuad();
    for (ImportedVertex &vertex : complete.vertices)
    {
        vertex.tangent[0] = 0.0f;
        vertex.tangent[1] = 0.0f;
        vertex.tangent[2] = 1.0f;
        vertex.tangent[3] = 1.0f;
    }
    const TangentResult untouched = GenerateTangents(complete.vertices, complete.indices);
    EXPECT_TRUE(untouched.ok);
    EXPECT_EQ(untouched.verticesWritten, 0u);
    ExpectTangent(complete.vertices[2], {0, 0, 1}, 1.0f, "kept as it was");
}

TEST(TangentGenerationTest, AVertexNeedingTwoTangentsIsDuplicatedForTheTriangleThatDiffers)
{
    // Two triangles meet along the edge (0,0)-(0,1) with the same vertex 0 and equal uv at it, but mirrored: the
    // first maps u along +x, the second along -x. A vertex has room for one tangent, so one triangle gets a copy.
    std::vector<ImportedVertex> vertices = {
        MakeVertex(0, 0, 0, 0, 0),   // 0: shared
        MakeVertex(1, 0, 0, 1, 0),   // 1
        MakeVertex(0, 1, 0, 0, 1),   // 2
        MakeVertex(0, 1, 0, 0, 1),   // 3: the second triangle's own corner at (0, 1)
        MakeVertex(-1, 0, 0, 1, 0),  // 4
    };
    std::vector<std::uint32_t> indices = {0, 1, 2, 0, 3, 4};
    const TangentResult result = GenerateTangents(vertices, indices);
    ASSERT_TRUE(result.ok);
    EXPECT_GE(result.verticesAdded, 1u);
    ASSERT_EQ(indices.size(), 6u) << "the index count is unchanged";
    ASSERT_EQ(vertices.size(), 5u + result.verticesAdded);

    // The first triangle's corners follow +x with w = +1, the second's -x with w = -1
    for (std::size_t c = 0; c < 3; ++c)
    {
        ExpectTangent(vertices[indices[c]], {1, 0, 0}, 1.0f, "first triangle corner " + std::to_string(c));
        ExpectTangent(vertices[indices[3 + c]], {-1, 0, 0}, -1.0f, "second triangle corner " + std::to_string(c));
    }
    // The copy sits where the original does
    EXPECT_NE(indices[0], indices[3]);
    for (std::size_t c = 0; c < 3; ++c)
    {
        EXPECT_FLOAT_EQ(vertices[indices[3]].position[c], vertices[0].position[c]);
    }
}

TEST(TangentGenerationTest, ADegenerateTriangleNeverTakesAVertexsSlotOrAsksForACopy)
{
    // Vertex 0 is shared by a triangle with one uv for all its corners, listed first, and a good one
    std::vector<ImportedVertex> vertices = {
        MakeVertex(0, 0, 0, 0, 0),       // 0: shared
        MakeVertex(0, 1, 0, 0, 0),       // 1 (degenerate uv)
        MakeVertex(-1, 0, 0, 0, 0),      // 2 (degenerate uv)
        MakeVertex(1, 0, 0, 1, 0),       // 3
        MakeVertex(0, 1, 0, 0, 1),       // 4
    };
    std::vector<std::uint32_t> indices = {0, 1, 2, 0, 3, 4};
    const TangentResult result = GenerateTangents(vertices, indices);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.verticesAdded, 0u) << "the degenerate corner uses the slot instead of asking for a copy";
    ExpectTangent(vertices[0], {1, 0, 0}, 1.0f, "the shared vertex has the good triangle's tangent");
    for (std::size_t i = 0; i < vertices.size(); ++i)
    {
        ExpectValidTangent(vertices[i], "vertex " + std::to_string(i));
    }
    EXPECT_EQ(indices, (std::vector<std::uint32_t>{0, 1, 2, 0, 3, 4}));
}

TEST(TangentGenerationTest, TheVertexLimitStopsDuplicationAndLeavesTheMeshUnchanged)
{
    std::vector<ImportedVertex> vertices = {MakeVertex(0, 0, 0, 0, 0), MakeVertex(1, 0, 0, 1, 0),
                                            MakeVertex(0, 1, 0, 0, 1), MakeVertex(0, 1, 0, 0, 1),
                                            MakeVertex(-1, 0, 0, 1, 0)};
    std::vector<std::uint32_t> indices = {0, 1, 2, 0, 3, 4};
    const auto verticesBefore = vertices.size();
    const auto indicesBefore = indices;
    const TangentResult result = GenerateTangents(vertices, indices, TangentSelect::Missing, verticesBefore);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(vertices.size(), verticesBefore);
    EXPECT_EQ(indices, indicesBefore);
    for (const ImportedVertex &vertex : vertices)
    {
        EXPECT_EQ(vertex.tangent[0], 0.0f) << "unchanged";
        EXPECT_EQ(vertex.tangent[3], 0.0f) << "unchanged";
    }
}

TEST(TangentGenerationTest, AnIndexPastTheLastVertexFailsWithoutChangingTheMesh)
{
    QuadMesh quad = PlainQuad();
    quad.indices[4] = 9;
    const auto indicesBefore = quad.indices;
    const TangentResult result = GenerateTangents(quad.vertices, quad.indices);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(quad.indices, indicesBefore);
    EXPECT_EQ(quad.vertices.size(), 4u);
}

TEST(TangentGenerationTest, VerticesNoTriangleReachesGetAFallbackTangent)
{
    QuadMesh quad = PlainQuad();
    quad.vertices.push_back(MakeVertex(7, 7, 7, 0, 0)); // unused
    const TangentResult result = GenerateTangents(quad.vertices, quad.indices);
    ASSERT_TRUE(result.ok);
    EXPECT_GE(result.fallbackVertices, 1u);
    ExpectValidTangent(quad.vertices[4], "unused vertex");

    // And a mesh with no triangles at all
    std::vector<ImportedVertex> lone = {MakeVertex(0, 0, 0, 0, 0)};
    std::vector<std::uint32_t> none;
    ASSERT_TRUE(GenerateTangents(lone, none).ok);
    ExpectValidTangent(lone[0], "no triangles");
}

TEST(TangentGenerationTest, TheResultIsTheSameEveryTime)
{
    QuadMesh first = PlainQuad();
    QuadMesh second = PlainQuad();
    ASSERT_TRUE(GenerateTangents(first.vertices, first.indices).ok);
    ASSERT_TRUE(GenerateTangents(second.vertices, second.indices).ok);
    for (std::size_t i = 0; i < first.vertices.size(); ++i)
    {
        for (std::size_t c = 0; c < 4; ++c)
        {
            EXPECT_EQ(first.vertices[i].tangent[c], second.vertices[i].tangent[c]);
        }
    }
}

// ============================================================================
// The glTF importer
// ============================================================================

namespace
{
    ImportedScene ImportOrFail(const std::vector<std::uint8_t> &bytes, const ModelImportSettings &settings = {})
    {
        auto result = GltfImporter().Import(bytes, {}, settings);
        if (!result)
        {
            ADD_FAILURE() << "import failed: " << ToString(result.error().code) << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    /// A unit quad facing +z with normals and uvs (glTF's v runs down: the bottom-left vertex has v = 1), indexed.
    /// `normalMapped` gives its primitive a material with a normal texture (so generateTangents = ifMissing generates).
    Builder QuadFile(const bool withNormals = true, const bool withUvs = true, const bool normalMapped = true)
    {
        Builder b;
        int material = -1;
        if (normalMapped)
        {
            const int image = b.AddImage("Normals", {1, 2, 3, 4}, "image/png");
            const int texture = b.AddTexture(image);
            nlohmann::json json;
            json["normalTexture"] = {{"index", texture}};
            material = b.AddMaterial(json);
        }
        const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
        const int normal = withNormals ? b.AddFloats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}, 3) : -1;
        const int uv = withUvs ? b.AddFloats({0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}, 2) : -1;
        const int indices = b.AddIndicesU16({0, 1, 2, 0, 2, 3});
        const int mesh = b.AddMesh("Quad", {Builder::Primitive(position, indices, material, normal, uv)});
        b.SetScene({b.AddNode("Quad", {{"mesh", mesh}})});
        return b;
    }

    /// Adds a TANGENT accessor (the same vec4 on all four vertices) to the first primitive
    void AddTangents(Builder &b, const std::array<float, 4> &tangent)
    {
        std::vector<float> values;
        for (int i = 0; i < 4; ++i)
        {
            values.insert(values.end(), tangent.begin(), tangent.end());
        }
        b.doc["meshes"][0]["primitives"][0]["attributes"]["TANGENT"] = b.AddFloats(values, 4);
    }
}

TEST(GltfTangentImportTest, ATangentlessModelGetsMikkTSpaceTangentsWithTheVFlipApplied)
{
    const ImportedScene scene = ImportOrFail(QuadFile().ToGltf());
    ASSERT_EQ(scene.meshes.size(), 1u);
    const ImportedMesh &mesh = scene.meshes[0];
    EXPECT_TRUE(mesh.generatedTangents);
    EXPECT_FALSE(mesh.generatedNormals);
    ASSERT_EQ(mesh.vertices.size(), 4u) << "no vertex needed copying";
    for (const ImportedVertex &vertex : mesh.vertices)
    {
        // u along +x, and the engine's v up +y: the same tangent space as the same quad authored v-up
        ExpectTangent(vertex, {1, 0, 0}, 1.0f, "generated");
    }
}

TEST(GltfTangentImportTest, TheFilesTangentsAreKeptAndTheirHandednessNormalised)
{
    Builder b = QuadFile();
    AddTangents(b, {0.0f, 1.0f, 0.0f, -2.5f});
    const ImportedScene scene = ImportOrFail(b.ToGltf());
    const ImportedMesh &mesh = scene.meshes[0];
    EXPECT_FALSE(mesh.generatedTangents);
    ASSERT_EQ(mesh.vertices.size(), 4u);
    for (const ImportedVertex &vertex : mesh.vertices)
    {
        ExpectTangent(vertex, {0, 1, 0}, -1.0f, "kept from the file");
    }

    Builder positive = QuadFile();
    AddTangents(positive, {1.0f, 0.0f, 0.0f, 0.0f}); // a w of 0 reads as +1
    EXPECT_FLOAT_EQ(ImportOrFail(positive.ToGltf()).meshes[0].vertices[0].tangent[3], 1.0f);
}

TEST(GltfTangentImportTest, NeverKeepsOnlyTheFilesAndAlwaysIgnoresIt)
{
    ModelImportSettings never;
    never.generateTangents = TangentGeneration::Never;
    const ImportedScene none = ImportOrFail(QuadFile().ToGltf(), never);
    EXPECT_FALSE(none.meshes[0].generatedTangents);
    for (const ImportedVertex &vertex : none.meshes[0].vertices)
    {
        for (const float component : vertex.tangent)
        {
            EXPECT_EQ(component, 0.0f) << "no tangent: drawn without normal mapping";
        }
    }

    Builder withFile = QuadFile();
    AddTangents(withFile, {0.0f, 1.0f, 0.0f, 1.0f});
    const ImportedScene kept = ImportOrFail(withFile.ToGltf(), never);
    ExpectTangent(kept.meshes[0].vertices[0], {0, 1, 0}, 1.0f, "the file's, with Never");

    ModelImportSettings always;
    always.generateTangents = TangentGeneration::Always;
    const ImportedScene regenerated = ImportOrFail(withFile.ToGltf(), always);
    EXPECT_TRUE(regenerated.meshes[0].generatedTangents);
    ExpectTangent(regenerated.meshes[0].vertices[0], {1, 0, 0}, 1.0f, "replaced by MikkTSpace");
}

TEST(GltfTangentImportTest, FlatNormalsGeneratedByTheImporterReplaceTheFilesTangentsToo)
{
    // No NORMAL: flat normals are generated, and the file's tangent (which belonged to other normals) is not used
    Builder b = QuadFile(false);
    AddTangents(b, {0.0f, 1.0f, 0.0f, 1.0f});
    const ImportedScene scene = ImportOrFail(b.ToGltf());
    const ImportedMesh &mesh = scene.meshes[0];
    EXPECT_TRUE(mesh.generatedNormals);
    EXPECT_TRUE(mesh.generatedTangents);
    EXPECT_EQ(mesh.vertices.size(), 6u);
    for (const ImportedVertex &vertex : mesh.vertices)
    {
        ExpectTangent(vertex, {1, 0, 0}, 1.0f, "generated with the generated normals");
    }
}

TEST(GltfTangentImportTest, AMeshWithoutTextureCoordinatesKeepsZeroTangents)
{
    const ImportedScene scene = ImportOrFail(QuadFile(true, false).ToGltf());
    EXPECT_FALSE(scene.meshes[0].generatedTangents);
    for (const ImportedVertex &vertex : scene.meshes[0].vertices)
    {
        EXPECT_EQ(vertex.tangent[0], 0.0f);
        EXPECT_EQ(vertex.tangent[3], 0.0f);
    }
}

TEST(GltfTangentImportTest, GeneratedTangentsKeepEverySubmeshRangeValid)
{
    // Two primitives sharing the quad's vertices; submeshes merged by material afterwards
    Builder b;
    const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
    const int normal = b.AddFloats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}, 3);
    const int uv = b.AddFloats({0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}, 2);
    const int first = b.AddIndicesU16({0, 1, 2});
    const int second = b.AddIndicesU16({0, 2, 3});
    const int image = b.AddImage("Normals", {1, 2, 3, 4}, "image/png");
    nlohmann::json bumpy;
    bumpy["normalTexture"] = {{"index", b.AddTexture(image)}};
    const int material = b.AddMaterial(bumpy);
    const int mesh = b.AddMesh("Halves", {Builder::Primitive(position, first, material, normal, uv),
                                          Builder::Primitive(position, second, material, normal, uv)});
    b.SetScene({b.AddNode("Halves", {{"mesh", mesh}})});

    for (const bool merge : {false, true})
    {
        ModelImportSettings settings;
        settings.mergeSubmeshesByMaterial = merge;
        const ImportedScene scene = ImportOrFail(b.ToGltf(), settings);
        const ImportedMesh &imported = scene.meshes[0];
        EXPECT_EQ(imported.vertices.size(), 4u) << "the shared vertices stay shared";
        std::size_t covered = 0;
        for (const ImportedSubmesh &submesh : imported.submeshes)
        {
            EXPECT_EQ(submesh.firstIndex % 3, 0u);
            EXPECT_EQ(submesh.indexCount % 3, 0u);
            EXPECT_LE(static_cast<std::size_t>(submesh.firstIndex) + submesh.indexCount, imported.indices.size());
            covered += submesh.indexCount;
        }
        EXPECT_EQ(covered, imported.indices.size());
        for (const std::uint32_t index : imported.indices)
        {
            ASSERT_LT(index, imported.vertices.size());
        }
        for (const ImportedVertex &vertex : imported.vertices)
        {
            ExpectTangent(vertex, {1, 0, 0}, 1.0f, "shared vertex");
        }
    }
}

TEST(GltfTangentImportTest, AMalformedTangentAccessorIsIgnoredWithAWarningWhateverTheSetting)
{
    Builder wrongType = QuadFile();
    wrongType.doc["meshes"][0]["primitives"][0]["attributes"]["TANGENT"] =
        wrongType.AddFloats({0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0}, 3);
    Builder wrongCount = QuadFile();
    wrongCount.doc["meshes"][0]["primitives"][0]["attributes"]["TANGENT"] =
        wrongCount.AddFloats({0, 1, 0, 1, 0, 1, 0, 1}, 4);

    for (const Builder *file : {&wrongType, &wrongCount})
    {
        for (const TangentGeneration mode : {TangentGeneration::IfMissing, TangentGeneration::Always, TangentGeneration::Never})
        {
            ModelImportSettings settings;
            settings.generateTangents = mode;
            const auto result = GltfImporter().Import(file->ToGltf(), {}, settings);
            ASSERT_TRUE(result.has_value()) << "ignored, not an error";
            bool warned = false;
            for (const std::string &warning : result->warnings)
            {
                warned = warned || warning.find("TANGENT") != std::string::npos;
            }
            EXPECT_TRUE(warned) << "a warning names it";
            if (mode != TangentGeneration::Never)
            {
                ExpectTangent(result->meshes[0].vertices[0], {1, 0, 0}, 1.0f, "generated in its place");
            }
        }
    }
}

TEST(GltfTangentImportTest, OneNonFiniteComponentMakesTheWholeTangentMissing)
{
    Builder b = QuadFile();
    AddTangents(b, {0.0f, 1.0f, std::nanf(""), 1.0f}); // (0, 1, 0) would be half read, and wrong
    const ImportedScene scene = ImportOrFail(b.ToGltf());
    for (const ImportedVertex &vertex : scene.meshes[0].vertices)
    {
        ExpectTangent(vertex, {1, 0, 0}, 1.0f, "generated, not the half read (0, 1, 0)");
    }
}

TEST(GltfTangentImportTest, IfMissingGeneratesOnlyForMeshesWithANormalMappedMaterial)
{
    // No material at all: nothing to normal map, so nothing is generated (and the vertices are not copied)
    const ImportedScene plain = ImportOrFail(QuadFile(true, true, false).ToGltf());
    EXPECT_FALSE(plain.meshes[0].generatedTangents);
    for (const ImportedVertex &vertex : plain.meshes[0].vertices)
    {
        EXPECT_EQ(vertex.tangent[0], 0.0f);
        EXPECT_EQ(vertex.tangent[3], 0.0f);
    }

    // A material without a normal texture is the same
    Builder unlit = QuadFile(true, true, false);
    const int material = unlit.AddMaterial(nlohmann::json::object());
    unlit.doc["meshes"][0]["primitives"][0]["material"] = material;
    EXPECT_FALSE(ImportOrFail(unlit.ToGltf()).meshes[0].generatedTangents);

    // With materials not imported there is no normal texture to find
    ModelImportSettings noMaterials;
    noMaterials.importMaterials = false;
    EXPECT_FALSE(ImportOrFail(QuadFile().ToGltf(), noMaterials).meshes[0].generatedTangents);

    // Always is for the tangents of a normal map assigned later: every mesh with texture coordinates
    ModelImportSettings always;
    always.generateTangents = TangentGeneration::Always;
    const ImportedScene forced = ImportOrFail(QuadFile(true, true, false).ToGltf(), always);
    EXPECT_TRUE(forced.meshes[0].generatedTangents);
    ExpectTangent(forced.meshes[0].vertices[0], {1, 0, 0}, 1.0f, "generated for no material");

    // And the normal-mapped one generates by default
    EXPECT_TRUE(ImportOrFail(QuadFile().ToGltf()).meshes[0].generatedTangents);
}

TEST(GltfTangentImportTest, ANonFiniteFileTangentIsReadAsZeroAndGenerated)
{
    Builder b = QuadFile();
    AddTangents(b, {std::nanf(""), 0.0f, 0.0f, 1.0f});
    const ImportedScene scene = ImportOrFail(b.ToGltf());
    for (const ImportedVertex &vertex : scene.meshes[0].vertices)
    {
        ExpectTangent(vertex, {1, 0, 0}, 1.0f, "generated in its place");
    }
}

TEST(GltfTangentImportTest, TheNormalTexturesScaleIsImportedAndClamped)
{
    Builder b = QuadFile(true, true, false);
    const int image = b.AddImage("Normals", {1, 2, 3, 4}, "image/png");
    const int texture = b.AddTexture(image);
    nlohmann::json scaled;
    scaled["normalTexture"] = {{"index", texture}, {"scale", 2.5}};
    nlohmann::json plain;
    plain["normalTexture"] = {{"index", texture}};
    nlohmann::json huge;
    huge["normalTexture"] = {{"index", texture}, {"scale", 100.0}};
    nlohmann::json none = nlohmann::json::object();
    nlohmann::json flipped;
    flipped["normalTexture"] = {{"index", texture}, {"scale", -2.5}};
    nlohmann::json veryNegative;
    veryNegative["normalTexture"] = {{"index", texture}, {"scale", -100.0}};
    b.AddMaterial(scaled);
    b.AddMaterial(plain);
    b.AddMaterial(huge);
    b.AddMaterial(none);
    b.AddMaterial(flipped);
    b.AddMaterial(veryNegative);

    const ImportedScene scene = ImportOrFail(b.ToGltf());
    ASSERT_EQ(scene.materials.size(), 6u);
    EXPECT_FLOAT_EQ(scene.materials[4].normalScale, -2.5f) << "a negative scale flips the map, as glTF allows";
    EXPECT_FLOAT_EQ(scene.materials[5].normalScale, -4.0f) << "clamped";
    EXPECT_FLOAT_EQ(scene.materials[0].normalScale, 2.5f);
    EXPECT_FLOAT_EQ(scene.materials[1].normalScale, 1.0f) << "glTF's default";
    EXPECT_FLOAT_EQ(scene.materials[2].normalScale, 4.0f) << "clamped";
    EXPECT_FLOAT_EQ(scene.materials[3].normalScale, 1.0f) << "no normal texture";
    EXPECT_GE(scene.materials[0].normalTexture, 0);
    EXPECT_FALSE(scene.images.empty());
    EXPECT_FALSE(scene.images[0].colour) << "a normal map is data, never sRGB";
}

TEST(GltfTangentImportTest, TheMetallicRoughnessTextureAndFactorsAreImportedAsData)
{
    Builder b = QuadFile(true, true, false);
    const int image = b.AddImage("MR", {1, 2, 3, 4}, "image/png");
    const int texture = b.AddTexture(image);
    nlohmann::json material;
    material["pbrMetallicRoughness"] = {{"metallicFactor", 0.5}, {"roughnessFactor", 0.2},
                                        {"metallicRoughnessTexture", {{"index", texture}}}};
    b.AddMaterial(material);
    const ImportedScene scene = ImportOrFail(b.ToGltf());
    ASSERT_EQ(scene.materials.size(), 1u);
    EXPECT_EQ(scene.materials[0].metallicRoughnessTexture, 0);
    EXPECT_NEAR(scene.materials[0].metallic, 0.5f, 1e-6f);
    EXPECT_NEAR(scene.materials[0].smoothness, 0.8f, 1e-6f);
    ASSERT_EQ(scene.images.size(), 1u);
    EXPECT_FALSE(scene.images[0].colour) << "green and blue are data, never sRGB";
}
