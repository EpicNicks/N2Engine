#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/io/AssetMetadata.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/rendering/Texture.hpp"

#include "MeshTestSupport.hpp"
#include "ModelTestSupport.hpp"

// The Model asset in a project (a temporary folder with .glb files written by the test): its resource type, its
// meshes, materials and textures as sub-assets with deterministic UUIDs (stable across rescans, restarts and a
// re-export that reorders the meshes), the .meta sub-asset index written when the model first loads (never by the
// scan), LoadByUUID of a sub-asset loading its parent, the name-or-index key rule, and the import settings.

using namespace N2Engine;
using Rendering::Material;
using Rendering::Mesh;
using Rendering::Model;
using Rendering::ModelNormals;
using Rendering::ModelSettings;
using Rendering::Texture;
using MeshTestSupport::WarningCapture;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    const IO::ResourcePath kRobot("res://models/robot.glb");
}

class ModelAssetTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;

    static fs::path MetaPath(const std::string &relative)
    {
        return s_root / ".import" / (relative + ".meta");
    }

    static void WriteFile(const std::string &relative, const std::vector<std::uint8_t> &bytes)
    {
        const fs::path path = s_root / "assets" / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    static json ReadMetaCustomData(const std::string &relative)
    {
        return IO::AssetMetadata::FromFile(MetaPath(relative)).customData;
    }

    static IO::ResourceLoader &Loader() { return IO::ResourceLoader::Instance(); }

    void SetUp() override
    {
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_model_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets");
        WriteFile("models/robot.glb", ModelTestSupport::Robot().ToGlb());

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "ModelAssetTest"));
        Loader().ClearCache();
        Loader().Initialize(s_root);
    }

    void TearDown() override
    {
        Loader().ClearCache();
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }
};

TEST_F(ModelAssetTest, GltfAndGlbFilesAreScannedAsModelsWithoutAnIndexUntilTheyLoad)
{
    WriteFile("models/Other.GLTF", ModelTestSupport::Robot().ToGltf());
    Loader().Initialize(s_root);

    for (const std::string relative : {"models/robot.glb", "models/Other.GLTF"})
    {
        const IO::ResourcePath path("res://" + relative);
        const IO::AssetMetadata *meta = Loader().GetMetadata(path);
        ASSERT_NE(meta, nullptr) << relative;
        EXPECT_EQ(meta->resourceType, "Model") << relative;
        EXPECT_EQ(meta->uuid, IO::ResourceUUID::FromPath(path)) << relative;
        // The scan never parses a model: no index yet, in memory or on disk
        EXPECT_FALSE(meta->customData.is_object() && meta->customData.contains("subAssets")) << relative;
        const json onDisk = ReadMetaCustomData(relative);
        EXPECT_FALSE(onDisk.is_object() && onDisk.contains("subAssets")) << relative;
    }
    EXPECT_EQ(Loader().GetAssetsByType("Model").size(), 2u);
}

TEST_F(ModelAssetTest, LoadingGivesSubAssetsDeterministicUuidsAndWritesTheIndex)
{
    const auto model = Loader().Load<Model>(kRobot);
    ASSERT_NE(model, nullptr);
    ASSERT_TRUE(model->IsLoaded());
    EXPECT_EQ(model->GetName(), "robot");
    EXPECT_EQ(model->GetResourceType(), "Model");
    ASSERT_EQ(model->GetMeshes().size(), 2u);
    ASSERT_EQ(model->GetMaterials().size(), 2u);
    ASSERT_EQ(model->GetTextures().size(), 1u);

    const auto body = std::dynamic_pointer_cast<Mesh>(model->FindSubAsset("mesh/Body"));
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body, model->GetMeshes()[0]);
    EXPECT_EQ(body->GetUUID(), IO::ResourceUUID::FromSubAsset(kRobot, "mesh/Body"));
    EXPECT_NE(body->GetUUID(), model->GetUUID());
    EXPECT_TRUE(body->IsSubResource());
    EXPECT_EQ(body->GetSubAssetKey(), "mesh/Body");
    EXPECT_EQ(body->GetResourcePath(), kRobot);
    EXPECT_EQ(model->FindSubAsset("material/Red"), model->GetMaterials()[0]);
    EXPECT_EQ(model->FindSubAsset("material/Green"), model->GetMaterials()[1]);
    EXPECT_EQ(model->FindSubAsset("texture/Albedo"), model->GetTextures()[0]);
    EXPECT_EQ(model->FindSubAsset("mesh/Nope"), nullptr);
    EXPECT_EQ(model->GetMaterials()[1]->GetUUID(), IO::ResourceUUID::FromSubAsset(kRobot, "material/Green"));

    // The index, in memory and in the .meta file
    const json index = ReadMetaCustomData("models/robot.glb").at("subAssets");
    ASSERT_TRUE(index.is_object());
    EXPECT_EQ(index.size(), 5u) << index.dump();
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"mesh/Body", "Mesh"}, {"mesh/Arm", "Mesh"}, {"material/Red", "Material"}, {"material/Green", "Material"},
        {"texture/Albedo", "Texture"}};
    for (const auto &[key, type] : expected)
    {
        ASSERT_TRUE(index.contains(key)) << key;
        EXPECT_EQ(index[key].at("type"), type) << key;
        EXPECT_EQ(index[key].at("uuid"), IO::ResourceUUID::FromSubAsset(kRobot, key).ToString()) << key;
        const IO::ResourceLoader::SubAssetLocation *location =
            Loader().FindSubAssetLocation(IO::ResourceUUID::FromSubAsset(kRobot, key));
        ASSERT_NE(location, nullptr) << key;
        EXPECT_EQ(location->parent, kRobot);
        EXPECT_EQ(location->key, key);
    }
    EXPECT_TRUE(Loader().GetMetadata(kRobot)->customData.contains("subAssets"));
}

