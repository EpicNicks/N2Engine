#include <gtest/gtest.h>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <engine/Logger.hpp>
#include <engine/io/AssetMetadata.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <math/UUID.hpp>

using namespace N2Engine;
namespace fs = std::filesystem;
using json = nlohmann::json;

// Stable asset UUIDs (editor protocol 1.24.0): the UUID a .meta stores is the asset's identity and is kept, even when it
// is not the hash of the file's path; a new file's is minted from its path ("path#2" when a moved asset holds that one);
// a broken .meta (all-zero UUID, a stale resourcePath, a lost or corrupt file) is repaired; two files with one UUID are
// told apart (the first keeps it); and ResourceLoader::MovePath carries an asset's .meta, UUID and cached copy along.
namespace
{
    class WarningLog
    {
    public:
        WarningLog()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                if (level == Logger::LogLevel::Warn || level == Logger::LogLevel::Error)
                {
                    messages.emplace_back(message);
                }
            };
        }
        ~WarningLog() { Logger::logEvent -= _id; }
        WarningLog(const WarningLog &) = delete;
        WarningLog &operator=(const WarningLog &) = delete;

        [[nodiscard]] bool Mentions(const std::string_view text) const
        {
            return std::ranges::any_of(messages, [text](const std::string &m) { return m.find(text) != std::string::npos; });
        }

        std::vector<std::string> messages;

    private:
        size_t _id = 0;
    };

    class StableUuidTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _root = fs::temp_directory_path() / "n2-stable-uuid-test" / info->name();
            std::error_code error;
            fs::remove_all(_root, error);
            fs::create_directories(_root / "assets");
            IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, info->name()));
            Loader().SetReadOnly(false);
            Loader().ClearCache();
        }

        void TearDown() override
        {
            Loader().SetReadOnly(false);
            Loader().ClearCache();
            std::error_code error;
            fs::remove_all(_root, error);
        }

        static IO::ResourceLoader &Loader() { return IO::ResourceLoader::Instance(); }

        void Write(const std::string &relative, const std::string &text) const
        {
            const fs::path path = _root / "assets" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        }

        [[nodiscard]] fs::path MetaPath(const std::string &relative) const
        {
            return _root / ".import" / (relative + ".meta");
        }

        void WriteMeta(const std::string &relative, const json &meta) const
        {
            const fs::path path = MetaPath(relative);
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary | std::ios::trunc) << meta.dump(2);
        }

        [[nodiscard]] static std::string ReadFile(const fs::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }

        [[nodiscard]] json ReadMeta(const std::string &relative) const { return json::parse(ReadFile(MetaPath(relative))); }

        /// MovePath with the file system rename a host would do
        std::expected<std::vector<IO::ResourceLoader::MovedAsset>, std::string> Move(const std::string &from,
                                                                                    const std::string &to) const
        {
            const fs::path source = _root / "assets" / from;
            const fs::path target = _root / "assets" / to;
            return Loader().MovePath(IO::ResourcePath("res://" + from), IO::ResourcePath("res://" + to),
                                     [&]() -> std::expected<void, std::string>
                                     {
                                         std::error_code error;
                                         fs::create_directories(target.parent_path(), error);
                                         fs::rename(source, target, error);
                                         if (error)
                                         {
                                             return std::unexpected(error.message());
                                         }
                                         return {};
                                     });
        }

        fs::path _root;
    };

    json MetaJson(const Math::UUID &uuid, const std::string &resourcePath, const std::string &type = "Material")
    {
        return json{{"uuid", uuid.ToString()},
                    {"resourcePath", resourcePath},
                    {"resourceType", type},
                    {"lastModified", 0},
                    {"fileSize", 0}};
    }
}

// ==================== The UUID rule ====================

TEST_F(StableUuidTest, ANewAssetGetsTheHashOfItsPathAndTheMetaStoresIt)
{
    Write("a.mat", "{}");
    Loader().Initialize(_root);

    const IO::ResourcePath path("res://a.mat");
    EXPECT_EQ(Loader().GetUUID(path), IO::ResourceUUID::FromPath(path)) << "the same UUID every earlier engine gave it";
    EXPECT_EQ(ReadMeta("a.mat").at("uuid"), IO::ResourceUUID::FromPath(path).ToString());
    EXPECT_FALSE(ReadMeta("a.mat").contains("uuidSeed")) << "a file that never moved has no seed";
}

