#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <assetimport/GltfImporter.hpp>
#include <nlohmann/json.hpp>

#include "GltfTestBuilder.hpp"

// GltfImporter on glTF files built in the test (JSON with base64 data: URIs, external .bin files, and GLB
// containers assembled byte by byte): the geometry and its fix-ups (index types, strips and fans, sparse
// accessors, generated normals, the v flip, primitives as submeshes), materials, the node tree (TRS and matrix
// nodes, mirroring), the warnings for unsupported features, and rejection of malformed or hostile files: out-of-
// range indices, bad strides, URIs leaving the model's folder, Draco, and a GLB cut short at every byte.

using namespace N2Engine::AssetImport;
using GltfTest::Builder;
namespace fs = std::filesystem;

namespace
{
    std::expected<ImportedScene, ModelImportError> Import(const std::vector<std::uint8_t> &bytes,
                                                          const ModelImportSettings &settings = {},
                                                          const fs::path &baseDirectory = {})
    {
        return GltfImporter().Import(bytes, baseDirectory, settings);
    }

    ImportedScene ImportOrFail(const std::vector<std::uint8_t> &bytes, const ModelImportSettings &settings = {},
                               const fs::path &baseDirectory = {})
    {
        auto result = Import(bytes, settings, baseDirectory);
        if (!result)
        {
            ADD_FAILURE() << "import failed: " << ToString(result.error().code) << ": " << result.error().message;
            return {};
        }
        return std::move(*result);
    }

    void ExpectFails(const std::vector<std::uint8_t> &bytes, const ModelImportErrorCode code, const std::string &what,
                     const fs::path &baseDirectory = {})
    {
        const auto result = Import(bytes, {}, baseDirectory);
        ASSERT_FALSE(result.has_value()) << what << ": expected an error";
        EXPECT_EQ(result.error().code, code) << what << ": " << result.error().message;
        EXPECT_FALSE(result.error().message.empty()) << what;
    }

    bool HasWarning(const ImportedScene &scene, const std::string &text)
    {
        for (const std::string &warning : scene.warnings)
        {
            if (warning.find(text) != std::string::npos)
                return true;
        }
        return false;
    }

    /// One triangle (counter-clockwise from +Z), no indices, no normals
    Builder Triangle()
    {
        Builder b;
        const int position = b.AddFloats({0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}, 3);
        const int mesh = b.AddMesh("Triangle", {Builder::Primitive(position)});
        b.SetScene({b.AddNode("Node", {{"mesh", mesh}})});
        return b;
    }

    /// A unit quad facing +Z with normals (pointing `normalZ`), UVs (v down, as glTF has it) and u16 indices
    Builder IndexedQuad(const float normalZ = 1.0f)
    {
        Builder b;
        const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
        const int normal = b.AddFloats({0, 0, normalZ, 0, 0, normalZ, 0, 0, normalZ, 0, 0, normalZ}, 3);
        // glTF UVs: (0, 0) is the image's top-left; the bottom-left vertex samples the bottom row (v = 1)
        const int uv = b.AddFloats({0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}, 2);
        const int indices = b.AddIndicesU16({0, 1, 2, 0, 2, 3});
        const int mesh = b.AddMesh("Quad", {Builder::Primitive(position, indices, -1, normal, uv)});
        b.SetScene({b.AddNode("Quad", {{"mesh", mesh}})});
        return b;
    }

    void ExpectNear3(const float *actual, const std::array<float, 3> &expected, const std::string &what,
                     const float tolerance = 1e-5f)
    {
        for (std::size_t i = 0; i < 3; ++i)
        {
            EXPECT_NEAR(actual[i], expected[i], tolerance) << what << "[" << i << "]";
        }
    }

    /// Quaternions q and -q are the same rotation
    void ExpectRotation(const float *actual, const std::array<float, 4> &expected, const std::string &what)
    {
        const float dot = actual[0] * expected[0] + actual[1] * expected[1] + actual[2] * expected[2] +
                          actual[3] * expected[3];
        EXPECT_NEAR(std::abs(dot), 1.0f, 1e-4f) << what << ": got (" << actual[0] << ", " << actual[1] << ", "
                                                << actual[2] << ", " << actual[3] << ")";
    }

