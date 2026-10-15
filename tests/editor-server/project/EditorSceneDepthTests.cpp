#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/serialization/JsonLimits.hpp>

#include "DeepSceneJson.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// Scene JSON nested too deep (audit F4, #sec-scene-depth-cap) through EditorServer::ExecuteCommand: LoadScene, OpenScene,
// WritePlaySnapshot, the play snapshot, the autosave restore and SetEntityParent. Part of the editor_project_tests program,
// which owns ResourceLoader and SceneManager for its tests.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);

    constexpr std::size_t Cap = JsonLimits::MaxHierarchyDepth;
    const char *const DepthMessage = "the scene JSON nests more than 1088 levels deep";

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

    void WriteFile(const fs::path &path, const std::string &text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    std::string ChainId(const std::size_t level)
    {
        char id[64];
        std::snprintf(id, sizeof(id), "00000000-0000-0000-0000-%012zx", level);
        return id;
    }

    // How many levels the first root of the open scene has: walked down the first children, no recursion
    std::size_t OpenSceneChainLevels()
    {
        const Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr || scene->GetRootGameObjectCount() == 0)
            return 0;
        std::size_t levels = 0;
        for (GameObject::Ptr current = scene->GetRootGameObjects().front(); current != nullptr;
             current = current->GetChildCount() > 0 ? current->GetChild(0) : nullptr)
        {
            ++levels;
        }
        return levels;
    }

    class EditorSceneDepthTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-scene-depth-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "My Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "My Game");

            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        [[nodiscard]] fs::path Assets() const { return _root / "assets"; }

        Frame LoadScene(const std::string &text) { return Execute(server, CommandType::LoadScene, Strings({text})); }

        // A small scene open, to see that a refused load leaves it alone
        void OpenSmallScene()
        {
            ASSERT_EQ(LoadScene(R"({"name":"Before","rootGameObjects":[]})").type, OkType);
            ASSERT_NE(SceneManager::GetCurScene(), nullptr);
            ASSERT_EQ(SceneManager::GetCurScene()->sceneName, "Before");
        }

        void ExpectBeforeIsStillOpen()
        {
            ASSERT_NE(SceneManager::GetCurScene(), nullptr);
            EXPECT_EQ(SceneManager::GetCurScene()->sceneName, "Before");
            EXPECT_EQ(Execute(server, CommandType::GetOpenScene).type, static_cast<uint8_t>(ResponseType::SceneInfo))
                << "the host answers the next command";
        }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };
}

TEST_F(EditorSceneDepthTest, LoadSceneRefusesAHundredThousandLevelChainAndTheHostAnswersTheNextCommand)
{
    OpenSmallScene();
    const Frame frame = LoadScene(TestJson::SceneText(100000));
    EXPECT_EQ(frame.type, ErrorType);
    EXPECT_NE(frame.Text().find(DepthMessage), std::string::npos) << frame.Text();
    ExpectBeforeIsStillOpen();
}

TEST_F(EditorSceneDepthTest, LoadSceneRefusesAHierarchyOneLevelPastTheCap)
{
    OpenSmallScene();
    // within the JSON cap, past the hierarchy cap: GameObject::Deserialize refuses it
    const Frame frame = LoadScene(TestJson::SceneText(Cap + 1));
    EXPECT_EQ(frame.type, ErrorType);
    EXPECT_NE(frame.Text().find("invalid or corrupt"), std::string::npos) << frame.Text();
    ExpectBeforeIsStillOpen();
}

TEST_F(EditorSceneDepthTest, LoadSceneLoadsAHierarchyOneLevelUnderTheCap)
{
    ASSERT_EQ(LoadScene(TestJson::SceneText(Cap - 1)).type, OkType);
    EXPECT_EQ(OpenSceneChainLevels(), Cap - 1);
    ASSERT_EQ(LoadScene(TestJson::SceneText(Cap)).type, OkType);
    EXPECT_EQ(OpenSceneChainLevels(), Cap);
}

TEST_F(EditorSceneDepthTest, OpenSceneRefusesDeepFilesFromDisk)
{
    // the file as an editor can leave it with WriteTextAsset (4 MiB at most) or as a project can hold it from git
    WriteFile(Assets() / "scenes" / "VeryDeep.scene", TestJson::SceneText(100000));
    WriteFile(Assets() / "scenes" / "PastTheCap.scene", TestJson::SceneText(Cap + 1));
    WriteFile(Assets() / "scenes" / "UnderTheCap.scene", TestJson::SceneText(Cap - 1, "UnderTheCap"));
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
    OpenSmallScene();

    const Frame veryDeep = Execute(server, CommandType::OpenScene, Strings({"res://scenes/VeryDeep.scene"}));
    EXPECT_EQ(veryDeep.type, ErrorType);
    EXPECT_NE(veryDeep.Text().find(DepthMessage), std::string::npos) << veryDeep.Text();
    ExpectBeforeIsStillOpen();

    EXPECT_EQ(Execute(server, CommandType::OpenScene, Strings({"res://scenes/PastTheCap.scene"})).type, ErrorType);
    ExpectBeforeIsStillOpen();

    EXPECT_NE(Execute(server, CommandType::OpenScene, Strings({"res://scenes/UnderTheCap.scene"})).type, ErrorType);
    EXPECT_EQ(OpenSceneChainLevels(), Cap - 1);
}