TEST_F(StableUuidTest, AStoredUuidThatIsNotThePathHashIsKeptAndTheMetaIsNotRewritten)
{
    Write("a.mat", "{}");
    const Math::UUID stored = Math::UUID::GenerateNameBased(Math::UUID::ZERO, "somewhere else");
    WriteMeta("a.mat", MetaJson(stored, "res://a.mat"));
    const std::string before = ReadFile(MetaPath("a.mat"));

    WarningLog log;
    Loader().Initialize(_root);

    const IO::ResourcePath path("res://a.mat");
    EXPECT_EQ(Loader().GetUUID(path), stored) << "the .meta is the asset's identity";
    ASSERT_NE(Loader().GetMetadata(stored), nullptr);
    EXPECT_EQ(Loader().GetMetadata(stored)->resourcePath, path);
    EXPECT_EQ(ReadFile(MetaPath("a.mat")), before) << "nothing to repair";
    EXPECT_TRUE(log.messages.empty()) << log.messages.front();

    // A rescan and a restart change nothing
    (void)Loader().RescanAssets();
    Loader().Initialize(_root);
    EXPECT_EQ(Loader().GetUUID(path), stored);
}

TEST_F(StableUuidTest, ChangingTheProjectNamespaceKeepsTheStoredUuids)
{
    Write("a.mat", "{}");
    Loader().Initialize(_root);
    const IO::ResourcePath path("res://a.mat");
    const Math::UUID first = Loader().GetUUID(path);

    IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "another project id"));
    Loader().Initialize(_root);
    EXPECT_EQ(Loader().GetUUID(path), first) << "references saved before the change still resolve";
    EXPECT_NE(IO::ResourceUUID::FromPath(path), first);

    // A new file is minted in the new namespace
    Write("b.mat", "{}");
    (void)Loader().RescanAssets();
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://b.mat")), IO::ResourceUUID::FromPath(IO::ResourcePath("res://b.mat")));
}

TEST_F(StableUuidTest, AnAllZeroUuidIsMintedAgainWithAWarning)
{
    Write("a.mat", "{}");
    WriteMeta("a.mat", MetaJson(Math::UUID::ZERO, "res://a.mat"));

    WarningLog log;
    Loader().Initialize(_root);

    const IO::ResourcePath path("res://a.mat");
    EXPECT_EQ(Loader().GetUUID(path), IO::ResourceUUID::FromPath(path));
    EXPECT_EQ(ReadMeta("a.mat").at("uuid"), IO::ResourceUUID::FromPath(path).ToString());
    EXPECT_TRUE(log.Mentions("all zeros")) << ::testing::PrintToString(log.messages);
}

TEST_F(StableUuidTest, AMetaThatNamesAnotherPathIsCorrectedFromTheFileAndItsUuidKept)
{
    // A file and its .meta moved together outside the editor (git mv of both): the .meta still names the old path
    Write("new/b.mat", "{}");
    const Math::UUID stored = IO::ResourceUUID::FromPath(IO::ResourcePath("res://old/b.mat"));
    WriteMeta("new/b.mat", MetaJson(stored, "res://old/b.mat"));

    Loader().Initialize(_root);

    const IO::ResourcePath path("res://new/b.mat");
    EXPECT_EQ(Loader().GetUUID(path), stored);
    EXPECT_FALSE(Loader().Exists(IO::ResourcePath("res://old/b.mat"))) << "indexed where the file is";
    const json meta = ReadMeta("new/b.mat");
    EXPECT_EQ(meta.at("resourcePath"), "res://new/b.mat");
    EXPECT_EQ(meta.at("uuidSeed"), "res://old/b.mat") << "its sub-asset UUIDs were hashed from the old path";
}

TEST_F(StableUuidTest, ALostMetaGetsTheHashOfTheCurrentPathWithAWarning)
{
    Write("a.mat", "{}");
    Loader().Initialize(_root);
    ASSERT_TRUE(Move("a.mat", "moved/a.mat").has_value());
    const IO::ResourcePath moved("res://moved/a.mat");
    ASSERT_EQ(Loader().GetUUID(moved), IO::ResourceUUID::FromPath(IO::ResourcePath("res://a.mat")));

    fs::remove(MetaPath("moved/a.mat"));
    WarningLog log;
    Loader().Initialize(_root);
    EXPECT_EQ(Loader().GetUUID(moved), IO::ResourceUUID::FromPath(moved)) << "the old UUID is gone with the .meta";
    EXPECT_TRUE(log.Mentions("lost its .meta")) << ::testing::PrintToString(log.messages);
}

