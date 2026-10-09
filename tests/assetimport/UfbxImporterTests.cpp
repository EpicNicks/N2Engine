// UfbxImporter (#3 P5), built only with N2ENGINE_MODEL_UFBX: ASCII FBX files written by the tests (a triangle, a quad,
// materials, in y-up metres and in z-up centimetres), the committed tests/assets/models/textured_cube.fbx, small OBJ
// files with their .mtl, and the same assertions on that cube through cgltf and ufbx (as the font backends are
// compared). The files are plain text, so no binary test data is committed except the glTF cube that already is.
#ifdef N2ENGINE_MODEL_UFBX

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <assetimport/GltfImporter.hpp>
#include <assetimport/UfbxImporter.hpp>

#include "TestImages.hpp"

using namespace N2Engine::AssetImport;
namespace fs = std::filesystem;

namespace
{
    using Bytes = std::vector<std::uint8_t>;

    Bytes ToBytes(const std::string &text)
    {
        return Bytes(text.begin(), text.end());
    }

    std::string ReadText(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    Bytes ReadBytes(const fs::path &path)
    {
        return ToBytes(ReadText(path));
    }

    fs::path ModelsDirectory()
    {
        return fs::path(N2_TEST_ASSETS_DIR) / "models";
    }

    std::expected<ImportedScene, ModelImportError> Import(const Bytes &bytes, const ModelImportSettings &settings = {},
                                                          const fs::path &baseDirectory = {})
    {
        return UfbxImporter().Import(bytes, baseDirectory, settings);
    }

    ImportedScene ImportOrFail(const Bytes &bytes, const ModelImportSettings &settings = {},
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

    bool HasWarning(const ImportedScene &scene, const std::string &text)
    {
        return std::ranges::any_of(scene.warnings, [&](const std::string &warning)
        {
            return warning.find(text) != std::string::npos;
        });
    }

    struct TempFolder
    {
        fs::path root;

        explicit TempFolder(const std::string &name)
        {
            root = fs::temp_directory_path() / ("n2engine_ufbx_" + name);
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

        void Write(const fs::path &relative, const Bytes &bytes) const
        {
            const fs::path path = root / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary)
                .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        void Write(const fs::path &relative, const std::string &text) const
        {
            Write(relative, ToBytes(text));
        }
    };

    /// A 2 x 2 PNG, for the texture files the models name
    Bytes TestPng()
    {
        AssetImportTests::PngSpec spec;
        spec.width = 2;
        spec.height = 2;
        spec.samples = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
        return AssetImportTests::MakePng(spec);
    }

    // ===== ASCII FBX written here =====

    struct FbxSpec
    {
        /// FBX axis settings: y up, +z front (FBX's default), or z up with -y front (Blender's and Maya's z up)
        bool zUp = false;
        /// UnitScaleFactor: the unit's size in centimetres (100 is metres, 1 is centimetres)
        double unitScaleFactor = 100.0;
        std::vector<double> positions;
        /// Polygons' indices, the last of each already stored as -(i) - 1
        std::vector<int> polygonIndices;
        /// Per polygon vertex (ByPolygonVertex) or per control point (ByVertice); empty for none
        std::vector<double> normals;
        std::vector<double> uvs;
        bool attributesByControlPoint = false;
        /// One entry per polygon; empty for no material layer
        std::vector<int> polygonMaterials;
        struct Material
        {
            std::string name;
            std::array<double, 3> diffuse = {1.0, 1.0, 1.0};
        };
        std::vector<Material> materials;
        std::array<double, 3> translation = {0.0, 0.0, 0.0};
    };

    std::string Numbers(const std::vector<double> &values)
    {
        std::string text;
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            text += (i == 0 ? "" : ",") + std::format("{}", values[i]);
        }
        return text;
    }

    std::string Numbers(const std::vector<int> &values)
    {
        std::string text;
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            text += (i == 0 ? "" : ",") + std::format("{}", values[i]);
        }
        return text;
    }

    std::string MakeFbx(const FbxSpec &spec)
    {
        std::ostringstream out;
        out << "; FBX 7.4.0 project file\n";
        out << "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n}\n";
        out << "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n";
        if (spec.zUp)
        {
            out << "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",2\n\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
                   "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1\n";
        }
        else
        {
            out << "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
                   "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n";
        }
        out << "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n";
        out << "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\"," << spec.unitScaleFactor << "\n";
        out << "\t\tP: \"OriginalUnitScaleFactor\", \"double\", \"Number\", \"\"," << spec.unitScaleFactor << "\n";
        out << "\t}\n}\n\nObjects:  {\n";
        out << "\tGeometry: 1000, \"Geometry::Mesh\", \"Mesh\" {\n";
        out << "\t\tVertices: *" << spec.positions.size() << " {\n\t\t\ta: " << Numbers(spec.positions) << "\n\t\t}\n";
        out << "\t\tPolygonVertexIndex: *" << spec.polygonIndices.size() << " {\n\t\t\ta: " << Numbers(spec.polygonIndices)
            << "\n\t\t}\n";
        const char *mapping = spec.attributesByControlPoint ? "ByVertice" : "ByPolygonVertex";
        if (!spec.normals.empty())
        {
            out << "\t\tLayerElementNormal: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"" << mapping
                << "\"\n\t\t\tReferenceInformationType: \"Direct\"\n\t\t\tNormals: *" << spec.normals.size()
                << " {\n\t\t\t\ta: " << Numbers(spec.normals) << "\n\t\t\t}\n\t\t}\n";
        }
        if (!spec.uvs.empty())
        {
            out << "\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"UVMap\"\n\t\t\tMappingInformationType: \"" << mapping
                << "\"\n\t\t\tReferenceInformationType: \"Direct\"\n\t\t\tUV: *" << spec.uvs.size() << " {\n\t\t\t\ta: "
                << Numbers(spec.uvs) << "\n\t\t\t}\n\t\t}\n";
        }
        if (!spec.polygonMaterials.empty())
        {
            out << "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygon\"\n"
                   "\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tMaterials: *"
                << spec.polygonMaterials.size() << " {\n\t\t\t\ta: " << Numbers(spec.polygonMaterials) << "\n\t\t\t}\n\t\t}\n";
        }
        out << "\t\tLayer: 0 {\n\t\t\tVersion: 100\n";
        if (!spec.normals.empty())
            out << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementNormal\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        if (!spec.polygonMaterials.empty())
            out << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementMaterial\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        if (!spec.uvs.empty())
            out << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        out << "\t\t}\n\t}\n";
        out << "\tModel: 2001, \"Model::Thing\", \"Mesh\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n"
               "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\","
            << spec.translation[0] << "," << spec.translation[1] << "," << spec.translation[2] << "\n\t\t}\n\t}\n";
        for (std::size_t i = 0; i < spec.materials.size(); ++i)
        {
            const FbxSpec::Material &material = spec.materials[i];
            out << "\tMaterial: " << 3001 + i << ", \"Material::" << material.name
                << "\", \"\" {\n\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n\t\tMultiLayer: 0\n\t\tProperties70:  {\n"
                   "\t\t\tP: \"DiffuseColor\", \"Color\", \"\", \"A\","
                << material.diffuse[0] << "," << material.diffuse[1] << "," << material.diffuse[2] << "\n\t\t}\n\t}\n";
        }
        out << "}\n\nConnections:  {\n\tC: \"OO\",2001,0\n\tC: \"OO\",1000,2001\n";
        for (std::size_t i = 0; i < spec.materials.size(); ++i)
        {
            out << "\tC: \"OO\"," << 3001 + i << ",2001\n";
        }
        out << "}\n";
        return out.str();
    }

    /// A triangle in the xz plane, counter-clockwise seen from +y: its geometric normal is +y (in y-up metres)
    FbxSpec FloorTriangle()
    {
        FbxSpec spec;
        spec.positions = {0, 0, 0, 0, 0, 1, 1, 0, 0};
        spec.polygonIndices = {0, 1, -3};
        return spec;
    }

    /// A unit quad in the xy plane facing +z as one polygon, with UVs and normals by polygon vertex
    FbxSpec Quad()
    {
        FbxSpec spec;
        spec.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        spec.polygonIndices = {0, 1, 2, -4};
        spec.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
        spec.uvs = {0, 0, 1, 0, 1, 1, 0, 1};
        return spec;
    }

    // ===== Looking at an imported scene =====

    struct Vec3
    {
        double x = 0;
        double y = 0;
        double z = 0;
    };

    Vec3 Rotate(const float q[4], const Vec3 &v)
    {
        // t = 2 * cross(q.xyz, v); v' = v + w * t + cross(q.xyz, t)
        const Vec3 t{2 * (q[1] * v.z - q[2] * v.y), 2 * (q[2] * v.x - q[0] * v.z), 2 * (q[0] * v.y - q[1] * v.x)};
        return {v.x + q[3] * t.x + (q[1] * t.z - q[2] * t.y), v.y + q[3] * t.y + (q[2] * t.x - q[0] * t.z),
                v.z + q[3] * t.z + (q[0] * t.y - q[1] * t.x)};
    }

    Vec3 ApplyNode(const ImportedNode &node, const Vec3 &p)
    {
        const Vec3 scaled{p.x * node.scale[0], p.y * node.scale[1], p.z * node.scale[2]};
        const Vec3 rotated = Rotate(node.rotation, scaled);
        return {rotated.x + node.translation[0], rotated.y + node.translation[1], rotated.z + node.translation[2]};
    }

    std::vector<int> ParentsOf(const ImportedScene &scene)
    {
        std::vector<int> parents(scene.nodes.size(), -1);
        for (std::size_t i = 0; i < scene.nodes.size(); ++i)
        {
            for (const std::uint32_t child : scene.nodes[i].children)
            {
                parents[child] = static_cast<int>(i);
            }
        }
        return parents;
    }

    Vec3 ToWorld(const ImportedScene &scene, const std::vector<int> &parents, int node, Vec3 p)
    {
        for (; node >= 0; node = parents[static_cast<std::size_t>(node)])
        {
            p = ApplyNode(scene.nodes[static_cast<std::size_t>(node)], p);
        }
        return p;
    }

    /// A direction on a node taken to world space (rotations only, so a unit normal stays unit under the uniform scales
    /// the tests use)
    Vec3 DirectionToWorld(const ImportedScene &scene, const std::vector<int> &parents, int node, Vec3 v)
    {
        for (; node >= 0; node = parents[static_cast<std::size_t>(node)])
        {
            v = Rotate(scene.nodes[static_cast<std::size_t>(node)].rotation, v);
        }
        return v;
    }

    struct Bounds
    {
        Vec3 min{1e30, 1e30, 1e30};
        Vec3 max{-1e30, -1e30, -1e30};
    };

    /// The world-space bounds of the mesh on a node
    Bounds WorldBounds(const ImportedScene &scene, const std::size_t node)
    {
        Bounds bounds;
        const std::vector<int> parents = ParentsOf(scene);
        const std::int32_t meshIndex = scene.nodes[node].meshIndex;
        if (meshIndex < 0)
            return bounds;
        for (const ImportedVertex &vertex : scene.meshes[static_cast<std::size_t>(meshIndex)].vertices)
        {
            const Vec3 w = ToWorld(scene, parents, static_cast<int>(node), {vertex.position[0], vertex.position[1], vertex.position[2]});
            bounds.min = {std::min(bounds.min.x, w.x), std::min(bounds.min.y, w.y), std::min(bounds.min.z, w.z)};
            bounds.max = {std::max(bounds.max.x, w.x), std::max(bounds.max.y, w.y), std::max(bounds.max.z, w.z)};
        }
        return bounds;
    }

    void ExpectBounds(const Bounds &bounds, const Vec3 &min, const Vec3 &max, const std::string &what)
    {
        EXPECT_NEAR(bounds.min.x, min.x, 1e-4) << what;
        EXPECT_NEAR(bounds.min.y, min.y, 1e-4) << what;
        EXPECT_NEAR(bounds.min.z, min.z, 1e-4) << what;
        EXPECT_NEAR(bounds.max.x, max.x, 1e-4) << what;
        EXPECT_NEAR(bounds.max.y, max.y, 1e-4) << what;
        EXPECT_NEAR(bounds.max.z, max.z, 1e-4) << what;
    }

    std::size_t NodeNamed(const ImportedScene &scene, const std::string &name)
    {
        for (std::size_t i = 0; i < scene.nodes.size(); ++i)
        {
            if (scene.nodes[i].name == name)
                return i;
        }
        ADD_FAILURE() << "no node named " << name;
        return 0;
    }

    /// The first node that has a mesh
    std::size_t FirstMeshNode(const ImportedScene &scene)
    {
        for (std::size_t i = 0; i < scene.nodes.size(); ++i)
        {
            if (scene.nodes[i].meshIndex >= 0)
                return i;
        }
        ADD_FAILURE() << "no node has a mesh";
        return 0;
    }

    /// Every index is in range and every submesh covers whole triangles inside the index list
    void ExpectWellFormed(const ImportedScene &scene)
    {
        for (const ImportedMesh &mesh : scene.meshes)
        {
            EXPECT_EQ(mesh.indices.size() % 3, 0u);
            for (const std::uint32_t index : mesh.indices)
            {
                ASSERT_LT(index, mesh.vertices.size());
            }
            std::size_t covered = 0;
            for (const ImportedSubmesh &submesh : mesh.submeshes)
            {
                EXPECT_EQ(submesh.firstIndex, covered);
                EXPECT_GT(submesh.indexCount, 0u);
                EXPECT_EQ(submesh.indexCount % 3, 0u);
                covered += submesh.indexCount;
                EXPECT_LT(submesh.materialIndex, static_cast<std::int32_t>(scene.materials.size()));
            }
            if (!mesh.submeshes.empty())
            {
                EXPECT_EQ(covered, mesh.indices.size());
            }
        }
        for (std::size_t i = 0; i < scene.nodes.size(); ++i)
        {
            const ImportedNode &node = scene.nodes[i];
            EXPECT_LT(node.meshIndex, static_cast<std::int32_t>(scene.meshes.size()));
            for (const std::uint32_t child : node.children)
            {
                EXPECT_LT(child, scene.nodes.size());
            }
        }
        for (const std::uint32_t root : scene.rootNodes)
        {
            EXPECT_LT(root, scene.nodes.size());
        }
    }
}

// ===== The importer itself =====

TEST(UfbxImporterTest, ReadsFbxAndObjExtensions)
{
    const UfbxImporter importer;
    EXPECT_TRUE(importer.HandlesExtension(".fbx"));
    EXPECT_TRUE(importer.HandlesExtension(".obj"));
    EXPECT_FALSE(importer.HandlesExtension(".glb"));
    EXPECT_FALSE(importer.HandlesExtension(".gltf"));
    EXPECT_FALSE(importer.GetName().empty());
}

TEST(UfbxImporterTest, ATriangleInYUpMetresImportsAsWritten)
{
    ModelImportSettings never;
    never.generateNormals = NormalGeneration::Never;
    const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(FloorTriangle())), never);
    ExpectWellFormed(scene);
    ASSERT_EQ(scene.meshes.size(), 1u);
    const ImportedMesh &mesh = scene.meshes[0];
    ASSERT_EQ(mesh.vertices.size(), 3u);
    EXPECT_EQ(mesh.indices.size(), 3u);
    ASSERT_EQ(mesh.submeshes.size(), 1u);
    EXPECT_EQ(mesh.submeshes[0].indexCount, 3u);
    EXPECT_EQ(mesh.submeshes[0].materialIndex, -1);

    // The file's three corners, in some order, with no conversion
    std::set<std::array<int, 3>> corners;
    for (const ImportedVertex &vertex : mesh.vertices)
    {
        corners.insert({static_cast<int>(std::lround(vertex.position[0])), static_cast<int>(std::lround(vertex.position[1])),
                        static_cast<int>(std::lround(vertex.position[2]))});
    }
    EXPECT_EQ(corners, (std::set<std::array<int, 3>>{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}}));

    EXPECT_FALSE(scene.nodes.empty());
    ASSERT_FALSE(scene.rootNodes.empty());
    EXPECT_GE(scene.nodes[NodeNamed(scene, "Thing")].meshIndex, 0);
}

