#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <assetimport/ModelImporters.hpp>

// Which importers a build has (#3 P5): glTF always, FBX and OBJ only with N2ENGINE_MODEL_UFBX. The engine asks this
// registry which extensions to load, so the answers here must match how the library was built.
using namespace N2Engine::AssetImport;

namespace
{
    bool Contains(const std::vector<std::string> &list, const std::string &item)
    {
        return std::ranges::find(list, item) != list.end();
    }
}

TEST(ModelImportersTest, GltfIsAlwaysBuilt)
{
    ASSERT_FALSE(GetModelImporters().empty());
    for (const char *extension : {".gltf", ".glb", ".GLB", ".Gltf"})
    {
        const IModelImporter *importer = FindModelImporter(extension);
        ASSERT_NE(importer, nullptr) << extension;
        EXPECT_NE(importer->GetName().find("glTF"), std::string_view::npos) << extension;
    }
    const std::vector<std::string> extensions = GetModelExtensions();
    EXPECT_TRUE(Contains(extensions, ".gltf"));
    EXPECT_TRUE(Contains(extensions, ".glb"));
}

TEST(ModelImportersTest, OtherExtensionsHaveNoImporter)
{
    for (const char *extension : {"", ".png", ".blend", ".dae", "glb", ".glb "})
    {
        EXPECT_EQ(FindModelImporter(extension), nullptr) << "'" << extension << "'";
    }
}

TEST(ModelImportersTest, EveryListedExtensionIsHandled)
{
    for (const std::string &extension : GetModelExtensions())
    {
        EXPECT_NE(FindModelImporter(extension), nullptr) << extension;
    }
    // And every importer handles at least one listed extension
    for (const IModelImporter *importer : GetModelImporters())
    {
        const std::vector<std::string> extensions = GetModelExtensions();
        EXPECT_TRUE(std::ranges::any_of(extensions, [&](const std::string &e) { return importer->HandlesExtension(e); }))
            << importer->GetName();
    }
}

#ifdef N2ENGINE_MODEL_UFBX

TEST(ModelImportersTest, UfbxAddsFbxAndObj)
{
    EXPECT_TRUE(IsUfbxAvailable());
    for (const char *extension : {".fbx", ".FBX", ".obj", ".Obj"})
    {
        const IModelImporter *importer = FindModelImporter(extension);
        ASSERT_NE(importer, nullptr) << extension;
        EXPECT_NE(importer->GetName().find("ufbx"), std::string_view::npos) << extension;
    }
    const std::vector<std::string> extensions = GetModelExtensions();
    EXPECT_TRUE(Contains(extensions, ".fbx"));
    EXPECT_TRUE(Contains(extensions, ".obj"));
    EXPECT_EQ(GetModelImporters().size(), 2u);
}

#else

TEST(ModelImportersTest, WithoutUfbxFbxAndObjAreNotHandled)
{
    EXPECT_FALSE(IsUfbxAvailable());
    EXPECT_EQ(FindModelImporter(".fbx"), nullptr);
    EXPECT_EQ(FindModelImporter(".obj"), nullptr);
    const std::vector<std::string> extensions = GetModelExtensions();
    EXPECT_FALSE(Contains(extensions, ".fbx"));
    EXPECT_FALSE(Contains(extensions, ".obj"));
    EXPECT_EQ(GetModelImporters().size(), 1u);
}

#endif
