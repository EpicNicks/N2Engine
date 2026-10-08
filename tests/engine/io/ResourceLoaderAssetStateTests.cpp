#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <engine/io/AssetMetadata.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourcePath.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <math/UUID.hpp>

using namespace N2Engine;
using nlohmann::json;
namespace fs = std::filesystem;

// E8 (#81): what a scan keeps where (.meta files hold UUIDs, types and import settings; the last-seen size and time go
// to .n2/asset-state.json), the cheap check for changed files the editor's watcher uses, RefreshAsset for one file,
// SetImportSettings and the sub-asset listing
namespace
{
    class AssetStateTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _root = fs::temp_directory_path() / "n2-asset-state-test" / info->name();
            std::error_code error;
            fs::remove_all(_root, error);
            fs::create_directories(_root / "assets" / "scenes");
            _userData = _root.parent_path() / (_root.filename().string() + "-user");

            IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, info->name()));
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_root.parent_path(), error);
        }

        static IO::ResourceLoader &Loader() { return IO::ResourceLoader::Instance(); }

        void Write(const std::string &relative, const std::string &text)
        {
            const fs::path path = _root / "assets" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        void Init() { Loader().Initialize(_root, _userData); }

        [[nodiscard]] fs::path Meta(const std::string &relative) const { return _root / ".import" / (relative + ".meta"); }
        [[nodiscard]] fs::path Asset(const std::string &relative) const { return _root / "assets" / relative; }

        static std::string ReadFile(const fs::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            std::ostringstream text;
            text << file.rdbuf();
            return text.str();
        }

        static json ReadJson(const fs::path &path) { return json::parse(ReadFile(path)); }

        /// Moves a file's modification time on (a later edit), keeping its size
        static void Touch(const fs::path &path, const std::chrono::milliseconds by = std::chrono::milliseconds(2000))
        {
            fs::last_write_time(path, fs::last_write_time(path) + by);
        }

        fs::path _root;
        fs::path _userData;
    };
}

TEST_F(AssetStateTest, AMetaHoldsNoSizeOrTimeAndIsNotRewrittenWhenTheFileChanges)
{
    Write("a.mat", "{}");
    Init();

    const json meta = ReadJson(Meta("a.mat"));
    EXPECT_TRUE(meta.contains("uuid"));
    EXPECT_EQ(meta.at("resourcePath"), "res://a.mat");
    EXPECT_EQ(meta.at("resourceType"), "Material");
    EXPECT_FALSE(meta.contains("lastModified")) << "it changes with every edit, and a .meta is committed";
    EXPECT_FALSE(meta.contains("fileSize"));

    const std::string before = ReadFile(Meta("a.mat"));
    Write("a.mat", R"({"changed": "and longer"})");
    const IO::ResourceLoader::RescanResult changes = Loader().RescanAssets();
    ASSERT_EQ(changes.modified.size(), 1u);
    EXPECT_EQ(ReadFile(Meta("a.mat")), before) << "an edit of the file leaves its .meta alone";
    EXPECT_EQ(Loader().GetMetadata(IO::ResourcePath("res://a.mat"))->fileSize, 25u) << "the size is still known";
}

TEST_F(AssetStateTest, TheStateFileHoldsWhatTheScanSaw)
{
    Write("a.mat", "{}");
    Write("scenes/Level.scene", R"({"name":"Level","rootGameObjects":[]})");
    Init();

    ASSERT_TRUE(fs::exists(Loader().GetAssetStatePath()));
    EXPECT_EQ(Loader().GetAssetStatePath(), _root / ".n2" / "asset-state.json");
    const json state = ReadJson(Loader().GetAssetStatePath());
    EXPECT_EQ(state.at("formatVersion"), 1);
    const json &assets = state.at("assets");
    ASSERT_TRUE(assets.contains("res://a.mat"));
    EXPECT_EQ(assets.at("res://a.mat").at("fileSize"), 2u);
    EXPECT_GT(assets.at("res://a.mat").at("lastModified").get<std::uint64_t>(), 0u);
    EXPECT_TRUE(assets.contains("res://scenes/Level.scene"));

    // A change is written there; a deleted file is dropped
    Write("a.mat", R"({"x": 1})");
    fs::remove(Asset("scenes/Level.scene"));
    (void)Loader().RescanAssets();
    const json after = ReadJson(Loader().GetAssetStatePath());
    EXPECT_EQ(after.at("assets").at("res://a.mat").at("fileSize"), 8u);
    EXPECT_FALSE(after.at("assets").contains("res://scenes/Level.scene"));
}