    /// The face normal of triangle t (counter-clockwise front)
    std::array<float, 3> TriangleNormal(const ImportedMesh &mesh, const std::size_t t)
    {
        const auto &a = mesh.vertices[mesh.indices[t * 3]].position;
        const auto &b = mesh.vertices[mesh.indices[t * 3 + 1]].position;
        const auto &c = mesh.vertices[mesh.indices[t * 3 + 2]].position;
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        return {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    }

    /// A temporary folder, removed afterwards
    struct TempFolder
    {
        fs::path root;

        explicit TempFolder(const std::string &name)
        {
            root = fs::temp_directory_path() / ("n2engine_gltf_" + name);
            std::error_code ec;
            fs::remove_all(root, ec);
            fs::create_directories(root);
        }
        ~TempFolder()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
        TempFolder(const TempFolder &) = delete;
        TempFolder &operator=(const TempFolder &) = delete;

        void Write(const fs::path &relative, const std::vector<std::uint8_t> &bytes) const
        {
            const fs::path path = root / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary)
                .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
    };
}

// ===== Geometry =====

TEST(GltfImporterTest, ATriangleWithoutIndicesOrNormalsImports)
{
    const ImportedScene scene = ImportOrFail(Triangle().ToGltf());
    ASSERT_EQ(scene.meshes.size(), 1u);
    const ImportedMesh &mesh = scene.meshes[0];
    EXPECT_EQ(mesh.name, "Triangle");
    ASSERT_EQ(mesh.vertices.size(), 3u);
    EXPECT_EQ(mesh.indices, (std::vector<std::uint32_t>{0, 1, 2}));
    ASSERT_EQ(mesh.submeshes.size(), 1u);
    EXPECT_EQ(mesh.submeshes[0].firstIndex, 0u);
    EXPECT_EQ(mesh.submeshes[0].indexCount, 3u);
    EXPECT_EQ(mesh.submeshes[0].materialIndex, -1);
    EXPECT_TRUE(mesh.generatedNormals);
    for (const ImportedVertex &vertex : mesh.vertices)
    {
        ExpectNear3(vertex.normal, {0.0f, 0.0f, 1.0f}, "a flat normal facing the counter-clockwise side");
        EXPECT_EQ(vertex.color[3], 1.0f) << "white when the file has no COLOR_0";
    }
    ExpectNear3(mesh.vertices[1].position, {1.0f, 0.0f, 0.0f}, "position");

    ASSERT_EQ(scene.nodes.size(), 1u);
    EXPECT_EQ(scene.nodes[0].name, "Node");
    EXPECT_EQ(scene.nodes[0].meshIndex, 0);
    EXPECT_EQ(scene.rootNodes, (std::vector<std::uint32_t>{0}));
    EXPECT_TRUE(scene.warnings.empty());
}

TEST(GltfImporterTest, AnIndexedQuadKeepsItsNormalsAndFlipsV)
{
    const ImportedScene scene = ImportOrFail(IndexedQuad().ToGltf());
    ASSERT_EQ(scene.meshes.size(), 1u);
    const ImportedMesh &mesh = scene.meshes[0];
    ASSERT_EQ(mesh.vertices.size(), 4u);
    EXPECT_EQ(mesh.indices, (std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3}));
    EXPECT_FALSE(mesh.generatedNormals);
    // glTF v runs down the image; the engine's runs up: the bottom-left vertex now has v = 0
    EXPECT_FLOAT_EQ(mesh.vertices[0].texCoord[0], 0.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[0].texCoord[1], 0.0f) << "bottom-left: v' = 1 - v";
    EXPECT_FLOAT_EQ(mesh.vertices[2].texCoord[1], 1.0f) << "top-right";
    for (std::size_t t = 0; t < 2; ++t)
    {
        EXPECT_GT(TriangleNormal(mesh, t)[2], 0.0f) << "counter-clockwise from +Z";
    }
}

TEST(GltfImporterTest, U8U16AndU32IndicesGiveTheSameMesh)
{
    const std::vector<std::uint32_t> expected = {0, 1, 2, 0, 2, 3};
    for (int type = 0; type < 3; ++type)
    {
        Builder b;
        const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
        int indices = -1;
        if (type == 0)
            indices = b.AddIndicesU8({0, 1, 2, 0, 2, 3});
        else if (type == 1)
            indices = b.AddIndicesU16({0, 1, 2, 0, 2, 3});
        else
            indices = b.AddIndicesU32({0, 1, 2, 0, 2, 3});
        const int mesh = b.AddMesh("Quad", {Builder::Primitive(position, indices)});
        b.SetScene({b.AddNode("Quad", {{"mesh", mesh}})});

        ModelImportSettings keep;
        keep.generateNormals = NormalGeneration::Never; // keep the shared vertices, so the indices are comparable
        const ImportedScene scene = ImportOrFail(b.ToGltf(), keep);
        ASSERT_EQ(scene.meshes.size(), 1u) << "index type " << type;
        EXPECT_EQ(scene.meshes[0].indices, expected) << "index type " << type;
        EXPECT_EQ(scene.meshes[0].vertices.size(), 4u);
    }
}

TEST(GltfImporterTest, TwoPrimitivesBecomeTwoSubmeshesOfOneVertexBuffer)
{
    const ImportedScene scene = ImportOrFail(GltfTest::TwoMaterialQuad().ToGltf());
    ASSERT_EQ(scene.meshes.size(), 1u);
    const ImportedMesh &mesh = scene.meshes[0];
    EXPECT_EQ(mesh.name, "Halves");
    // Normals are generated flat, so each triangle has its own three vertices
    EXPECT_EQ(mesh.vertices.size(), 12u);
    EXPECT_EQ(mesh.indices.size(), 12u);
    ASSERT_EQ(mesh.submeshes.size(), 2u);
    EXPECT_EQ(mesh.submeshes[0].firstIndex, 0u);
    EXPECT_EQ(mesh.submeshes[0].indexCount, 6u);
    EXPECT_EQ(mesh.submeshes[0].materialIndex, 0);
    EXPECT_EQ(mesh.submeshes[1].firstIndex, 6u);
    EXPECT_EQ(mesh.submeshes[1].indexCount, 6u);
    EXPECT_EQ(mesh.submeshes[1].materialIndex, 1);
    for (const std::uint32_t index : mesh.indices)
    {
        EXPECT_LT(index, mesh.vertices.size());
    }
    // The second submesh's vertices are the right half
    for (std::uint32_t i = 6; i < 12; ++i)
    {
        EXPECT_GE(mesh.vertices[mesh.indices[i]].position[0], 0.0f);
    }

    ASSERT_EQ(scene.materials.size(), 2u);
    EXPECT_EQ(scene.materials[0].name, "Red");
    EXPECT_TRUE(scene.materials[0].unlit);
    EXPECT_FLOAT_EQ(scene.materials[0].baseColor[0], 1.0f);
    EXPECT_FLOAT_EQ(scene.materials[1].baseColor[1], 1.0f);
}