TEST(UfbxImporterTest, ZUpCentimetresConvertToYUpMetres)
{
    FbxSpec spec;
    spec.zUp = true;
    spec.unitScaleFactor = 1.0; // centimetres
    // A triangle on the file's floor (xy plane is horizontal in z up), the file's (x, y, z) cm is (x, z, -y) / 100 m
    spec.positions = {0, 0, 0, 100, 0, 0, 0, 100, 0};
    spec.polygonIndices = {0, 1, -3};
    spec.translation = {0, -200, 0}; // 2 m along the file's -y: the engine's +z

    const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(spec)));
    ExpectWellFormed(scene);
    ASSERT_EQ(scene.meshes.size(), 1u);
    const std::size_t thing = NodeNamed(scene, "Thing");
    const std::vector<int> parents = ParentsOf(scene);
    std::set<std::array<long, 3>> corners;
    for (const ImportedVertex &vertex : scene.meshes[0].vertices)
    {
        const Vec3 w = ToWorld(scene, parents, static_cast<int>(thing), {vertex.position[0], vertex.position[1], vertex.position[2]});
        corners.insert({std::lround(w.x * 1000), std::lround(w.y * 1000), std::lround(w.z * 1000)});
    }
    // (0,0,0) -> (0,0,0); (100,0,0) -> (1,0,0); (0,100,0) -> (0,0,-1); all moved by the node's (0, 0, 2)
    EXPECT_EQ(corners, (std::set<std::array<long, 3>>{{0, 0, 2000}, {1000, 0, 2000}, {0, 0, 1000}}));

    // The generated normal, in world space, says the winding survived: file (+x then +y) is counter-clockwise from the
    // file's +z, the engine's +y
    ASSERT_FALSE(scene.meshes[0].vertices.empty());
    EXPECT_TRUE(scene.meshes[0].generatedNormals);
    const ImportedVertex &first = scene.meshes[0].vertices[0];
    const Vec3 normal = DirectionToWorld(scene, parents, static_cast<int>(thing), {first.normal[0], first.normal[1], first.normal[2]});
    EXPECT_NEAR(normal.x, 0.0, 1e-4);
    EXPECT_NEAR(normal.y, 1.0, 1e-4);
    EXPECT_NEAR(normal.z, 0.0, 1e-4);
}

