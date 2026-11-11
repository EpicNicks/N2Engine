#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

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

TEST_F(PackedProjectTest, APackedModelReadsItsBufferFromThePack)
{
    // A glTF whose buffer is an external .bin: the importer reads it through the mount (ModelImportSettings::readFile)
    const fs::path models = fs::path(N2_TEST_ASSETS_DIR) / "models";
    fs::path gltf;
    for (const auto &entry : fs::directory_iterator(models))
    {
        if (entry.path().extension() == ".gltf" && fs::exists(entry.path().parent_path() / (entry.path().stem().string() + ".bin")))
        {
            gltf = entry.path();
            break;
        }
    }
    if (gltf.empty())
    {
        GTEST_SKIP() << "no .gltf with an external .bin in tests/assets/models";
    }
    const std::string name = gltf.filename().string();
    const std::string bin = gltf.stem().string() + ".bin";
    const std::string project = R"({"formatVersion":1,"name":"Packed Model","projectId":"6f1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d"})";
    std::vector<Tests::ZipEntry> entries{{"project.n2proj", project}};
    for (const std::string &file : {name, bin})
    {
        std::ifstream in(models / file, std::ios::binary);
        entries.push_back({"assets/" + file, std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())});
    }
    const fs::path pack = _dir / "model.n2pack";
    std::ofstream(pack, std::ios::binary) << Tests::MakeZip(entries);
    IO::Vfs::Instance().Mount(_dir / "model", std::move(*IO::Pack::Open(pack)));

    const auto assets = Snapshot(_dir / "model", _dir / "user");
    const auto it = assets.find("res://" + name);
    ASSERT_NE(it, assets.end());
    EXPECT_TRUE(it->second.loaded) << "the model and its .bin were read from the pack";
}