TEST(GltfImporterTest, PrimitivesWithTheSameAttributesShareTheirVertices)
{
    // One quad's vertices, drawn by two primitives (one triangle each) with two materials, as exporters write a
    // mesh whose faces have different materials
    Builder b;
    const int red = b.AddUnlitMaterial("Red", 1, 0, 0);
    const int green = b.AddUnlitMaterial("Green", 0, 1, 0);
    const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
    const int normal = b.AddFloats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}, 3);
    const int first = b.AddIndicesU16({0, 1, 2});
    const int second = b.AddIndicesU16({0, 2, 3});
    b.SetScene({b.AddNode("N", {{"mesh", b.AddMesh("Shared", {Builder::Primitive(position, first, red, normal),
                                                               Builder::Primitive(position, second, green, normal)})}})});

    const ImportedScene scene = ImportOrFail(b.ToGltf());
    const ImportedMesh &mesh = scene.meshes[0];
    EXPECT_EQ(mesh.vertices.size(), 4u) << "the four vertices once, not once per primitive";
    EXPECT_EQ(mesh.indices, (std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3}));
    ASSERT_EQ(mesh.submeshes.size(), 2u);
    EXPECT_EQ(mesh.submeshes[1].materialIndex, green);
}

TEST(GltfImporterTest, MergeSubmeshesByMaterialJoinsPrimitivesThatShareAMaterial)
{
    Builder b;
    const int a = b.AddUnlitMaterial("A", 1, 0, 0);
    const int c = b.AddUnlitMaterial("B", 0, 0, 1);
    std::vector<nlohmann::json> primitives;
    for (const int material : {a, c, a})
    {
        primitives.push_back(Builder::Primitive(b.AddFloats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3), -1, material));
    }
    b.SetScene({b.AddNode("N", {{"mesh", b.AddMesh("M", primitives)}})});

    const ImportedScene apart = ImportOrFail(b.ToGltf());
    ASSERT_EQ(apart.meshes[0].submeshes.size(), 3u);

    ModelImportSettings merge;
    merge.mergeSubmeshesByMaterial = true;
    const ImportedScene merged = ImportOrFail(b.ToGltf(), merge);
    const ImportedMesh &mesh = merged.meshes[0];
    ASSERT_EQ(mesh.submeshes.size(), 2u);
    EXPECT_EQ(mesh.submeshes[0].materialIndex, a);
    EXPECT_EQ(mesh.submeshes[0].indexCount, 6u) << "both triangles with material A";
    EXPECT_EQ(mesh.submeshes[1].materialIndex, c);
    EXPECT_EQ(mesh.submeshes[1].firstIndex, 6u);
    EXPECT_EQ(mesh.indices.size(), 9u);
}

TEST(GltfImporterTest, StripsAndFansBecomeListsWithTheirWindingKept)
{
    // Four vertices, as a strip: bottom-left, bottom-right, top-left, top-right
    const std::vector<float> stripPositions = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, -0.5f, 0.5f, 0, 0.5f, 0.5f, 0};
    ModelImportSettings keep;
    keep.generateNormals = NormalGeneration::Never;
    {
        Builder b;
        const int position = b.AddFloats(stripPositions, 3);
        const int mesh = b.AddMesh("Strip", {Builder::Primitive(position, -1, -1, -1, -1, GltfTest::kModeTriangleStrip)});
        b.SetScene({b.AddNode("Strip", {{"mesh", mesh}})});
        const ImportedScene scene = ImportOrFail(b.ToGltf(), keep);
        ASSERT_EQ(scene.meshes.size(), 1u);
        EXPECT_EQ(scene.meshes[0].indices, (std::vector<std::uint32_t>{0, 1, 2, 1, 3, 2}));
        for (std::size_t t = 0; t < 2; ++t)
        {
            EXPECT_GT(TriangleNormal(scene.meshes[0], t)[2], 0.0f) << "strip triangle " << t << " faces +Z";
        }
    }
    {
        // A fan around the bottom-left corner: bottom-left, bottom-right, top-right, top-left
        Builder b;
        const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
        const int mesh = b.AddMesh("Fan", {Builder::Primitive(position, -1, -1, -1, -1, GltfTest::kModeTriangleFan)});
        b.SetScene({b.AddNode("Fan", {{"mesh", mesh}})});
        const ImportedScene scene = ImportOrFail(b.ToGltf(), keep);
        ASSERT_EQ(scene.meshes.size(), 1u);
        ASSERT_EQ(scene.meshes[0].indices.size(), 6u);
        for (std::size_t t = 0; t < 2; ++t)
        {
            EXPECT_GT(TriangleNormal(scene.meshes[0], t)[2], 0.0f) << "fan triangle " << t << " faces +Z";
        }
    }
}

TEST(GltfImporterTest, MissingNormalsAreGeneratedFlatUnlessTurnedOff)
{
    Builder b;
    const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
    const int indices = b.AddIndicesU16({0, 1, 2, 0, 2, 3});
    b.SetScene({b.AddNode("Q", {{"mesh", b.AddMesh("Q", {Builder::Primitive(position, indices)})}})});

    const ImportedScene generated = ImportOrFail(b.ToGltf());
    const ImportedMesh &flat = generated.meshes[0];
    EXPECT_TRUE(flat.generatedNormals);
    EXPECT_EQ(flat.vertices.size(), 6u) << "flat: one vertex per triangle corner";
    for (const ImportedVertex &vertex : flat.vertices)
    {
        ExpectNear3(vertex.normal, {0.0f, 0.0f, 1.0f}, "generated normal");
    }

    ModelImportSettings never;
    never.generateNormals = NormalGeneration::Never;
    const ImportedScene kept = ImportOrFail(b.ToGltf(), never);
    EXPECT_FALSE(kept.meshes[0].generatedNormals);
    EXPECT_EQ(kept.meshes[0].vertices.size(), 4u);

    // Always replaces the file's normals, even wrong ones
    ModelImportSettings always;
    always.generateNormals = NormalGeneration::Always;
    const ImportedScene regenerated = ImportOrFail(IndexedQuad(-1.0f).ToGltf(), always);
    EXPECT_TRUE(regenerated.meshes[0].generatedNormals);
    ExpectNear3(regenerated.meshes[0].vertices[0].normal, {0.0f, 0.0f, 1.0f}, "regenerated");
    const ImportedScene asFiled = ImportOrFail(IndexedQuad(-1.0f).ToGltf());
    ExpectNear3(asFiled.meshes[0].vertices[0].normal, {0.0f, 0.0f, -1.0f}, "the file's normal is kept by default");
}

