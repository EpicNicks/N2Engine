#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/Positionable.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourcePath.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/serialization/ComponentRegistry.hpp>
#include <engine/serialization/ComponentSerializer.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// Prefab assets (protocol 1.19.0, EN3): CreatePrefab, InstantiatePrefab, ApplyPrefab and UnpackPrefab through
// EditorServer::ExecuteCommand, against a project made by IO::CreateProject. Part of the editor_project_tests program,
// which owns ResourceLoader and SceneManager for its tests.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t HierarchyType = static_cast<uint8_t>(ResponseType::Hierarchy);
    constexpr uint8_t EntityDataType = static_cast<uint8_t>(ResponseType::EntityData);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);
    constexpr uint8_t AssetListType = static_cast<uint8_t>(ResponseType::AssetList);
    constexpr uint8_t AssetCreatedType = static_cast<uint8_t>(ResponseType::AssetCreated);
    constexpr uint8_t AssetDetailType = static_cast<uint8_t>(ResponseType::AssetDetail);

    constexpr int32_t Last = -1;

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

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    /// A component with references to a GameObject, to see what a prefab does with them
    class PrefabTestHolder final : public SerializableComponent
    {
    public:
        explicit PrefabTestHolder(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterGameObjectRef("target", target);
        }

        [[nodiscard]] std::string GetTypeName() const override { return "EditorPrefabTest_Holder"; }

        GameObject *target = nullptr;
    };

    /// A single tree of `levels` levels, each object with one child, as a prefab file's text
    std::string DeepPrefabText(const std::size_t levels)
    {
        std::string text = R"({"name":"deep","rootObject":)";
        for (std::size_t i = 0; i < levels; ++i)
        {
            text += R"({"name":"n","uuid":")" + Math::UUID::Random().ToString() + "\"";
            text += i + 1 < levels ? R"(,"children":[)" : "}";
        }
        for (std::size_t i = 1; i < levels; ++i)
            text += "]}";
        text += "}";
        return text;
    }

    class EditorPrefabTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ComponentRegistry::Instance().Register(
                "EditorPrefabTest_Holder",
                [](GameObject &gameObject) -> std::unique_ptr<Component> { return std::make_unique<PrefabTestHolder>(gameObject); });

            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-prefab-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "My Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "My Game");

            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Prefab Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
        }

        void TearDown() override
        {
            IO::ResourceLoader::Instance().SetReadOnly(false);
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        [[nodiscard]] fs::path Assets() const { return _root / "assets"; }

        void WriteAsset(const std::string &relative, const std::string &text)
        {
            const fs::path path = Assets() / IO::PathFromUtf8(relative);
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        std::string Create(const std::string &name, const std::string &parentId = "", const std::string &preset = "Empty")
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString(parentId);
            w.WriteI32(Last);
            w.WriteString(preset);
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            if (created.type != EntityCreatedType)
                return {};
            BufferReader r(created.payload);
            return r.ReadString();
        }

        Frame CreatePrefabFrame(const std::string &entityId, const std::string &path, bool overwrite = false)
        {
            BufferWriter w;
            w.WriteString(entityId);
            w.WriteString(path);
            w.WriteBool(overwrite);
            return Execute(server, CommandType::CreatePrefab, w.Release());
        }

        /// CreatePrefab's answer: the prefab asset's UUID (the path is checked against what was asked for)
        std::string CreatePrefab(const std::string &entityId, const std::string &path, bool overwrite = false)
        {
            const Frame frame = CreatePrefabFrame(entityId, path, overwrite);
            EXPECT_EQ(frame.type, AssetCreatedType) << frame.Text();
            if (frame.type != AssetCreatedType)
                return {};
            BufferReader r(frame.payload);
            EXPECT_EQ(r.ReadString(), path);
            const std::string uuid = r.ReadString();
            EXPECT_FALSE(r.HasData());
            return uuid;
        }

        Frame InstantiateFrame(const std::string &uuidOrPath, const std::string &parentId = "", int32_t siblingIndex = Last,
                               bool hasPosition = false, float x = 0.0f, float y = 0.0f, float z = 0.0f)
        {
            BufferWriter w;
            w.WriteString(uuidOrPath);
            w.WriteString(parentId);
            w.WriteI32(siblingIndex);
            w.WriteBool(hasPosition);
            w.WriteF32(x);
            w.WriteF32(y);
            w.WriteF32(z);
            return Execute(server, CommandType::InstantiatePrefab, w.Release());
        }

        std::string Instantiate(const std::string &uuidOrPath, const std::string &parentId = "", int32_t siblingIndex = Last)
        {
            const Frame frame = InstantiateFrame(uuidOrPath, parentId, siblingIndex);
            EXPECT_EQ(frame.type, EntityCreatedType) << frame.Text();
            if (frame.type != EntityCreatedType)
                return {};
            BufferReader r(frame.payload);
            return r.ReadString();
        }

        Frame Apply(const std::string &entityId) { return Execute(server, CommandType::ApplyPrefab, Strings({entityId})); }
        Frame Unpack(const std::string &entityId) { return Execute(server, CommandType::UnpackPrefab, Strings({entityId})); }

        json Nodes()
        {
            const Frame frame = Execute(server, CommandType::GetHierarchy);
            EXPECT_EQ(frame.type, HierarchyType) << frame.Text();
            if (frame.type != HierarchyType)
                return json::array();
            BufferReader r(frame.payload);
            (void)r.ReadU32();
            return ReadJson(r);
        }

        std::vector<std::string> Names()
        {
            std::vector<std::string> names;
            for (const json &node : Nodes())
                names.push_back(node.at("name").get<std::string>());
            return names;
        }

        std::vector<std::string> Ids()
        {
            std::vector<std::string> ids;
            for (const json &node : Nodes())
                ids.push_back(node.at("id").get<std::string>());
            return ids;
        }

        json Node(const std::string &id)
        {
            for (const json &node : Nodes())
            {
                if (node.at("id") == id)
                    return node;
            }
            ADD_FAILURE() << "no node " << id;
            return json::object();
        }

        json GetEntity(const std::string &id)
        {
            const Frame frame = Execute(server, CommandType::GetEntity, Strings({id}));
            EXPECT_EQ(frame.type, EntityDataType) << frame.Text();
            if (frame.type != EntityDataType)
                return json::object();
            BufferReader r(frame.payload);
            return ReadJson(r);
        }

        uint32_t Revision()
        {
            const Frame frame = Execute(server, CommandType::GetOpenScene);
            EXPECT_EQ(frame.type, SceneInfoType) << frame.Text();
            BufferReader r(frame.payload);
            (void)r.ReadString();
            (void)r.ReadString();
            (void)r.ReadString();
            return r.ReadU32();
        }

        /// Undo or Redo: the step's label
        std::string Step(CommandType type)
        {
            const Frame frame = Execute(server, type);
            EXPECT_EQ(frame.type, EditResultType) << frame.Text();
            if (frame.type != EditResultType)
                return {};
            BufferReader r(frame.payload);
            return r.ReadString();
        }

        void SetName(const std::string &id, const std::string &name)
        {
            BufferWriter w;
            w.WriteString(id);
            WriteJson(w, json{{"name", name}});
            ASSERT_EQ(Execute(server, CommandType::SetEntityProperties, w.Release()).type, OkType);
        }

        /// The assets ListAssets finds, recursively, by path
        json ListedAssets(const std::string &folder = "res://")
        {
            BufferWriter w;
            w.WriteString(folder);
            w.WriteBool(true);
            const Frame frame = Execute(server, CommandType::ListAssets, w.Release());
            EXPECT_EQ(frame.type, AssetListType) << frame.Text();
            if (frame.type != AssetListType)
                return json::array();
            BufferReader r(frame.payload);
            (void)ReadJson(r);
            return ReadJson(r);
        }

        json AssetAt(const std::string &path)
        {
            for (const json &asset : ListedAssets())
            {
                if (asset.at("path") == path)
                    return asset;
            }
            return json();
        }

        json PrefabFileJson(const std::string &relative) { return json::parse(ReadFile(Assets() / IO::PathFromUtf8(relative))); }

        std::vector<json> EventsOfKind(const std::string &kind)
        {
            std::vector<json> found;
            for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
            {
                if (event.value("kind", "") == kind)
                    found.push_back(event);
            }
            return found;
        }

        [[nodiscard]] static Scene &LoadedScene() { return SceneManager::GetCurSceneRef(); }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };
}

