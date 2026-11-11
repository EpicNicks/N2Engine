#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

// One script calling another: LuaComponent:GetScript and GameObject:GetScript return the running instance table
class LuaScriptAccessTest : public ::testing::Test
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

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_script_access_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        WriteAsset("access/Health.lua", R"(
            local Health = { SerializableFields = { hp = 10 } }
            Health.__index = Health

            function Health:Damage(amount)
                self.hp = self.hp - amount
                return self.hp
            end

            return Health
        )");

        WriteAsset("access/Tag.lua", R"(
            local Tag = {}
            Tag.__index = Tag
            Tag.label = "tag"
            return Tag
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
            ADD_FAILURE() << "expected a Lua error from: " << code;
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

    static bool Contains(const std::string &text, const std::string &part)
    {
        return text.find(part) != std::string::npos;
    }

    Scene *_scene = nullptr;

    void SetUp() override
    {
        SceneManager::AddScene(Scene::Create("LuaScriptAccess_" + std::string(
                                   ::testing::UnitTest::GetInstance()->current_test_info()->name())), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
    }

    void TearDown() override
    {
        for (const char *name : {"access_go", "access_plain", "access_health", "access_first", "access_component"})
        {
            Lua()[name] = sol::lua_nil;
        }
    }

    /// A scene object running Health.lua, then Tag.lua
    GameObject::Ptr SpawnWithScripts(const std::string &name)
    {
        const auto go = GameObject::Create(name);
        go->AddComponent<LuaComponent>()->SetScript(IO::ResourcePath("res://access/Health.lua"));
        go->AddComponent<LuaComponent>()->SetScript(IO::ResourcePath("res://access/Tag.lua"));
        _scene->AddRootGameObject(go);
        _scene->ProcessAttachQueue();
        return go;
    }
};

TEST_F(LuaScriptAccessTest, AnotherScriptsFunctionsCanBeCalled)
{
    const auto go = SpawnWithScripts("Enemy");
    Lua()["access_go"] = GameObjectRef(go);

    // By path: the instance is the script's self, with its fields and methods
    EXPECT_EQ(Eval<double>("access_go:GetScript('res://access/Health.lua'):Damage(3)"), 7.0);
    EXPECT_EQ(Eval<double>("access_go:GetScript('res://access/Health.lua').hp"), 7.0);
    EXPECT_EQ(Eval<std::string>("access_go:GetScript('res://access/Tag.lua').label"), "tag");

    // Without a path: the first running script
    EXPECT_TRUE(Eval<bool>("access_go:GetScript() == access_go:GetScript('res://access/Health.lua')"));

    // The same table every time, so it can key a table
    Run("access_health = access_go:GetScript('res://access/Health.lua')");
    EXPECT_TRUE(Eval<bool>("access_go:GetScript('res://access/Health.lua') == access_health"));
    EXPECT_EQ(Eval<std::string>("access_health.gameObject:GetName()"), "Enemy");

    // Through the component handle too
    Run("access_component = access_go:GetComponent('LuaComponent')");
    EXPECT_TRUE(Eval<bool>("access_component:GetScript() == access_health"));
}

TEST_F(LuaScriptAccessTest, NoMatchingScriptIsNil)
{
    const auto go = SpawnWithScripts("Enemy");
    Lua()["access_go"] = GameObjectRef(go);
    EXPECT_TRUE(Eval<bool>("access_go:GetScript('res://access/Missing.lua') == nil"));

    const auto plain = GameObject::Create("Plain");
    _scene->AddRootGameObject(plain);
    Lua()["access_plain"] = GameObjectRef(plain);
    EXPECT_TRUE(Eval<bool>("access_plain:GetScript() == nil"));

    // A component whose script failed to load has no instance
    const auto broken = GameObject::Create("Broken");
    auto *component = broken->AddComponent<LuaComponent>();
    component->SetScript(IO::ResourcePath("res://access/DoesNotExist.lua"));
    _scene->AddRootGameObject(broken);
    Lua()["access_plain"] = GameObjectRef(broken);
    EXPECT_TRUE(Eval<bool>("access_plain:GetScript() == nil"));
    EXPECT_TRUE(Eval<bool>("access_plain:GetComponent('LuaComponent'):GetScript() == nil"));
}

TEST_F(LuaScriptAccessTest, AKeptInstanceIsCutOffWhenItsObjectIsDestroyed)
{
    const auto go = SpawnWithScripts("Enemy");
    Lua()["access_go"] = GameObjectRef(go);
    Run("access_health = access_go:GetScript('res://access/Health.lua')");
    Run("access_component = access_go:GetComponent('LuaComponent')");

    go->Destroy();
    _scene->ProcessDestroyed();

    // The table survives as a plain table, but no longer reaches the object (as ReleaseScript does for self)
    EXPECT_TRUE(Eval<bool>("access_health.gameObject == nil"));
    EXPECT_TRUE(Eval<bool>("access_health.component == nil"));
    EXPECT_FALSE(Eval<bool>("access_go:IsValid()"));
    const std::string error = RunExpectingError("access_component:GetScript()");
    EXPECT_TRUE(Contains(error, "destroyed")) << error;
}

TEST(LuaAudioBindingTest, AMissingClipIsALuaErrorNamingThePath)
{
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    const sol::protected_function_result result = LuaRuntime::Instance().GetState().safe_script(
        "return Audio.PlayOneShot('res://no/such/clip.ogg')", sol::script_pass_on_error);
    ASSERT_FALSE(result.valid());
    const sol::error err = result;
    EXPECT_NE(std::string(err.what()).find("res://no/such/clip.ogg"), std::string::npos) << err.what();
}
