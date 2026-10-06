#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/rendering/Texture.hpp"

#include "MeshTestSupport.hpp"
#include "ModelTestSupport.hpp"

// Model::Instantiate: the GameObject hierarchy (shape, names, local transforms), a MeshRenderer per mesh node with
// the mesh's materials in its slots, fresh UUIDs for every instance; a scene holding an instance saved and loaded
// again with its sub-asset references resolved; and the committed sample model (tests/assets/models).

using namespace N2Engine;
using Rendering::Material;
using Rendering::Mesh;
using Rendering::MeshRenderer;
using Rendering::Model;
namespace fs = std::filesystem;

namespace
{
    std::shared_ptr<Model> RobotModel()
    {
        const std::vector<std::uint8_t> glb = ModelTestSupport::Robot().ToGlb();
        return Model::LoadFromMemory(glb, {}, {}, "robot");
    }

    void ExpectVector(const Math::Vector3 &actual, const float x, const float y, const float z, const std::string &what)
    {
        EXPECT_NEAR(actual.x, x, 1e-5f) << what;
        EXPECT_NEAR(actual.y, y, 1e-5f) << what;
        EXPECT_NEAR(actual.z, z, 1e-5f) << what;
    }

    /// Every GameObject and component UUID in a hierarchy
    void CollectUuids(const GameObject &gameObject, std::set<std::string> &out)
    {
        out.insert(gameObject.GetUUID().ToString());
        for (const auto &component : gameObject.GetAllComponents())
        {
            out.insert(component->GetUUID().ToString());
        }
        for (const auto &child : gameObject.GetChildren())
        {
            CollectUuids(*child, out);
        }
    }
}

TEST(ModelInstantiateTest, BuildsTheNodeHierarchyWithNamesAndLocalTransforms)
{
    const auto model = RobotModel();
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->GetNodes().size(), 2u);
    ASSERT_EQ(model->GetRootNodes().size(), 1u);

    const auto root = model->Instantiate();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->GetName(), "robot") << "the root object is named after the model";
    EXPECT_EQ(root->GetScene(), nullptr) << "in no scene until added";
    ASSERT_NE(root->GetPositionable(), nullptr);
    ASSERT_EQ(root->GetChildCount(), 1u);

    const auto robot = root->GetChild(0);
    EXPECT_EQ(robot->GetName(), "Robot");
    ExpectVector(robot->GetPositionable()->GetLocalPosition(), 0, 1, 0, "Robot position");
    ASSERT_EQ(robot->GetChildCount(), 1u);

    const auto arm = robot->GetChild(0);
    EXPECT_EQ(arm->GetName(), "Arm");
    EXPECT_EQ(arm->GetChildCount(), 0u);
    ExpectVector(arm->GetPositionable()->GetLocalPosition(), 1, 0, 0, "Arm position");
    ExpectVector(arm->GetPositionable()->GetLocalScale(), 2, 2, 2, "Arm scale");
    const Math::Quaternion rotation = arm->GetPositionable()->GetLocalRotation();
    const float s = std::sqrt(0.5f);
    EXPECT_NEAR(std::abs(rotation.w * s + rotation.y * s), 1.0f, 1e-4f) << "90 degrees about y";
    // The arm's world position: the robot's plus its own
    ExpectVector(arm->GetPositionable()->GetPosition(), 1, 1, 0, "Arm world position");
}

TEST(ModelInstantiateTest, EveryMeshNodeGetsAMeshRendererWithItsMaterialsInTheSlots)
{
    const auto model = RobotModel();
    ASSERT_NE(model, nullptr);
    const auto root = model->Instantiate();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->GetComponent<MeshRenderer>(), nullptr) << "the root has no mesh";

    const auto *body = root->GetChild(0)->GetComponent<MeshRenderer>();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->GetMesh(), model->FindSubAsset("mesh/Body"));
    ASSERT_EQ(body->GetMaterialCount(), 1u);
    EXPECT_EQ(body->GetMaterial(0), model->FindSubAsset("material/Red"));

    const auto *arm = root->GetChild(0)->GetChild(0)->GetComponent<MeshRenderer>();
    ASSERT_NE(arm, nullptr);
    EXPECT_EQ(arm->GetMesh(), model->FindSubAsset("mesh/Arm"));
    EXPECT_EQ(arm->GetMaterial(0), model->FindSubAsset("material/Green"));

    // Without materials, every slot is empty (drawn with the default)
    Rendering::ModelSettings bare;
    bare.importMaterials = false;
    const std::vector<std::uint8_t> glb = ModelTestSupport::Robot().ToGlb();
    const auto plain = Model::LoadFromMemory(glb, {}, bare, "plain");
    ASSERT_NE(plain, nullptr);
    const auto plainRoot = plain->Instantiate();
    const auto *plainBody = plainRoot->GetChild(0)->GetComponent<MeshRenderer>();
    ASSERT_NE(plainBody, nullptr);
    EXPECT_EQ(plainBody->GetMaterial(0), nullptr);
    EXPECT_EQ(plainBody->GetEffectiveMaterial(0), Material::GetDefault());
}