TEST_F(StableUuidTest, ACorruptMetaIsRegeneratedWithAWarning)
{
    Write("a.mat", "{}");
    fs::create_directories(MetaPath("a.mat").parent_path());
    std::ofstream(MetaPath("a.mat"), std::ios::binary) << "{ not json";

    WarningLog log;
    Loader().Initialize(_root);
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://a.mat")), IO::ResourceUUID::FromPath(IO::ResourcePath("res://a.mat")));
    EXPECT_TRUE(log.Mentions("Corrupt metadata")) << ::testing::PrintToString(log.messages);
    EXPECT_FALSE(log.Mentions("lost its .meta")) << "said once";
}

// ==================== Two files, one UUID ====================

TEST_F(StableUuidTest, AFileCopiedWithItsMetaGetsANewUuidAndTheFirstKeepsIt)
{
    Write("a.mat", "{}");
    Loader().Initialize(_root);
    const IO::ResourcePath original("res://a.mat");
    const Math::UUID uuid = Loader().GetUUID(original);

    // Copied in the file explorer, .meta and all
    Write("copies/a.mat", "{}");
    fs::create_directories(MetaPath("copies/a.mat").parent_path());
    fs::copy_file(MetaPath("a.mat"), MetaPath("copies/a.mat"));

    WarningLog log;
    (void)Loader().RescanAssets();

    const IO::ResourcePath copy("res://copies/a.mat");
    EXPECT_EQ(Loader().GetUUID(original), uuid) << "the asset that held it keeps it";
    const Math::UUID copyUuid = Loader().GetUUID(copy);
    EXPECT_NE(copyUuid, uuid);
    EXPECT_EQ(copyUuid, IO::ResourceUUID::FromPath(copy));
    EXPECT_EQ(Loader().GetMetadata(uuid)->resourcePath, original);
    EXPECT_EQ(ReadMeta("copies/a.mat").at("uuid"), copyUuid.ToString()) << "written";
    EXPECT_EQ(ReadMeta("copies/a.mat").at("resourcePath"), "res://copies/a.mat");
    EXPECT_TRUE(log.Mentions("had the same UUID as res://a.mat")) << ::testing::PrintToString(log.messages);

    // Stable afterwards
    Loader().Initialize(_root);
    EXPECT_EQ(Loader().GetUUID(original), uuid);
    EXPECT_EQ(Loader().GetUUID(copy), copyUuid);
}

TEST_F(StableUuidTest, OnAFirstScanTheSmallestPathKeepsADuplicatedUuid)
{
    // No index before (a fresh clone holding two .meta files with one UUID): the order is by path, never the
    // directory's, so every machine agrees
    const Math::UUID shared = Math::UUID::GenerateNameBased(Math::UUID::ZERO, "shared");
    Write("B/x.mat", "{}");
    Write("a/x.mat", "{}");
    WriteMeta("B/x.mat", MetaJson(shared, "res://B/x.mat"));
    WriteMeta("a/x.mat", MetaJson(shared, "res://a/x.mat"));

    Loader().Initialize(_root);
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://a/x.mat")), shared) << "res://a/x.mat sorts first, ignoring case";
    EXPECT_NE(Loader().GetUUID(IO::ResourcePath("res://B/x.mat")), shared);
}

TEST_F(StableUuidTest, ANewFileAtTheOldPathOfAMovedAssetGetsTheNextVariant)
{
    Write("a.mat", "{}");
    Loader().Initialize(_root);
    const IO::ResourcePath path("res://a.mat");
    const Math::UUID uuid = Loader().GetUUID(path);
    ASSERT_TRUE(Move("a.mat", "b/a.mat").has_value());

    // A new file where the moved one was: the hash of its path is taken by the moved asset
    Write("a.mat", "{}");
    (void)Loader().RescanAssets();
    const Math::UUID fresh = Loader().GetUUID(path);
    EXPECT_NE(fresh, uuid);
    EXPECT_EQ(fresh, IO::ResourceUUID::FromPath(path, 2));
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://b/a.mat")), uuid);

    // A fresh clone agrees: the stored UUID wins over the one a scan mints, whatever the order the folder is read in
    fs::remove_all(_root / ".n2");
    fs::remove(MetaPath("a.mat"));
    Loader().Initialize(_root);
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://b/a.mat")), uuid);
    EXPECT_EQ(Loader().GetUUID(path), IO::ResourceUUID::FromPath(path, 2));
}

