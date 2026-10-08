#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/SceneFile.hpp>
#include <math/UUID.hpp>

using namespace N2Engine;
namespace fs = std::filesystem;

// The project-facing parts of ResourceLoader: a rescan forgetting deleted files and reporting what changed, the
// per-project user:// folder, and .scene files as "Scene" assets
namespace
{
    class ResourceLoaderProjectTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _root = fs::temp_directory_path() / "n2-loader-project-test" / info->name();
            std::error_code error;
            fs::remove_all(_root, error);
            fs::create_directories(_root / "assets" / "scenes");
            // Outside the project folder, which one test moves
            _userData = _root.parent_path() / (_root.filename().string() + "-user");

            IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, info->name()));
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_root.parent_path(), error);
        }

        void Write(const std::string &relative, const std::string &text)
        {
            const fs::path path = _root / "assets" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        static IO::ResourceLoader &Loader() { return IO::ResourceLoader::Instance(); }

        /// A folder rename, retried for a moment: on Windows a virus scanner or the search indexer may still hold
        /// a file the loader just wrote
        static bool RenameWithRetry(const fs::path &from, const fs::path &to)
        {
            std::error_code error;
            for (int attempt = 0; attempt < 20; ++attempt)
            {
                fs::rename(from, to, error);
                if (!error)
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ADD_FAILURE() << "can't rename " << from << " to " << to << ": " << error.message();
            return false;
        }

        static std::vector<std::string> Strings(const std::vector<IO::ResourcePath> &paths)
        {
            std::vector<std::string> strings;
            for (const IO::ResourcePath &path : paths)
                strings.push_back(path.ToString());
            return strings;
        }

        fs::path _root;
        fs::path _userData;
    };
}

TEST_F(ResourceLoaderProjectTest, ARescanForgetsDeletedFilesAndReportsThem)
{
    Write("a.mat", "{}");
    Write("scenes/Level.scene", R"({"name":"Level","rootGameObjects":[]})");
    Loader().Initialize(_root, _userData);
    const IO::ResourcePath deleted("res://a.mat");
    const Math::UUID deletedUuid = Loader().GetUUID(deleted);
    ASSERT_TRUE(Loader().Exists(deleted));
    ASSERT_NE(Loader().GetMetadata(deletedUuid), nullptr);

    fs::remove(_root / "assets" / "a.mat");
    const IO::ResourceLoader::RescanResult changes = Loader().RescanAssets();

    EXPECT_EQ(Strings(changes.removed), (std::vector<std::string>{"res://a.mat"}));
    EXPECT_TRUE(changes.added.empty());
    EXPECT_TRUE(changes.modified.empty());
    EXPECT_FALSE(Loader().Exists(deleted)) << "a deleted file is no longer an asset";
    EXPECT_EQ(Loader().GetMetadata(deletedUuid), nullptr) << "nor found by its UUID";
    EXPECT_EQ(Loader().GetUUID(deleted), Math::UUID::ZERO);
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://scenes/Level.scene"))) << "the rest is kept";
    for (const IO::AssetMetadata &meta : Loader().GetAllAssets())
    {
        EXPECT_NE(meta.resourcePath, deleted);
    }

    // The .meta stays, so a file put back gets its import settings (and its UUID) back
    EXPECT_TRUE(fs::exists(_root / ".import" / "a.mat.meta"));
    Write("a.mat", "{}");
    const IO::ResourceLoader::RescanResult restored = Loader().RescanAssets();
    EXPECT_EQ(Strings(restored.added), (std::vector<std::string>{"res://a.mat"}));
    EXPECT_EQ(Loader().GetUUID(deleted), deletedUuid);
}