TEST(GltfImporterTest, NormalisedAndIntegerAttributesAreConverted)
{
    Builder b;
    const int position = b.AddFloats(GltfTest::QuadPositions(), 3);
    // UVs as normalised unsigned shorts, colours as normalised unsigned bytes (RGB)
    const std::vector<std::uint16_t> uvs = {0, 65535, 65535, 65535, 65535, 0, 0, 0};
    const int uv = b.AddAccessor(b.AddView(uvs), GltfTest::kUnsignedShort, 4, "VEC2", true);
    const std::vector<std::uint8_t> colours = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
    const int colour = b.AddAccessor(b.AddView(colours), GltfTest::kUnsignedByte, 4, "VEC3", true);
    nlohmann::json primitive = Builder::Primitive(position, b.AddIndicesU8({0, 1, 2, 0, 2, 3}), -1, -1, uv);
    primitive["attributes"]["COLOR_0"] = colour;
    b.SetScene({b.AddNode("Q", {{"mesh", b.AddMesh("Q", {primitive})}})});

    ModelImportSettings keep;
    keep.generateNormals = NormalGeneration::Never;
    const ImportedScene scene = ImportOrFail(b.ToGltf(), keep);
    const ImportedMesh &mesh = scene.meshes[0];
    ASSERT_EQ(mesh.vertices.size(), 4u);
    EXPECT_FLOAT_EQ(mesh.vertices[1].texCoord[0], 1.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[1].texCoord[1], 0.0f) << "v = 1 flipped to 0";
    EXPECT_FLOAT_EQ(mesh.vertices[0].color[0], 1.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[0].color[1], 0.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[2].color[2], 1.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[2].color[3], 1.0f) << "alpha 1 for an RGB colour";
}

TEST(GltfImporterTest, SparseAccessorsSubstituteTheirValues)
{
    // A triangle whose second vertex is replaced by a sparse substitution, on top of a base view
    Builder b;
    const int base = b.AddFloats({0, 0, 0, 9, 9, 9, 0, 1, 0}, 3);
    const std::vector<std::uint32_t> sparseIndices = {1};
    const std::vector<float> sparseValues = {1, 0, 0};
    const int indexView = b.AddView(sparseIndices);
    const int valueView = b.AddView(sparseValues);
    b.doc["accessors"][static_cast<std::size_t>(base)]["sparse"] = {
        {"count", 1},
        {"indices", {{"bufferView", indexView}, {"componentType", GltfTest::kUnsignedInt}}},
        {"values", {{"bufferView", valueView}}}};
    b.doc["accessors"][static_cast<std::size_t>(base)].erase("min");
    b.doc["accessors"][static_cast<std::size_t>(base)].erase("max");

    // And one with no base view at all: zeros except the substituted element
    const int zeros = b.AddAccessor(-1, GltfTest::kFloat, 3, "VEC3");
    const std::vector<std::uint16_t> zeroIndices = {2};
    const std::vector<float> zeroValues = {0, 0, 1};
    b.doc["accessors"][static_cast<std::size_t>(zeros)]["sparse"] = {
        {"count", 1},
        {"indices", {{"bufferView", b.AddView(zeroIndices)}, {"componentType", GltfTest::kUnsignedShort}}},
        {"values", {{"bufferView", b.AddView(zeroValues)}}}};

    nlohmann::json primitive = Builder::Primitive(base);
    primitive["attributes"]["NORMAL"] = zeros;
    b.SetScene({b.AddNode("N", {{"mesh", b.AddMesh("Sparse", {primitive})}})});

    const ImportedScene scene = ImportOrFail(b.ToGltf());
    const ImportedMesh &mesh = scene.meshes[0];
    ASSERT_EQ(mesh.vertices.size(), 3u);
    ExpectNear3(mesh.vertices[1].position, {1.0f, 0.0f, 0.0f}, "substituted position");
    ExpectNear3(mesh.vertices[0].position, {0.0f, 0.0f, 0.0f}, "base position");
    ExpectNear3(mesh.vertices[2].normal, {0.0f, 0.0f, 1.0f}, "substituted normal");
    ExpectNear3(mesh.vertices[0].normal, {0.0f, 0.0f, 0.0f}, "a zero normal with no base view");
    EXPECT_FALSE(mesh.generatedNormals);
}