TEST_F(StableUuidTest, AReadOnlyLoaderAppliesTheRulesInMemoryAndWritesNothing)
{
    Write("a.mat", "{}");
    Write("copy.mat", "{}");
    const Math::UUID stored = Math::UUID::GenerateNameBased(Math::UUID::ZERO, "stored");
    WriteMeta("a.mat", MetaJson(stored, "res://elsewhere/a.mat"));
    WriteMeta("copy.mat", MetaJson(stored, "res://copy.mat"));
    const std::string aBefore = ReadFile(MetaPath("a.mat"));
    const std::string copyBefore = ReadFile(MetaPath("copy.mat"));

    Loader().SetReadOnly(true);
    Loader().Initialize(_root);

    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://a.mat")), stored);
    EXPECT_NE(Loader().GetUUID(IO::ResourcePath("res://copy.mat")), stored);
    EXPECT_EQ(ReadFile(MetaPath("a.mat")), aBefore);
    EXPECT_EQ(ReadFile(MetaPath("copy.mat")), copyBefore);
    EXPECT_FALSE(Move("a.mat", "b.mat").has_value()) << "a read-only loader moves nothing";
    EXPECT_TRUE(fs::exists(_root / "assets" / "a.mat"));
}

// ==================== MovePath ====================

TEST_F(StableUuidTest, MovingAFileKeepsItsUuidImportSettingsAndCachedCopy)
{
    Write("a.mat", "{}");
    Loader().Initialize(_root);
    const IO::ResourcePath from("res://a.mat");
    const IO::ResourcePath to("res://materials/b.mat");
    const Math::UUID uuid = Loader().GetUUID(from);
    ASSERT_TRUE(Loader().SetImportSettings(from, json{{"keep", true}}).has_value());
    const auto loaded = Loader().Load<Base::Asset>(from);
    ASSERT_NE(loaded, nullptr);

    const auto moved = Move("a.mat", "materials/b.mat");
    ASSERT_TRUE(moved.has_value()) << moved.error();
    ASSERT_EQ(moved->size(), 1u);
    EXPECT_EQ(moved->at(0).from, from);
    EXPECT_EQ(moved->at(0).to, to);
    EXPECT_EQ(moved->at(0).uuid, uuid);
    EXPECT_EQ(moved->at(0).type, "Material");

    EXPECT_FALSE(Loader().Exists(from));
    EXPECT_EQ(Loader().GetUUID(to), uuid);
    EXPECT_EQ(Loader().GetMetadata(uuid)->resourcePath, to);
    EXPECT_EQ(Loader().GetCached<Base::Asset>(to), loaded) << "the cached copy follows the file";
    EXPECT_EQ(Loader().GetCachedByUUID<Base::Asset>(uuid), loaded);
    EXPECT_EQ(loaded->GetResourcePath(), to);
    EXPECT_EQ(Loader().LoadByUUID<Base::Asset>(uuid), loaded);

    EXPECT_FALSE(fs::exists(MetaPath("a.mat"))) << "the old .meta goes";
    const json meta = ReadMeta("materials/b.mat");
    EXPECT_EQ(meta.at("uuid"), uuid.ToString());
    EXPECT_EQ(meta.at("resourcePath"), "res://materials/b.mat");
    EXPECT_EQ(meta.at("customData"), (json{{"keep", true}}));
    EXPECT_FALSE(meta.contains("uuidSeed")) << "a material has no parts";

    // A rescan finds nothing to report, and a restart gives the same UUID
    const IO::ResourceLoader::RescanResult changes = Loader().RescanAssets();
    EXPECT_TRUE(changes.Empty());
    Loader().ClearCache();
    Loader().Initialize(_root);
    EXPECT_EQ(Loader().GetUUID(to), uuid);
}