TEST_F(ModelAssetTest, SubAssetUuidsAreStableAcrossRescansAndRestarts)
{
    const Math::UUID arm = IO::ResourceUUID::FromSubAsset(kRobot, "mesh/Arm");
    const auto first = Loader().Load<Model>(kRobot);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->GetMeshes()[1]->GetUUID(), arm);

    Loader().RescanAssets();
    EXPECT_EQ(Loader().LoadByUUID<Mesh>(arm), first->GetMeshes()[1]) << "the cached model's own mesh";

    // A restart: nothing is cached, the index is read back from the .meta, and the UUID still names the arm
    Loader().Initialize(s_root);
    EXPECT_EQ(Loader().GetCached<Model>(kRobot), nullptr);
    ASSERT_NE(Loader().FindSubAssetLocation(arm), nullptr) << "the scan read the index";
    const auto reloaded = Loader().LoadByUUID<Mesh>(arm);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->GetUUID(), arm);
    EXPECT_EQ(reloaded->GetSubAssetKey(), "mesh/Arm");
    EXPECT_EQ(reloaded->GetVertices().size(), 6u) << "the quad (flat normals: 6 vertices)";
}

TEST_F(ModelAssetTest, LoadByUuidOfASubAssetLoadsItsParent)
{
    ASSERT_NE(Loader().Load<Model>(kRobot), nullptr); // writes the index
    Loader().ClearCache();

    const Math::UUID red = IO::ResourceUUID::FromSubAsset(kRobot, "material/Red");
    EXPECT_EQ(Loader().GetCachedByUUID<Material>(red), nullptr) << "nothing loaded yet";
    const auto material = IO::Resources::Instance().LoadByUUID<Material>(red);
    ASSERT_NE(material, nullptr);
    const auto model = Loader().GetCached<Model>(kRobot);
    ASSERT_NE(model, nullptr) << "the parent loaded";
    EXPECT_EQ(material, model->GetMaterials()[0]);
    EXPECT_EQ(Loader().GetCachedByUUID<Material>(red), material);
    EXPECT_EQ(IO::Resources::Instance().GetAsset<Material>(red), material);
    EXPECT_EQ(Loader().LoadByUUID<Mesh>(red), nullptr) << "a material isn't a mesh";
}

TEST_F(ModelAssetTest, RemoveUnusedKeepsAModelWhileItsSubAssetsAreInUse)
{
    std::shared_ptr<Mesh> body;
    std::shared_ptr<Texture> albedo;
    {
        const auto model = Loader().Load<Model>(kRobot);
        ASSERT_NE(model, nullptr);
        body = model->GetMeshes()[0];
    }
    Loader().RemoveUnused();
    EXPECT_NE(Loader().GetCached<Model>(kRobot), nullptr) << "its mesh is still held";
    EXPECT_EQ(Loader().LoadByUUID<Mesh>(body->GetUUID()), body) << "the same object, not a second import";

    // A texture only its own materials use doesn't count; one held outside does
    body.reset();
    albedo = std::dynamic_pointer_cast<Texture>(Loader().GetCached<Model>(kRobot)->FindSubAsset("texture/Albedo"));
    ASSERT_NE(albedo, nullptr);
    Loader().RemoveUnused();
    EXPECT_NE(Loader().GetCached<Model>(kRobot), nullptr) << "its texture is still held";

    albedo.reset();
    Loader().RemoveUnused();
    EXPECT_EQ(Loader().GetCached<Model>(kRobot), nullptr) << "nothing holds any of it any more";
}

