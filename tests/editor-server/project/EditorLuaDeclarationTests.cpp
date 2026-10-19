#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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
#include <engine/scripting/DeclarationEvaluator.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <engine/scripting/ScriptDeclarationCache.hpp>

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

// Lua edit-host option B, PR L2, at the editor host: with the execution policy DeclarationsOnly (what N2EditorHost sets
// when it edits) a client's scene, a hot reload or an undo never runs a script's code in the engine, the declarations come
// from the ScriptDeclarationCache, a scene is built once, and a script that would stall the host cannot. Part of the
// editor_project_tests program, which owns ResourceLoader and SceneManager for its tests.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t ComponentAddedType = static_cast<uint8_t>(ResponseType::ComponentAdded);
    constexpr uint8_t ComponentDataType = static_cast<uint8_t>(ResponseType::ComponentData);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);
    constexpr uint8_t LuaFieldsType = static_cast<uint8_t>(ResponseType::LuaFields);
    constexpr uint8_t PlaySnapshotType = static_cast<uint8_t>(ResponseType::PlaySnapshot);

    constexpr const char *ObjectId = "123e4567-e89b-12d3-a456-426614174000";

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

    // What a test thread blocks on inside the evaluator hook; shared with the hook, so a worker that is still waiting
    // never touches a destroyed object
    struct Gate
    {
        std::atomic<bool> open{false};

        void Open() { open = true; }

        // Polls: the single-threaded web build has no condition variable (and a test that blocks skips itself there)
        void Wait() const
        {
            while (!open.load())
            {
                std::this_thread::sleep_for(1ms);
            }
        }
    };

    const char *HostileScript = R"LUA(
        L2E_GLOBAL = "set by a script"
        Application.Quit()
        local C = {}
        C.__index = C
        C.SerializableFields = { speed = 4, label = { type = "string", default = "hi" } }
        function C:OnDestroy() L2E_DESTROYED = true end
        function C:OnDisable() L2E_DISABLED = true end
        return C
    )LUA";

    class EditorLuaDeclarationTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            _watchdog = std::make_unique<N2Engine::Tests::HangWatchdog>();
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-lua-declaration-test" / info->name();
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
            server.GetHistory().SetCoalesceWindow(-1ms);

            ASSERT_TRUE(LuaRuntime::Instance().Initialize());
            ClearGlobals();
            Cache().ResetForTest();
            _quitAllowed = Application::IsScriptQuitAllowed();
            Application::SetScriptQuitAllowed(true); // the policy, not this older flag, is what protects the host
            Application::GetInstance().ResetQuitRequest();
            // The host's own setting (Host.cpp) for an editor that edits
            LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::DeclarationsOnly);
        }

        void TearDown() override
        {
            LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::Run);
            _gate->Open();
            WaitForNoWorkers();
            Cache().ResetForTest();
            ClearGlobals();
            Application::SetScriptQuitAllowed(_quitAllowed);
            Application::GetInstance().ResetQuitRequest();
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
            _watchdog.reset();
        }

        static ScriptDeclarationCache &Cache() { return ScriptDeclarationCache::Instance(); }
        static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

        // A global of the engine's state that is not nil (read with Lua's own API: no reference is made)
        static bool GlobalIsSet(const char *name)
        {
            lua_State *state = Lua().lua_state();
            lua_getglobal(state, name);
            const bool set = lua_type(state, -1) != LUA_TNIL;
            lua_pop(state, 1);
            return set;
        }

        static void ClearGlobals()
        {
            for (const char *name : {"L2E_GLOBAL", "L2E_DESTROYED", "L2E_DISABLED"})
            {
                Lua()[name] = sol::lua_nil;
            }
        }

        static bool WaitForNoWorkers(const std::chrono::milliseconds limit = 20s)
        {
            const auto end = std::chrono::steady_clock::now() + limit;
            while (Cache().LiveWorkers() > 0 && std::chrono::steady_clock::now() < end)
            {
                std::this_thread::sleep_for(5ms);
            }
            return Cache().LiveWorkers() == 0;
        }

        void WriteAsset(const std::string &relative, const std::string &text)
        {
            const fs::path path = _root / "assets" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        void Rescan() { EXPECT_EQ(Execute(server, CommandType::RescanAssets).type, OkType); }

        // A scene with one object that has a LuaComponent running `scriptPath`
        static std::string SceneWithScript(const std::string &scriptPath, const std::string &sceneName = "Declared")
        {
            const json object = {{"uuid", ObjectId},
                                 {"name", "Scripted"},
                                 {"tag", "Untagged"},
                                 {"layer", 0},
                                 {"isActive", true},
                                 {"components", json::array({json{{"type", "LuaComponent"},
                                                                  {"data", json{{"scriptPath", scriptPath}}}}})},
                                 {"children", json::array()}};
            const json scene = {{"name", sceneName}, {"rootGameObjects", json::array({object})}};
            return scene.dump();
        }

        Frame LoadScene(const std::string &scriptPath, const std::string &sceneName = "Declared")
        {
            return Execute(server, CommandType::LoadScene, Strings({SceneWithScript(scriptPath, sceneName)}));
        }

        Frame OpenScene(const std::string &path) { return Execute(server, CommandType::OpenScene, Strings({path})); }

        // The host keeps answering, and nothing a script could do has happened
        void ExpectHostIntactAndNothingRan(const std::string &when)
        {
            EXPECT_EQ(Execute(server, CommandType::RescanAssets).type, OkType) << when << ": the host answers";
            EXPECT_FALSE(GlobalIsSet("L2E_GLOBAL")) << when << ": no script's top level ran";
            EXPECT_FALSE(GlobalIsSet("L2E_DESTROYED")) << when;
            EXPECT_FALSE(GlobalIsSet("L2E_DISABLED")) << when;
            EXPECT_FALSE(Application::GetInstance().IsQuitRequested()) << when << ": the host loop would have ended";
        }

        std::string ScriptId(const std::string &resourcePath) const
        {
            return IO::ResourceLoader::Instance().GetUUID(IO::ResourcePath(resourcePath)).ToString();
        }

        std::string CreateEntity(const std::string &name)
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString("");
            w.WriteI32(-1);
            w.WriteString("");
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            if (created.type != EntityCreatedType)
                return {};
            BufferReader r(created.payload);
            return r.ReadString();
        }

        std::string AddLuaComponent(const std::string &entity)
        {
            const Frame added = Execute(server, CommandType::AddComponent, Strings({entity, "LuaComponent"}));
            EXPECT_EQ(added.type, ComponentAddedType) << added.Text();
            if (added.type != ComponentAddedType)
                return {};
            BufferReader r(added.payload);
            return r.ReadString();
        }

        Frame SetFields(const std::string &entity, const std::string &component, const json &values)
        {
            BufferWriter w;
            w.WriteString(entity);
            w.WriteString(component);
            WriteJson(w, values);
            return Execute(server, CommandType::SetComponentFields, w.Release());
        }

        json GetComponent(const std::string &entity, const std::string &component)
        {
            const Frame frame = Execute(server, CommandType::GetComponent, Strings({entity, component}));
            EXPECT_EQ(frame.type, ComponentDataType) << frame.Text();
            if (frame.type != ComponentDataType)
                return json::object();
            BufferReader r(frame.payload);
            return ReadJson(r);
        }

        // The names (in order) and kinds of GetLuaFields' fields
        std::vector<std::string> LuaFieldNames(const std::string &entity, const std::string &component)
        {
            std::vector<std::string> names;
            const Frame frame = Execute(server, CommandType::GetLuaFields, Strings({entity, component}));
            EXPECT_EQ(frame.type, LuaFieldsType) << frame.Text();
            if (frame.type != LuaFieldsType)
                return names;
            BufferReader r(frame.payload);
            const json schema = ReadJson(r); // kept: a range over a part of a temporary would dangle
            for (const json &field : schema.at("fields"))
            {
                names.push_back(field.at("name").get<std::string>() + ":" + field.at("kind").get<std::string>());
            }
            return names;
        }

        // A script on the open scene, chosen the way the inspector does
        struct Scripted
        {
            std::string entity;
            std::string component;
        };

        Scripted ScriptedObject(const std::string &scriptResourcePath)
        {
            Scripted result;
            result.entity = CreateEntity("Scripted");
            result.component = AddLuaComponent(result.entity);
            EXPECT_EQ(SetFields(result.entity, result.component, json{{"scriptUUID", ScriptId(scriptResourcePath)}}).type,
                      ComponentDataType);
            return result;
        }

        // Makes `Gate` block every evaluation
        void BlockEvaluations()
        {
            const std::shared_ptr<Gate> gate = _gate;
            Cache().SetEvaluatorForTest([gate](std::string_view, const std::string &, const DeclarationLimits &)
            {
                gate->Wait();
                DeclarationResult result;
                result.declared = true;
                return result;
            });
        }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
        std::unique_ptr<N2Engine::Tests::HangWatchdog> _watchdog;
        std::shared_ptr<Gate> _gate = std::make_shared<Gate>();
        bool _quitAllowed = true;
    };
}