TEST_F(AssetStateTest, ARestartReadsTheStateAndLeavesTheMetaFilesAlone)
{
    Write("a.mat", "{}");
    Init();
    const std::string meta = ReadFile(Meta("a.mat"));
    const std::string state = ReadFile(Loader().GetAssetStatePath());

    Init();

    EXPECT_EQ(ReadFile(Meta("a.mat")), meta);
    EXPECT_EQ(ReadFile(Loader().GetAssetStatePath()), state) << "nothing changed, so nothing is written";
    EXPECT_FALSE(Loader().AssetsChangedOnDisk());
}

TEST_F(AssetStateTest, ACorruptStateFileIsRebuilt)
{
    Write("a.mat", "{}");
    Init();
    std::ofstream(Loader().GetAssetStatePath(), std::ios::trunc) << "{ not json";

    Init();

    const json state = ReadJson(Loader().GetAssetStatePath());
    EXPECT_TRUE(state.at("assets").contains("res://a.mat"));
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://a.mat")));
}

TEST_F(AssetStateTest, AnOldMetaWithSizeAndTimeIsRewrittenWithoutThemKeepingItsSettings)
{
    Write("a.mat", "{}");
    Init();
    const Math::UUID uuid = Loader().GetUUID(IO::ResourcePath("res://a.mat"));

    // What an older version wrote: the same file, plus lastModified (in seconds) and fileSize, and import settings
    json old = ReadJson(Meta("a.mat"));
    old["lastModified"] = 1700000000;
    old["fileSize"] = 2;
    old["customData"] = {{"material", {{"note", "kept"}}}};
    std::ofstream(Meta("a.mat"), std::ios::trunc) << old.dump(2);

    Init();

    const json meta = ReadJson(Meta("a.mat"));
    EXPECT_FALSE(meta.contains("lastModified"));
    EXPECT_FALSE(meta.contains("fileSize"));
    EXPECT_EQ(meta.at("customData").at("material").at("note"), "kept");
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://a.mat")), uuid);
    EXPECT_GT(Loader().GetMetadata(IO::ResourcePath("res://a.mat"))->lastModified, 1700000000000ull)
        << "the time is in milliseconds now, read from the file";
}

TEST_F(AssetStateTest, AssetsChangedOnDiskSeesAddedEditedAndDeletedFiles)
{
    Write("a.mat", "{}");
    Init();
    EXPECT_FALSE(Loader().AssetsChangedOnDisk()) << "nothing changed since the scan";

    Write("b.mat", "{}");
    EXPECT_TRUE(Loader().AssetsChangedOnDisk()) << "a file was added";
    (void)Loader().RescanAssets();
    EXPECT_FALSE(Loader().AssetsChangedOnDisk());

    Write("a.mat", R"({"edited": true})");
    EXPECT_TRUE(Loader().AssetsChangedOnDisk()) << "a file's size changed";
    (void)Loader().RescanAssets();
    EXPECT_FALSE(Loader().AssetsChangedOnDisk());

    Touch(Asset("a.mat"), std::chrono::milliseconds(50));
    EXPECT_TRUE(Loader().AssetsChangedOnDisk()) << "a file's time changed by 50 ms, its size the same";
    const IO::ResourceLoader::RescanResult touched = Loader().RescanAssets();
    EXPECT_EQ(touched.modified.size(), 1u) << "and the scan sees it as modified too";
    EXPECT_FALSE(Loader().AssetsChangedOnDisk());

    fs::remove(Asset("b.mat"));
    EXPECT_TRUE(Loader().AssetsChangedOnDisk()) << "a file was deleted";
    (void)Loader().RescanAssets();
    EXPECT_FALSE(Loader().AssetsChangedOnDisk());
}