TEST(UfbxImporterTest, TheScaleSettingMultipliesPositionsAndTranslations)
{
    FbxSpec spec;
    spec.zUp = true;
    spec.unitScaleFactor = 1.0;
    spec.positions = {0, 0, 0, 100, 0, 0, 0, 100, 0};
    spec.polygonIndices = {0, 1, -3};
    spec.translation = {0, -200, 0};
    ModelImportSettings settings;
    settings.scale = 2.0f;
    const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(spec)), settings);
    ASSERT_EQ(scene.meshes.size(), 1u);
    const Bounds bounds = WorldBounds(scene, NodeNamed(scene, "Thing"));
    ExpectBounds(bounds, {0, 0, 2}, {2, 0, 4}, "scale 2");
}

TEST(UfbxImporterTest, ANonPositiveOrNonFiniteScaleIsOne)
{
    ModelImportSettings bad;
    bad.scale = -3.0f;
    const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(FloorTriangle())), bad);
    ASSERT_EQ(scene.meshes.size(), 1u);
    const Bounds bounds = WorldBounds(scene, NodeNamed(scene, "Thing"));
    ExpectBounds(bounds, {0, 0, 0}, {1, 0, 1}, "scale -3 acts as 1");
}

TEST(UfbxImporterTest, APolygonIsTriangulatedAndItsSharedCornersWelded)
{
    const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(Quad())));
    ExpectWellFormed(scene);
    ASSERT_EQ(scene.meshes.size(), 1u);
    const ImportedMesh &mesh = scene.meshes[0];
    EXPECT_EQ(mesh.indices.size(), 6u) << "two triangles";
    EXPECT_EQ(mesh.vertices.size(), 4u) << "the diagonal's corners are shared";
    for (const ImportedVertex &vertex : mesh.vertices)
    {
        EXPECT_NEAR(vertex.normal[2], 1.0f, 1e-5f);
        // The UV is the position's x and y: FBX is v up already, so nothing is flipped
        EXPECT_NEAR(vertex.texCoord[0], vertex.position[0], 1e-5f);
        EXPECT_NEAR(vertex.texCoord[1], vertex.position[1], 1e-5f);
    }
}