// ==================== CreatePrefab ====================

TEST_F(EditorPrefabTest, CreatePrefabWritesTheFileLinksTheSourceAndIsOneUndoStep)
{
    const std::string root = Create("Root", "", "Cube");
    const std::string kid = Create("Kid", root, "Sphere");
    const uint32_t revisionBefore = Revision();

    const std::string uuid = CreatePrefab(root, "res://prefabs/Root.prefab");

    // The file: Prefab::Serialize's form, two-space indented, a newline at the end, the root unlinked
    const std::string text = ReadFile(Assets() / "prefabs" / "Root.prefab");
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.back(), '\n');
    EXPECT_NE(text.find("\n  \"name\""), std::string::npos) << "indented by two spaces";
    const json file = json::parse(text);
    EXPECT_EQ(file.at("name"), "Root");
    EXPECT_EQ(file.at("rootObject").at("name"), "Root");
    EXPECT_EQ(file.at("rootObject").at("uuid"), root);
    EXPECT_FALSE(file.at("rootObject").contains("prefabReference")) << "a file is not linked to itself";
    ASSERT_EQ(file.at("rootObject").at("children").size(), 1u);
    EXPECT_EQ(file.at("rootObject").at("children")[0].at("name"), "Kid");
    EXPECT_EQ(file.at("rootObject").at("children")[0].at("uuid"), kid);

    // It is an asset: listed with the type Prefab and the UUID CreatePrefab answered
    const json asset = AssetAt("res://prefabs/Root.prefab");
    ASSERT_TRUE(asset.is_object());
    EXPECT_EQ(asset.at("type"), "Prefab");
    EXPECT_EQ(asset.at("uuid"), uuid);
    EXPECT_EQ(asset.at("size"), text.size());
    const Frame details = Execute(server, CommandType::GetAssetInfo, Strings({uuid}));
    ASSERT_EQ(details.type, AssetDetailType) << details.Text();
    const auto added = EventsOfKind("assetsChanged");
    ASSERT_FALSE(added.empty());
    EXPECT_NE(std::find(added.back().at("added").begin(), added.back().at("added").end(), "res://prefabs/Root.prefab"),
              added.back().at("added").end());

    // The source is linked, as a hierarchy node and as an entity header say; its child is not
    EXPECT_EQ(Node(root).at("prefab"), uuid);
    EXPECT_EQ(GetEntity(root).at("header").at("prefab"), uuid);
    EXPECT_FALSE(Node(kid).contains("prefab"));
    EXPECT_GT(Revision(), revisionBefore);
    const auto changed = EventsOfKind("sceneChanged");
    ASSERT_FALSE(changed.empty());
    ASSERT_TRUE(changed.back().contains("entityIds"));
    EXPECT_EQ(changed.back().at("entityIds"), json::array({root}));

    // One step: undo clears the link and leaves the file, redo links again
    EXPECT_EQ(Step(CommandType::Undo), "Create Prefab Root");
    EXPECT_FALSE(Node(root).contains("prefab"));
    EXPECT_TRUE(fs::exists(Assets() / "prefabs" / "Root.prefab")) << "writing the file is not an undo step";
    EXPECT_EQ(Step(CommandType::Redo), "Create Prefab Root");
    EXPECT_EQ(Node(root).at("prefab"), uuid);
    EXPECT_EQ(Step(CommandType::Undo), "Create Prefab Root");
    EXPECT_EQ(Step(CommandType::Undo), "Create Kid") << "the next step back is the one before it";
}