// ==================== a scene that is loaded ====================

TEST_F(EditorLuaDeclarationTest, LoadSceneBuildsTheSceneOnceAndRunsNoScript)
{
    WriteAsset("scripts/Hostile.lua", HostileScript);
    Rescan();

    const Frame loaded = LoadScene("res://scripts/Hostile.lua");

    EXPECT_EQ(loaded.type, OkType) << loaded.Text();
    EXPECT_EQ(Cache().GetStats().lookups, 1u) << "one build of the scene: it used to be built two or three times";
    EXPECT_EQ(Cache().GetStats().evaluations, 1u);
    ExpectHostIntactAndNothingRan("after LoadScene");

    // The same scene again (a name that exists: its stored data is replaced): one more build, read from the cache
    const Frame again = LoadScene("res://scripts/Hostile.lua");
    EXPECT_EQ(again.type, OkType) << again.Text();
    EXPECT_EQ(Cache().GetStats().lookups, 2u);
    EXPECT_EQ(Cache().GetStats().hits, 1u);
    EXPECT_EQ(Cache().GetStats().evaluations, 1u);
    ExpectHostIntactAndNothingRan("after the second LoadScene");

    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(SceneManager::GetCurScene()->sceneName, "Declared");
}

TEST_F(EditorLuaDeclarationTest, LoadSceneKeepsItsValidationAndItsNameCollisionBehaviour)
{
    WriteAsset("scripts/Hostile.lua", HostileScript);
    Rescan();
    ASSERT_EQ(LoadScene("res://scripts/Hostile.lua", "First").type, OkType);
    ASSERT_EQ(LoadScene("res://scripts/Hostile.lua", "Second").type, OkType);
    EXPECT_EQ(SceneManager::GetCurScene()->sceneName, "Second");
    EXPECT_TRUE(SceneManager::HasScene("First"));
    EXPECT_TRUE(SceneManager::HasScene("Second"));

    // Refused as before, and nothing of the open scene changes
    EXPECT_EQ(Execute(server, CommandType::LoadScene, Strings({"{ not json"})).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::LoadScene, Strings({R"({"name":"NoRoots"})"})).type, ErrorType);
    EXPECT_EQ(SceneManager::GetCurScene()->sceneName, "Second");

    // The same name as the one open: replaced, still one stored scene of that name
    ASSERT_EQ(LoadScene("res://scripts/Hostile.lua", "Second").type, OkType);
    EXPECT_EQ(SceneManager::GetCurScene()->sceneName, "Second");
    EXPECT_EQ(SceneManager::GetSceneIndex("Second"), SceneManager::GetCurSceneIndex());
}