TEST(UfbxImporterTest, ControlPointAttributesWeldTheSameWay)
{
    FbxSpec spec;
    spec.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    spec.polygonIndices = {0, 1, -3, 0, 2, -4}; // two triangles, [0, 1, 2] then [0, 2, 3]
    spec.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    spec.uvs = {0, 0, 1, 0, 1, 1, 0, 1};
    spec.attributesByControlPoint = true;
    const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(spec)));
    ExpectWellFormed(scene);
    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].vertices.size(), 4u);
    EXPECT_EQ(scene.meshes[0].indices.size(), 6u);
}

TEST(UfbxImporterTest, NormalsAreKeptNormalisedGeneratedWhenMissingAndFlatOnRequest)
{
    // The file's normals, tilted and not unit length
    FbxSpec tilted = Quad();
    tilted.normals = {1.2, 0, 1.6, 1.2, 0, 1.6, 1.2, 0, 1.6, 1.2, 0, 1.6};
    {
        const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(tilted)));
        ASSERT_EQ(scene.meshes.size(), 1u);
        EXPECT_FALSE(scene.meshes[0].generatedNormals);
        for (const ImportedVertex &vertex : scene.meshes[0].vertices)
        {
            EXPECT_NEAR(vertex.normal[0], 0.6f, 1e-5f);
            EXPECT_NEAR(vertex.normal[2], 0.8f, 1e-5f);
        }
    }
    {
        // Always: the file's are ignored and every triangle gets its own flat ones (no welding)
        ModelImportSettings always;
        always.generateNormals = NormalGeneration::Always;
        const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(tilted)), always);
        ASSERT_EQ(scene.meshes.size(), 1u);
        EXPECT_TRUE(scene.meshes[0].generatedNormals);
        EXPECT_EQ(scene.meshes[0].vertices.size(), 6u);
        for (const ImportedVertex &vertex : scene.meshes[0].vertices)
        {
            EXPECT_NEAR(vertex.normal[0], 0.0f, 1e-5f);
            EXPECT_NEAR(vertex.normal[2], 1.0f, 1e-5f);
        }
    }

    // A mesh with none: generated (IfMissing), or (0, 0, 1) (Never)
    {
        const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(FloorTriangle())));
        ASSERT_EQ(scene.meshes.size(), 1u);
        EXPECT_TRUE(scene.meshes[0].generatedNormals);
        for (const ImportedVertex &vertex : scene.meshes[0].vertices)
        {
            EXPECT_NEAR(vertex.normal[1], 1.0f, 1e-4f);
        }
    }
    {
        ModelImportSettings never;
        never.generateNormals = NormalGeneration::Never;
        const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(FloorTriangle())), never);
        ASSERT_EQ(scene.meshes.size(), 1u);
        EXPECT_FALSE(scene.meshes[0].generatedNormals);
        for (const ImportedVertex &vertex : scene.meshes[0].vertices)
        {
            EXPECT_FLOAT_EQ(vertex.normal[2], 1.0f);
        }
    }
}