TEST_F(ResourceLoaderProjectTest, ARescanReportsAddedAndModifiedFilesSorted)
{
    Write("b.mat", "{}");
    Loader().Initialize(_root, _userData);

    Write("z.mat", "{}");
    Write("a/c.mat", "{}");
    Write("b.mat", R"({"changed": "and longer"})");
    const IO::ResourceLoader::RescanResult changes = Loader().RescanAssets();
    EXPECT_EQ(Strings(changes.added), (std::vector<std::string>{"res://a/c.mat", "res://z.mat"}));
    EXPECT_EQ(Strings(changes.modified), (std::vector<std::string>{"res://b.mat"}));
    EXPECT_TRUE(changes.removed.empty());

    const IO::ResourceLoader::RescanResult nothing = Loader().RescanAssets();
    EXPECT_TRUE(nothing.Empty());
}

TEST_F(ResourceLoaderProjectTest, ADeletedLoadedAssetIsDroppedFromTheCache)
{
    Write("scenes/Gone.scene", R"({"name":"Gone","rootGameObjects":[]})");
    Loader().Initialize(_root, _userData);
    const IO::ResourcePath path("res://scenes/Gone.scene");
    const auto held = Loader().Load<SceneFile>(path);
    ASSERT_NE(held, nullptr);
    const Math::UUID uuid = held->GetUUID();

    fs::remove(_root / "assets" / "scenes" / "Gone.scene");
    (void)Loader().RescanAssets();

    EXPECT_EQ(Loader().GetCached<SceneFile>(path), nullptr);
    EXPECT_EQ(Loader().GetCachedByUUID<SceneFile>(uuid), nullptr);
    EXPECT_EQ(held->GetSceneName(), "Gone") << "whoever holds it keeps it";
}

TEST_F(ResourceLoaderProjectTest, UserPathsResolveUnderTheProjectsOwnFolder)
{
    Loader().Initialize(_root, _userData);
    EXPECT_EQ(Loader().GetUserDataRoot(), _userData);
    EXPECT_TRUE(fs::is_directory(_userData)) << "Initialize creates it";
    EXPECT_EQ(Loader().Resolve(IO::ResourcePath("user://saves/1.json")),
              (_userData / "saves/1.json").lexically_normal());
    EXPECT_TRUE(Loader().Resolve(IO::ResourcePath("user://../escape.json")).empty());
}

TEST_F(ResourceLoaderProjectTest, TwoProjectsGetTwoUserFolders)
{
    IO::ProjectFile first;
    first.name = "Same Name";
    first.projectId = Math::UUID::Random();
    IO::ProjectFile second = first;
    second.projectId = Math::UUID::Random();

    Loader().Initialize(_root, first.UserDataPath(_userData));
    const fs::path firstSave = Loader().Resolve(IO::ResourcePath("user://save.json"));
    Loader().Initialize(_root, second.UserDataPath(_userData));
    const fs::path secondSave = Loader().Resolve(IO::ResourcePath("user://save.json"));
    EXPECT_NE(firstSave, secondSave);
    EXPECT_EQ(firstSave.parent_path().parent_path(), _userData);
}

TEST_F(ResourceLoaderProjectTest, WithoutAUserFolderUserPathsUseTheSharedOne)
{
    Loader().Initialize(_root);
    EXPECT_EQ(Loader().GetUserDataRoot(), IO::ProjectFile::UserDataBase());
}

TEST_F(ResourceLoaderProjectTest, SceneFilesAreSceneAssetsWithPathDerivedUuids)
{
    Write("scenes/Main.scene", R"({"name":"Main","rootGameObjects":[]})");
    Loader().Initialize(_root, _userData);

    const IO::ResourcePath path("res://scenes/Main.scene");
    const IO::AssetMetadata *meta = Loader().GetMetadata(path);
    ASSERT_NE(meta, nullptr) << ".scene files are scanned";
    EXPECT_EQ(meta->resourceType, "Scene") << "Scene's own resource type, not a second kind";
    EXPECT_EQ(meta->uuid, IO::ResourceUUID::FromPath(path));

    const auto scene = Loader().Load<SceneFile>(path);
    ASSERT_NE(scene, nullptr);
    EXPECT_EQ(scene->GetResourceType(), "Scene");
    EXPECT_EQ(scene->GetUUID(), meta->uuid);
    EXPECT_EQ(scene->GetSceneName(), "Main");
    EXPECT_EQ(Loader().LoadByUUID<SceneFile>(meta->uuid), scene);
}