TEST_F(EditorLuaDeclarationTest, ASceneWhoseScriptLoopsForeverLoadsAndTheHostKeepsAnswering)
{
    WriteAsset("scripts/Spin.lua", "while true do end\nreturn {}\n");
    Rescan();

    const Frame loaded = LoadScene("res://scripts/Spin.lua");

    EXPECT_EQ(loaded.type, OkType) << loaded.Text();
    ExpectHostIntactAndNothingRan("after LoadScene");
    const auto cached = Cache().Peek(IO::ResourcePath("res://scripts/Spin.lua"), "while true do end\nreturn {}\n");
    ASSERT_NE(cached, nullptr);
    EXPECT_FALSE(cached->declared);
    EXPECT_TRUE(cached->budgetExceeded) << "the evaluator's budget ended it in its own state";
}

// ==================== a scene that is opened, snapshotted, undone ====================

TEST_F(EditorLuaDeclarationTest, OpenSceneWritePlaySnapshotAndUndoRunNoScript)
{
    WriteAsset("scripts/Hostile.lua", HostileScript);
    WriteAsset("scenes/Scripted.scene", SceneWithScript("res://scripts/Hostile.lua", "Scripted"));
    Rescan();

    const Frame opened = OpenScene("res://scenes/Scripted.scene");
    ASSERT_EQ(opened.type, SceneInfoType) << opened.Text();
    EXPECT_EQ(Cache().GetStats().lookups, 1u) << "OpenScene builds the scene once";
    ExpectHostIntactAndNothingRan("after OpenScene");

    // WritePlaySnapshot validates the scene by building it (a play host runs it; this host never does)
    const Frame snapshot = Execute(server, CommandType::WritePlaySnapshot, Strings({""}));
    EXPECT_EQ(snapshot.type, PlaySnapshotType) << snapshot.Text();
    ExpectHostIntactAndNothingRan("after WritePlaySnapshot");
    EXPECT_EQ(Cache().GetStats().evaluations, 1u) << "the script's text was read once for all of it";

    // Delete (a scene snapshot is taken), undo (the scene is rebuilt from it), redo
    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, Strings({ObjectId})).type, OkType);
    ExpectHostIntactAndNothingRan("after DestroyEntity");
    EXPECT_EQ(Execute(server, CommandType::Undo).type, EditResultType);
    ExpectHostIntactAndNothingRan("after Undo");
    EXPECT_EQ(Execute(server, CommandType::Redo).type, EditResultType);
    ExpectHostIntactAndNothingRan("after Redo");
    EXPECT_EQ(Execute(server, CommandType::Undo).type, EditResultType);
    ExpectHostIntactAndNothingRan("after the second Undo");
    EXPECT_EQ(Cache().GetStats().evaluations, 1u) << "every rebuild was a cache hit";

    const Frame entity = Execute(server, CommandType::GetEntity, Strings({ObjectId}));
    EXPECT_EQ(entity.type, static_cast<uint8_t>(ResponseType::EntityData)) << "the undo brought the object back";
}