TEST_F(StableUuidTest, MovingAFolderMovesEveryAssetAndTheMetaFilesOfUnindexedFiles)
{
    Write("art/a.mat", "{}");
    Write("art/deep/b.mat", "{}");
    Write("art/notes.txt", "not an asset");
    Loader().Initialize(_root);
    const Math::UUID a = Loader().GetUUID(IO::ResourcePath("res://art/a.mat"));
    const Math::UUID b = Loader().GetUUID(IO::ResourcePath("res://art/deep/b.mat"));
    // The .meta of a file this build does not index (an .fbx without ufbx) follows its file too
    Write("art/robot.unknownext", "x");
    WriteMeta("art/robot.unknownext", MetaJson(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "fbx"), "res://art/robot.unknownext", "Model"));

    const auto moved = Move("art", "library/art");
    ASSERT_TRUE(moved.has_value()) << moved.error();
    ASSERT_EQ(moved->size(), 2u);
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://library/art/a.mat")), a);
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://library/art/deep/b.mat")), b);
    EXPECT_TRUE(fs::exists(_root / "assets" / "library" / "art" / "notes.txt"));
    EXPECT_TRUE(fs::exists(MetaPath("library/art/robot.unknownext")));
    EXPECT_FALSE(fs::exists(_root / ".import" / "art")) << "the old .import folder is gone once empty";
}

TEST_F(StableUuidTest, AStaleMetaAtTheDestinationIsReplacedAndAMoveBackRestoresEverything)
{
    Write("a.mat", "{}");
    Loader().Initialize(_root);
    const Math::UUID uuid = Loader().GetUUID(IO::ResourcePath("res://a.mat"));
    // Left by a file deleted long ago
    WriteMeta("b.mat", MetaJson(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "old"), "res://b.mat"));

    ASSERT_TRUE(Move("a.mat", "b.mat").has_value());
    EXPECT_EQ(ReadMeta("b.mat").at("uuid"), uuid.ToString());

    // A chain, then back where it started
    ASSERT_TRUE(Move("b.mat", "c/c.mat").has_value());
    ASSERT_TRUE(Move("c/c.mat", "a.mat").has_value());
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://a.mat")), uuid);
    EXPECT_FALSE(Loader().Exists(IO::ResourcePath("res://c/c.mat")));
    EXPECT_FALSE(fs::exists(MetaPath("c/c.mat")));
}

TEST_F(StableUuidTest, AFailedRenameLeavesTheIndexAndTheMetaFilesAsTheyWere)
{
    Write("a.mat", "{}");
    Loader().Initialize(_root);
    const Math::UUID uuid = Loader().GetUUID(IO::ResourcePath("res://a.mat"));
    WriteMeta("b.mat", MetaJson(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "stale"), "res://b.mat"));
    const std::string staleBefore = ReadFile(MetaPath("b.mat"));
    const std::string metaBefore = ReadFile(MetaPath("a.mat"));

    const auto moved = Loader().MovePath(IO::ResourcePath("res://a.mat"), IO::ResourcePath("res://b.mat"),
                                         []() -> std::expected<void, std::string> { return std::unexpected("the file is open"); });
    ASSERT_FALSE(moved.has_value());
    EXPECT_EQ(moved.error(), "the file is open");
    EXPECT_EQ(Loader().GetUUID(IO::ResourcePath("res://a.mat")), uuid);
    EXPECT_FALSE(Loader().Exists(IO::ResourcePath("res://b.mat")));
    EXPECT_EQ(ReadFile(MetaPath("a.mat")), metaBefore);
    EXPECT_EQ(ReadFile(MetaPath("b.mat")), staleBefore) << "the stale .meta is put back";
}

TEST_F(StableUuidTest, MovingOntoAnIndexedAssetIsRefused)
{
    Write("a.mat", "{}");
    Write("b.mat", "{}");
    Loader().Initialize(_root);
    bool renamed = false;
    const auto moved = Loader().MovePath(IO::ResourcePath("res://a.mat"), IO::ResourcePath("res://b.mat"),
                                         [&]() -> std::expected<void, std::string> { renamed = true; return {}; });
    EXPECT_FALSE(moved.has_value());
    EXPECT_FALSE(renamed);
}
