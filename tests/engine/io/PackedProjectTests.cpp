#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <engine/base/Asset.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/io/Vfs.hpp>
#include <engine/scripting/LuaScript.hpp>

#include "TestZip.hpp"

// A project runs the same from its folder and from its pack (build targets stage 4; docs/packs.html): lua_project, packed
// whole into a zip and mounted where a folder would be, reads the same project file, indexes the same assets with the same
// UUIDs and types, and loads each one to the same thing; a packed project writes nothing.
using namespace N2Engine;
namespace fs = std::filesystem;

namespace
{
    struct AssetSnapshot
    {
        std::string uuid;
        std::string type;
        std::size_t size = 0;
        bool loaded = false;
        std::string serialized; ///< what the loaded asset serializes to (its source, for a script)

        bool operator==(const AssetSnapshot &) const = default;
    };

    std::ostream &operator<<(std::ostream &out, const AssetSnapshot &s)
    {
        return out << s.type << " " << s.uuid << " " << s.size << (s.loaded ? " loaded" : " not loaded");
    }

    /// Opens projectDir as the player does (read-only) and loads every asset it indexes
    std::map<std::string, AssetSnapshot> Snapshot(const fs::path &projectDir, const fs::path &userData)
    {
        const auto project = IO::ProjectFile::Load(projectDir);
        EXPECT_TRUE(project.has_value()) << (project ? "" : project.error());
        if (!project)
        {
            return {};
        }
        IO::ResourceUUID::Initialize(project->projectId);
        IO::ResourceLoader &loader = IO::ResourceLoader::Instance();
        loader.SetReadOnly(true);
        loader.Initialize(projectDir, userData);
        std::map<std::string, AssetSnapshot> assets;
        for (const IO::AssetMetadata &meta : loader.GetAllAssets())
        {
            AssetSnapshot s;
            s.uuid = meta.uuid.ToString();
            s.type = meta.resourceType;
            s.size = meta.fileSize;
            const auto asset = loader.Load<Base::Asset>(meta.resourcePath);
            s.loaded = asset != nullptr;
            if (const auto script = std::dynamic_pointer_cast<LuaScript>(asset))
            {
                s.serialized = script->GetSourceCode();
            }
            assets[meta.resourcePath.ToString()] = s;
        }
        return assets;
    }

    class PackedProjectTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            _dir = fs::temp_directory_path() / "n2-packed-project-test";
            std::error_code error;
            fs::remove_all(_dir, error);
            fs::create_directories(_dir);
        }

        void TearDown() override
        {
            IO::Vfs::Instance().UnmountAll();
            IO::ResourceLoader::Instance().SetReadOnly(false);
            std::error_code error;
            fs::remove_all(_dir, error);
        }

        fs::path _dir;
    };
}

TEST_F(PackedProjectTest, AProjectRunsTheSameFromItsFolderAndFromItsPack)
{
    const fs::path folder = fs::path(N2_LUA_PROJECT_DIR);
    const auto fromFolder = Snapshot(folder, _dir / "user-folder");
    ASSERT_FALSE(fromFolder.empty());
    EXPECT_FALSE(IO::ResourceLoader::Instance().IsPacked());

    // The pack: what n2build's cook step puts in one (project.n2proj, assets/, .import/)
    const fs::path pack = _dir / "game.n2pack";
    std::ofstream(pack, std::ios::binary)
        << Tests::MakeZip(Tests::FolderEntries(folder, {"assets", ".import"}, {"project.n2proj"}));
    auto mount = IO::Pack::Open(pack);
    ASSERT_TRUE(mount.has_value()) << mount.error();
    const fs::path game = _dir / "game"; // no such folder: the pack is mounted there
    IO::Vfs::Instance().Mount(game, std::move(*mount));

    const auto fromPack = Snapshot(game, _dir / "user-pack");
    EXPECT_TRUE(IO::ResourceLoader::Instance().IsPacked());
    EXPECT_TRUE(IO::ResourceLoader::Instance().IsReadOnly()) << "a pack is never written";

    ASSERT_EQ(fromPack.size(), fromFolder.size());
    for (const auto &[path, expected] : fromFolder)
    {
        const auto it = fromPack.find(path);
        ASSERT_NE(it, fromPack.end()) << path << " is missing from the pack's index";
        EXPECT_EQ(it->second, expected) << path;
        EXPECT_EQ(it->second.serialized, expected.serialized) << path;
    }
    EXPECT_FALSE(fs::exists(game)) << "nothing was written where the pack is mounted (no .import, no .n2)";
}

