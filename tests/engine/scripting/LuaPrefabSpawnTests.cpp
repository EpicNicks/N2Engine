#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <math/UUID.hpp>
#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/n2net/SimTransport.hpp"
#include "engine/prefabs/Prefab.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaFieldDeclaration.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/serialization/FieldInfo.hpp"

// Prefab references from Lua (editor protocol 1.25.0, orchestrator/proposals/prefab-references.md): the
// `type = "Prefab"` field, the Prefab handle, handle:Instantiate and Prefab.Instantiate, GameObject:GetPrefab, against
// .prefab files in a temporary project

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
    constexpr const char *EnemyPath = "res://prefabs/enemy.prefab";
    constexpr const char *ScriptPath = "res://scripts/Spawner.lua";

    /// Collects every log line while it lives
    class LogCapture
    {
    public:
        LogCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, Logger::LogLevel)
            { lines.emplace_back(message); };
        }
        ~LogCapture() { Logger::logEvent -= _id; }
        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        [[nodiscard]] std::size_t Count(const std::string_view text) const
        {
            std::size_t count = 0;
            for (const std::string &line : lines)
            {
                count += line.find(text) != std::string::npos ? 1 : 0;
            }
            return count;
        }

        std::vector<std::string> lines;

    private:
        std::size_t _id = 0;
    };
}

class LuaPrefabSpawnTest : public ::testing::Test
{
protected:
    static inline fs::path s_root;

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static void SetUpTestSuite() { ASSERT_TRUE(LuaRuntime::Instance().Initialize()); }

    static void WritePrefab(const std::string &relative, const GameObject::Ptr &root)
    {
        const fs::path path = s_root / "assets" / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << Prefab(root->GetName(), root).Serialize().dump();
    }