TEST_F(EditorPrefabTest, ASavedSceneKeepsTheLinkAndALoadedSceneShowsIt)
{
    const std::string root = Create("Root");
    const std::string uuid = CreatePrefab(root, "res://Root.prefab");
    ASSERT_EQ(Execute(server, CommandType::SaveSceneToFile, Strings({"res://scenes/Linked.scene"})).type, SceneInfoType);
    EXPECT_NE(ReadFile(Assets() / "scenes" / "Linked.scene").find(uuid), std::string::npos);
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Other"})).type, SceneInfoType);
    ASSERT_EQ(Execute(server, CommandType::OpenScene, Strings({"res://scenes/Linked.scene"})).type, SceneInfoType);
    EXPECT_EQ(Node(root).at("prefab"), uuid);
}

TEST_F(EditorPrefabTest, CreatePrefabRefusesAnExistingFileUnlessOverwriteAndKeepsTheUuid)
{
    const std::string root = Create("Root");
    const std::string uuid = CreatePrefab(root, "res://prefabs/Root.prefab");
    const std::string first = ReadFile(Assets() / "prefabs" / "Root.prefab");
    const std::string other = Create("Other");
    const uint32_t revision = Revision();

    const Frame refused = CreatePrefabFrame(other, "res://prefabs/Root.prefab");
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("already exists"), std::string::npos) << refused.Text();
    EXPECT_EQ(ReadFile(Assets() / "prefabs" / "Root.prefab"), first);
    EXPECT_EQ(Revision(), revision);
    EXPECT_FALSE(Node(other).contains("prefab"));

    // Overwrite replaces the file under the same asset UUID, and links the other object
    EXPECT_EQ(CreatePrefab(other, "res://prefabs/Root.prefab", true), uuid);
    EXPECT_EQ(PrefabFileJson("prefabs/Root.prefab").at("name"), "Other");
    EXPECT_EQ(Node(other).at("prefab"), uuid);
    EXPECT_EQ(Step(CommandType::Undo), "Create Prefab Other");

    // The same object again: the file is written, and there is nothing new to undo
    const uint32_t before = Revision();
    EXPECT_EQ(CreatePrefab(root, "res://prefabs/Root.prefab", true), uuid);
    EXPECT_EQ(Revision(), before) << "already linked to it";
    EXPECT_EQ(Step(CommandType::Undo), "Create Other") << "no step was recorded";
}