TEST_F(ModelAssetTest, ASubAssetLoadsByUuidEvenBeforeItsModelWasEverIndexed)
{
    // A fresh project (no .import yet): the model has never loaded, so there's no index to read
    const Math::UUID body = IO::ResourceUUID::FromSubAsset(kRobot, "mesh/Body");
    ASSERT_EQ(Loader().FindSubAssetLocation(body), nullptr);
    const auto mesh = Loader().LoadByUUID<Mesh>(body);
    ASSERT_NE(mesh, nullptr) << "LoadByUUID loads the models without an index, then looks again";
    EXPECT_EQ(mesh->GetSubAssetKey(), "mesh/Body");
    EXPECT_EQ(Loader().LoadByUUID<Mesh>(Math::UUID::Random()), nullptr) << "an unknown UUID is still unknown";
}

TEST_F(ModelAssetTest, AReExportThatReordersTheMeshesKeepsEveryUuidOnItsMesh)
{
    const Math::UUID body = IO::ResourceUUID::FromSubAsset(kRobot, "mesh/Body");
    const Math::UUID arm = IO::ResourceUUID::FromSubAsset(kRobot, "mesh/Arm");
    {
        const auto model = Loader().Load<Model>(kRobot);
        ASSERT_NE(model, nullptr);
        EXPECT_EQ(model->GetMeshes()[0]->GetUUID(), body);
    }

    // Exported again with Arm first
    WriteFile("models/robot.glb", ModelTestSupport::Robot(true).ToGlb());
    Loader().Initialize(s_root);
    const auto model = Loader().Load<Model>(kRobot);
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->GetMeshes()[0]->GetUUID(), arm) << "now first in the file";
    EXPECT_EQ(model->GetMeshes()[1]->GetUUID(), body);
    const auto bodyMesh = Loader().LoadByUUID<Mesh>(body);
    ASSERT_NE(bodyMesh, nullptr);
    EXPECT_EQ(bodyMesh->GetVertices().size(), 3u) << "the body is still the triangle";
    EXPECT_EQ(Loader().LoadByUUID<Mesh>(arm)->GetVertices().size(), 6u);
}

TEST_F(ModelAssetTest, ARepeatedNameKeysEveryAssetOfThatKindByIndex)
{
    GltfTest::Builder b;
    const int first = b.AddMesh("Same", {GltfTest::Builder::Primitive(b.AddFloats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3))});
    const int second = b.AddMesh("Same", {GltfTest::Builder::Primitive(b.AddFloats({0, 0, 0, 2, 0, 0, 0, 2, 0}, 3))});
    const int third = b.AddMesh("Unique", {GltfTest::Builder::Primitive(b.AddFloats({0, 0, 0, 3, 0, 0, 0, 3, 0}, 3))});
    b.AddUnlitMaterial("A", 1, 0, 0);
    b.AddUnlitMaterial("B", 0, 1, 0);
    b.SetScene({b.AddNode("N1", {{"mesh", first}}), b.AddNode("N2", {{"mesh", second}}), b.AddNode("N3", {{"mesh", third}})});
    WriteFile("models/dupes.glb", b.ToGlb());
    Loader().Initialize(s_root);

    const IO::ResourcePath path("res://models/dupes.glb");
    const auto model = Loader().Load<Model>(path);
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->FindSubAsset("mesh/0"), model->GetMeshes()[0]);
    EXPECT_EQ(model->FindSubAsset("mesh/1"), model->GetMeshes()[1]);
    EXPECT_EQ(model->FindSubAsset("mesh/2"), model->GetMeshes()[2]) << "every mesh by index, the unique one too";
    EXPECT_EQ(model->FindSubAsset("mesh/Same"), nullptr);
    EXPECT_EQ(model->FindSubAsset("mesh/Unique"), nullptr);
    EXPECT_NE(model->FindSubAsset("material/A"), nullptr) << "materials keep their names";
    EXPECT_EQ(model->GetMeshes()[1]->GetUUID(), IO::ResourceUUID::FromSubAsset(path, "mesh/1"));

    EXPECT_EQ(Model::MakeSubAssetKeys("mesh", {"a", "b"}), (std::vector<std::string>{"mesh/a", "mesh/b"}));
    EXPECT_EQ(Model::MakeSubAssetKeys("mesh", {"a", ""}), (std::vector<std::string>{"mesh/0", "mesh/1"}));
    EXPECT_EQ(Model::MakeSubAssetKeys("texture", {"x", "x"}), (std::vector<std::string>{"texture/0", "texture/1"}));
}