TEST(GltfImporterTest, AGlbRoundTripsToTheSameSceneAsItsGltf)
{
    const Builder b = GltfTest::TwoMaterialQuad();
    const ImportedScene fromGltf = ImportOrFail(b.ToGltf());
    const std::vector<std::uint8_t> glb = b.ToGlb();
    ASSERT_EQ(static_cast<char>(glb[0]), 'g');
    const ImportedScene fromGlb = ImportOrFail(glb);
    ASSERT_EQ(fromGlb.meshes.size(), fromGltf.meshes.size());
    EXPECT_EQ(fromGlb.meshes[0].indices, fromGltf.meshes[0].indices);
    ASSERT_EQ(fromGlb.meshes[0].vertices.size(), fromGltf.meshes[0].vertices.size());
    for (std::size_t i = 0; i < fromGlb.meshes[0].vertices.size(); ++i)
    {
        EXPECT_EQ(std::memcmp(&fromGlb.meshes[0].vertices[i], &fromGltf.meshes[0].vertices[i], sizeof(ImportedVertex)), 0)
            << "vertex " << i;
    }
    ASSERT_EQ(fromGlb.materials.size(), 2u);
    EXPECT_EQ(fromGlb.materials[1].name, "Green");
    EXPECT_EQ(fromGlb.nodes[0].name, "Quad");
}

TEST(GltfImporterTest, AnExternalBinInTheModelsFolderLoadsWithItsUriPercentDecoded)
{
    const TempFolder folder("external");
    const Builder b = GltfTest::TwoMaterialQuad();
    folder.Write("models/mesh data.bin", b.bin);
    const ImportedScene scene = ImportOrFail(b.ToGltfExternal("mesh%20data.bin"), {}, folder.root / "models");
    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].submeshes.size(), 2u);

    // A sub-folder is fine too
    folder.Write("models/data/mesh.bin", b.bin);
    EXPECT_TRUE(Import(b.ToGltfExternal("data/mesh.bin"), {}, folder.root / "models").has_value());

    // A .bin shorter than the buffer says
    std::vector<std::uint8_t> shortBin = b.bin;
    shortBin.resize(shortBin.size() / 2);
    folder.Write("models/short.bin", shortBin);
    ExpectFails(b.ToGltfExternal("short.bin"), ModelImportErrorCode::BufferLoadFailed, "short .bin", folder.root / "models");
    ExpectFails(b.ToGltfExternal("missing.bin"), ModelImportErrorCode::BufferLoadFailed, "missing .bin",
                folder.root / "models");
    ExpectFails(b.ToGltfExternal("mesh.bin"), ModelImportErrorCode::BufferLoadFailed, "no folder given");
}

// ===== Nodes =====

TEST(GltfImporterTest, ANodeTreeKeepsTrsAndDecomposesMatrices)
{
    Builder b;
    const int mesh = b.AddMesh("T", {Builder::Primitive(b.AddFloats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3))});
    const float s = std::sqrt(0.5f);
    // Child: translation (4, 5, 6), 90 degrees about z, scale (1, 2, 3), as a column-major matrix
    const std::vector<float> matrix = {0, 1, 0, 0, -2, 0, 0, 0, 0, 0, 3, 0, 4, 5, 6, 1};
    const int child = b.AddNode("Child", {{"matrix", matrix}, {"mesh", mesh}});
    const int leaf = b.AddNode("Leaf");
    b.doc["nodes"][static_cast<std::size_t>(child)]["children"] = {leaf};
    const int root = b.AddNode("Root", {{"translation", {1, 2, 3}}, {"rotation", {0, s, 0, s}}, {"scale", {2, 2, 2}},
                                        {"children", {child}}});
    const int other = b.AddNode("Other");
    b.SetScene({root, other});

    const ImportedScene scene = ImportOrFail(b.ToGltf());
    ASSERT_EQ(scene.nodes.size(), 4u);
    EXPECT_EQ(scene.rootNodes, (std::vector<std::uint32_t>{static_cast<std::uint32_t>(root), static_cast<std::uint32_t>(other)}));
    const ImportedNode &rootNode = scene.nodes[static_cast<std::size_t>(root)];
    EXPECT_EQ(rootNode.name, "Root");
    ExpectNear3(rootNode.translation, {1, 2, 3}, "root translation");
    ExpectRotation(rootNode.rotation, {0, s, 0, s}, "root rotation");
    ExpectNear3(rootNode.scale, {2, 2, 2}, "root scale");
    EXPECT_EQ(rootNode.meshIndex, -1);
    EXPECT_EQ(rootNode.children, (std::vector<std::uint32_t>{static_cast<std::uint32_t>(child)}));

    const ImportedNode &childNode = scene.nodes[static_cast<std::size_t>(child)];
    EXPECT_EQ(childNode.meshIndex, mesh);
    ExpectNear3(childNode.translation, {4, 5, 6}, "matrix translation");
    ExpectRotation(childNode.rotation, {0, 0, s, s}, "matrix rotation (90 degrees about z)");
    ExpectNear3(childNode.scale, {1, 2, 3}, "matrix scale");
    EXPECT_EQ(childNode.children, (std::vector<std::uint32_t>{static_cast<std::uint32_t>(leaf)}));
}

TEST(GltfImporterTest, NegativeScaleIsKeptAndAMirroringMatrixGetsANegativeXScale)
{
    Builder b;
    const int mesh = b.AddMesh("T", {Builder::Primitive(b.AddFloats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3))});
    const int scaled = b.AddNode("Scaled", {{"scale", {-1, 1, 1}}, {"mesh", mesh}});
    const std::vector<float> mirror = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1}; // z mirrored
    const int matrix = b.AddNode("Matrix", {{"matrix", mirror}, {"mesh", mesh}});
    b.SetScene({scaled, matrix});

    const ImportedScene scene = ImportOrFail(b.ToGltf());
    ExpectNear3(scene.nodes[static_cast<std::size_t>(scaled)].scale, {-1, 1, 1}, "TRS scale untouched");
    const ImportedNode &decomposed = scene.nodes[static_cast<std::size_t>(matrix)];
    EXPECT_LT(decomposed.scale[0] * decomposed.scale[1] * decomposed.scale[2], 0.0f) << "still mirrored";
    EXPECT_NEAR(std::abs(decomposed.scale[0]), 1.0f, 1e-5f);
    // x-mirror times a 180-degree turn about y is the z-mirror
    ExpectRotation(decomposed.rotation, {0, 1, 0, 0}, "the mirror's rotation part");
}