TEST(UfbxImporterTest, TangentsFollowTheSettings)
{
    const Bytes quad = ToBytes(MakeFbx(Quad()));
    {
        // Nothing normal maps the mesh, so IfMissing makes none
        const ImportedScene scene = ImportOrFail(quad);
        ASSERT_EQ(scene.meshes.size(), 1u);
        EXPECT_FALSE(scene.meshes[0].generatedTangents);
        for (const ImportedVertex &vertex : scene.meshes[0].vertices)
        {
            EXPECT_EQ(vertex.tangent[0], 0.0f);
            EXPECT_EQ(vertex.tangent[1], 0.0f);
            EXPECT_EQ(vertex.tangent[2], 0.0f);
        }
    }
    {
        ModelImportSettings always;
        always.generateTangents = TangentGeneration::Always;
        const ImportedScene scene = ImportOrFail(quad, always);
        ASSERT_EQ(scene.meshes.size(), 1u);
        EXPECT_TRUE(scene.meshes[0].generatedTangents);
        for (const ImportedVertex &vertex : scene.meshes[0].vertices)
        {
            const double length = std::sqrt(vertex.tangent[0] * vertex.tangent[0] + vertex.tangent[1] * vertex.tangent[1] +
                                            vertex.tangent[2] * vertex.tangent[2]);
            EXPECT_NEAR(length, 1.0, 1e-4);
            // Perpendicular to the normal, along increasing u (+x on this quad)
            const double along = vertex.tangent[0] * vertex.normal[0] + vertex.tangent[1] * vertex.normal[1] +
                                 vertex.tangent[2] * vertex.normal[2];
            EXPECT_NEAR(along, 0.0, 1e-4);
            EXPECT_NEAR(vertex.tangent[0], 1.0f, 1e-4f);
        }
    }
}

// ===== Materials and images =====

TEST(UfbxImporterTest, MaterialsBecomeSubmeshesWithTheirColours)
{
    FbxSpec spec = Quad();
    // Two triangles instead of the quad polygon, one material each
    spec.polygonIndices = {0, 1, -3, 0, 2, -4};
    spec.polygonMaterials = {0, 1};
    spec.materials = {{"Red", {1.0, 0.0, 0.0}}, {"Blue", {0.2, 0.4, 1.0}}};
    const ImportedScene scene = ImportOrFail(ToBytes(MakeFbx(spec)));
    ExpectWellFormed(scene);
    ASSERT_EQ(scene.materials.size(), 2u);
    EXPECT_EQ(scene.materials[0].name, "Red");
    EXPECT_EQ(scene.materials[1].name, "Blue");
    EXPECT_NEAR(scene.materials[1].baseColor[0], 0.2f, 1e-4f);
    EXPECT_NEAR(scene.materials[1].baseColor[1], 0.4f, 1e-4f);
    EXPECT_NEAR(scene.materials[1].baseColor[2], 1.0f, 1e-4f);
    EXPECT_FLOAT_EQ(scene.materials[1].baseColor[3], 1.0f);
    EXPECT_EQ(scene.materials[1].alphaMode, ImportedAlphaMode::Opaque);
    EXPECT_GE(scene.materials[1].smoothness, 0.0f);
    EXPECT_LE(scene.materials[1].smoothness, 1.0f);

    ASSERT_EQ(scene.meshes.size(), 1u);
    const ImportedMesh &mesh = scene.meshes[0];
    ASSERT_EQ(mesh.submeshes.size(), 2u);
    EXPECT_EQ(mesh.submeshes[0].indexCount, 3u);
    EXPECT_EQ(mesh.submeshes[1].indexCount, 3u);
    EXPECT_EQ(mesh.submeshes[0].materialIndex, 0);
    EXPECT_EQ(mesh.submeshes[1].materialIndex, 1);

    // Every submesh material of a merged mesh is still one per material
    ModelImportSettings merge;
    merge.mergeSubmeshesByMaterial = true;
    const ImportedScene merged = ImportOrFail(ToBytes(MakeFbx(spec)), merge);
    ASSERT_EQ(merged.meshes.size(), 1u);
    EXPECT_EQ(merged.meshes[0].submeshes.size(), 2u);

    // importMaterials off: no materials, no images, material -1 everywhere
    ModelImportSettings none;
    none.importMaterials = false;
    const ImportedScene bare = ImportOrFail(ToBytes(MakeFbx(spec)), none);
    EXPECT_TRUE(bare.materials.empty());
    EXPECT_TRUE(bare.images.empty());
    ASSERT_EQ(bare.meshes.size(), 1u);
    for (const ImportedSubmesh &submesh : bare.meshes[0].submeshes)
    {
        EXPECT_EQ(submesh.materialIndex, -1);
    }
}