TEST_F(ModelAssetTest, TheEmbeddedImageIsATextureSubAssetTheMaterialUses)
{
    const auto model = Loader().Load<Model>(kRobot);
    ASSERT_NE(model, nullptr);
    const auto texture = std::dynamic_pointer_cast<Texture>(model->FindSubAsset("texture/Albedo"));
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->IsLoaded());
    EXPECT_EQ(texture->GetWidth(), 2u);
    EXPECT_EQ(texture->GetSettings().filter, Renderer::Common::TextureFilter::Nearest) << "from the glTF sampler";
    EXPECT_EQ(texture->GetSettings().wrap, Renderer::Common::TextureWrap::ClampToEdge);
    EXPECT_TRUE(texture->GetSettings().srgb) << "a base colour texture";

    const auto red = model->GetMaterials()[0];
    EXPECT_EQ(red->GetBaseColorTexture(), texture);
    EXPECT_EQ(red->GetShading(), Rendering::ShadingModel::Unlit) << "KHR_materials_unlit";
    // Saved by UUID, since its resource path is the model's file; and that UUID loads it back
    EXPECT_EQ(red->ToJson().at("baseColorTexture"), texture->GetUUID().ToString());
    EXPECT_EQ(Loader().LoadByUUID<Texture>(texture->GetUUID()), texture);
}

TEST_F(ModelAssetTest, ImportSettingsComeFromCustomDataAndSurviveTheIndexWrite)
{
    IO::AssetMetadata meta = IO::AssetMetadata::FromFile(MetaPath("models/robot.glb"));
    meta.customData = {{"model", {{"scale", 2.0}, {"importMaterials", false}, {"generateNormals", "never"},
                                  {"mergeSubmeshesByMaterial", true}}}};
    ASSERT_TRUE(meta.SaveToFile(MetaPath("models/robot.glb")));
    Loader().Initialize(s_root);

    const auto model = Loader().Load<Model>(kRobot);
    ASSERT_NE(model, nullptr);
    ModelSettings expected;
    expected.scale = 2.0f;
    expected.importMaterials = false;
    expected.generateNormals = ModelNormals::Never;
    expected.mergeSubmeshesByMaterial = true;
    EXPECT_EQ(model->GetSettings(), expected);
    EXPECT_TRUE(model->GetMaterials().empty());
    EXPECT_TRUE(model->GetTextures().empty());
    EXPECT_EQ(model->GetMeshes()[1]->GetVertices().size(), 4u) << "no generated normals: the quad's 4 vertices";
    EXPECT_FLOAT_EQ(model->GetMeshes()[1]->GetBounds().max.x, 1.0f) << "scaled by 2";

    const json customData = ReadMetaCustomData("models/robot.glb");
    EXPECT_TRUE(customData.contains("model")) << "the settings are kept";
    EXPECT_TRUE(customData.contains("subAssets"));
}

TEST_F(ModelAssetTest, BadSettingsAreIgnoredWithAWarning)
{
    const WarningCapture warnings;
    const ModelSettings settings = Model::ParseSettings(
        json{{"model", {{"scale", -1}, {"importMaterials", "yes"}, {"generateNormals", "sometimes"}, {"bogus", 1}}}});
    EXPECT_EQ(settings, ModelSettings{});
    EXPECT_TRUE(warnings.Mentions("scale"));
    EXPECT_TRUE(warnings.Mentions("importMaterials"));
    EXPECT_TRUE(warnings.Mentions("generateNormals"));
    EXPECT_TRUE(warnings.Mentions("bogus"));
    EXPECT_EQ(Model::ParseSettings(json{{"texture", {{"srgb", true}}}}), ModelSettings{}) << "no model key";
    EXPECT_EQ(Model::ParseSettings(json{{"model", {{"generateNormals", "always"}}}}).generateNormals, ModelNormals::Always);
}

TEST_F(ModelAssetTest, AModelThatDoesntImportFailsToLoadWithAnError)
{
    WriteFile("models/broken.glb", {'g', 'l', 'T', 'F', 2, 0, 0, 0, 200, 0, 0, 0});
    Loader().Initialize(s_root);
    const WarningCapture errors;
    EXPECT_EQ(Loader().Load<Model>(IO::ResourcePath("res://models/broken.glb")), nullptr);
    EXPECT_TRUE(errors.Mentions("broken.glb"));
}
