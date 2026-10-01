#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <math/UUID.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp" // ActionMap.hpp only forward-declares it
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;

// Scripts keep references (self.component, self.gameObject, input subscriptions) that must not
// outlive their component. These drive an InputAction directly, so no window is needed.
class LuaLifetimeTest : public ::testing::Test
{
protected:
    static inline fs::path s_projectRoot;

    static void WriteAsset(const std::string &relativePath, const std::string &source)
    {
        const fs::path path = s_projectRoot / "assets" / relativePath;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << source;
    }

    // Reports its fields when updated; the second version adds a field
    static std::string FieldsSource(const bool withAddedField)
    {
        return std::string(R"(
            local Fields = {}
            Fields.__index = Fields
            Fields.SerializableFields = {
                speed = { default = 1 },
                target = { type = "GameObject" },
            )") + (withAddedField ? "added = { default = 7 }," : "") + R"(
            }
            function Fields:OnUpdate()
                fields_speed = self.speed
                fields_added = self.added
                fields_target_valid = self.target ~= nil and self.target:IsValid()
            end
            return Fields
        )";
    }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_lifetime_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        // Subscribes in OnAttach and records its teardown, like the shipped CameraController
        WriteAsset("lifetime/Subscriber.lua", R"(
            local Subscriber = {}
            Subscriber.__index = Subscriber

            function Subscriber:OnAttach()
                lifetime_attaches = lifetime_attaches + 1
                lifetime_action:Subscribe(function(action)
                    lifetime_hits = lifetime_hits + 1
                    self.gameObject:SetName("hit")
                end)
            end

            function Subscriber:OnDisable()
                table.insert(lifetime_events, "disable")
            end

            function Subscriber:OnDestroy()
                table.insert(lifetime_events, "destroy")
                lifetime_self = self
            end

            return Subscriber
        )");

        // Counts how often the script body runs (initial load, module reload, per-component reload)
        WriteAsset("lifetime/Counted.lua", R"(
            reload_loads = (reload_loads or 0) + 1
            local Counted = {}
            Counted.__index = Counted
            return Counted
        )");

        WriteAsset("lifetime/Fields.lua", FieldsSource(false));

