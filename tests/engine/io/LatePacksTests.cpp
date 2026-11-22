#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <engine/io/LatePacks.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/io/Vfs.hpp>
#include <engine/sceneManagement/SceneFile.hpp>
#include <math/UUID.hpp>

using namespace N2Engine;
namespace fs = std::filesystem;

// #425 (docs/web-build.html#late-packs): folders whose files arrive after the start. Pending until their pack is mounted;
// a load of a pending file fails, and once the pack arrives the asset scan indexes its files and they load.
namespace
{
    class LatePacksTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _root = fs::temp_directory_path() / "n2-late-packs-test" / info->name();
            std::error_code error;
            fs::remove_all(_root, error);
            fs::create_directories(_root / "project" / "assets" / "scenes");
            fs::create_directories(_root / "pack");
            IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, info->name()));
            IO::LatePacks::Reset();
        }

        void TearDown() override
        {
            IO::LatePacks::Reset();
            IO::Vfs::Instance().UnmountAll();
            std::error_code error;
            fs::remove_all(_root.parent_path(), error);
        }

        static void Write(const fs::path &path, const std::string &text)
        {
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        fs::path _root;
    };
}

TEST_F(LatePacksTest, AFolderIsPendingUntilItsPackArrivesAndOnlyWhatIsInsideIt)
{
    const fs::path late = _root / "project" / "assets" / "late";
    IO::LatePacks::Register(late / ""); // a trailing separator names the same folder
    IO::LatePacks::Register(late);      // registered once
    ASSERT_EQ(IO::LatePacks::Pending().size(), 1u);
    EXPECT_TRUE(IO::LatePacks::IsPending(late));
    EXPECT_TRUE(IO::LatePacks::IsPending(late / "two" / "Level.scene"));
    EXPECT_FALSE(IO::LatePacks::IsPending(_root / "project" / "assets" / "later" / "x.scene")) << "a sibling with the same prefix";
    EXPECT_FALSE(IO::LatePacks::IsPending(_root / "project" / "assets" / "scenes" / "Main.scene"));

    Write(_root / "pack" / "Level.scene", R"({"name":"Level","rootGameObjects":[]})");
    EXPECT_FALSE(IO::LatePacks::Arrive(_root / "project" / "assets" / "other", IO::MakeFolderMount(_root / "pack")))
        << "a folder that was not registered";
    ASSERT_TRUE(IO::LatePacks::Arrive(late, IO::MakeFolderMount(_root / "pack")));
    EXPECT_FALSE(IO::LatePacks::IsPending(late / "Level.scene"));
    EXPECT_TRUE(IO::LatePacks::Pending().empty());
    ASSERT_EQ(IO::LatePacks::Arrived().size(), 1u);
    EXPECT_TRUE(IO::Vfs::Instance().IsRegularFile(late / "Level.scene")) << "the pack is mounted at the folder";
    EXPECT_FALSE(IO::LatePacks::Arrive(late, IO::MakeFolderMount(_root / "pack"))) << "it arrives once";
}

TEST_F(LatePacksTest, AFailedPackStaysPendingAndIsCountedUntilItArrives)
{
    const fs::path late = _root / "project" / "assets" / "late";
    IO::LatePacks::Register(late);
    IO::LatePacks::Fail(_root / "project" / "assets" / "unknown"); // not registered: nothing
    EXPECT_TRUE(IO::LatePacks::Failed().empty());
    IO::LatePacks::Fail(late);
    ASSERT_EQ(IO::LatePacks::Failed().size(), 1u);
    EXPECT_TRUE(IO::LatePacks::IsPending(late / "x.scene")) << "its files are still not there";
    fs::create_directories(_root / "pack");
    ASSERT_TRUE(IO::LatePacks::Arrive(late, IO::MakeFolderMount(_root / "pack")));
    EXPECT_TRUE(IO::LatePacks::Failed().empty()) << "a pack that arrives after all is no longer failed";
}

TEST_F(LatePacksTest, ALateSceneLoadsOnlyAfterItsPackArrivedAndTheScanIndexedIt)
{
    Write(_root / "project" / "assets" / "scenes" / "Main.scene", R"({"name":"Main","rootGameObjects":[]})");
    Write(_root / "pack" / "Level.scene", R"({"name":"Level","rootGameObjects":[]})");
    const fs::path late = _root / "project" / "assets" / "late";
    IO::LatePacks::Register(late);
    IO::ResourceLoader &loader = IO::ResourceLoader::Instance();
    loader.Initialize(_root / "project", _root / "user");

    EXPECT_NE(loader.Load<SceneFile>(IO::ResourcePath("res://scenes/Main.scene")), nullptr);
    EXPECT_EQ(loader.Load<SceneFile>(IO::ResourcePath("res://late/Level.scene")), nullptr) << "not there yet";

    ASSERT_TRUE(IO::LatePacks::Arrive(late, IO::MakeFolderMount(_root / "pack")));
    const auto level = loader.Load<SceneFile>(IO::ResourcePath("res://late/Level.scene"));
    ASSERT_NE(level, nullptr) << "the rescan at the arrival indexed the mounted folder";
    EXPECT_NE(loader.GetUUID(IO::ResourcePath("res://late/Level.scene")), Math::UUID::ZERO);
}