namespace
{
    std::string DecodeBase64(const std::string_view text)
    {
        static constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        std::uint32_t buffer = 0;
        int bits = 0;
        for (const char c : text)
        {
            const auto value = Alphabet.find(c);
            if (value == std::string_view::npos)
            {
                continue; // '=' padding
            }
            buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out += static_cast<char>((buffer >> bits) & 0xff);
            }
        }
        return out;
    }
}

TEST_F(PackedProjectTest, APackedModelReadsItsBufferAndImageFromThePack)
{
    // lua_project's robot with its embedded buffer and image moved out to robot.bin and robot.png: the importer then reads
    // both through the mount (ModelImportSettings::readFile), not from the disk
    std::ifstream in(fs::path(N2_LUA_PROJECT_DIR) / "assets" / "models" / "smoke_robot.gltf", std::ios::binary);
    nlohmann::json gltf = nlohmann::json::parse(in);
    std::ifstream projectFile(fs::path(N2_LUA_PROJECT_DIR) / "project.n2proj", std::ios::binary);
    std::vector<Tests::ZipEntry> entries{
        {"project.n2proj", std::string(std::istreambuf_iterator<char>(projectFile), std::istreambuf_iterator<char>())}};
    const auto externalise = [&](nlohmann::json &item, const std::string &file) {
        const std::string uri = item.at("uri").get<std::string>();
        const auto comma = uri.find(',');
        ASSERT_NE(comma, std::string::npos);
        entries.push_back({"assets/" + file, DecodeBase64(std::string_view(uri).substr(comma + 1))});
        item["uri"] = file;
    };
    ASSERT_EQ(gltf.at("buffers").size(), 1u);
    externalise(gltf.at("buffers")[0], "robot.bin");
    ASSERT_GE(gltf.value("images", nlohmann::json::array()).size(), 1u);
    externalise(gltf.at("images")[0], "robot.png");
    entries.push_back({"assets/robot.gltf", gltf.dump()});

    const fs::path pack = _dir / "model.n2pack";
    std::ofstream(pack, std::ios::binary) << Tests::MakeZip(entries);
    IO::Vfs::Instance().Mount(_dir / "model", std::move(*IO::Pack::Open(pack)));

    const auto assets = Snapshot(_dir / "model", _dir / "user");
    const auto it = assets.find("res://robot.gltf");
    ASSERT_NE(it, assets.end());
    EXPECT_TRUE(it->second.loaded) << "the model and its .bin were read from the pack";
    EXPECT_FALSE(fs::exists(_dir / "model")) << "nothing on disk where the pack is mounted";
}

TEST_F(PackedProjectTest, LoadTimesOfAFolderAndItsPacks)
{
    // A measurement, not a check (docs/packs.html#measured): N2_PACK_TIMING_DIR names a folder holding lua_project cooked
    // as lua_project-zip.n2pack, -stored.n2pack and -7z.n2pack (n2build cook --pack ...). Each way of opening the project
    // is timed opening it and loading every asset, the median of 7 runs.
    const char *dir = std::getenv("N2_PACK_TIMING_DIR");
    if (dir == nullptr || *dir == '\0')
    {
        GTEST_SKIP() << "set N2_PACK_TIMING_DIR to a folder of cooked lua_project packs to measure";
    }
    const auto median = [&](const fs::path &projectDir) {
        std::vector<double> times;
        for (int i = 0; i < 7; ++i)
        {
            const auto start = std::chrono::steady_clock::now();
            const auto assets = Snapshot(projectDir, _dir / "user");
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            EXPECT_FALSE(assets.empty());
        }
        std::ranges::sort(times);
        return times[times.size() / 2];
    };
    std::cout << "folder: " << median(fs::path(N2_LUA_PROJECT_DIR)) << " ms" << std::endl;
    for (const char *format : {"zip", "stored", "7z"})
    {
        const fs::path pack = fs::path(dir) / (std::string("lua_project-") + format + ".n2pack");
        auto mount = IO::Pack::Open(pack);
        ASSERT_TRUE(mount.has_value()) << mount.error();
        IO::Vfs::Instance().Mount(_dir / format, std::move(*mount));
        std::cout << format << ": " << median(_dir / format) << " ms" << std::endl;
    }
}