TEST_F(EditorPrefabTest, CreatePrefabChecksTheObjectAndThePath)
{
    const std::string root = Create("Root");
    const uint32_t revision = Revision();
    const auto refused = [&](const std::string &id, const std::string &path, const char *why)
    {
        const Frame frame = CreatePrefabFrame(id, path, true);
        EXPECT_EQ(frame.type, ErrorType) << why << ": " << path;
        EXPECT_EQ(Revision(), revision) << why;
        EXPECT_FALSE(Node(root).contains("prefab")) << why;
        return frame.Text();
    };

    refused("00000000-0000-0000-0000-000000000000", "res://a.prefab", "unknown object");
    refused("nonsense", "res://a.prefab", "not an id");
    refused(root, "res://a.txt", "not a .prefab");
    refused(root, "res://a", "no extension");
    refused(root, "res://.prefab", "no name");
    refused(root, "res://", "the assets folder");
    refused(root, "res://prefabs/", "a folder");
    refused(root, "a.prefab", "not a res:// path");
    refused(root, "/etc/outside.prefab", "an absolute path");

    // Paths that leave the assets folder, in every spelling: refused, and no file is made
    for (const char *path : {"res://../outside.prefab", "res://a/../../outside.prefab", 
                             "res://C:/outside.prefab", "res://nul.prefab",
                             "res://a:stream.prefab"})
    {
        const std::string message = refused(root, path, "outside");
        EXPECT_EQ(message.find(_base.string()), std::string::npos) << "no host path: " << message;
    }
    EXPECT_FALSE(fs::exists(_root / "outside.prefab"));
    EXPECT_FALSE(fs::exists(_base / "outside.prefab"));
    EXPECT_FALSE(fs::exists(Assets() / "a"));

    // A folder in the way is never replaced
    fs::create_directories(Assets() / "Dir.prefab");
    refused(root, "res://Dir.prefab", "a folder in the way");
    EXPECT_TRUE(fs::is_directory(Assets() / "Dir.prefab"));
}

TEST_F(EditorPrefabTest, PrefabFilesAreNotWritableAsText)
{
    const std::string root = Create("Root");
    const std::string uuid = CreatePrefab(root, "res://Root.prefab");
    const std::string before = ReadFile(Assets() / "Root.prefab");

    for (const char *path : {"res://Root.prefab", "res://New.prefab", "res://ROOT.PREFAB"})
    {
        const Frame frame = Execute(server, CommandType::WriteTextAsset, Strings({path, "{}"}));
        EXPECT_EQ(frame.type, ErrorType) << path;
        EXPECT_NE(frame.Text().find("Not a text asset"), std::string::npos) << frame.Text();
    }
    EXPECT_EQ(ReadFile(Assets() / "Root.prefab"), before);
    EXPECT_FALSE(fs::exists(Assets() / "New.prefab"));
    EXPECT_EQ(Execute(server, CommandType::ReadTextAsset, Strings({"res://Root.prefab"})).type, ErrorType);
    EXPECT_EQ(AssetAt("res://Root.prefab").at("uuid"), uuid);
}

// ==================== InstantiatePrefab ====================

TEST_F(EditorPrefabTest, InstantiateBuildsACopyWithFreshIdsLinkedToThePrefabAsOneUndoStep)
{
    const std::string root = Create("Root", "", "Cube");
    const std::string kid = Create("Kid", root, "Sphere");
    const std::string uuid = CreatePrefab(root, "res://prefabs/Root.prefab");
    const std::set<std::string> originals{root, kid};
    const uint32_t revision = Revision();

    const std::string instance = Instantiate("res://prefabs/Root.prefab");
    EXPECT_GT(Revision(), revision);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Root", "Kid", "Root (1)", "Kid"})) << "a name a sibling has is numbered on";

    // Fresh UUIDs for every object; the root is linked to the prefab asset, its child is not
    std::vector<std::string> fresh;
    for (const std::string &id : Ids())
    {
        if (!originals.contains(id))
            fresh.push_back(id);
    }
    ASSERT_EQ(fresh.size(), 2u);
    EXPECT_EQ(fresh[0], instance);
    EXPECT_EQ(Node(instance).at("prefab"), uuid);
    EXPECT_FALSE(Node(fresh[1]).contains("prefab"));
    EXPECT_EQ(Node(fresh[1]).at("parentId"), instance);
    EXPECT_EQ(GetEntity(instance).at("header").at("prefab"), uuid);
    EXPECT_EQ(Node(root).at("prefab"), uuid) << "the source keeps its link";
    EXPECT_EQ(GetEntity(instance).at("components").size(), GetEntity(root).at("components").size());

    // The UUID names the same prefab as the path
    const std::string byUuid = Instantiate(uuid);
    EXPECT_NE(byUuid, instance);
    EXPECT_EQ(Node(byUuid).at("name"), "Root (2)");

    // One step each: undo removes the whole instance
    EXPECT_EQ(Step(CommandType::Undo), "Instantiate Root (2)");
    EXPECT_EQ(Names(), (std::vector<std::string>{"Root", "Kid", "Root (1)", "Kid"}));
    EXPECT_EQ(Step(CommandType::Undo), "Instantiate Root (1)");
    EXPECT_EQ(Names(), (std::vector<std::string>{"Root", "Kid"}));
    EXPECT_EQ(Ids(), (std::vector<std::string>{root, kid}));

    // Redo builds the same objects again, with the link
    EXPECT_EQ(Step(CommandType::Redo), "Instantiate Root (1)");
    EXPECT_EQ(Names(), (std::vector<std::string>{"Root", "Kid", "Root (1)", "Kid"}));
    EXPECT_EQ(Ids()[2], instance);
    EXPECT_EQ(Node(instance).at("prefab"), uuid);
}

