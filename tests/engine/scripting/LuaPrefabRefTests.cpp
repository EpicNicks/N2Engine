#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/prefabs/Prefab.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;
namespace fs = std::filesystem;
using json = nlohmann::json;

// Lua "$ref" fields are UUID strings in the component's scriptData, not pointers, so instantiating a
// prefab must renumber them along with the objects they name
class LuaPrefabRefTest : public ::testing::Test
{
protected:
    static inline fs::path s_projectRoot;

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_prefab_ref_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
        const fs::path script = s_projectRoot / "assets" / "prefabref" / "Holder.lua";
        fs::create_directories(script.parent_path());
        std::ofstream(script) << R"(
            local Holder = {}
            Holder.__index = Holder
            Holder.SerializableFields = {
                target = { type = "GameObject" },
            }
            function Holder:OnUpdate()
                prefabref_target_name = self.target ~= nil and self.target:GetName() or "nil"
            end
            return Holder
        )";

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    // "Turret" with a LuaComponent whose target field refers to `target`
    static GameObject::Ptr MakeTurret(const GameObject::Ptr &barrel, const GameObject &target)
    {
        auto root = GameObject::Create("Turret");
        root->AddChild(barrel);
        auto *component = root->AddComponent<LuaComponent>();
        component->SetScriptData({{"target", {{"$ref", target.GetUUID().ToString()}}}});
        component->SetScript(IO::ResourcePath("res://prefabref/Holder.lua"));
        return root;
    }

    static json SavedRef(const GameObject &instance)
    {
        const json saved = instance.Serialize();
        for (const auto &component : saved.at("components"))
        {
            if (component.at("type") == "LuaComponent")
            {
                return component.at("data").at("scriptData").at("target").at("$ref");
            }
        }
        ADD_FAILURE() << "no LuaComponent in the instance";
        return nullptr;
    }
};

TEST_F(LuaPrefabRefTest, RefFieldPointsAtTheInstancesOwnChild)
{
    const auto barrel = GameObject::Create("Barrel");
    const auto source = MakeTurret(barrel, *barrel);
    const std::string sourceBarrelId = barrel->GetUUID().ToString();
    const json prefabJson = Prefab("Turret", source).Serialize();

    const auto instance = PrefabManager::InstantiatePrefab(prefabJson);
    ASSERT_NE(instance, nullptr);
    ASSERT_EQ(instance->GetChildCount(), 1u);
    const auto instanceBarrel = instance->GetChild(0);
    ASSERT_NE(instanceBarrel->GetUUID(), barrel->GetUUID());

    // Saved form: the ref names the instance's own child, never the prefab's (source) object
    const json ref = SavedRef(*instance);
    ASSERT_TRUE(ref.is_string());
    EXPECT_EQ(ref.get<std::string>(), instanceBarrel->GetUUID().ToString());
    EXPECT_NE(ref.get<std::string>(), sourceBarrelId);

    // Live form: the script's field is a handle to the instance's child
    Lua()["prefabref_target_name"] = sol::lua_nil;
    instance->GetComponent<LuaComponent>()->OnUpdate();
    EXPECT_EQ(Lua()["prefabref_target_name"].get<std::string>(), "Barrel");

    // A save and reload keeps the link (the source object would be unreachable here anyway)
    const auto reloaded = GameObject::Deserialize(instance->Serialize());
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(SavedRef(*reloaded).get<std::string>(), instanceBarrel->GetUUID().ToString());
}

TEST_F(LuaPrefabRefTest, TwoInstancesGetDifferentRefs)
{
    const auto barrel = GameObject::Create("Barrel");
    const json prefabJson = Prefab("Turret", MakeTurret(barrel, *barrel)).Serialize();

    const auto first = PrefabManager::InstantiatePrefab(prefabJson);
    const auto second = PrefabManager::InstantiatePrefab(prefabJson);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(SavedRef(*first).get<std::string>(), first->GetChild(0)->GetUUID().ToString());
    EXPECT_EQ(SavedRef(*second).get<std::string>(), second->GetChild(0)->GetUUID().ToString());
}

TEST_F(LuaPrefabRefTest, RefOutsideThePrefabBecomesNull)
{
    const auto barrel = GameObject::Create("Barrel");
    const auto outsider = GameObject::Create("Outsider");
    const json prefabJson = Prefab("Turret", MakeTurret(barrel, *outsider)).Serialize();

    const auto instance = PrefabManager::InstantiatePrefab(prefabJson);
    ASSERT_NE(instance, nullptr);

    EXPECT_TRUE(SavedRef(*instance).is_null()) << "an external ref would re-link to the source after a reload";
    Lua()["prefabref_target_name"] = sol::lua_nil;
    instance->GetComponent<LuaComponent>()->OnUpdate();
    EXPECT_EQ(Lua()["prefabref_target_name"].get<std::string>(), "nil");
}