TEST_F(ResourceLoaderProjectTest, ANonAsciiFileNameIsAUtf8ResourcePathWithItsMetaBesideIt)
{
    // "Caf\xC3\xA9" is "Cafe" with an e-acute, as UTF-8 (the source stays ASCII); the Windows code page needn't be
    // able to spell it
    const std::string name = "Caf\xC3\xA9.scene";
    const fs::path file = _root / "assets" / "scenes" / IO::PathFromUtf8(name);
    std::ofstream(file, std::ios::binary) << R"({"name":"Cafe","rootGameObjects":[]})";
    Loader().Initialize(_root, _userData);

    const IO::ResourcePath path("res://scenes/" + name);
    const IO::AssetMetadata *meta = Loader().GetMetadata(path);
    ASSERT_NE(meta, nullptr) << "indexed under its UTF-8 res:// path";
    EXPECT_EQ(meta->resourcePath.ToString(), "res://scenes/" + name);
    EXPECT_EQ(meta->uuid, IO::ResourceUUID::FromPath(path));
    EXPECT_TRUE(fs::exists(_root / ".import" / "scenes" / IO::PathFromUtf8(name + ".meta")));
    EXPECT_EQ(Loader().Resolve(path), file.lexically_normal());
    const auto scene = Loader().Load<SceneFile>(path);
    ASSERT_NE(scene, nullptr);
    EXPECT_EQ(scene->GetSceneName(), "Cafe");

    // A second scan finds nothing new
    EXPECT_TRUE(Loader().RescanAssets().Empty());
}

TEST(ResourcePathUtf8Test, PartsAreUtf8)
{
    const IO::ResourcePath path("res://d\xC3\xA9j\xC3\xA0/vu.scene");
    EXPECT_EQ(path.GetPath(), "d\xC3\xA9j\xC3\xA0/vu.scene");
    EXPECT_EQ(path.GetParent().ToString(), "res://d\xC3\xA9j\xC3\xA0");
    EXPECT_EQ(path.GetFilename(), "vu.scene");
    EXPECT_EQ((IO::ResourcePath("res://a") / "\xC3\xA9.lua").GetPath(), "a/\xC3\xA9.lua");
    EXPECT_EQ(IO::PathToUtf8(IO::PathFromUtf8("x/\xC3\xA9")), "x/\xC3\xA9");
}

TEST_F(ResourceLoaderProjectTest, ASceneFileThatIsntAnObjectDoesntLoad)
{
    Write("scenes/Broken.scene", "[1, 2");
    Loader().Initialize(_root, _userData);
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://scenes/Broken.scene"))) << "it is still a file of the project";
    EXPECT_EQ(Loader().Load<SceneFile>(IO::ResourcePath("res://scenes/Broken.scene")), nullptr);
}

TEST_F(ResourceLoaderProjectTest, TheNamespaceIsTheProjectIdSoUuidsDontDependOnTheFolder)
{
    // The same project in two folders (moved, or a second clone) gives its assets the same UUIDs
    const Math::UUID projectId = Math::UUID::FromString("8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10").value();
    const fs::path elsewhere = _root.parent_path() / (_root.filename().string() + "-moved");
    std::error_code error;
    fs::remove_all(elsewhere, error);

    Write("scenes/Main.scene", R"({"name":"Main","rootGameObjects":[]})");
    IO::ResourceUUID::Initialize(projectId);
    Loader().Initialize(_root, _userData);
    const Math::UUID here = Loader().GetUUID(IO::ResourcePath("res://scenes/Main.scene"));

    ASSERT_TRUE(RenameWithRetry(_root, elsewhere));
    IO::ResourceUUID::Initialize(projectId);
    Loader().Initialize(elsewhere, _userData);
    const Math::UUID there = Loader().GetUUID(IO::ResourcePath("res://scenes/Main.scene"));
    ASSERT_TRUE(RenameWithRetry(elsewhere, _root));

    EXPECT_NE(here, Math::UUID::ZERO);
    EXPECT_EQ(here, there);
}