TEST_F(EditorPrefabTest, InstantiateKeepsReferencesInsideThePrefabAndLeavesOutsideOnesEmpty)
{
    Scene &scene = LoadedScene();
    const auto outsider = GameObject::Create("Outsider");
    const auto root = GameObject::Create("Root");
    const auto inner = GameObject::Create("Inner");
    root->AddChild(inner);
    root->AddComponent<PrefabTestHolder>()->target = inner.get(); // inside the prefab
    inner->AddComponent<PrefabTestHolder>()->target = outsider.get(); // outside it, but in this very scene
    scene.AddRootGameObject(outsider);
    scene.AddRootGameObject(root);
    (void)CreatePrefab(root->GetUUID().ToString(), "res://Holder.prefab");

    const std::string instanceId = Instantiate("res://Holder.prefab");
    const auto instance = scene.FindGameObjectByUUID(Math::UUID::FromString(instanceId).value());
    ASSERT_NE(instance, nullptr);
    ASSERT_EQ(instance->GetChildCount(), 1u);
    const auto copyInner = instance->GetChild(0);
    ASSERT_NE(instance->GetComponent<PrefabTestHolder>(), nullptr);
    ASSERT_NE(copyInner->GetComponent<PrefabTestHolder>(), nullptr);
    EXPECT_EQ(instance->GetComponent<PrefabTestHolder>()->target, copyInner.get()) << "inside: the copy";
    EXPECT_EQ(copyInner->GetComponent<PrefabTestHolder>()->target, nullptr)
        << "outside: left empty, though the scene has the object";
    // The source is as it was
    EXPECT_EQ(root->GetComponent<PrefabTestHolder>()->target, inner.get());
    EXPECT_EQ(inner->GetComponent<PrefabTestHolder>()->target, outsider.get());

    // Undo and redo rebuild the same references
    EXPECT_EQ(Step(CommandType::Undo), "Instantiate Root (1)");
    EXPECT_EQ(Step(CommandType::Redo), "Instantiate Root (1)");
    const auto again = scene.FindGameObjectByUUID(Math::UUID::FromString(instanceId).value());
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->GetComponent<PrefabTestHolder>()->target, again->GetChild(0).get());
    EXPECT_EQ(again->GetChild(0)->GetComponent<PrefabTestHolder>()->target, nullptr);
}

TEST_F(EditorPrefabTest, InstantiatePlacesTheInstanceUnderAParentAtAnIndexAndAtAWorldPosition)
{
    const std::string source = Create("Source");
    (void)CreatePrefab(source, "res://Source.prefab");
    const std::string parent = Create("Parent", "", "Cube");
    (void)Create("X", parent);
    (void)Create("Y", parent);

    BufferWriter local;
    local.WriteString(parent);
    for (const float v : {10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f})
        local.WriteF32(v);
    ASSERT_EQ(Execute(server, CommandType::SetLocalTransform, local.Release()).type, OkType);

    const std::string placed = Instantiate("res://Source.prefab", parent, 1);
    EXPECT_EQ(Node(placed).at("parentId"), parent);
    EXPECT_EQ(Node(placed).at("index"), 1u);
    const std::string last = Instantiate("res://Source.prefab", parent, 99);
    EXPECT_EQ(Node(last).at("index"), 3u) << "past the last place: the last place";

    // A world position, with the parent 10 along x
    const Frame positioned = InstantiateFrame("res://Source.prefab", parent, Last, true, 1.0f, 2.0f, 3.0f);
    ASSERT_EQ(positioned.type, EntityCreatedType) << positioned.Text();
    BufferReader r(positioned.payload);
    const std::string positionedId = r.ReadString();
    const auto object = LoadedScene().FindGameObjectByUUID(Math::UUID::FromString(positionedId).value());
    ASSERT_NE(object, nullptr);
    ASSERT_TRUE(object->HasPositionable());
    const Math::Vector3 world = object->GetPositionable()->GetPosition();
    EXPECT_NEAR(world.x, 1.0f, 1e-4f);
    EXPECT_NEAR(world.y, 2.0f, 1e-4f);
    EXPECT_NEAR(world.z, 3.0f, 1e-4f);
    EXPECT_NEAR(object->GetPositionable()->GetLocalPosition().x, -9.0f, 1e-4f);
    EXPECT_EQ(Step(CommandType::Undo), "Instantiate Source (2)");

    const uint32_t revision = Revision();
    EXPECT_EQ(InstantiateFrame("res://Source.prefab", "00000000-0000-0000-0000-000000000000").type, ErrorType);
    EXPECT_EQ(InstantiateFrame("res://Source.prefab", "nonsense").type, ErrorType);
    EXPECT_EQ(InstantiateFrame("res://Source.prefab", "", Last, true, std::numeric_limits<float>::quiet_NaN()).type, ErrorType);
    EXPECT_EQ(Revision(), revision);
}