TEST(UfbxImporterTest, ATextureNamedByTheFileIsReadFromTheModelsFolder)
{
    const Bytes fbx = ReadBytes(ModelsDirectory() / "textured_cube.fbx");
    ASSERT_FALSE(fbx.empty());
    const Bytes png = TestPng();
    TempFolder folder("texture");
    folder.Write("models/checker.png", png);

    const ImportedScene scene = ImportOrFail(fbx, {}, folder.root / "models");
    ExpectWellFormed(scene);
    ASSERT_EQ(scene.materials.size(), 2u);
    ASSERT_EQ(scene.images.size(), 1u);
    EXPECT_EQ(scene.images[0].bytes, png);
    EXPECT_EQ(scene.images[0].uri, "checker.png");
    EXPECT_EQ(scene.images[0].mimeType, "image/png");
    EXPECT_TRUE(scene.images[0].colour);
    EXPECT_TRUE(scene.images[0].clamp);
    EXPECT_FALSE(scene.images[0].nearest);

    const auto checker = std::ranges::find_if(scene.materials, [](const ImportedMaterial &m) { return m.name == "Checker"; });
    const auto blue = std::ranges::find_if(scene.materials, [](const ImportedMaterial &m) { return m.name == "Blue"; });
    ASSERT_NE(checker, scene.materials.end());
    ASSERT_NE(blue, scene.materials.end());
    EXPECT_EQ(checker->baseColorTexture, 0);
    EXPECT_EQ(blue->baseColorTexture, -1);
}

TEST(UfbxImporterTest, AMissingTextureFileIsAWarningNotAnError)
{
    const Bytes fbx = ReadBytes(ModelsDirectory() / "textured_cube.fbx");
    TempFolder folder("missing");
    const ImportedScene scene = ImportOrFail(fbx, {}, folder.root);
    ASSERT_EQ(scene.images.size(), 1u);
    EXPECT_TRUE(scene.images[0].bytes.empty());
    EXPECT_TRUE(HasWarning(scene, "checker.png"));
    EXPECT_EQ(scene.meshes.size() >= 1u, true) << "the geometry still imports";

    // No folder at all: the same, with its own reason
    const ImportedScene noFolder = ImportOrFail(fbx);
    ASSERT_EQ(noFolder.images.size(), 1u);
    EXPECT_TRUE(noFolder.images[0].bytes.empty());
    EXPECT_TRUE(HasWarning(noFolder, "no folder"));
}

TEST(UfbxImporterTest, ATextureOutsideTheModelsFolderIsNotRead)
{
    std::string text = ReadText(ModelsDirectory() / "textured_cube.fbx");
    ASSERT_NE(text.find("\"checker.png\""), std::string::npos);
    for (std::size_t at = text.find("\"checker.png\""); at != std::string::npos; at = text.find("\"checker.png\""))
    {
        text.replace(at, 13, "\"../outside/secret.png\"");
    }
    TempFolder folder("outside");
    folder.Write("outside/secret.png", TestPng());
    const ImportedScene scene = ImportOrFail(ToBytes(text), {}, folder.root / "models");
    ASSERT_EQ(scene.images.size(), 1u);
    EXPECT_TRUE(scene.images[0].bytes.empty()) << "../ leaves the model's folder";
    EXPECT_FALSE(scene.warnings.empty());
}

TEST(UfbxImporterTest, AnAbsolutePathFromTheArtistsMachineFallsBackToTheFileName)
{
    std::string text = ReadText(ModelsDirectory() / "textured_cube.fbx");
    for (std::size_t at = text.find("\"checker.png\""); at != std::string::npos; at = text.find("\"checker.png\""))
    {
        text.replace(at, 13, "\"C:/Users/artist/textures/checker.png\"");
    }
    const Bytes png = TestPng();
    TempFolder folder("absolute");
    folder.Write("checker.png", png);
    const ImportedScene scene = ImportOrFail(ToBytes(text), {}, folder.root);
    ASSERT_EQ(scene.images.size(), 1u);
    EXPECT_EQ(scene.images[0].bytes, png);
    EXPECT_EQ(scene.images[0].uri, "checker.png");
}

// ===== The committed cube =====

TEST(UfbxImporterTest, TheCommittedCubeHasItsNodesMeshAndMaterials)
{
    const Bytes fbx = ReadBytes(ModelsDirectory() / "textured_cube.fbx");
    ASSERT_FALSE(fbx.empty()) << "tests/assets/models/textured_cube.fbx";
    const ImportedScene scene = ImportOrFail(fbx);
    ExpectWellFormed(scene);

    // Cube, with Child below it, and Mirrored: three nodes, two roots
    ASSERT_EQ(scene.nodes.size(), 3u);
    EXPECT_EQ(scene.rootNodes.size(), 2u);
    const std::size_t cube = NodeNamed(scene, "Cube");
    const std::size_t child = NodeNamed(scene, "Child");
    const std::size_t mirrored = NodeNamed(scene, "Mirrored");
    ASSERT_EQ(scene.nodes[cube].children.size(), 1u);
    EXPECT_EQ(scene.nodes[cube].children[0], child);
    EXPECT_TRUE(scene.nodes[child].children.empty());
    for (const std::size_t node : {cube, child, mirrored})
    {
        ASSERT_GE(scene.nodes[node].meshIndex, 0);
        const ImportedMesh &mesh = scene.meshes[static_cast<std::size_t>(scene.nodes[node].meshIndex)];
        EXPECT_EQ(mesh.vertices.size(), 24u);
        EXPECT_EQ(mesh.indices.size(), 36u);
        ASSERT_EQ(mesh.submeshes.size(), 2u);
        EXPECT_EQ(mesh.submeshes[0].indexCount, 18u);
        EXPECT_EQ(mesh.submeshes[1].indexCount, 18u);
    }

    // Z up and centimetres in the file, y up and metres here: a one metre cube at the origin, half size above it, and
    // one mirrored in x two metres to the right (the mirrored bounds are the same box)
    ExpectBounds(WorldBounds(scene, cube), {-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}, "Cube");
    ExpectBounds(WorldBounds(scene, child), {-0.25, 0.75, -0.25}, {0.25, 1.25, 0.25}, "Child");
    ExpectBounds(WorldBounds(scene, mirrored), {1.5, -0.5, -0.5}, {2.5, 0.5, 0.5}, "Mirrored");

    ASSERT_EQ(scene.materials.size(), 2u);
    const auto blue = std::ranges::find_if(scene.materials, [](const ImportedMaterial &m) { return m.name == "Blue"; });
    ASSERT_NE(blue, scene.materials.end());
    EXPECT_NEAR(blue->baseColor[0], 0.2f, 1e-4f);
    EXPECT_NEAR(blue->baseColor[1], 0.4f, 1e-4f);
    EXPECT_NEAR(blue->baseColor[2], 1.0f, 1e-4f);
}