TEST_F(EditorLuaDeclarationTest, DestroyingAScriptedObjectRunsNoLuaOnDestroy)
{
    WriteAsset("scripts/Hostile.lua", HostileScript);
    WriteAsset("scenes/Scripted.scene", SceneWithScript("res://scripts/Hostile.lua", "Scripted"));
    Rescan();
    ASSERT_EQ(OpenScene("res://scenes/Scripted.scene").type, SceneInfoType);

    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, Strings({ObjectId})).type, OkType);
    EXPECT_FALSE(GlobalIsSet("L2E_DESTROYED")) << "the script's OnDestroy did not run";
    EXPECT_FALSE(GlobalIsSet("L2E_DISABLED"));

    // Switching to another scene is a teardown of the open one too
    ASSERT_EQ(Execute(server, CommandType::Undo).type, EditResultType);
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Another"})).type, SceneInfoType);
    ExpectHostIntactAndNothingRan("after the scene was replaced");
}

TEST_F(EditorLuaDeclarationTest, UnderTheRunPolicyTheSameDestroyDoesRunTheScript)
{
    // The control: what the tests above rule out is what happens when scripts run (a play host, a game)
    LuaRuntime::SetExecutionPolicy(LuaExecutionPolicy::Run);
    Application::SetScriptQuitAllowed(false); // as the host would have it, so the script cannot end the test program
    WriteAsset("scripts/Hostile.lua", HostileScript);
    WriteAsset("scenes/Scripted.scene", SceneWithScript("res://scripts/Hostile.lua", "Scripted"));
    Rescan();
    ASSERT_EQ(OpenScene("res://scenes/Scripted.scene").type, SceneInfoType);
    EXPECT_TRUE(GlobalIsSet("L2E_GLOBAL")) << "its top level ran when the scene was built";

    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, Strings({ObjectId})).type, OkType);
    EXPECT_TRUE(GlobalIsSet("L2E_DESTROYED")) << "and its OnDestroy ran";
}