TEST_F(EditorPrefabTest, SeveralInstantiationsInAnEditGroupAreOneStep)
{
    const std::string source = Create("Source");
    (void)CreatePrefab(source, "res://Source.prefab");
    ASSERT_EQ(Execute(server, CommandType::BeginEditGroup, Strings({"Two prefabs"})).type, OkType);
    (void)Instantiate("res://Source.prefab");
    (void)Instantiate("res://Source.prefab");
    ASSERT_EQ(Execute(server, CommandType::EndEditGroup).type, OkType);
    EXPECT_EQ(Names().size(), 3u);
    EXPECT_EQ(Step(CommandType::Undo), "Two prefabs");
    EXPECT_EQ(Names(), (std::vector<std::string>{"Source"}));
}

TEST_F(EditorPrefabTest, APrefabFileOfAnotherToolIsAnAssetAndInstantiates)
{
    const std::string text = json{{"name", "Hand"},
                                  {"rootObject", json{{"name", "Hand"},
                                                      {"uuid", Math::UUID::Random().ToString()},
                                                      {"children", json::array({json{{"name", "Finger"}, {"uuid", Math::UUID::Random().ToString()}}})}}}}
                                 .dump(2);
    WriteAsset("hand/Hand.prefab", text);
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);

    const json asset = AssetAt("res://hand/Hand.prefab");
    ASSERT_TRUE(asset.is_object());
    EXPECT_EQ(asset.at("type"), "Prefab");
    EXPECT_EQ(asset.at("uuid"), IO::ResourceUUID::FromPath(IO::ResourcePath("res://hand/Hand.prefab")).ToString());

    const std::string instance = Instantiate(asset.at("uuid"));
    EXPECT_EQ(Names(), (std::vector<std::string>{"Hand", "Finger"}));
    EXPECT_EQ(Node(instance).at("prefab"), asset.at("uuid"));
}

TEST_F(EditorPrefabTest, InstantiateRefusesWhatIsNotAUsablePrefabAndChangesNothing)
{
    (void)Create("Anchor");
    const uint32_t revision = Revision();
    const auto refused = [&](const std::string &uuidOrPath, const char *why)
    {
        const Frame frame = InstantiateFrame(uuidOrPath);
        EXPECT_EQ(frame.type, ErrorType) << why;
        EXPECT_EQ(Revision(), revision) << why << ": nothing changed";
        EXPECT_EQ(Names(), (std::vector<std::string>{"Anchor"})) << why;
        return frame.Text();
    };

    refused("res://missing.prefab", "missing");
    refused("nonsense", "neither a path nor a UUID");
    refused("00000000-0000-0000-0000-000000000000", "unknown UUID");
    refused("res://scenes/Main.scene", "a scene is not a prefab");
    refused("res://../outside.prefab", "outside");
    refused("res://a\\..\\b.prefab", "backslashes");
    const std::string scene = AssetAt("res://scenes/Main.scene").at("uuid");
    EXPECT_NE(refused(scene, "a scene's UUID").find("not a prefab"), std::string::npos);

    // A file that is not a prefab: the message says what is wrong, and does not repeat the file
    WriteAsset("bad/NotJson.prefab", "SECRET not json");
    WriteAsset("bad/NotObject.prefab", "[1,2,3]");
    WriteAsset("bad/NoName.prefab", R"({"rootObject":{"name":"x"}})");
    WriteAsset("bad/NoRoot.prefab", R"({"name":"x"})");
    WriteAsset("bad/NullRoot.prefab", R"({"name":"x","rootObject":null})");
    WriteAsset("bad/BadObject.prefab", R"({"name":"x","rootObject":{"name":"SECRET"}})");
    for (const char *path : {"NotJson", "NotObject", "NoName", "NoRoot", "NullRoot", "BadObject"})
    {
        const std::string message = refused(std::string("res://bad/") + path + ".prefab", path);
        EXPECT_EQ(message.find("SECRET"), std::string::npos) << path << ": " << message;
        EXPECT_EQ(message.find(_base.string()), std::string::npos) << path << ": " << message;
    }

    // A folder that happens to be named like a prefab
    fs::create_directories(Assets() / "Folder.prefab");
    refused("res://Folder.prefab", "a folder");

    EXPECT_EQ(Step(CommandType::Undo), "Create Anchor") << "no step was recorded by any of them";
}

TEST_F(EditorPrefabTest, ADepthCappedPrefabFileIsRefused)
{
    (void)Create("Anchor");
    const uint32_t revision = Revision();

    // Nested JSON far past the cap: refused from the text, before it is parsed
    WriteAsset("deep/Json.prefab", R"({"name":"deep","rootObject":)" + std::string(100000, '[') + std::string(100000, ']') + "}");
    const Frame nested = InstantiateFrame("res://deep/Json.prefab");
    EXPECT_EQ(nested.type, ErrorType);
    EXPECT_NE(nested.Text().find("nests more than"), std::string::npos) << nested.Text();

    // 513 object levels: under the JSON cap, over the hierarchy cap
    WriteAsset("deep/Levels513.prefab", DeepPrefabText(513));
    const Frame tooTall = InstantiateFrame("res://deep/Levels513.prefab");
    EXPECT_EQ(tooTall.type, ErrorType);
    EXPECT_NE(tooTall.Text().find("512"), std::string::npos) << tooTall.Text();
    EXPECT_EQ(Revision(), revision);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Anchor"}));

    // 512 levels fit at the root, but not under a parent (which takes one level)
    WriteAsset("deep/Levels512.prefab", DeepPrefabText(512));
    EXPECT_EQ(InstantiateFrame("res://deep/Levels512.prefab", Ids()[0]).type, ErrorType);
    EXPECT_EQ(Names().size(), 1u);
    EXPECT_EQ(InstantiateFrame("res://deep/Levels512.prefab").type, EntityCreatedType);
    EXPECT_EQ(Names().size(), 513u);
}

