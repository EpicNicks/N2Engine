#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <engine/io/Vfs.hpp>

#include "TestZip.hpp"

// The virtual filesystem and its packs (build targets stage 4; docs/packs.html): zip (stored and deflate) and 7z (non-solid
// and solid) read the same bytes; a pack whose names could leave it, or whose data is damaged, is refused; mounts serve
// their paths and everything else is the disk.
using namespace N2Engine::IO;
using N2Engine::Tests::MakeZip;
namespace fs = std::filesystem;

namespace
{
    const fs::path Packs = fs::path(N2_TEST_ASSETS_DIR) / "packs";

    /// What tests/assets/packs/make_packs.py writes into every sample pack
    std::map<std::string, std::string> SampleFiles()
    {
        std::string script;
        for (int i = 0; i < 20; ++i)
        {
            script += "-- a script\nreturn { answer = 42 }\n";
        }
        std::string data;
        for (int r = 0; r < 8; ++r)
        {
            for (int b = 0; b < 256; ++b)
            {
                data += static_cast<char>(b);
            }
        }
        return {
            {"project.n2proj", "{\"formatVersion\":1,\"name\":\"Pack Sample\"}\n"},
            {"assets/scripts/main.lua", script},
            {"assets/sub/notes.txt", "\xc3\xbc" "n" "\xc3\xaf" "c" "\xc3\xb6" "d" "\xc3\xa9" " text\n"},
            {"assets/data.bin", data},
            {"assets/empty.txt", ""},
        };
    }

    std::string AsText(const VfsBytes &bytes) { return {reinterpret_cast<const char *>(bytes.data()), bytes.size()}; }

    class VfsPackTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _dir = fs::temp_directory_path() / "n2-vfs-pack-test" / info->name();
            std::error_code error;
            fs::remove_all(_dir, error);
            fs::create_directories(_dir);
        }

        void TearDown() override
        {
            Vfs::Instance().UnmountAll();
            std::error_code error;
            fs::remove_all(_dir, error);
        }

        fs::path Write(const std::string &name, const std::string &bytes) const
        {
            const fs::path path = _dir / name;
            std::ofstream(path, std::ios::binary) << bytes;
            return path;
        }

        fs::path _dir;
    };

    void ExpectSampleContents(const IMount &mount)
    {
        for (const auto &[name, data] : SampleFiles())
        {
            const auto entry = mount.Stat(name);
            ASSERT_TRUE(entry.has_value()) << mount.Kind() << ": " << name;
            EXPECT_FALSE(entry->directory) << name;
            EXPECT_EQ(entry->size, data.size()) << mount.Kind() << ": " << name;
            const auto bytes = mount.Read(name);
            ASSERT_TRUE(bytes.has_value()) << mount.Kind() << ": " << name << ": " << bytes.error();
            EXPECT_EQ(AsText(*bytes), data) << mount.Kind() << ": " << name;
        }
        EXPECT_TRUE(mount.Stat("assets")->directory);
        EXPECT_TRUE(mount.Stat("assets/scripts")->directory);
        EXPECT_TRUE(mount.Stat("")->directory);
        EXPECT_FALSE(mount.Stat("assets/missing.lua").has_value());
        EXPECT_FALSE(mount.Read("assets/missing.lua").has_value());

        std::vector<std::string> direct;
        for (const VfsEntry &e : mount.List("assets", false))
        {
            direct.push_back(e.path + (e.directory ? "/" : ""));
        }
        std::ranges::sort(direct);
        EXPECT_EQ(direct, (std::vector<std::string>{"assets/data.bin", "assets/empty.txt", "assets/scripts/", "assets/sub/"}));
        EXPECT_EQ(mount.List("", true).size(), SampleFiles().size() + 3) << "5 files and the 3 folders";
    }
}