TEST_F(EditorSceneDepthTest, WriteTextAssetKeepsADeepSceneAsTextAndTheCommandsThatReadItRefuseIt)
{
    // about 3 MB: what the 4 MiB cap of WriteTextAsset lets an editor write
    const std::string deep = TestJson::SceneText(40000);
    ASSERT_LT(deep.size(), 4u * 1024u * 1024u);
    ASSERT_EQ(Execute(server, CommandType::WriteTextAsset, Strings({"res://scenes/Written.scene", deep})).type, OkType);
    OpenSmallScene();

    EXPECT_EQ(Execute(server, CommandType::OpenScene, Strings({"res://scenes/Written.scene"})).type, ErrorType);
    ExpectBeforeIsStillOpen();
    EXPECT_EQ(Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/Written.scene"})).type, ErrorType);
    ExpectBeforeIsStillOpen();
}

TEST_F(EditorSceneDepthTest, WritePlaySnapshotRefusesADeepSceneAndWritesNoSnapshot)
{
    WriteFile(Assets() / "scenes" / "VeryDeep.scene", TestJson::SceneText(100000));
    WriteFile(Assets() / "scenes" / "PastTheCap.scene", TestJson::SceneText(Cap + 1));
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
    OpenSmallScene();

    const Frame veryDeep = Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/VeryDeep.scene"}));
    EXPECT_EQ(veryDeep.type, ErrorType);
    EXPECT_NE(veryDeep.Text().find(DepthMessage), std::string::npos) << veryDeep.Text();
    EXPECT_EQ(Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/PastTheCap.scene"})).type, ErrorType);
    EXPECT_FALSE(fs::exists(_root / ".n2" / "play")) << "no snapshot was written";
    ExpectBeforeIsStillOpen();
}

TEST_F(EditorSceneDepthTest, ThePlaySnapshotLoadRefusesADeepFile)
{
    // the --play child loads the snapshot file a launcher points it at: any file, so it checks it too
    WriteFile(_root / "deep.scene", TestJson::SceneText(100000));
    EditorServer host;
    const auto entered = host.EnterPlayMode(_root / "deep.scene");
    ASSERT_FALSE(entered);
    EXPECT_NE(entered.error().find(DepthMessage), std::string::npos) << entered.error();
    EXPECT_FALSE(host.IsPlayMode());

    WriteFile(_root / "past.scene", TestJson::SceneText(Cap + 1));
    EXPECT_FALSE(host.EnterPlayMode(_root / "past.scene"));
    EXPECT_FALSE(host.IsPlayMode());
}

TEST_F(EditorSceneDepthTest, RestoreAutosaveRefusesADeepAutosave)
{
    ASSERT_NE(Execute(server, CommandType::OpenScene, Strings({"res://scenes/Main.scene"})).type, ErrorType);
    const fs::path autosave = _root / ".n2" / "autosave" / "scenes" / "Main.scene";
    WriteFile(autosave, TestJson::SceneText(100000, "Main"));

    const Frame frame = Execute(server, CommandType::RestoreAutosave);
    EXPECT_EQ(frame.type, ErrorType);
    EXPECT_NE(frame.Text().find(DepthMessage), std::string::npos) << frame.Text();
    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(Execute(server, CommandType::GetOpenScene).type, static_cast<uint8_t>(ResponseType::SceneInfo));
}

TEST_F(EditorSceneDepthTest, SetEntityParentRefusesAMoveThatWouldNestPastTheCap)
{
    // a chain of Cap objects, and a root object beside it that could be moved under the deepest of them
    const std::string extraId = ChainId(0xFFFF);
    const std::string scene = "{\"name\":\"Capped\",\"rootGameObjects\":[" + TestJson::ChainText(Cap) +
                              ",{\"uuid\":\"" + extraId + "\",\"name\":\"extra\",\"children\":[]}]}";
    ASSERT_EQ(LoadScene(scene).type, OkType);

    BufferWriter request;
    request.WriteString(extraId);
    request.WriteString(ChainId(Cap)); // the deepest object
    request.WriteI32(-1);
    request.WriteBool(false);
    const Frame refused = Execute(server, CommandType::SetEntityParent, request.Release());
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("nest more than 512 levels deep"), std::string::npos) << refused.Text();
    EXPECT_EQ(SceneManager::GetCurSceneRef().GetRootGameObjectCount(), 2u) << "the object is still a root";

    // one level shallower the same move is fine
    BufferWriter allowed;
    allowed.WriteString(extraId);
    allowed.WriteString(ChainId(Cap - 1));
    allowed.WriteI32(-1);
    allowed.WriteBool(false);
    EXPECT_EQ(Execute(server, CommandType::SetEntityParent, allowed.Release()).type, OkType);
    EXPECT_EQ(SceneManager::GetCurSceneRef().GetRootGameObjectCount(), 1u);
}

namespace
{
    // An object of the open scene by its UUID (nullptr if it has none)
    GameObject::Ptr SceneEntity(const std::string &id)
    {
        const auto uuid = Math::UUID::FromString(id);
        return uuid ? SceneManager::GetCurSceneRef().FindGameObjectByUUID(*uuid) : nullptr;
    }

    std::vector<uint8_t> CreateEntityExPayload(const std::string &name, const std::string &parentId)
    {
        BufferWriter w;
        w.WriteString(name);
        w.WriteString(parentId);
        w.WriteI32(-1);
        w.WriteString(""); // an empty object
        return w.Release();
    }
}

TEST_F(EditorSceneDepthTest, CreateEntityExRefusesAParentAtTheCapAndRecordsNothing)
{
    ASSERT_EQ(LoadScene(TestJson::SceneText(Cap, "Capped")).type, OkType);
    const GameObject::Ptr deepest = SceneEntity(ChainId(Cap));
    ASSERT_NE(deepest, nullptr);

    const Frame refused = Execute(server, CommandType::CreateEntityEx, CreateEntityExPayload("Too deep", ChainId(Cap)));
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("nest more than 512 levels deep"), std::string::npos) << refused.Text();
    EXPECT_EQ(deepest->GetChildCount(), 0u);
    EXPECT_EQ(SceneManager::GetCurSceneRef().FindGameObject("Too deep"), nullptr);
    EXPECT_EQ(Execute(server, CommandType::Undo).type, ErrorType) << "no Create step was recorded";

    // one level up it is made
    const Frame created = Execute(server, CommandType::CreateEntityEx, CreateEntityExPayload("Fits", ChainId(Cap - 1)));
    ASSERT_EQ(created.type, static_cast<uint8_t>(ResponseType::EntityCreated)) << created.Text();
    BufferReader r(created.payload);
    const GameObject::Ptr made = SceneEntity(r.ReadString());
    ASSERT_NE(made, nullptr);
    EXPECT_EQ(made->GetParent(), SceneEntity(ChainId(Cap - 1)));
}