TEST_F(EditorPrefabTest, APrefabFileWithAnAssetPathOutsideTheProjectIsRefused)
{
    (void)Create("Anchor");
    const uint32_t revision = Revision();
    for (const char *path : {"res://../outside.png", "res://a\\..\\b", "res://C:/x", "res://a/./b", "res://nul"})
    {
        const json object = {{"name", "x"},
                             {"uuid", Math::UUID::Random().ToString()},
                             {"components", json::array({json{{"type", "CubeRenderer"}, {"data", json{{"texture", path}}}}})}};
        WriteAsset("escape/Escape.prefab", json{{"name", "x"}, {"rootObject", object}}.dump());
        const Frame frame = InstantiateFrame("res://escape/Escape.prefab");
        EXPECT_EQ(frame.type, ErrorType) << path;
        EXPECT_EQ(frame.Text().find(path), std::string::npos) << "the path is not echoed: " << frame.Text();
        EXPECT_EQ(Revision(), revision) << path;
    }
}

// ==================== ApplyPrefab and UnpackPrefab ====================

TEST_F(EditorPrefabTest, ApplyOverwritesThePrefabFileFromTheLinkedObjectAndKeepsItsNames)
{
    const std::string root = Create("Root", "", "Cube");
    (void)Create("Kid", root);
    const std::string uuid = CreatePrefab(root, "res://prefabs/Root.prefab");
    const std::string instance = Instantiate(uuid);
    EXPECT_EQ(Node(instance).at("name"), "Root (1)");

    // Change the instance: a new child, and the root's name
    const std::string extra = Create("Extra", instance);
    const uint32_t revision = Revision();
    ASSERT_EQ(Apply(instance).type, OkType);

    const json file = PrefabFileJson("prefabs/Root.prefab");
    EXPECT_EQ(file.at("name"), "Root") << "the prefab keeps its name";
    EXPECT_EQ(file.at("rootObject").at("name"), "Root") << "and its root's, not the instance's";
    ASSERT_EQ(file.at("rootObject").at("children").size(), 2u);
    EXPECT_EQ(file.at("rootObject").at("children")[1].at("name"), "Extra");
    EXPECT_EQ(file.at("rootObject").at("children")[1].at("uuid"), extra);
    EXPECT_FALSE(file.at("rootObject").contains("prefabReference"));
    EXPECT_EQ(AssetAt("res://prefabs/Root.prefab").at("uuid"), uuid) << "the same asset";
    const auto events = EventsOfKind("assetsChanged");
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().at("modified"), json::array({"res://prefabs/Root.prefab"}));

    // Nothing in the scene changed and no step was recorded; the other objects are not updated
    EXPECT_EQ(Revision(), revision);
    EXPECT_EQ(Step(CommandType::Undo), "Create Extra");
    EXPECT_EQ(Step(CommandType::Redo), "Create Extra");
    EXPECT_EQ(Names(), (std::vector<std::string>{"Root", "Kid", "Root (1)", "Kid", "Extra"}));

    // The next instance has the new child
    const std::string again = Instantiate(uuid);
    EXPECT_EQ(Node(again).at("name"), "Root (2)");
    EXPECT_EQ(Names().size(), 8u);

    // A rename of the linked object alone does not rename the prefab either
    SetName(root, "Renamed");
    ASSERT_EQ(Apply(root).type, OkType);
    EXPECT_EQ(PrefabFileJson("prefabs/Root.prefab").at("name"), "Root");
    EXPECT_EQ(PrefabFileJson("prefabs/Root.prefab").at("rootObject").at("name"), "Root");
}

TEST_F(EditorPrefabTest, ApplyRefusesAnUnlinkedObjectAndAMissingPrefab)
{
    const std::string root = Create("Root");
    const uint32_t revision = Revision();
    const Frame unlinked = Apply(root);
    EXPECT_EQ(unlinked.type, ErrorType);
    EXPECT_NE(unlinked.Text().find("isn't linked"), std::string::npos) << unlinked.Text();
    EXPECT_EQ(Apply("00000000-0000-0000-0000-000000000000").type, ErrorType);
    EXPECT_EQ(Apply("nonsense").type, ErrorType);

    (void)CreatePrefab(root, "res://Root.prefab");
    fs::remove(Assets() / "Root.prefab");
    const Frame gone = Apply(root);
    EXPECT_EQ(gone.type, ErrorType);
    EXPECT_FALSE(fs::exists(Assets() / "Root.prefab")) << "a missing prefab is not written again";
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
    EXPECT_EQ(Apply(root).type, ErrorType) << "after the rescan the asset is gone";
    EXPECT_GT(Revision(), revision);
}