TEST_F(VfsPackTest, ZipStoredAndDeflateEntriesReadTheSameBytes)
{
    auto mount = Pack::Open(Packs / "sample.zip");
    ASSERT_TRUE(mount.has_value()) << mount.error();
    EXPECT_EQ((*mount)->Kind(), "zip");
    ExpectSampleContents(**mount);
    EXPECT_GT((*mount)->Stat("assets/data.bin")->modified, 0u) << "the zip's stored time";
}

TEST_F(VfsPackTest, SevenZipNonSolidAndSolidReadTheSameBytes)
{
    for (const char *name : {"sample.7z", "sample-solid.7z"})
    {
        auto mount = Pack::Open(Packs / name);
        ASSERT_TRUE(mount.has_value()) << name << ": " << mount.error();
        EXPECT_EQ((*mount)->Kind(), "7z");
        ExpectSampleContents(**mount);
        // Reading in another order, again: the decoder's block cache must not hand back the wrong file
        EXPECT_EQ(AsText(*(*mount)->Read("assets/sub/notes.txt")), SampleFiles()["assets/sub/notes.txt"]) << name;
        EXPECT_EQ(AsText(*(*mount)->Read("project.n2proj")), SampleFiles()["project.n2proj"]) << name;
    }
}

TEST_F(VfsPackTest, EntryNamesThatCouldLeaveThePackAreRefused)
{
    EXPECT_EQ(Pack::NormalizeEntryName("assets/a.lua"), "assets/a.lua");
    EXPECT_EQ(Pack::NormalizeEntryName("assets/"), "assets");
    for (const char *bad : {"../evil.lua", "assets/../../evil", "/etc/passwd", "C:/x", "a\\..\\b", "a//b", "./a", "a/./b", "", "a\nb"})
    {
        EXPECT_FALSE(Pack::NormalizeEntryName(bad).has_value()) << bad;
    }
    for (const char *bad : {"../evil.lua", "/abs.lua", "dir\\..\\..\\x.lua"})
    {
        const fs::path zip = Write("slip.zip", MakeZip({{"ok.txt", "fine"}, {bad, "evil"}}));
        const auto mount = Pack::Open(zip);
        ASSERT_FALSE(mount.has_value()) << bad;
        EXPECT_NE(mount.error().find("could leave the pack"), std::string::npos) << mount.error();
    }
}