namespace
{
    /// A vertex of a node's mesh as text, in world space and rounded, for comparing the meshes two importers made
    /// (one may keep the file's axes in the mesh and turn the node, the other bake them)
    std::string WorldVertexKey(const ImportedScene &scene, const std::vector<int> &parents, const std::size_t node,
                               const ImportedVertex &v)
    {
        const Vec3 p = ToWorld(scene, parents, static_cast<int>(node), {v.position[0], v.position[1], v.position[2]});
        const Vec3 n = DirectionToWorld(scene, parents, static_cast<int>(node), {v.normal[0], v.normal[1], v.normal[2]});
        const auto r = [](const double x) { return std::lround(x * 1000.0); };
        return std::format("{},{},{}|{},{},{}|{},{}", r(p.x), r(p.y), r(p.z), r(n.x), r(n.y), r(n.z), r(v.texCoord[0]),
                           r(v.texCoord[1]));
    }

    /// The triangles of a node's mesh as text (each starting at its smallest corner, so the winding is kept), sorted
    std::vector<std::string> TriangleKeys(const ImportedScene &scene, const std::size_t node)
    {
        const std::vector<int> parents = ParentsOf(scene);
        const ImportedMesh &mesh = scene.meshes[static_cast<std::size_t>(scene.nodes[node].meshIndex)];
        std::vector<std::string> vertexKeys;
        for (const ImportedVertex &vertex : mesh.vertices)
        {
            vertexKeys.push_back(WorldVertexKey(scene, parents, node, vertex));
        }
        std::vector<std::string> keys;
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            std::array<std::string, 3> corners = {vertexKeys[mesh.indices[i]], vertexKeys[mesh.indices[i + 1]],
                                                  vertexKeys[mesh.indices[i + 2]]};
            const auto first = std::ranges::min_element(corners);
            std::ranges::rotate(corners, first);
            keys.push_back(corners[0] + " / " + corners[1] + " / " + corners[2]);
        }
        std::ranges::sort(keys);
        return keys;
    }
}

TEST(UfbxImporterConsistencyTest, TheFbxAndTheGlbOfTheSameCubeImportTheSame)
{
    const ImportedScene fbx = ImportOrFail(ReadBytes(ModelsDirectory() / "textured_cube.fbx"));
    auto glb = GltfImporter().Import(ReadBytes(ModelsDirectory() / "textured_cube.glb"), ModelsDirectory(), {});
    ASSERT_TRUE(glb.has_value()) << glb.error().message;

    ASSERT_EQ(fbx.nodes.size(), glb->nodes.size());
    EXPECT_EQ(fbx.rootNodes.size(), glb->rootNodes.size());
    ASSERT_EQ(fbx.materials.size(), glb->materials.size());

    for (const ImportedNode &glbNode : glb->nodes)
    {
        const std::size_t f = NodeNamed(fbx, glbNode.name);
        const std::size_t g = NodeNamed(*glb, glbNode.name);
        EXPECT_EQ(fbx.nodes[f].children.size(), glb->nodes[g].children.size()) << glbNode.name;
        ASSERT_GE(fbx.nodes[f].meshIndex, 0);
        ASSERT_GE(glb->nodes[g].meshIndex, 0);

        const Bounds fb = WorldBounds(fbx, f);
        const Bounds gb = WorldBounds(*glb, g);
        ExpectBounds(fb, gb.min, gb.max, "world bounds of " + glbNode.name);

        const ImportedMesh &fm = fbx.meshes[static_cast<std::size_t>(fbx.nodes[f].meshIndex)];
        const ImportedMesh &gm = glb->meshes[static_cast<std::size_t>(glb->nodes[g].meshIndex)];
        EXPECT_EQ(fm.vertices.size(), gm.vertices.size());
        ASSERT_EQ(fm.submeshes.size(), gm.submeshes.size());
        for (std::size_t s = 0; s < gm.submeshes.size(); ++s)
        {
            EXPECT_EQ(fm.submeshes[s].indexCount, gm.submeshes[s].indexCount);
            // The same material, by name
            ASSERT_GE(fm.submeshes[s].materialIndex, 0);
            ASSERT_GE(gm.submeshes[s].materialIndex, 0);
            EXPECT_EQ(fbx.materials[static_cast<std::size_t>(fm.submeshes[s].materialIndex)].name,
                      glb->materials[static_cast<std::size_t>(gm.submeshes[s].materialIndex)].name);
        }
    }

    // The Cube node's mesh in world space, triangle for triangle (positions, normals, texture coordinates, winding)
    EXPECT_EQ(TriangleKeys(fbx, NodeNamed(fbx, "Cube")), TriangleKeys(*glb, NodeNamed(*glb, "Cube")));
}

// ===== OBJ =====

namespace
{
    const char *kQuadObj =
        "# a quad\n"
        "mtllib quad.mtl\n"
        "o Quad\n"
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "vn 0 0 1\n"
        "usemtl Red\n"
        "f 1/1/1 2/2/1 3/3/1 4/4/1\n";