TEST_F(AssetStateTest, AssetsChangedOnDiskIgnoresWhatTheScanIgnores)
{
    Write("a.mat", "{}");
    Init();

    Write("notes.unknownext", "text");
    Write("sub/readme.unknownext", "text");
    Write("a.mat.meta", "{}");
    fs::create_directories(Asset("empty folder"));
    EXPECT_FALSE(Loader().AssetsChangedOnDisk()) << "no loader for the extension, a .meta file, a folder";

    // A real change is still seen, and a rescan makes it the new normal
    Write("zzz.mat", "{}");
    EXPECT_TRUE(Loader().AssetsChangedOnDisk());
    (void)Loader().RescanAssets();
    EXPECT_FALSE(Loader().AssetsChangedOnDisk());
}

TEST_F(AssetStateTest, AnAssetsFolderThatAppearsAfterAScanWithoutOneIsAChange)
{
    // The scan found no assets folder: nothing to watch yet, and no change
    fs::remove_all(_root / "assets");
    Init();
    EXPECT_FALSE(Loader().AssetsChangedOnDisk());

    Write("a.mat", "{}");
    EXPECT_TRUE(Loader().AssetsChangedOnDisk());
    const IO::ResourceLoader::RescanResult changes = Loader().RescanAssets();
    EXPECT_EQ(changes.added.size(), 1u);
    EXPECT_FALSE(Loader().AssetsChangedOnDisk());
}

TEST_F(AssetStateTest, RefreshAssetIndexesOneFile)
{
    Write("a.mat", "{}");
    Init();
    using Result = IO::ResourceLoader::RefreshResult;
    const IO::ResourcePath fresh("res://fresh.mat");

    Write("fresh.mat", "{}");
    EXPECT_EQ(Loader().RefreshAsset(fresh), Result::Added);
    EXPECT_TRUE(Loader().Exists(fresh));
    EXPECT_EQ(Loader().GetUUID(fresh), IO::ResourceUUID::FromPath(fresh));
    EXPECT_TRUE(fs::exists(Meta("fresh.mat")));
    EXPECT_EQ(Loader().RefreshAsset(fresh), Result::Unchanged);

    Write("fresh.mat", R"({"edited": true})");
    EXPECT_EQ(Loader().RefreshAsset(fresh), Result::Modified);
    EXPECT_EQ(Loader().GetMetadata(fresh)->fileSize, 16u);
    EXPECT_TRUE(ReadJson(Loader().GetAssetStatePath()).at("assets").contains("res://fresh.mat"));

    EXPECT_EQ(Loader().RefreshAsset(IO::ResourcePath("res://missing.mat")), Result::Missing);
    Write("notes.unknownext", "text");
    EXPECT_EQ(Loader().RefreshAsset(IO::ResourcePath("res://notes.unknownext")), Result::Missing) << "no loader";
    EXPECT_EQ(Loader().RefreshAsset(IO::ResourcePath("res://../escape.mat")), Result::Missing) << "outside the folder";
    EXPECT_EQ(Loader().RefreshAsset(IO::ResourcePath("user://a.mat")), Result::Missing) << "not a project file";
    fs::create_directories(Asset("folder.mat"));
    EXPECT_EQ(Loader().RefreshAsset(IO::ResourcePath("res://folder.mat")), Result::Missing) << "not a file";
}

TEST_F(AssetStateTest, SetImportSettingsReplacesTheMetaCustomData)
{
    Write("a.mat", "{}");
    Init();
    const IO::ResourcePath path("res://a.mat");

    auto set = Loader().SetImportSettings(path, json{{"material", {{"note", "one"}}}, {"other", 2}});
    ASSERT_TRUE(set) << set.error();
    EXPECT_EQ(Loader().GetMetadata(path)->customData, (json{{"material", {{"note", "one"}}}, {"other", 2}}));
    EXPECT_EQ(IO::AssetMetadata::FromFile(Meta("a.mat")).customData, Loader().GetMetadata(path)->customData);

    // A replacement, not a merge
    set = Loader().SetImportSettings(path, json{{"other", 3}});
    ASSERT_TRUE(set) << set.error();
    EXPECT_EQ(Loader().GetMetadata(path)->customData, (json{{"other", 3}}));

    // null (or {}) clears the settings, and the .meta keeps no customData key
    set = Loader().SetImportSettings(path, json(nullptr));
    ASSERT_TRUE(set) << set.error();
    EXPECT_TRUE(Loader().GetMetadata(path)->customData.empty());
    EXPECT_FALSE(ReadJson(Meta("a.mat")).contains("customData"));

    // The settings survive a restart
    ASSERT_TRUE(Loader().SetImportSettings(path, json{{"kept", true}}));
    Init();
    EXPECT_EQ(Loader().GetMetadata(path)->customData, (json{{"kept", true}}));
}