    static void WriteText(const std::string &relative, const std::string &text)
    {
        const fs::path path = s_root / "assets" / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    void SetUp() override
    {
        s_root = fs::temp_directory_path() /
                 (std::string("n2engine_lua_prefab_spawn_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(s_root, ec);

        // "Enemy" (local position 1, 2, 3) with a child "Eye"
        const GameObject::Ptr enemy = GameObject::Create("Enemy");
        enemy->CreatePositionable();
        enemy->GetPositionable()->SetLocalPosition(Math::Vector3{1.0f, 2.0f, 3.0f});
        enemy->AddChild(GameObject::Create("Eye"), false);
        WritePrefab("prefabs/enemy.prefab", enemy);

        // A networked prefab: a NetworkObject on its root
        const GameObject::Ptr networked = GameObject::Create("Networked");
        networked->AddComponent<Network::NetworkObject>();
        WritePrefab("prefabs/networked.prefab", networked);

        WriteText("scripts/Spawner.lua", R"(
            local Spawner = {}
            Spawner.__index = Spawner
            Spawner.SerializableFields = {
                enemy = { type = "Prefab" },
                withDefault = { type = "Prefab", default = "res://prefabs/enemy.prefab" },
                count = 3,
            }
            function Spawner:OnUpdate()
                prefabtest_enemy = self.enemy
                prefabtest_default = self.withDefault
            end
            return Spawner
        )");
        WriteText("scripts/Holder.lua", R"(
            local Holder = {}
            Holder.__index = Holder
            Holder.SerializableFields = { target = { type = "GameObject" } }
            function Holder:OnUpdate() prefabtest_target = self.target end
            return Holder
        )");

        IO::ResourceUUID::Initialize(Math::UUID::GenerateNameBased(Math::UUID::ZERO, "LuaPrefabSpawnTest"));
        IO::ResourceLoader::Instance().ClearCache();
        IO::ResourceLoader::Instance().Initialize(s_root);

        SceneManager::AddScene(Scene::Create("LuaPrefabSpawnTest"), true);
        SceneManager::ProcessAnyPendingSceneChange();
    }

    void TearDown() override
    {
        Run("prefabtest_enemy = nil; prefabtest_default = nil; prefabtest_target = nil; prefabtest_go = nil; "
            "prefabtest_parent = nil; prefabtest_other = nil");
        SceneManager::AddScene(Scene::Create("LuaPrefabSpawnTest_Empty"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        Lua().collect_garbage();
        IO::ResourceLoader::Instance().ClearCache();
        std::error_code ec;
        fs::remove_all(s_root, ec);
    }

    static std::string Uuid(const std::string &path)
    {
        return IO::ResourceLoader::Instance().GetUUID(IO::ResourcePath(path)).ToString();
    }

    static void Run(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            FAIL() << "Lua error: " << err.what() << "\nin: " << code;
        }
    }

    static std::string RunExpectingError(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid())
        {
            ADD_FAILURE() << "expected an error from: " << code;
            return {};
        }
        const sol::error err = result;
        return err.what();
    }

    template <typename T>
    static T Eval(const std::string &expression)
    {
        const sol::protected_function_result result =
            Lua().safe_script("return " + expression, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << expression;
            return T{};
        }
        return result.get<T>();
    }

    /// An object in the scene with a LuaComponent running Spawner.lua with this scriptData
    static LuaComponent *Spawner(const json &scriptData)
    {
        const GameObject::Ptr holder = GameObject::Create("Spawner");
        SceneManager::GetCurSceneRef().AddRootGameObject(holder);
        auto *component = holder->AddComponent<LuaComponent>();
        component->SetScriptData(scriptData);
        component->SetScript(IO::ResourcePath(ScriptPath));
        component->OnUpdate();
        return component;
    }

    static std::size_t RootCount() { return SceneManager::GetCurSceneRef().GetRootGameObjects().size(); }
};

// ==================== the field ====================

TEST_F(LuaPrefabSpawnTest, APrefabDeclarationIsAPrefabRefField)
{
    const FieldInfo info = MakeLuaFieldInfo("enemy", "Prefab", LuaDefaultKind::Other);
    EXPECT_EQ(info.kind, FieldKind::PrefabRef);
    EXPECT_EQ(info.typeName, "Prefab");
    EXPECT_EQ(info.assetType, "Prefab");
    EXPECT_EQ(info.container, "scriptData");
    EXPECT_FALSE(info.replicated);
    const json schema = info.ToJson();
    EXPECT_EQ(schema.at("kind"), "PrefabRef");
    EXPECT_FALSE(schema.contains("assetPath")) << "only GetLuaFields fills it in";

    // A plain UUID string (even inside the container), "" or null; never the {"$ref"} form
    EXPECT_FALSE(ValidateFieldValue(info, Uuid(EnemyPath)).has_value());
    EXPECT_FALSE(ValidateFieldValue(info, "").has_value());
    EXPECT_FALSE(ValidateFieldValue(info, nullptr).has_value());
    EXPECT_TRUE(ValidateFieldValue(info, json{{"$ref", Uuid(EnemyPath)}}).has_value());
    EXPECT_TRUE(ValidateFieldValue(info, "res://prefabs/enemy.prefab").has_value());
    EXPECT_TRUE(ValidateFieldValue(info, 5).has_value());
}

TEST_F(LuaPrefabSpawnTest, TheFieldHoldsAHandleOfTheStoredUuidAndHasNoDefault)
{
    LuaComponent *spawner = Spawner(json{{"enemy", Uuid(EnemyPath)}});

    EXPECT_TRUE(Eval<bool>("prefabtest_enemy ~= nil"));
    EXPECT_TRUE(Eval<bool>("prefabtest_enemy.valid"));
    EXPECT_EQ(Eval<std::string>("prefabtest_enemy.uuid"), Uuid(EnemyPath));
    EXPECT_EQ(Eval<std::string>("prefabtest_enemy.path"), EnemyPath);
    EXPECT_EQ(Eval<std::string>("prefabtest_enemy.name"), "enemy");
    EXPECT_EQ(Eval<std::string>("tostring(prefabtest_enemy)"), std::string("Prefab(") + EnemyPath + ")");
    EXPECT_TRUE(Eval<bool>("prefabtest_default == nil")) << "a Prefab field's default is ignored";

    // Saved as the plain UUID string; nothing is stored for the field without a value
    const json saved = spawner->Serialize().at("scriptData");
    EXPECT_EQ(saved.at("enemy"), Uuid(EnemyPath));
    EXPECT_FALSE(saved.contains("withDefault"));
    EXPECT_EQ(saved.at("count"), 3);
}

TEST_F(LuaPrefabSpawnTest, AnEmptyValueIsNilAndAStringThatIsNotAUuidIsKeptAndReadsAsNil)
{
    Spawner(json{{"enemy", ""}});
    EXPECT_TRUE(Eval<bool>("prefabtest_enemy == nil"));

    LogCapture log;
    LuaComponent *broken = Spawner(json{{"enemy", "res://prefabs/enemy.prefab"}});
    EXPECT_TRUE(Eval<bool>("prefabtest_enemy == nil"));
    EXPECT_GE(log.Count("is not a prefab UUID"), 1u);
    EXPECT_EQ(broken->Serialize().at("scriptData").at("enemy"), "res://prefabs/enemy.prefab") << "a hand edit is kept";
}

TEST_F(LuaPrefabSpawnTest, AHandleOfAnUnknownUuidIsInvalidAndFailsAtUse)
{
    const std::string unknown = Math::UUID::Random().ToString();
    Spawner(json{{"enemy", unknown}});
    EXPECT_TRUE(Eval<bool>("prefabtest_enemy ~= nil")) << "loading is deferred to the first use";
    EXPECT_FALSE(Eval<bool>("prefabtest_enemy.valid"));
    EXPECT_EQ(Eval<std::string>("prefabtest_enemy.path"), "");
    const std::size_t before = RootCount();
    const std::string error = RunExpectingError("prefabtest_enemy:Instantiate()");
    EXPECT_NE(error.find("no prefab '" + unknown + "'"), std::string::npos) << error;
    EXPECT_EQ(RootCount(), before);
}

// ==================== Instantiate ====================

TEST_F(LuaPrefabSpawnTest, InstantiateWithoutAParentAddsARootOfTheSceneLinkedToThePrefab)
{
    Spawner(json{{"enemy", Uuid(EnemyPath)}});
    const std::size_t before = RootCount();
    Run("prefabtest_go = prefabtest_enemy:Instantiate()");

    EXPECT_EQ(RootCount(), before + 1);
    EXPECT_EQ(Eval<std::string>("prefabtest_go:GetName()"), "Enemy");
    EXPECT_EQ(Eval<std::string>("prefabtest_go:FindChild('Eye'):GetName()"), "Eye");
    EXPECT_TRUE(Eval<bool>("prefabtest_go:GetParent() == nil"));
    // The prefab root's own saved transform when no position is given
    EXPECT_FLOAT_EQ(Eval<float>("prefabtest_go:GetPositionable():GetPosition().y"), 2.0f);
    // The instance carries its origin
    EXPECT_TRUE(Eval<bool>("prefabtest_go:GetPrefab() == prefabtest_enemy"));
    EXPECT_EQ(Eval<std::string>("prefabtest_go:GetPrefab().path"), EnemyPath);
    EXPECT_TRUE(Eval<bool>("prefabtest_go:FindChild('Eye'):GetPrefab() == nil")) << "only the root is linked";

    // The scene owns it: nothing is lost when the script drops the handle
    Run("prefabtest_go = nil");
    Lua().collect_garbage();
    EXPECT_EQ(RootCount(), before + 1);
}

TEST_F(LuaPrefabSpawnTest, InstantiateUnderAParentAtAWorldPositionAndRotation)
{
    Spawner(json{{"enemy", Uuid(EnemyPath)}});
    const std::size_t before = RootCount();
    Run(R"(
        prefabtest_parent = GameObject.Create("Parent")
        prefabtest_parent:CreatePositionable()
        prefabtest_parent:GetPositionable():SetPosition(Vector3(10, 0, 0))
        SceneManager.GetCurrentScene():AddRootGameObject(prefabtest_parent)
        prefabtest_go = prefabtest_enemy:Instantiate(prefabtest_parent, Vector3(1, 1, 1), Quaternion(0.70710678, 0, 0.70710678, 0))
    )");
    EXPECT_EQ(RootCount(), before + 1) << "only the parent is a root";
    EXPECT_TRUE(Eval<bool>("prefabtest_go:GetParent() == prefabtest_parent"));
    EXPECT_NEAR(Eval<float>("prefabtest_go:GetPositionable():GetPosition().x"), 1.0f, 1e-4f) << "a world position";
    const GameObject::Ptr parent = SceneManager::GetCurSceneRef().FindGameObject("Parent");
    ASSERT_NE(parent, nullptr);
    EXPECT_NEAR(parent->GetChild(0)->GetPositionable()->GetLocalPosition().x, -9.0f, 1e-4f);
    EXPECT_NEAR(Eval<float>("prefabtest_go:GetPositionable():GetRotation().y"), 0.70710678f, 1e-4f);

    // nil skips an argument: a position in the scene root
    Run("prefabtest_other = prefabtest_enemy:Instantiate(nil, Vector3(0, 5, 0))");
    EXPECT_TRUE(Eval<bool>("prefabtest_other:GetParent() == nil"));
    EXPECT_NEAR(Eval<float>("prefabtest_other:GetPositionable():GetPosition().y"), 5.0f, 1e-4f);
}

TEST_F(LuaPrefabSpawnTest, TwoInstancesGetFreshUuids)
{
    Run(std::string("prefabtest_go = Prefab.Instantiate('") + EnemyPath + "')");
    Run(std::string("prefabtest_other = Prefab.Instantiate('") + Uuid(EnemyPath) + "')");
    const std::string first = Eval<std::string>("prefabtest_go:GetName()");
    EXPECT_EQ(first, "Enemy");
    EXPECT_FALSE(Eval<bool>("prefabtest_go == prefabtest_other"));
    EXPECT_TRUE(Eval<bool>("prefabtest_go:GetPrefab() == prefabtest_other:GetPrefab()"));

    std::vector<Math::UUID> ids;
    for (const GameObject::Ptr &root : SceneManager::GetCurSceneRef().GetRootGameObjects())
    {
        if (root->GetName() == "Enemy")
        {
            ids.push_back(root->GetUUID());
            ids.push_back(root->GetChild(0)->GetUUID());
        }
    }
    ASSERT_EQ(ids.size(), 4u);
    EXPECT_NE(ids[0], ids[2]);
    EXPECT_NE(ids[1], ids[3]);
}

TEST_F(LuaPrefabSpawnTest, AMissingAssetOrOneThatIsNotAPrefabIsACatchableError)
{
    const std::size_t before = RootCount();
    const std::string missing = RunExpectingError("Prefab.Instantiate('res://prefabs/missing.prefab')");
    EXPECT_NE(missing.find("no prefab 'res://prefabs/missing.prefab'"), std::string::npos) << missing;
    const std::string wrongType = RunExpectingError(std::string("Prefab.Instantiate('") + ScriptPath + "')");
    EXPECT_NE(wrongType.find("is a LuaScript, not a Prefab"), std::string::npos) << wrongType;
    const std::string badArgument = RunExpectingError(std::string("Prefab.Instantiate('") + EnemyPath + "', 5)");
    EXPECT_NE(badArgument.find("argument 2 must be a GameObject"), std::string::npos) << badArgument;
    EXPECT_FALSE(Eval<bool>("pcall(Prefab.Instantiate, 'res://prefabs/missing.prefab')"));
    EXPECT_EQ(RootCount(), before) << "nothing was added";
}

TEST_F(LuaPrefabSpawnTest, ReferencesOutsideThePrefabAreClearedWithOneWarningPerPrefab)
{
    // A holder whose GameObject field points at an object that is not part of the prefab
    const GameObject::Ptr holder = GameObject::Create("Holder");
    auto *component = holder->AddComponent<LuaComponent>();
    component->SetScriptData(json{{"target", {{"$ref", Math::UUID::Random().ToString()}}}});
    component->SetScript(IO::ResourcePath("res://scripts/Holder.lua"));
    WritePrefab("prefabs/holder.prefab", holder);
    IO::ResourceLoader::Instance().Initialize(s_root);

    LogCapture log;
    Run("prefabtest_go = Prefab.Instantiate('res://prefabs/holder.prefab')");
    Run("prefabtest_other = Prefab.Instantiate('res://prefabs/holder.prefab')");
    EXPECT_EQ(log.Count("were cleared"), 1u) << "once per prefab, not per spawn";

    SceneManager::GetCurSceneRef().FindGameObject("Holder")->GetComponent<LuaComponent>()->OnUpdate();
    EXPECT_TRUE(Eval<bool>("prefabtest_target == nil"));
}

TEST_F(LuaPrefabSpawnTest, ANetworkObjectPrefabIsRefusedWhileASessionRunsAndSpawnsWithoutOne)
{
    const std::size_t before = RootCount();
    {
        N2Net::SimNetwork network(1);
        Network::NetworkWorld world;
        Network::NetworkServer server(network.AddEndpoint(), world, Network::NetworkServerOptions{});
        const std::string error = RunExpectingError("Prefab.Instantiate('res://prefabs/networked.prefab')");
        EXPECT_NE(error.find("has a NetworkObject: use Network.Spawn"), std::string::npos) << error;
        EXPECT_EQ(RootCount(), before) << "nothing was added";
        // A prefab without one is a local spawn, allowed during a session
        Run(std::string("prefabtest_go = Prefab.Instantiate('") + EnemyPath + "')");
        EXPECT_EQ(RootCount(), before + 1);
    }
    Run("prefabtest_other = Prefab.Instantiate('res://prefabs/networked.prefab')");
    EXPECT_EQ(RootCount(), before + 2);
}

TEST_F(LuaPrefabSpawnTest, APrefabOverTheLimitsIsRefused)
{
    // 513 levels: one more than a scene may have
    std::string deep = R"({"name":"deep","rootObject":)";
    constexpr std::size_t Levels = 513;
    for (std::size_t i = 0; i < Levels; ++i)
    {
        deep += R"({"name":"n","uuid":")" + Math::UUID::Random().ToString() + "\"";
        deep += i + 1 < Levels ? R"(,"children":[)" : "}";
    }
    for (std::size_t i = 1; i < Levels; ++i)
        deep += "]}";
    deep += "}";
    WriteText("prefabs/deep.prefab", deep);

    // 10001 objects: one more than a spawn builds
    std::string wide = R"({"name":"wide","rootObject":{"name":"root","children":[)";
    for (std::size_t i = 0; i < 10000; ++i)
        wide += std::string(i == 0 ? "" : ",") + R"({"name":"c"})";
    wide += "]}}";
    WriteText("prefabs/wide.prefab", wide);
    IO::ResourceLoader::Instance().Initialize(s_root);

    const std::size_t before = RootCount();
    const std::string deepError = RunExpectingError("Prefab.Instantiate('res://prefabs/deep.prefab')");
    EXPECT_NE(deepError.find("would nest deeper"), std::string::npos) << deepError;
    const std::string wideError = RunExpectingError("Prefab.Instantiate('res://prefabs/wide.prefab')");
    EXPECT_NE(wideError.find("more than 10000 objects"), std::string::npos) << wideError;
    EXPECT_EQ(RootCount(), before);
}