    const char *kQuadMtl =
        "newmtl Red\n"
        "Kd 0.5 0.25 0.75\n"
        "map_Kd checker.png\n";
}

TEST(UfbxImporterObjTest, AQuadWithItsMaterialLibraryAndTexture)
{
    const Bytes png = TestPng();
    TempFolder folder("obj");
    folder.Write("quad.mtl", std::string(kQuadMtl));
    folder.Write("checker.png", png);
    const ImportedScene scene = ImportOrFail(ToBytes(kQuadObj), {}, folder.root);
    ExpectWellFormed(scene);
    ASSERT_EQ(scene.meshes.size(), 1u);
    const ImportedMesh &mesh = scene.meshes[0];
    EXPECT_EQ(mesh.vertices.size(), 4u);
    EXPECT_EQ(mesh.indices.size(), 6u);
    ASSERT_EQ(mesh.submeshes.size(), 1u);
    EXPECT_EQ(mesh.submeshes[0].materialIndex, 0);

    // y up, metres: nothing is converted; v is kept (OBJ is v up)
    for (const ImportedVertex &vertex : mesh.vertices)
    {
        EXPECT_NEAR(vertex.position[0], vertex.texCoord[0], 1e-5f);
        EXPECT_NEAR(vertex.position[1], vertex.texCoord[1], 1e-5f);
        EXPECT_NEAR(vertex.position[2], 0.0f, 1e-5f);
        EXPECT_NEAR(vertex.normal[2], 1.0f, 1e-5f);
    }

    ASSERT_EQ(scene.materials.size(), 1u);
    EXPECT_EQ(scene.materials[0].name, "Red");
    EXPECT_NEAR(scene.materials[0].baseColor[0], 0.5f, 1e-4f);
    EXPECT_NEAR(scene.materials[0].baseColor[1], 0.25f, 1e-4f);
    EXPECT_NEAR(scene.materials[0].baseColor[2], 0.75f, 1e-4f);
    ASSERT_EQ(scene.images.size(), 1u);
    EXPECT_EQ(scene.images[0].bytes, png);
    EXPECT_EQ(scene.materials[0].baseColorTexture, 0);
}

TEST(UfbxImporterObjTest, TheScaleSettingAppliesToAnObj)
{
    ModelImportSettings settings;
    settings.scale = 0.01f;
    const ImportedScene scene = ImportOrFail(ToBytes(kQuadObj), settings);
    ASSERT_EQ(scene.meshes.size(), 1u);
    const Bounds bounds = WorldBounds(scene, FirstMeshNode(scene));
    ExpectBounds(bounds, {0, 0, 0}, {0.01, 0.01, 0}, "a centimetre quad");
}

TEST(UfbxImporterObjTest, AnObjWithoutAFolderSkipsItsMaterialLibraryWithAWarning)
{
    const ImportedScene scene = ImportOrFail(ToBytes(kQuadObj));
    EXPECT_TRUE(HasWarning(scene, "material library"));
    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].indices.size(), 6u);
}

TEST(UfbxImporterObjTest, AMaterialLibraryOutsideTheFolderIsNotRead)
{
    TempFolder folder("objoutside");
    folder.Write("secret.mtl", std::string("newmtl Secret\nKd 1 0 0\n"));
    const std::string obj = "mtllib ../secret.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Secret\nf 1 2 3\n";
    fs::create_directories(folder.root / "models");
    const ImportedScene scene = ImportOrFail(ToBytes(obj), {}, folder.root / "models");
    EXPECT_TRUE(HasWarning(scene, "material library"));
    for (const ImportedMaterial &material : scene.materials)
    {
        EXPECT_NE(material.name, "Secret") << "the library outside the model's folder was read";
    }
}

// ===== Bad input =====

TEST(UfbxImporterErrorTest, EmptyInputAndGarbageAreErrorsNotCrashes)
{
    const auto empty = Import({});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().code, ModelImportErrorCode::EmptyInput);

    const auto text = Import(ToBytes("this is not a model\n"));
    ASSERT_FALSE(text.has_value());
    EXPECT_EQ(text.error().code, ModelImportErrorCode::ParseFailed);
    EXPECT_FALSE(text.error().message.empty());

    const auto binary = Import(Bytes{1, 2, 3, 0, 255, 254, 253});
    EXPECT_FALSE(binary.has_value());

    // An FBX header and nothing else is an error or an empty scene, never a crash
    const auto header = Import(ToBytes("; FBX 7.4.0 project file\nObjects:  {\n"));
    if (header.has_value())
    {
        EXPECT_TRUE(header->meshes.empty());
    }
}

TEST(UfbxImporterErrorTest, AFileCutShortAtEveryByteNeverCrashesAndNeverGivesBadIndices)
{
    const Bytes fbx = ReadBytes(ModelsDirectory() / "textured_cube.fbx");
    ASSERT_FALSE(fbx.empty());
    ASSERT_TRUE(Import(fbx).has_value());
    for (std::size_t length = 0; length < fbx.size(); length += 7)
    {
        const Bytes cut(fbx.begin(), fbx.begin() + static_cast<std::ptrdiff_t>(length));
        const auto result = Import(cut);
        if (result.has_value())
        {
            ExpectWellFormed(*result);
        }
        else
        {
            EXPECT_FALSE(result.error().message.empty()) << "cut at " << length;
        }
    }
}

TEST(UfbxImporterErrorTest, PolygonsWithBadIndicesAreCaughtByTheFormat)
{
    // An index past the last control point
    FbxSpec spec;
    spec.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    spec.polygonIndices = {0, 1, -100};
    const auto result = Import(ToBytes(MakeFbx(spec)));
    if (result.has_value())
    {
        ExpectWellFormed(*result);
    }
}

#endif // N2ENGINE_MODEL_UFBX
