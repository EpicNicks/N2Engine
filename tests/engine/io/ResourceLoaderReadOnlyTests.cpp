#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include <engine/io/AssetMetadata.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourcePath.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <math/UUID.hpp>

using namespace N2Engine;
namespace fs = std::filesystem;

// ResourceLoader::SetReadOnly (#82, E9): the editor's play host shares the project folder with the host that edits, and
// must write none of what the loader normally writes there: .meta files, .n2/asset-state.json, the .import folder.
namespace
{
    class ReadOnlyLoaderTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _root = fs::temp_directory_path() / "n2-read-only-loader-test" / info->name();
            std::error_code error;
            fs::remove_all(_root, error);
            fs::create_directories(_root / "assets");
            _userData = _root.parent_path() / (_root.filename().string() + "-user");
            IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, info->name()));
        }

        void TearDown() override
        {
            Loader().SetReadOnly(false);
            std::error_code error;
            fs::remove_all(_root.parent_path(), error);
        }

        static IO::ResourceLoader &Loader() { return IO::ResourceLoader::Instance(); }

        void Write(const std::string &relative, const std::string &text) const
        {
            const fs::path path = _root / "assets" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        static std::string ReadFile(const fs::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            std::ostringstream text;
            text << file.rdbuf();
            return text.str();
        }

        void Init() const { Loader().Initialize(_root, _userData); }

        fs::path _root;
        fs::path _userData;
    };
}

TEST_F(ReadOnlyLoaderTest, ItIsOffByDefault)
{
    EXPECT_FALSE(Loader().IsReadOnly());
}

TEST_F(ReadOnlyLoaderTest, AFreshProjectIsIndexedInMemoryAndNothingIsWritten)
{
    Write("a.mat", "{}");
    Loader().SetReadOnly(true);
    Init();

    const IO::ResourcePath path("res://a.mat");
    EXPECT_TRUE(Loader().Exists(path));
    ASSERT_NE(Loader().GetMetadata(path), nullptr) << "the asset is known, with its UUID, in memory";
    EXPECT_EQ(Loader().GetMetadata(path)->uuid, IO::ResourceUUID::FromPath(path));
    EXPECT_FALSE(fs::exists(_root / ".import")) << "not even the folder";
    EXPECT_FALSE(fs::exists(_root / ".n2" / "asset-state.json"));
}

TEST_F(ReadOnlyLoaderTest, ARescanOfAChangedProjectWritesNeitherMetaNorState)
{
    Write("a.mat", "{}");
    Init(); // as the host that edits does
    const fs::path meta = _root / ".import" / "a.mat.meta";
    const fs::path state = _root / ".n2" / "asset-state.json";
    ASSERT_TRUE(fs::is_regular_file(meta));
    ASSERT_TRUE(fs::is_regular_file(state));
    const std::string metaBefore = ReadFile(meta);
    const std::string stateBefore = ReadFile(state);

    // Then the files change under the read-only loader of a play host
    Loader().SetReadOnly(true);
    Init();
    Write("b.mat", "{}");
    Write("a.mat", R"({"changed": "and longer"})");
    const IO::ResourceLoader::RescanResult changes = Loader().RescanAssets();

    EXPECT_EQ(changes.added.size(), 1u) << "the new file is found";
    EXPECT_EQ(changes.modified.size(), 1u);
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://b.mat")));
    EXPECT_FALSE(fs::exists(_root / ".import" / "b.mat.meta")) << "no .meta for the new file";
    EXPECT_EQ(ReadFile(meta), metaBefore);
    EXPECT_EQ(ReadFile(state), stateBefore);
}

TEST_F(ReadOnlyLoaderTest, ImportSettingsAreRefused)
{
    Write("a.mat", "{}");
    Init();
    const std::string metaBefore = ReadFile(_root / ".import" / "a.mat.meta");
    Loader().SetReadOnly(true);
    Init();

    const auto result = Loader().SetImportSettings(IO::ResourcePath("res://a.mat"), nlohmann::json::object({{"k", 1}}));

    ASSERT_FALSE(result);
    EXPECT_NE(result.error().find("read-only"), std::string::npos) << result.error();
    EXPECT_EQ(ReadFile(_root / ".import" / "a.mat.meta"), metaBefore);
}

TEST_F(ReadOnlyLoaderTest, WritingComesBackWhenItIsTurnedOff)
{
    Loader().SetReadOnly(true);
    Init();
    Loader().SetReadOnly(false);
    Init();
    Write("a.mat", "{}");
    (void)Loader().RescanAssets();

    EXPECT_TRUE(fs::is_regular_file(_root / ".import" / "a.mat.meta"));
}