        WriteAsset("lifetime/Quitter.lua", R"(
            local Quitter = {}
            Quitter.__index = Quitter
            function Quitter:OnApplicationQuit()
                quit_calls = quit_calls + 1
            end
            return Quitter
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

    Input::InputAction _action{"LifetimeTest"};
    Scene *_scene = nullptr;

    void SetUp() override
    {
        Lua()["lifetime_action"] = InputActionRef(_action);
        Lua()["lifetime_hits"] = 0;
        Lua()["lifetime_attaches"] = 0;
        Lua()["lifetime_events"] = Lua().create_table();
        Lua()["lifetime_self"] = sol::lua_nil;

        SceneManager::AddScene(Scene::Create("LuaLifetime_" + std::string(
                                   ::testing::UnitTest::GetInstance()->current_test_info()->name())), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
    }

    void TearDown() override
    {
        Lua()["lifetime_action"] = sol::lua_nil;
        Lua()["lifetime_self"] = sol::lua_nil;
    }

    void Fire() { _action.GetOnStateChanged()(_action); }

    int Hits() { return Lua()["lifetime_hits"].get<int>(); }

    GameObject::Ptr SpawnSubscriber()
    {
        const auto go = GameObject::Create("Subscriber");
        auto *script = go->AddComponent<LuaComponent>();
        script->SetScript(IO::ResourcePath("res://lifetime/Subscriber.lua"));
        EXPECT_FALSE(script->HasMissingScript());
        _scene->AddRootGameObject(go);
        _scene->ProcessAttachQueue(); // OnAttach subscribes
        return go;
    }
};

TEST_F(LuaLifetimeTest, SubscriptionWorksWhileComponentLives)
{
    const auto go = SpawnSubscriber();

    Fire();

    EXPECT_EQ(Hits(), 1);
    EXPECT_EQ(go->GetName(), "hit");
}

TEST_F(LuaLifetimeTest, SubscriptionStopsAfterDestroy)
{
    const auto go = SpawnSubscriber();
    Fire();
    ASSERT_EQ(Hits(), 1);

    go->Destroy();
    _scene->ProcessDestroyed();
    Fire(); // used to run the handler against the destroyed component

    EXPECT_EQ(Hits(), 1);
}

TEST_F(LuaLifetimeTest, SubscriptionStopsAfterSceneSwitch)
{
    SpawnSubscriber();
    Fire();
    ASSERT_EQ(Hits(), 1);

    SceneManager::AddScene(Scene::Create("LuaLifetime_SwitchTarget"), true);
    SceneManager::ProcessAnyPendingSceneChange(); // frees the subscriber's component

    Fire(); // used to run with a freed self.component / self.gameObject
    EXPECT_EQ(Hits(), 1);
}

TEST_F(LuaLifetimeTest, ScriptSeesOnDisableThenOnDestroy)
{
    const auto go = SpawnSubscriber();

    go->Destroy();
    _scene->ProcessDestroyed();

    const sol::table events = Lua()["lifetime_events"];
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events.get<std::string>(1), "disable");
    EXPECT_EQ(events.get<std::string>(2), "destroy");
}

TEST_F(LuaLifetimeTest, DestroyedScriptLosesComponentReferences)
{
    const auto go = SpawnSubscriber();

    go->Destroy();
    _scene->ProcessDestroyed();

    // The script kept `self` (so could any closure); its engine references must be gone
    const sol::table self = Lua()["lifetime_self"];
    ASSERT_TRUE(self.valid());
    EXPECT_FALSE(self["component"].valid());
    EXPECT_FALSE(self["gameObject"].valid());
}

TEST_F(LuaLifetimeTest, SubscriptionsOutsideComponentsKeepWorking)
{
    // A scene setup script's subscription has no owning component, so nothing ever cancels it
    Lua()["free_hits"] = 0;
    Lua().script("lifetime_action:Subscribe(function() free_hits = free_hits + 1 end)");

    const auto go = SpawnSubscriber();
    go->Destroy();
    _scene->ProcessDestroyed();
    Fire();

    EXPECT_EQ(Lua()["free_hits"].get<int>(), 1);
    EXPECT_EQ(Hits(), 0);
}

TEST_F(LuaLifetimeTest, ReloadCallbacksAreReplacedAndRemoved)
{
    const IO::ResourcePath path("res://lifetime/Counted.lua");
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(path);
    ASSERT_NE(script, nullptr);
    Lua()["reload_loads"] = 0;

    const auto go = GameObject::Create("Reloaded");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScript(path);
    component->SetScript(path); // must replace, not add, its reload callback
    ASSERT_EQ(Lua()["reload_loads"].get<int>(), 2);

    // Module body once + this component's single reload callback once
    LuaRuntime::Instance().ReloadModule(path, script.get());
    EXPECT_EQ(Lua()["reload_loads"].get<int>(), 4);

    // After the component is gone only the module body runs (and nothing touches the freed component)
    go->RemoveComponent<LuaComponent>();
    LuaRuntime::Instance().ReloadModule(path, script.get());
    EXPECT_EQ(Lua()["reload_loads"].get<int>(), 5);
}

TEST_F(LuaLifetimeTest, ReloadRetiresTheOldInstanceAndAttachesTheNewOne)
{
    const IO::ResourcePath path("res://lifetime/Subscriber.lua");
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(path);
    ASSERT_NE(script, nullptr);
    SpawnSubscriber();
    Fire();
    ASSERT_EQ(Hits(), 1);

    LuaRuntime::Instance().ReloadModule(path, script.get());

    // The old instance is torn down like a removed component's script, and cut off
    const sol::table events = Lua()["lifetime_events"];
    ASSERT_EQ(events.size(), 2u) << "the old instance got no teardown";
    EXPECT_EQ(events.get<std::string>(1), "disable");
    EXPECT_EQ(events.get<std::string>(2), "destroy");
    const sol::table oldSelf = Lua()["lifetime_self"];
    ASSERT_TRUE(oldSelf.valid());
    EXPECT_FALSE(oldSelf["component"].valid());

    // The new instance is attached (and subscribes); the old subscription no longer fires
    EXPECT_EQ(Lua()["lifetime_attaches"].get<int>(), 2) << "the reloaded instance never got OnAttach";
    Fire();
    EXPECT_EQ(Hits(), 2) << "one hit per Fire: only the new instance's subscription";
}

TEST_F(LuaLifetimeTest, SettingAnotherScriptRetiresTheFirstInstance)
{
    const auto go = SpawnSubscriber();

    go->GetComponent<LuaComponent>()->SetScript(IO::ResourcePath("res://lifetime/Counted.lua"));
    Fire();

    EXPECT_EQ(Hits(), 0) << "the replaced script's subscription kept firing";
    const sol::table events = Lua()["lifetime_events"];
    EXPECT_EQ(events.size(), 2u) << "the replaced script got no OnDisable/OnDestroy";
}

TEST_F(LuaLifetimeTest, ReloadKeepsSavedFieldsAndReferencesAndDefaultsNewOnes)
{
    const IO::ResourcePath path("res://lifetime/Fields.lua");
    const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(path);
    ASSERT_NE(script, nullptr);

    const auto target = GameObject::Create("Target");
    const auto go = GameObject::Create("Fielded");
    auto *component = go->AddComponent<LuaComponent>();
    component->SetScriptData({{"speed", 5}, {"target", {{"$ref", target->GetUUID().ToString()}}}});
    component->SetScript(path);
    ReferenceResolver resolver;
    resolver.RegisterGameObject(target->GetUUID(), target.get());
    component->ResolveReferences(nlohmann::json::object(), &resolver);

    script->SetSourceCode(FieldsSource(true)); // the new version adds a field
    LuaRuntime::Instance().ReloadModule(path, script.get());
    script->SetSourceCode(FieldsSource(false));

    Lua()["fields_speed"] = sol::lua_nil;
    Lua()["fields_added"] = sol::lua_nil;
    Lua()["fields_target_valid"] = false;
    component->OnUpdate();

    EXPECT_EQ(Lua()["fields_speed"].get<int>(), 5) << "the saved value was lost";
    EXPECT_EQ(Lua()["fields_added"].get<int>(), 7) << "the added field got no default";
    EXPECT_TRUE(Lua()["fields_target_valid"].get<bool>()) << "the $ref field became nil";
    EXPECT_EQ(component->GetScriptData().value("added", 0), 7);
}

TEST_F(LuaLifetimeTest, OnApplicationQuitReachesScripts)
{
    Lua()["quit_calls"] = 0;
    const auto go = GameObject::Create("Quitter");
    go->AddComponent<LuaComponent>()->SetScript(IO::ResourcePath("res://lifetime/Quitter.lua"));
    _scene->AddRootGameObject(go);
    _scene->ProcessAttachQueue();

    _scene->OnApplicationQuit();

    EXPECT_EQ(Lua()["quit_calls"].get<int>(), 1);
}