TEST_F(EditorSceneDepthTest, RedoOfACreateUnderAParentMovedPastTheCapIsAnErrorAndLeavesNoObject)
{
    // a chain of Cap - 1 objects, and a root object beside it to create under
    const std::string holderId = ChainId(0xFFFF);
    const std::string scene = "{\"name\":\"Capped\",\"rootGameObjects\":[" + TestJson::ChainText(Cap - 1) +
                              ",{\"uuid\":\"" + holderId + "\",\"name\":\"holder\",\"children\":[]}]}";
    ASSERT_EQ(LoadScene(scene).type, OkType);

    const Frame created = Execute(server, CommandType::CreateEntityEx, CreateEntityExPayload("made", holderId));
    ASSERT_EQ(created.type, static_cast<uint8_t>(ResponseType::EntityCreated)) << created.Text();
    BufferReader r(created.payload);
    const std::string madeId = r.ReadString();
    ASSERT_NE(Execute(server, CommandType::Undo).type, ErrorType);
    ASSERT_EQ(SceneEntity(madeId), nullptr);

    // the holder moved under the deepest object behind the history's back (as a script can): it is level Cap now,
    // so the object the redo builds again would be level Cap + 1
    const GameObject::Ptr holder = SceneEntity(holderId);
    const GameObject::Ptr deepest = SceneEntity(ChainId(Cap - 1));
    ASSERT_NE(holder, nullptr);
    ASSERT_NE(deepest, nullptr);
    holder->SetParent(deepest, false);
    ASSERT_EQ(holder->GetParent(), deepest);

    const Frame redo = Execute(server, CommandType::Redo);
    EXPECT_EQ(redo.type, ErrorType);
    EXPECT_NE(redo.Text().find("nest more than 512 levels deep"), std::string::npos) << redo.Text();
    EXPECT_EQ(SceneEntity(madeId), nullptr) << "the object built again is not left anywhere";
    EXPECT_EQ(holder->GetChildCount(), 0u);
}
