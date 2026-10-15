#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/Application.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/Logger.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourcePath.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/scripting/LuaComponent.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <engine/scripting/LuaScript.hpp>
#include <math/UUID.hpp>

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// Editor host security S4 (audit F3a): a client that puts a script on a scene must not be able to hang the host, fill
// its memory or end it. The script runs on the host when the scene loads (that is unchanged); it is bounded now.
// Part of the editor_project_tests program, which owns ResourceLoader and SceneManager for its tests.
namespace
{
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return {payload.begin(), payload.end()}; }
    };

    Frame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(type), payload);
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const auto body = r.ReadBytes(r.ReadU32());
        out.payload.assign(body.begin(), body.end());
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the response payload";
        return out;
    }

    std::vector<uint8_t> Strings(std::initializer_list<std::string> values)
    {
        BufferWriter w;
        for (const std::string &value : values)
            w.WriteString(value);
        return w.Release();
    }

    class EditorLuaBudgetTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // The scripts here loop or allocate until the limits stop them: if those are broken, the program ends
            // naming the test instead of spinning until the CI job's limit
            _watchdog = std::make_unique<N2Engine::Tests::HangWatchdog>();
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-lua-budget-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "My Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "My Game");

            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
            server.SetAssetPollInterval(std::chrono::milliseconds(0));

            ASSERT_TRUE(LuaRuntime::Instance().Initialize());
            _savedLimits = LuaRuntime::Instance().GetLimits();
            // Lowered through the API, so no test spends long in a loop
            LuaRuntime::Instance().SetLimits(
                {200'000, LuaRuntime::Instance().GetMemoryUsed() + std::size_t{16} * 1024 * 1024});
            Application::GetInstance().ResetQuitRequest();
        }

        void TearDown() override
        {
            LuaRuntime::Instance().SetLimits(_savedLimits);
            Application::SetScriptQuitAllowed(true);
            Application::GetInstance().ResetQuitRequest();
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
            _watchdog.reset();
        }

        void WriteAsset(const std::string &relative, const std::string &text)
        {
            const fs::path path = _root / "assets" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        void Rescan() { EXPECT_EQ(Execute(server, CommandType::RescanAssets).type, OkType); }

        // A scene with one object that has a LuaComponent running `scriptPath`
        static std::string SceneWithScript(const std::string &scriptPath)
        {
            const json object = {{"uuid", "123e4567-e89b-12d3-a456-426614174000"},
                                 {"name", "Scripted"},
                                 {"tag", "Untagged"},
                                 {"layer", 0},
                                 {"isActive", true},
                                 {"components", json::array({json{{"type", "LuaComponent"},
                                                                  {"data", json{{"scriptPath", scriptPath}}}}})},
                                 {"children", json::array()}};
            const json scene = {{"name", "Budgeted"}, {"rootGameObjects", json::array({object})}};
            return scene.dump();
        }

        Frame LoadScene(const std::string &scriptPath)
        {
            return Execute(server, CommandType::LoadScene, Strings({SceneWithScript(scriptPath)}));
        }

        // True if a log event of the host (what a client sees) says this
        [[nodiscard]] bool Logged(const std::string &needle) const
        {
            for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
            {
                if (event.value("kind", "") == "log" && event.value("message", "").find(needle) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        LuaLimits _savedLimits;
        EditorServer server;
        std::unique_ptr<N2Engine::Tests::HangWatchdog> _watchdog;
    };
}

TEST_F(EditorLuaBudgetTest, ASceneWhoseScriptLoopsForeverLoadsAndTheHostKeepsAnswering)
{
    WriteAsset("scripts/Spin.lua", "while true do end\nreturn {}\n");
    Rescan();

    const Frame loaded = LoadScene("res://scripts/Spin.lua");

    EXPECT_EQ(loaded.type, OkType) << loaded.Text() << " (it used to hang here)";
    EXPECT_TRUE(Logged("instruction budget")) << "the script's abort is a log event, not a hang";
    EXPECT_EQ(Execute(server, CommandType::RescanAssets).type, OkType) << "the host answers the next command";
}

TEST_F(EditorLuaBudgetTest, ASceneWhoseScriptFillsTheMemoryLoadsAndTheHostKeepsAnswering)
{
    WriteAsset("scripts/Bomb.lua", "local t = {}\nwhile true do t[#t + 1] = string.rep('x', 1000000) end\nreturn {}\n");
    Rescan();

    const Frame loaded = LoadScene("res://scripts/Bomb.lua");

    EXPECT_EQ(loaded.type, OkType) << loaded.Text();
    EXPECT_TRUE(Logged("not enough memory"));
    EXPECT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
}

TEST_F(EditorLuaBudgetTest, ASceneWhoseScriptQuitsDoesNotEndTheHost)
{
    Application::SetScriptQuitAllowed(false); // what N2EditorHost does for a host that edits
    WriteAsset("scripts/Quit.lua", "Application.Quit()\nreturn {}\n");
    Rescan();

    const Frame loaded = LoadScene("res://scripts/Quit.lua");

    EXPECT_EQ(loaded.type, OkType) << loaded.Text();
    EXPECT_FALSE(Application::GetInstance().IsQuitRequested()) << "the host loop would have ended";
    EXPECT_TRUE(Logged("Application.Quit is ignored on an editor host"));
    EXPECT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
}

TEST_F(EditorLuaBudgetTest, WritingALoopingVersionOfALoadedScriptReloadsWithoutHangingTheHost)
{
    WriteAsset("scripts/Hot.lua", "return {}\n");
    Rescan();
    const IO::ResourcePath path("res://scripts/Hot.lua");
    ASSERT_NE(IO::ResourceLoader::Instance().Load<LuaScript>(path), nullptr);
    const auto owner = GameObject::Create("Hot");
    auto *component = owner->AddComponent<LuaComponent>();
    component->SetScript(path);

    const Frame written = Execute(server, CommandType::WriteTextAsset,
                                  Strings({"res://scripts/Hot.lua", "while true do end\nreturn {}\n"}));

    EXPECT_EQ(written.type, OkType) << written.Text();
    EXPECT_TRUE(Logged("instruction budget"));
    EXPECT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
    owner->RemoveComponent<LuaComponent>();
}