TEST_F(AssetStateTest, SetImportSettingsRefusesWhatIsNotAnAssetOrAnObject)
{
    Write("a.mat", "{}");
    Init();

    EXPECT_FALSE(Loader().SetImportSettings(IO::ResourcePath("res://nothing.mat"), json::object()));
    EXPECT_FALSE(Loader().SetImportSettings(IO::ResourcePath("user://a.mat"), json::object()));
    EXPECT_FALSE(Loader().SetImportSettings(IO::ResourcePath("res://a.mat"), json::array()));
    EXPECT_FALSE(Loader().SetImportSettings(IO::ResourcePath("res://a.mat"), json("text")));
    EXPECT_TRUE(Loader().GetMetadata(IO::ResourcePath("res://a.mat"))->customData.empty()) << "nothing was applied";
}

TEST_F(AssetStateTest, SetImportSettingsKeepsTheSubAssetIndex)
{
    Write("a.mat", "{}");
    Init();
    const IO::ResourcePath path("res://a.mat");
    const std::vector<IO::ResourceLoader::SubAssetIndexEntry> entries = {
        {"mesh/Arm", "Mesh", IO::ResourceUUID::FromSubAsset(path, "mesh/Arm")},
        {"material/Red", "Material", IO::ResourceUUID::FromSubAsset(path, "material/Red")}};
    ASSERT_TRUE(Loader().SetSubAssetIndex(path, entries));
    const json index = Loader().GetMetadata(path)->customData.at("subAssets");
    const json source = Loader().GetMetadata(path)->customData.at("subAssetsSource");

    // The client's copy of the settings may hold (or lack) the loader's own keys: they are ignored
    ASSERT_TRUE(Loader().SetImportSettings(path, json{{"model", {{"scale", 2.0}}}, {"subAssets", {{"bogus", 1}}}}));

    const json &customData = Loader().GetMetadata(path)->customData;
    EXPECT_EQ(customData.at("model").at("scale"), 2.0);
    EXPECT_EQ(customData.at("subAssets"), index);
    EXPECT_EQ(customData.at("subAssetsSource"), source);
    EXPECT_EQ(IO::AssetMetadata::FromFile(Meta("a.mat")).customData, customData);

    const auto listed = Loader().GetSubAssets(path);
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0].key, "material/Red") << "sorted by key";
    EXPECT_EQ(listed[0].type, "Material");
    EXPECT_EQ(listed[0].uuid, IO::ResourceUUID::FromSubAsset(path, "material/Red"));
    EXPECT_EQ(listed[1].key, "mesh/Arm");
    EXPECT_EQ(listed[1].type, "Mesh");
}

TEST_F(AssetStateTest, GetSubAssetsIsEmptyForAFileWithNoIndexOrAStaleOne)
{
    Write("a.mat", "{}");
    Init();
    const IO::ResourcePath path("res://a.mat");
    EXPECT_TRUE(Loader().GetSubAssets(path).empty());
    EXPECT_TRUE(Loader().GetSubAssets(IO::ResourcePath("res://nothing.mat")).empty());

    ASSERT_TRUE(Loader().SetSubAssetIndex(path, {{"mesh/Arm", "Mesh", IO::ResourceUUID::FromSubAsset(path, "mesh/Arm")}}));
    EXPECT_EQ(Loader().GetSubAssets(path).size(), 1u);

    // The file changed since the index was written: the scan forgets it, and the listing with it
    Write("a.mat", R"({"changed": "so the index is stale"})");
    (void)Loader().RescanAssets();
    EXPECT_TRUE(Loader().GetSubAssets(path).empty());
}