// ==================== the inspector ====================

TEST_F(EditorLuaDeclarationTest, ChoosingAScriptAndEditingItsFieldsUseTheDeclarationsOnly)
{
    WriteAsset("scripts/Hostile.lua", HostileScript);
    Rescan();
    ASSERT_EQ(OpenScene("res://scenes/Main.scene").type, SceneInfoType);

    const Scripted scripted = ScriptedObject("res://scripts/Hostile.lua");

    EXPECT_EQ(GetComponent(scripted.entity, scripted.component).at("scriptData"), (json{{"label", "hi"}, {"speed", 4}}));
    EXPECT_EQ(LuaFieldNames(scripted.entity, scripted.component),
              (std::vector<std::string>{"scriptUUID:AssetRef", "label:String", "speed:Int"}));
    ASSERT_EQ(SetFields(scripted.entity, scripted.component, json{{"scriptData", {{"speed", 9}}}}).type, ComponentDataType);
    EXPECT_EQ(GetComponent(scripted.entity, scripted.component).at("scriptData").at("speed"), 9);
    ExpectHostIntactAndNothingRan("after editing the fields");

    // Undo and redo of those edits
    EXPECT_EQ(Execute(server, CommandType::Undo).type, EditResultType);
    EXPECT_EQ(GetComponent(scripted.entity, scripted.component).at("scriptData").at("speed"), 4);
    EXPECT_EQ(Execute(server, CommandType::Redo).type, EditResultType);
    EXPECT_EQ(GetComponent(scripted.entity, scripted.component).at("scriptData").at("speed"), 9);
    ExpectHostIntactAndNothingRan("after undo and redo");
}

TEST_F(EditorLuaDeclarationTest, AFailedDeclarationKeepsScriptDataAndListsItsKeys)
{
    WriteAsset("scripts/Hostile.lua", HostileScript);
    WriteAsset("scripts/Broken.lua", "this is not lua\n");
    Rescan();
    ASSERT_EQ(OpenScene("res://scenes/Main.scene").type, SceneInfoType);
    const Scripted scripted = ScriptedObject("res://scripts/Hostile.lua");
    ASSERT_EQ(SetFields(scripted.entity, scripted.component, json{{"scriptData", {{"speed", 9}, {"label", "kept"}}}}).type,
              ComponentDataType);

    // Another script that cannot be read: the data stays, and its keys are what the inspector lists
    ASSERT_EQ(SetFields(scripted.entity, scripted.component, json{{"scriptUUID", ScriptId("res://scripts/Broken.lua")}}).type,
              ComponentDataType);

    EXPECT_EQ(GetComponent(scripted.entity, scripted.component).at("scriptData"), (json{{"label", "kept"}, {"speed", 9}}));
    EXPECT_EQ(LuaFieldNames(scripted.entity, scripted.component),
              (std::vector<std::string>{"scriptUUID:AssetRef", "label:String", "speed:Int"}));
    ExpectHostIntactAndNothingRan("after a script that cannot be read");
}

// ==================== hot reload ====================

