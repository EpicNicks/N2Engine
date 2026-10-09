#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/GameObjectScene.hpp"
#include "engine/io/Resources.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/Model.hpp"

#include "MeshTestSupport.hpp"

// FBX and OBJ models (#3 P5): with the optional ufbx importer (N2ENGINE_MODEL_UFBX) the committed FBX loads as a
// Model like the glTF cube does, through Model::Load and LoadFromMemory; without it an .fbx is refused with an error
// and is not a model file at all.

using namespace N2Engine;
using Rendering::Model;
namespace fs = std::filesystem;

namespace
{
    fs::path CubePath()
    {
        return fs::path(N2_TEST_ASSETS_DIR) / "models" / "textured_cube.fbx";
    }

    std::vector<std::uint8_t> CubeBytes()
    {
        std::ifstream file(CubePath(), std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
}

#ifdef N2ENGINE_MODEL_UFBX

TEST(ModelFbxTest, TheCommittedFbxLoadsFromMemoryByItsExtension)
{
    const std::vector<std::uint8_t> bytes = CubeBytes();
    ASSERT_FALSE(bytes.empty()) << CubePath().string();
    const auto model = Model::LoadFromMemory(bytes, CubePath().parent_path(), {}, "textured_cube", ".fbx");
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->GetNodes().size(), 3u);
    EXPECT_EQ(model->GetRootNodes().size(), 2u);
    ASSERT_GE(model->GetMeshes().size(), 1u);
    ASSERT_NE(model->GetMeshes()[0], nullptr);
    EXPECT_EQ(model->GetMeshes()[0]->GetVertices().size(), 24u);
    EXPECT_EQ(model->GetMeshes()[0]->GetSubmeshCount(), 2u);
    EXPECT_EQ(model->GetMaterials().size(), 2u);
}

TEST(ModelFbxTest, AnFbxWithoutItsExtensionIsReadAsGltfAndFails)
{
    const MeshTestSupport::WarningCapture errors;
    EXPECT_EQ(Model::LoadFromMemory(CubeBytes()), nullptr);
    EXPECT_FALSE(errors.messages.empty());
}

TEST(ModelFbxTest, TheCommittedFbxLoadsFromAFileAndInstantiates)
{
    Model model;
    ASSERT_TRUE(model.Load(CubePath()));
    EXPECT_EQ(model.GetName(), "textured_cube");
    EXPECT_EQ(model.GetNodes().size(), 3u);
    const auto root = model.Instantiate();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->GetChildCount(), 2u);
}

TEST(ModelFbxTest, AnFbxFileLoadsThroughResourcesByItsExtension)
{
    const auto model = IO::Resources::Instance().Load<Model>(CubePath());
    ASSERT_NE(model, nullptr) << "Model::RegisterLoader registers .fbx when ufbx is built";
    EXPECT_EQ(model->GetNodes().size(), 3u);
}

#else

TEST(ModelFbxTest, WithoutUfbxAnFbxIsNotAModelFile)
{
    const MeshTestSupport::WarningCapture errors;
    EXPECT_EQ(Model::LoadFromMemory(CubeBytes(), {}, {}, "cube", ".fbx"), nullptr);
    EXPECT_FALSE(errors.messages.empty());
}

#endif