TEST(ModelInstantiateTest, EveryInstanceHasFreshUuidsAndSharesTheModelsAssets)
{
    const auto model = RobotModel();
    ASSERT_NE(model, nullptr);
    const auto first = model->Instantiate();
    const auto second = model->Instantiate();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);

    std::set<std::string> a;
    std::set<std::string> b;
    CollectUuids(*first, a);
    CollectUuids(*second, b);
    EXPECT_EQ(a.size(), 5u) << "3 objects and 2 MeshRenderers";
    std::vector<std::string> shared;
    std::ranges::set_intersection(a, b, std::back_inserter(shared));
    EXPECT_TRUE(shared.empty()) << "no GameObject or component UUID is reused";

    EXPECT_EQ(first->GetChild(0)->GetComponent<MeshRenderer>()->GetMesh(),
              second->GetChild(0)->GetComponent<MeshRenderer>()->GetMesh())
        << "both draw the model's one mesh";
}

TEST(ModelInstantiateTest, UnnamedNodesGetANameAndAModelThatDidntLoadInstantiatesNothing)
{
    GltfTest::Builder b;
    const int mesh = b.AddMesh("", {GltfTest::Builder::Primitive(b.AddFloats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3))});
    b.SetScene({b.AddNode("", {{"mesh", mesh}})});
    const std::vector<std::uint8_t> glb = b.ToGlb();
    const auto model = Model::LoadFromMemory(glb);
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->GetName(), "Model");
    EXPECT_NE(model->FindSubAsset("mesh/0"), nullptr) << "an unnamed mesh is keyed by index";
    EXPECT_FALSE(model->GetMeshes()[0]->IsSubResource()) << "a model from memory makes runtime assets";
    const auto root = model->Instantiate();
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->GetChildCount(), 1u);
    EXPECT_EQ(root->GetChild(0)->GetName(), "Node 0");

    const MeshTestSupport::WarningCapture errors;
    Model empty;
    EXPECT_EQ(empty.Instantiate(), nullptr);
    EXPECT_FALSE(errors.messages.empty());
    EXPECT_EQ(Model::LoadFromMemory(std::vector<std::uint8_t>{1, 2, 3}), nullptr);
}