TEST_F(VfsPackTest, DamagedPacksAreRefusedWithTheReason)
{
    // A CRC that does not match the data
    const auto crc = Pack::Open(Write("crc.zip", MakeZip({{"a.txt", "hello", 0x12345678}})));
    ASSERT_TRUE(crc.has_value()) << crc.error();
    const auto read = (*crc)->Read("a.txt");
    ASSERT_FALSE(read.has_value());
    EXPECT_NE(read.error().find("CRC"), std::string::npos) << read.error();

    // A declared size past the end of the file
    const auto past = Pack::Open(Write("past.zip", MakeZip({{"a.txt", "hello", 0, 1000000}})));
    ASSERT_TRUE(past.has_value());
    EXPECT_FALSE((*past)->Read("a.txt").has_value());

    // The same name twice
    const auto twice = Pack::Open(Write("twice.zip", MakeZip({{"a.txt", "1"}, {"a.txt", "2"}})));
    ASSERT_FALSE(twice.has_value());
    EXPECT_NE(twice.error().find("twice"), std::string::npos);

    // Cut short: no directory at the end
    const std::string whole = MakeZip({{"a.txt", "hello"}});
    EXPECT_FALSE(Pack::Open(Write("short.zip", whole.substr(0, whole.size() - 10))).has_value());

    // Neither format
    const auto neither = Pack::Open(Write("plain.bin", "just some bytes"));
    ASSERT_FALSE(neither.has_value());
    EXPECT_NE(neither.error().find("not a pack"), std::string::npos);

    // A 7z cut short
    std::ifstream in(Packs / "sample.7z", std::ios::binary);
    const std::string seven((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(Pack::Open(Write("short.7z", seven.substr(0, seven.size() / 2))).has_value());
}

TEST_F(VfsPackTest, AMountServesItsPathsAndTheDiskServesTheRest)
{
    Vfs &vfs = Vfs::Instance();
    const fs::path game = _dir / "game"; // not on disk: the pack is
    vfs.Mount(game, std::move(*Pack::Open(Packs / "sample.zip")));
    const fs::path onDisk = Write("loose.txt", "from the disk");

    EXPECT_TRUE(vfs.IsMounted(game / "assets" / "data.bin"));
    EXPECT_FALSE(vfs.IsMounted(onDisk));
    EXPECT_FALSE(vfs.IsMounted(_dir / "gamex" / "assets")) << "a sibling whose name starts the same is not inside";
    EXPECT_EQ(vfs.MountKindOf(game / "project.n2proj"), "zip");
    EXPECT_EQ(vfs.MountKindOf(onDisk), "disk");

    EXPECT_EQ(*vfs.ReadText(game / "project.n2proj"), SampleFiles()["project.n2proj"]);
    EXPECT_EQ(*vfs.ReadText(game / "assets/./scripts/../sub/notes.txt"), SampleFiles()["assets/sub/notes.txt"]);
    EXPECT_EQ(*vfs.ReadText(onDisk), "from the disk");
    EXPECT_TRUE(vfs.IsDirectory(game));
    EXPECT_TRUE(vfs.IsDirectory(game / "assets"));
    EXPECT_TRUE(vfs.IsRegularFile(game / "assets" / "empty.txt"));
    EXPECT_FALSE(vfs.Exists(game / "assets" / "nope"));
    EXPECT_FALSE(vfs.IsMounted(game / ".." / "loose.txt")) << "a path that leaves the mount is the disk's, not the pack's";
    EXPECT_EQ(*vfs.ReadText(game / ".." / "loose.txt"), "from the disk");

    std::vector<std::string> files;
    for (const VfsEntry &e : vfs.ListFiles(game / "assets"))
    {
        files.push_back(e.path);
    }
    EXPECT_EQ(files, (std::vector<std::string>{"data.bin", "empty.txt", "scripts/main.lua", "sub/notes.txt"}));
    EXPECT_EQ(vfs.ListFiles(game / "assets", false).size(), 2u);

    EXPECT_TRUE(vfs.Unmount(game));
    EXPECT_FALSE(vfs.Exists(game / "project.n2proj"));
}

TEST_F(VfsPackTest, TheDeepestMountWinsAndAFolderMountStaysInsideItsRoot)
{
    Vfs &vfs = Vfs::Instance();
    const fs::path real = _dir / "real";
    fs::create_directories(real / "inner");
    std::ofstream(real / "inner" / "x.txt") << "folder";
    std::ofstream(_dir / "secret.txt") << "outside";

    vfs.Mount(_dir / "v", MakeFolderMount(real));
    vfs.Mount(_dir / "v" / "packed", std::move(*Pack::Open(Packs / "sample.7z")));
    EXPECT_EQ(*vfs.ReadText(_dir / "v" / "inner" / "x.txt"), "folder");
    EXPECT_EQ(vfs.MountKindOf(_dir / "v" / "inner" / "x.txt"), "folder");
    EXPECT_EQ(vfs.MountKindOf(_dir / "v" / "packed" / "project.n2proj"), "7z");
    EXPECT_EQ(*vfs.ReadText(_dir / "v" / "packed" / "project.n2proj"), SampleFiles()["project.n2proj"]);

    const auto folder = MakeFolderMount(real);
    EXPECT_FALSE(folder->Read("../secret.txt").has_value());
    EXPECT_FALSE(folder->Stat("../secret.txt").has_value());
    EXPECT_EQ(AsText(*folder->Read("inner/x.txt")), "folder");
}