TEST(GltfImporterTest, TheScaleSettingScalesPositionsAndTranslations)
{
    Builder b = Triangle();
    b.doc["nodes"][0]["translation"] = {100, 0, 0};
    ModelImportSettings settings;
    settings.scale = 0.01f;
    const ImportedScene scene = ImportOrFail(b.ToGltf(), settings);
    ExpectNear3(scene.meshes[0].vertices[1].position, {0.01f, 0, 0}, "scaled position");
    ExpectNear3(scene.nodes[0].translation, {1, 0, 0}, "scaled translation");
}

TEST(GltfImporterTest, ATooDeepNodeTreeIsRejected)
{
    Builder b;
    int previous = -1;
    for (int i = 0; i < static_cast<int>(kMaxNodeDepth) + 5; ++i)
    {
        const int node = b.AddNode("N" + std::to_string(i));
        if (previous >= 0)
        {
            b.doc["nodes"][static_cast<std::size_t>(node)]["children"] = {previous};
        }
        previous = node;
    }
    b.SetScene({previous});
    ExpectFails(b.ToGltf(), ModelImportErrorCode::InvalidData, "too deep");
}

// ===== Materials =====

TEST(GltfImporterTest, MaterialsKeepTheirFieldsWithRoughnessAsSmoothness)
{
    Builder b = Triangle();
    const std::vector<std::uint8_t> imageBytes = {1, 2, 3, 4}; // not decoded by the importer
    const int image = b.AddImage("Albedo", imageBytes, "image/png");
    const int texture = b.AddTexture(image, true, true);
    nlohmann::json material;
    material["name"] = "Painted";
    material["pbrMetallicRoughness"] = {{"baseColorFactor", {0.5, 0.25, 1.0, 0.75}},
                                        {"baseColorTexture", {{"index", texture}}},
                                        {"metallicFactor", 0.3},
                                        {"roughnessFactor", 0.25}};
    material["normalTexture"] = {{"index", texture}};
    material["emissiveFactor"] = {0.1, 0.2, 0.3};
    material["alphaMode"] = "MASK";
    material["alphaCutoff"] = 0.4;
    material["doubleSided"] = true;
    const int painted = b.AddMaterial(material);
    const int plain = b.AddMaterial(nlohmann::json::object());
    b.doc["meshes"][0]["primitives"][0]["material"] = painted;

    const ImportedScene scene = ImportOrFail(b.ToGltf());
    ASSERT_EQ(scene.materials.size(), 2u);
    const ImportedMaterial &m = scene.materials[static_cast<std::size_t>(painted)];
    EXPECT_EQ(m.name, "Painted");
    EXPECT_FALSE(m.unlit);
    EXPECT_FLOAT_EQ(m.baseColor[1], 0.25f);
    EXPECT_FLOAT_EQ(m.baseColor[3], 0.75f);
    EXPECT_EQ(m.baseColorTexture, image);
    EXPECT_EQ(m.normalTexture, image);
    EXPECT_NEAR(m.smoothness, 0.75f, 1e-6f) << "1 - roughness";
    EXPECT_NEAR(m.metallic, 0.3f, 1e-6f);
    EXPECT_NEAR(m.emissive[2], 0.3f, 1e-6f);
    EXPECT_EQ(m.alphaMode, ImportedAlphaMode::Mask);
    EXPECT_NEAR(m.alphaCutoff, 0.4f, 1e-6f);
    EXPECT_TRUE(m.doubleSided);
    EXPECT_EQ(scene.meshes[0].submeshes[0].materialIndex, painted);

    const ImportedMaterial &defaults = scene.materials[static_cast<std::size_t>(plain)];
    EXPECT_FLOAT_EQ(defaults.baseColor[0], 1.0f);
    EXPECT_FLOAT_EQ(defaults.smoothness, 0.0f) << "glTF's default roughness is 1";
    EXPECT_FLOAT_EQ(defaults.metallic, 1.0f) << "glTF's default metallic is 1";
    EXPECT_EQ(defaults.alphaMode, ImportedAlphaMode::Opaque);

    ASSERT_EQ(scene.images.size(), 1u);
    EXPECT_EQ(scene.images[0].name, "Albedo");
    EXPECT_EQ(scene.images[0].bytes, imageBytes);
    EXPECT_EQ(scene.images[0].mimeType, "image/png");
    EXPECT_TRUE(scene.images[0].nearest);
    EXPECT_TRUE(scene.images[0].clamp);
    EXPECT_TRUE(scene.images[0].colour) << "used as base colour";

    ModelImportSettings noMaterials;
    noMaterials.importMaterials = false;
    const ImportedScene bare = ImportOrFail(b.ToGltf(), noMaterials);
    EXPECT_TRUE(bare.materials.empty());
    EXPECT_TRUE(bare.images.empty());
    EXPECT_EQ(bare.meshes[0].submeshes[0].materialIndex, -1);
}

