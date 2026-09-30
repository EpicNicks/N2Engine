#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaJson.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

class LuaRobustnessTest : public ::testing::Test
{
protected:
    static inline fs::path s_projectRoot;

    static void WriteAsset(const std::string &relativePath, const std::string &source)
    {
        const fs::path path = s_projectRoot / "assets" / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << source;
    }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_robustness_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        // Shorthand field defaults next to the table form
        WriteAsset("robust/Fields.lua", R"(
            local Fields = {}
            Fields.__index = Fields
            Fields.SerializableFields = {
                count = 3,
                ratio = 0.5,
                label = { type = "string", default = "hi" },
                enabled = true,
            }

            function Fields:OnAttach()
                fields_pos = self.pos
            end

            return Fields
        )");

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    Scene *_scene = nullptr;

    void SetUp() override
    {
        SceneManager::AddScene(Scene::Create("LuaRobustness_" + std::string(
                                   ::testing::UnitTest::GetInstance()->current_test_info()->name())), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
    }
};

TEST_F(LuaRobustnessTest, VectorConstantsCannotBeModified)
{
    const float y = Lua().script(R"(
        local v = Vector3.Zero
        v.y = 5
        return Vector3.Zero.y
    )");

    // Vector3.Zero used to be one shared object, so this changed it for every script
    EXPECT_FLOAT_EQ(y, 0.0f);
}

TEST_F(LuaRobustnessTest, LuaToJsonSurvivesSelfReferencingTables)
{
    const sol::table table = Lua().script(R"(
        local t = { name = "loop" }
        t.self = t
        return t
    )");

    // Used to recurse until the stack overflowed
    const nlohmann::json json = LuaToJson(table);

    EXPECT_EQ(json["name"], "loop");
    EXPECT_TRUE(json["self"].is_null());
}

TEST_F(LuaRobustnessTest, CollisionIsReadableFromLua)
{
    const auto self = GameObject::Create("Self");
    const auto other = GameObject::Create("Other");
    Physics::Collision collision;
    collision.gameObject = self.get();
    collision.otherGameObject = other.get();
    Physics::ContactPoint contact;
    contact.point = Math::Vector3(1.0f, 2.0f, 3.0f);
    collision.contacts.push_back(contact);

    const sol::protected_function read = Lua().script(R"(
        return function(collision)
            return collision.otherGameObject:GetName(), collision.contactCount, collision:GetContact(1).point.y,
                   collision:GetContact(2) == nil
        end
    )");
    const sol::protected_function_result result = read(collision);

    // Collision had no Lua type, so any field access was "attempt to index a userdata value"
    if (!result.valid())
    {
        const sol::error error = result;
        FAIL() << error.what();
    }
    EXPECT_EQ(result.get<std::string>(0), "Other");
    EXPECT_EQ(result.get<int>(1), 1);
    EXPECT_FLOAT_EQ(result.get<float>(2), 2.0f);
    EXPECT_TRUE(result.get<bool>(3));
}

TEST_F(LuaRobustnessTest, ShorthandFieldDefaultsAreRead)
{
    const auto go = GameObject::Create("Fields");
    auto *script = go->AddComponent<LuaComponent>();

    // `count = 3` (not a table) used to abort the process when cast to one
    script->SetScript(IO::ResourcePath("res://robust/Fields.lua"));
    ASSERT_FALSE(script->HasMissingScript());

    const nlohmann::json &data = script->GetScriptData();
    ASSERT_TRUE(data.contains("count"));
    EXPECT_TRUE(data["count"].is_number_integer()) << "stored as " << data["count"].dump();
    EXPECT_EQ(data["count"], 3);
    EXPECT_TRUE(data["ratio"].is_number_float());
    EXPECT_DOUBLE_EQ(data["ratio"].get<double>(), 0.5);
    EXPECT_EQ(data["label"], "hi");
    EXPECT_EQ(data["enabled"], true);
}

TEST_F(LuaRobustnessTest, PartialVectorFieldDefaultsMissingAxesToZero)
{
    const auto go = GameObject::Create("Fields");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScriptData({{"pos", {{"x", 1.0}}}}); // no y or z: used to throw out of loading

    script->SetScript(IO::ResourcePath("res://robust/Fields.lua"));
    _scene->AddRootGameObject(go);
    _scene->ProcessAttachQueue();

    const sol::object pos = Lua()["fields_pos"];
    ASSERT_TRUE(pos.is<Math::Vector3>());
    const auto vec = pos.as<Math::Vector3>();
    EXPECT_FLOAT_EQ(vec.x, 1.0f);
    EXPECT_FLOAT_EQ(vec.y, 0.0f);
    EXPECT_FLOAT_EQ(vec.z, 0.0f);
}

TEST_F(LuaRobustnessTest, ScriptableComponentNamesAreReferenceTypes)
{
    const auto go = GameObject::Create("Fields");
    auto *script = go->AddComponent<LuaComponent>();

    // `type = "Rigidbody"` fields used to be skipped: only names ending in "Component" counted
    EXPECT_TRUE(script->IsComponentType("Rigidbody"));
    EXPECT_TRUE(script->IsComponentType("BoxCollider"));
    EXPECT_TRUE(script->IsComponentType("LuaComponent"));
    EXPECT_FALSE(script->IsComponentType("string"));
}