TEST_F(EditorLuaDeclarationTest, WritingALoadedScriptRereadsItsDeclarationsThroughTheCacheAndRunsNothing)
{
    WriteAsset("scripts/Hot.lua", R"LUA(
        local Hot = {}
        Hot.__index = Hot
        Hot.SerializableFields = { a = 1 }
        return Hot
    )LUA");
    Rescan();
    ASSERT_EQ(OpenScene("res://scenes/Main.scene").type, SceneInfoType);
    const Scripted scripted = ScriptedObject("res://scripts/Hot.lua");
    EXPECT_EQ(LuaFieldNames(scripted.entity, scripted.component),
              (std::vector<std::string>{"scriptUUID:AssetRef", "a:Int"}));
    const std::uint64_t evaluationsBefore = Cache().GetStats().evaluations;

    const Frame written = Execute(server, CommandType::WriteTextAsset,
                                  Strings({"res://scripts/Hot.lua", R"LUA(
        L2E_GLOBAL = "a reload ran this"
        while false do end
        local Hot = {}
        Hot.__index = Hot
        Hot.SerializableFields = { a = 1, b = { type = "string", default = "new" } }
        return Hot
    )LUA"}));

    EXPECT_EQ(written.type, OkType) << written.Text();
    // The changed declaration, through the cache: one new evaluation, the new field with its default, the saved one kept
    EXPECT_EQ(Cache().GetStats().evaluations, evaluationsBefore + 1);
    EXPECT_EQ(LuaFieldNames(scripted.entity, scripted.component),
              (std::vector<std::string>{"scriptUUID:AssetRef", "a:Int", "b:String"}));
    EXPECT_EQ(GetComponent(scripted.entity, scripted.component).at("scriptData"), (json{{"a", 1}, {"b", "new"}}));
    ExpectHostIntactAndNothingRan("after the reload");
    EXPECT_FALSE(LuaRuntime::Instance().GetModule("scripts.Hot").has_value()) << "not even run as a module";

    // A version that does not parse is a log line, the last good declarations stay
    const Frame broken = Execute(server, CommandType::WriteTextAsset, Strings({"res://scripts/Hot.lua", "this is not lua\n"}));
    EXPECT_EQ(broken.type, OkType) << broken.Text();
    EXPECT_EQ(LuaFieldNames(scripted.entity, scripted.component),
              (std::vector<std::string>{"scriptUUID:AssetRef", "a:Int", "b:String"}));

    // The same text again is a cache hit
    const std::uint64_t evaluations = Cache().GetStats().evaluations;
    ASSERT_EQ(Execute(server, CommandType::WriteTextAsset, Strings({"res://scripts/Hot.lua", "this is not lua\n"})).type, OkType);
    EXPECT_EQ(Cache().GetStats().evaluations, evaluations);
}

// ==================== a script that would stall the host ====================

TEST_F(EditorLuaDeclarationTest, AScriptWhoseReadingStallsDoesNotStallTheCommandThread)
{
    WriteAsset("scripts/Stall.lua", "local C = {} C.__index = C C.SerializableFields = { a = 1 } return C\n");
    Rescan();
    Cache().SetDeadlineMillis(120);
    BlockEvaluations();

    const auto start = std::chrono::steady_clock::now();
    const Frame loaded = LoadScene("res://scripts/Stall.lua");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(loaded.type, OkType) << loaded.Text();
    EXPECT_LT(elapsed, 5s) << "the evaluation is abandoned at its deadline; the worker is blocked for good";
    EXPECT_EQ(Cache().LiveWorkers(), 1) << "the abandoned worker is still inside its call";
    ExpectHostIntactAndNothingRan("after the stalled scene");

    // The timeout is recorded for that text: the same scene again does not wait or start a worker
    const auto again = std::chrono::steady_clock::now();
    EXPECT_EQ(LoadScene("res://scripts/Stall.lua").type, OkType);
    EXPECT_LT(std::chrono::steady_clock::now() - again, 2s);
    EXPECT_EQ(Cache().LiveWorkers(), 1);

    // Host exit: a bounded wait for it, then (once it can end) it is cleaned up
    EXPECT_EQ(Cache().Shutdown(50ms), 1);
    _gate->Open();
    EXPECT_EQ(Cache().Shutdown(10s), 0);
}