TEST(GltfImporterTest, ImagesInDataUrisAndInTheModelsFolderAreRead)
{
    const TempFolder folder("images");
    Builder b = Triangle();
    b.doc["images"] = nlohmann::json::array(
        {nlohmann::json{{"uri", "data:image/png;base64," + GltfTest::Base64({9, 8, 7})}},
         nlohmann::json{{"uri", "tex%20tures/albedo.png"}},
         nlohmann::json{{"uri", "../escape.png"}}});
    folder.Write("model/tex tures/albedo.png", {5, 6});
    folder.Write("escape.png", {1});

    const ImportedScene scene = ImportOrFail(b.ToGltf(), {}, folder.root / "model");
    ASSERT_EQ(scene.images.size(), 3u);
    EXPECT_EQ(scene.images[0].bytes, (std::vector<std::uint8_t>{9, 8, 7}));
    EXPECT_EQ(scene.images[1].bytes, (std::vector<std::uint8_t>{5, 6}));
    EXPECT_EQ(scene.images[1].uri, "tex tures/albedo.png");
    EXPECT_TRUE(scene.images[2].bytes.empty()) << "an image outside the model's folder isn't read";
    EXPECT_TRUE(HasWarning(scene, "../escape.png"));
}

// ===== Unsupported features =====

TEST(GltfImporterTest, UnsupportedFeaturesAreIgnoredWithOneWarningEach)
{
    Builder b = Triangle();
    b.doc["cameras"] = nlohmann::json::array(
        {nlohmann::json{{"type", "perspective"}, {"perspective", {{"yfov", 1.0}, {"znear", 0.1}}}}});
    b.doc["nodes"][0]["camera"] = 0;
    const int second = b.AddNode("Camera2", {{"camera", 0}});
    b.doc["scenes"][0]["nodes"].push_back(second);
    // A second UV set, KHR_texture_transform, and an extension nobody knows
    b.doc["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_1"] = b.AddFloats({0, 0, 1, 0, 0, 1}, 2);
    b.UseExtension("KHR_texture_transform");
    b.UseExtension("EXT_something_new");
    // A points primitive, skipped
    b.doc["meshes"][0]["primitives"].push_back(
        Builder::Primitive(b.AddFloats({0, 0, 0, 1, 1, 1}, 3), -1, -1, -1, -1, GltfTest::kModePoints));

    const ImportedScene scene = ImportOrFail(b.ToGltf());
    EXPECT_EQ(scene.meshes[0].submeshes.size(), 1u) << "the points primitive is skipped";
    EXPECT_TRUE(HasWarning(scene, "cameras are ignored"));
    EXPECT_TRUE(HasWarning(scene, "TEXCOORD_1"));
    EXPECT_TRUE(HasWarning(scene, "KHR_texture_transform"));
    EXPECT_TRUE(HasWarning(scene, "EXT_something_new"));
    EXPECT_TRUE(HasWarning(scene, "points and lines"));
    int cameraWarnings = 0;
    for (const std::string &warning : scene.warnings)
    {
        cameraWarnings += warning.find("cameras") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(cameraWarnings, 1) << "said once, though two nodes have cameras";
}

TEST(GltfImporterTest, DracoAndMeshoptAreErrors)
{
    for (const char *extension : {"KHR_draco_mesh_compression", "EXT_meshopt_compression"})
    {
        Builder used = Triangle();
        used.UseExtension(extension);
        ExpectFails(used.ToGltf(), ModelImportErrorCode::Unsupported, std::string(extension) + " used");
        Builder required = Triangle();
        required.UseExtension(extension, true);
        ExpectFails(required.ToGltf(), ModelImportErrorCode::Unsupported, std::string(extension) + " required");
    }
}

// ===== Malformed and hostile files =====

TEST(GltfImporterTest, OutOfRangeIndicesAreRejected)
{
    Builder b;
    const int position = b.AddFloats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3);
    const int indices = b.AddIndicesU16({0, 1, 3});
    b.SetScene({b.AddNode("N", {{"mesh", b.AddMesh("M", {Builder::Primitive(position, indices)})}})});
    ExpectFails(b.ToGltf(), ModelImportErrorCode::InvalidData, "index 3 of 3 vertices");
    ExpectFails(b.ToGlb(), ModelImportErrorCode::InvalidData, "index 3 of 3 vertices, GLB");
}