TEST(ModelInstantiateTest, TheCommittedSampleModelImportsAndInstantiates)
{
    const fs::path path = fs::path(N2_TEST_ASSETS_DIR) / "models" / "textured_cube.glb";
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open()) << path.string();
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto model = Model::LoadFromMemory(bytes, path.parent_path(), {}, "textured_cube");
    ASSERT_NE(model, nullptr);
    EXPECT_TRUE(model->GetImportWarnings().empty());

    ASSERT_EQ(model->GetMeshes().size(), 1u);
    const auto cube = model->GetMeshes()[0];
    ASSERT_NE(cube, nullptr);
    EXPECT_EQ(cube->GetSubmeshCount(), 2u) << "one submesh per material";
    EXPECT_EQ(cube->GetVertices().size(), 24u);
    EXPECT_FLOAT_EQ(cube->GetBounds().max.x, 0.5f);

    ASSERT_EQ(model->GetMaterials().size(), 2u);
    const auto checker = model->GetMaterials()[0];
    EXPECT_EQ(checker->GetShading(), Rendering::ShadingModel::Lit);
    EXPECT_NEAR(checker->GetSmoothness(), 0.5f, 1e-6f) << "1 - roughness 0.5";
    const auto texture = checker->GetBaseColorTexture();
    ASSERT_NE(texture, nullptr);
    ASSERT_EQ(texture->GetWidth(), 16u);
    // Stored bottom row first: the first pixel is the image's bottom-left (blue), the last its top-right (green)
    const auto &pixels = texture->GetPixels();
    EXPECT_EQ(pixels[0], 0);
    EXPECT_EQ(pixels[2], 255) << "blue at the bottom left";
    EXPECT_EQ(pixels[pixels.size() - 3], 255) << "green at the top right";
    EXPECT_EQ(texture->GetSettings().filter, Renderer::Common::TextureFilter::Nearest);
    EXPECT_NEAR(model->GetMaterials()[1]->GetBaseColor().b, 1.0f, 1e-6f);

    // The textured face (+Z) samples the bottom of the image at its bottom edge: v = 0 there after the flip
    for (const auto &vertex : cube->GetVertices())
    {
        if (vertex.normal[2] > 0.5f && vertex.position[1] < 0.0f)
        {
            EXPECT_FLOAT_EQ(vertex.texCoord[1], 0.0f);
        }
    }

    const auto root = model->Instantiate();
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->GetChildCount(), 2u);
    EXPECT_EQ(root->GetChild(0)->GetName(), "Cube");
    EXPECT_EQ(root->GetChild(1)->GetName(), "Mirrored");
    ASSERT_EQ(root->GetChild(0)->GetChildCount(), 1u);
    EXPECT_EQ(root->GetChild(0)->GetChild(0)->GetName(), "Child");
    ExpectVector(root->GetChild(1)->GetPositionable()->GetLocalScale(), -1, 1, 1, "mirrored scale");
    const auto *renderer = root->GetChild(1)->GetComponent<MeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    EXPECT_EQ(renderer->GetMaterialCount(), 2u);
    EXPECT_EQ(renderer->GetMaterial(1), model->GetMaterials()[1]);
}

// ===== A scene saved and loaded again =====

class ModelSceneRoundTripTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;

    void SetUp() override
    {
        s_root = fs::temp_directory_path() / "n2engine_model_scene_round_trip";
        std::error_code ec;
        fs::remove_all(s_root, ec);
        fs::create_directories(s_root / "assets" / "models");
        const std::vector<std::uint8_t> glb = ModelTestSupport::Robot().ToGlb();
        std::ofstream(s_root / "assets" / "models" / "robot.glb", std::ios::binary)
            .write(reinterpret_cast<const char *>(glb.data()), static_cast<std::streamsize>(glb.size()));

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "ModelSceneRoundTrip"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);
    }

    void TearDown() override
    {
        IO::ResourceLoader::Instance().ClearCache();
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }
};

TEST_F(ModelSceneRoundTripTest, ASavedInstanceLoadsBackWithItsSubAssetsResolved)
{
    const IO::ResourcePath path("res://models/robot.glb");
    nlohmann::json saved;
    Math::UUID armMesh;
    {
        const auto model = IO::ResourceLoader::Instance().Load<Model>(path);
        ASSERT_NE(model, nullptr);
        armMesh = model->FindSubAsset("mesh/Arm")->GetUUID();
        auto scene = Scene::Create("ModelScene");
        scene->AddRootGameObject(model->Instantiate());
        saved = scene->Serialize();
    }
    // The saved MeshRenderer names the sub-asset's deterministic UUID
    EXPECT_NE(saved.dump().find(armMesh.ToString()), std::string::npos);
    EXPECT_EQ(armMesh, IO::ResourceUUID::FromSubAsset(path, "mesh/Arm"));

    // A restart: nothing cached, the model loads again through its sub-asset references
    IO::ResourceLoader::Instance().Initialize(s_root);
    const MeshTestSupport::WarningCapture warnings;
    const auto loaded = Scene::FromJSON(saved);
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetRootGameObjects().size(), 1u);
    const auto arm = loaded->GetRootGameObjects()[0]->FindChildRecursive("Arm");
    ASSERT_NE(arm, nullptr);
    const auto *renderer = arm->GetComponent<MeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    ASSERT_NE(renderer->GetMesh(), nullptr);
    EXPECT_EQ(renderer->GetMesh()->GetUUID(), armMesh);
    EXPECT_EQ(renderer->GetMesh()->GetSubAssetKey(), "mesh/Arm");
    ASSERT_NE(renderer->GetMaterial(0), nullptr);
    EXPECT_EQ(renderer->GetMaterial(0)->GetSubAssetKey(), "material/Green");
    EXPECT_FALSE(warnings.Mentions("not found")) << "every reference resolved";
    EXPECT_NE(IO::ResourceLoader::Instance().GetCached<Model>(path), nullptr) << "the model loaded to resolve them";
}