TEST_F(EditorPrefabTest, UnpackClearsTheLinkAsOneUndoStep)
{
    const std::string root = Create("Root");
    const std::string kid = Create("Kid", root);
    const std::string uuid = CreatePrefab(root, "res://Root.prefab");

    ASSERT_EQ(Unpack(root).type, OkType);
    EXPECT_FALSE(Node(root).contains("prefab"));
    EXPECT_FALSE(GetEntity(root).at("header").contains("prefab"));
    EXPECT_TRUE(fs::exists(Assets() / "Root.prefab")) << "the prefab is not touched";
    EXPECT_EQ(Names(), (std::vector<std::string>{"Root", "Kid"})) << "nor the object";
    const auto changed = EventsOfKind("sceneChanged");
    ASSERT_FALSE(changed.empty());
    EXPECT_EQ(changed.back().at("entityIds"), json::array({root}));

    EXPECT_EQ(Step(CommandType::Undo), "Unpack Prefab Root");
    EXPECT_EQ(Node(root).at("prefab"), uuid);
    EXPECT_EQ(Step(CommandType::Redo), "Unpack Prefab Root");
    EXPECT_FALSE(Node(root).contains("prefab"));

    // An object that is not linked, or not there, is an error that records nothing
    const uint32_t revision = Revision();
    EXPECT_EQ(Unpack(root).type, ErrorType);
    EXPECT_EQ(Unpack(kid).type, ErrorType);
    EXPECT_EQ(Unpack("00000000-0000-0000-0000-000000000000").type, ErrorType);
    EXPECT_EQ(Unpack("nonsense").type, ErrorType);
    EXPECT_EQ(Revision(), revision);
    EXPECT_EQ(Step(CommandType::Undo), "Unpack Prefab Root") << "still the last step";
    EXPECT_EQ(Step(CommandType::Redo), "Unpack Prefab Root");

    // After unpacking, Apply has nothing to apply to; a fresh instantiation links again
    EXPECT_EQ(Apply(root).type, ErrorType);
}

TEST_F(EditorPrefabTest, ADuplicateOfALinkedObjectIsLinkedToo)
{
    const std::string root = Create("Root");
    const std::string uuid = CreatePrefab(root, "res://Root.prefab");
    const Frame duplicated = Execute(server, CommandType::DuplicateEntity, Strings({root}));
    ASSERT_EQ(duplicated.type, EntityCreatedType) << duplicated.Text();
    BufferReader r(duplicated.payload);
    EXPECT_EQ(Node(r.ReadString()).at("prefab"), uuid);
}

// ==================== Who may send them ====================

TEST_F(EditorPrefabTest, ViewersAndReadOnlyHostsCannotUseThePrefabCommands)
{
    const auto id = [](CommandType type) { return static_cast<uint8_t>(type); };
    const std::vector<CommandType> commands = {CommandType::CreatePrefab, CommandType::InstantiatePrefab,
                                               CommandType::ApplyPrefab, CommandType::UnpackPrefab};
    for (const CommandType type : commands)
    {
        EXPECT_EQ(EditorServer::ClassifyCommand(id(type)), EditorServer::CommandClass::Write);
        EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, id(type)));
        EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Editor, id(type)));
        EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, id(type)));
    }

    const std::string root = Create("Root");
    const std::string uuid = CreatePrefab(root, "res://Root.prefab");
    const std::string before = ReadFile(Assets() / "Root.prefab");
    ASSERT_TRUE(server.SetReadOnly(true));
    const uint32_t revision = Revision();
    EXPECT_EQ(CreatePrefabFrame(root, "res://Other.prefab").type, ErrorType);
    EXPECT_EQ(CreatePrefabFrame(root, "res://Root.prefab", true).type, ErrorType);
    EXPECT_EQ(InstantiateFrame("res://Root.prefab").type, ErrorType);
    EXPECT_EQ(Apply(root).type, ErrorType);
    EXPECT_EQ(Unpack(root).type, ErrorType);
    EXPECT_FALSE(fs::exists(Assets() / "Other.prefab"));
    EXPECT_EQ(ReadFile(Assets() / "Root.prefab"), before);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Root"}));
    EXPECT_EQ(Node(root).at("prefab"), uuid);
    EXPECT_EQ(Revision(), revision);
}

TEST_F(EditorPrefabTest, ThePrefabCommandsThatWriteFilesAreRefusedByAPlayHost)
{
    for (const CommandType type : {CommandType::CreatePrefab, CommandType::ApplyPrefab})
        EXPECT_TRUE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(type))) << static_cast<int>(type);
    for (const CommandType type : {CommandType::InstantiatePrefab, CommandType::UnpackPrefab})
        EXPECT_FALSE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(type))) << static_cast<int>(type);
}

TEST_F(EditorPrefabTest, HelloAdvertisesPrefabAssets)
{
    const auto capabilities = EditorServer::Capabilities();
    EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), "prefabAssets"), capabilities.end());
}