TEST(GltfImporterTest, BadStridesAndRangesAreRejected)
{
    {
        // POSITION's view says its elements are 4 bytes apart; they are 12 bytes each
        Builder b;
        const std::vector<float> positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const int view = b.AddView(positions, 4);
        const int position = b.AddAccessor(view, GltfTest::kFloat, 3, "VEC3");
        b.SetScene({b.AddNode("N", {{"mesh", b.AddMesh("M", {Builder::Primitive(position)})}})});
        ExpectFails(b.ToGltf(), ModelImportErrorCode::InvalidData, "stride below the element size");
    }
    {
        // More elements than the view holds
        Builder b;
        const int view = b.AddView(std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
        const int position = b.AddAccessor(view, GltfTest::kFloat, 30, "VEC3");
        b.SetScene({b.AddNode("N", {{"mesh", b.AddMesh("M", {Builder::Primitive(position)})}})});
        ExpectFails(b.ToGltf(), ModelImportErrorCode::InvalidData, "accessor past its view");
    }
    {
        // A view past its buffer, and an offset so large the sums would wrap
        Builder b = Triangle();
        b.doc["bufferViews"][0]["byteOffset"] = 4000000000ull;
        ExpectFails(b.ToGltf(), ModelImportErrorCode::InvalidData, "view past its buffer");
        Builder wrap = Triangle();
        wrap.doc["accessors"][0]["byteOffset"] = 18446744073709551600ull;
        ExpectFails(wrap.ToGltf(), ModelImportErrorCode::InvalidData, "wrapping accessor offset");
    }
    {
        // A sparse index past the accessor's elements
        Builder b = Triangle();
        const int indexView = b.AddView(std::vector<std::uint32_t>{7});
        const int valueView = b.AddView(std::vector<float>{1, 1, 1});
        b.doc["accessors"][0]["sparse"] = {
            {"count", 1},
            {"indices", {{"bufferView", indexView}, {"componentType", GltfTest::kUnsignedInt}}},
            {"values", {{"bufferView", valueView}}}};
        ExpectFails(b.ToGltf(), ModelImportErrorCode::InvalidData, "sparse index past the end");
    }
}

TEST(GltfImporterTest, UrisThatLeaveTheModelsFolderAreRejected)
{
    const TempFolder folder("escape");
    const Builder b = GltfTest::TwoMaterialQuad();
    folder.Write("outside.bin", b.bin);
    folder.Write("models/inside.bin", b.bin);
    const fs::path models = folder.root / "models";

    ASSERT_TRUE(Import(b.ToGltfExternal("inside.bin"), {}, models).has_value()) << "the control case loads";
    for (const char *uri : {"../outside.bin", "%2E%2E/outside.bin", "sub/../../outside.bin", "..%5Coutside.bin",
                            "/outside.bin", "C:/outside.bin", "file:///outside.bin", "http://example.com/a.bin",
                            "inside.bin%00.png"})
    {
        ExpectFails(b.ToGltfExternal(uri), ModelImportErrorCode::BufferLoadFailed, uri, models);
    }
}

TEST(GltfImporterTest, ResolveModelUriStaysInsideItsFolder)
{
    const fs::path base = fs::temp_directory_path() / "n2engine_model_dir";
    EXPECT_EQ(ResolveModelUri(base, "a.bin"), (base / "a.bin").lexically_normal());
    EXPECT_EQ(ResolveModelUri(base, "sub/./b.bin"), (base / "sub" / "b.bin").lexically_normal());
    EXPECT_EQ(ResolveModelUri(base, "sub/../c.bin"), (base / "c.bin").lexically_normal());
    EXPECT_TRUE(ResolveModelUri(base, "../a.bin").empty());
    EXPECT_TRUE(ResolveModelUri(base, "..").empty());
    EXPECT_TRUE(ResolveModelUri(base, ".").empty());
    EXPECT_TRUE(ResolveModelUri(base, "").empty());
    EXPECT_TRUE(ResolveModelUri(base, "D:x.bin").empty());
    EXPECT_TRUE(ResolveModelUri(base, "\\\\server\\share\\a.bin").empty());
    EXPECT_TRUE(ResolveModelUri({}, "a.bin").empty());
    EXPECT_EQ(PercentDecode("a%20b%2Fc%zz%4"), "a b/c%zz%4");
}

TEST(GltfImporterTest, AGlbCutShortAtAnyByteIsAnErrorNotACrash)
{
    const std::vector<std::uint8_t> glb = GltfTest::TwoMaterialQuad().ToGlb();
    ASSERT_TRUE(Import(glb).has_value());
    for (std::size_t length = 0; length < glb.size(); ++length)
    {
        const std::vector<std::uint8_t> truncated(glb.begin(), glb.begin() + static_cast<std::ptrdiff_t>(length));
        const auto result = Import(truncated);
        EXPECT_FALSE(result.has_value()) << "truncated to " << length << " of " << glb.size() << " bytes";
    }
}

TEST(GltfImporterTest, AGltfCutShortAtAnyByteIsAnErrorNotACrash)
{
    const std::vector<std::uint8_t> gltf = Triangle().ToGltf();
    for (std::size_t length = 0; length < gltf.size(); ++length)
    {
        const std::vector<std::uint8_t> truncated(gltf.begin(), gltf.begin() + static_cast<std::ptrdiff_t>(length));
        EXPECT_FALSE(Import(truncated).has_value()) << "truncated to " << length << " bytes";
    }
}

TEST(GltfImporterTest, CorruptGlbHeadersAndGarbageAreRejected)
{
    ExpectFails({}, ModelImportErrorCode::EmptyInput, "empty");
    ExpectFails(GltfTest::ToBytes("this is not a model"), ModelImportErrorCode::ParseFailed, "garbage");

    std::vector<std::uint8_t> glb = GltfTest::TwoMaterialQuad().ToGlb();
    {
        auto version1 = glb;
        version1[4] = 1;
        ExpectFails(version1, ModelImportErrorCode::ParseFailed, "GLB version 1");
    }
    {
        auto hugeJson = glb;
        hugeJson[12] = 0xFF;
        hugeJson[13] = 0xFF;
        hugeJson[14] = 0xFF;
        hugeJson[15] = 0x7F;
        ExpectFails(hugeJson, ModelImportErrorCode::ParseFailed, "JSON chunk past the file");
    }
    {
        auto notJson = glb;
        notJson[16] = 'X';
        ExpectFails(notJson, ModelImportErrorCode::ParseFailed, "first chunk not JSON");
    }
    {
        // The BIN chunk shorter than buffer 0 says: shrink the declared buffer length's backing by cutting the
        // chunk (and fixing up the lengths so the container itself is consistent)
        const GltfTest::Builder b = GltfTest::TwoMaterialQuad();
        std::vector<std::uint8_t> shortBin(b.bin.begin(), b.bin.begin() + 8);
        ExpectFails(GltfTest::MakeGlb(b.WithBuffer("").dump(), shortBin), ModelImportErrorCode::BufferLoadFailed,
                    "BIN chunk shorter than the buffer");
    }
}

TEST(GltfImporterTest, HandlesTheGltfExtensions)
{
    const GltfImporter importer;
    EXPECT_TRUE(importer.HandlesExtension(".gltf"));
    EXPECT_TRUE(importer.HandlesExtension(".glb"));
    EXPECT_FALSE(importer.HandlesExtension(".fbx"));
    EXPECT_FALSE(importer.GetName().empty());
}
