#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <math/UUID.hpp>

#include "../network/NetworkTestSupport.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/network/StateHash.hpp"
#include "engine/network/n2net/HashTree.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// The state-hash tree over Lua script fields declared `replicated = true`: the hash reads the script's LIVE values (what
// replication sends), so a script that runs differently on two peers is found at the exact object and field.

using namespace N2Engine;
using namespace N2Engine::Scripting;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;
using N2Engine::N2Net::Bisect;
using N2Engine::N2Net::Divergence;
using N2Engine::N2Net::HashTree;
namespace fs = std::filesystem;

class LuaStateHashTest : public ::testing::Test
{
protected:
    static inline fs::path s_projectRoot;

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_statehash_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
        const fs::path path = s_projectRoot / "assets" / "statehash" / "Counter.lua";
        fs::create_directories(path.parent_path());
        std::ofstream(path) << R"(
            local Counter = {}
            Counter.__index = Counter
            Counter.SerializableFields = {
                score = { type = "int", default = 1, replicated = true },
                speed = { default = 2.5, replicated = true },
                label = { type = "string", default = "a", replicated = true },
                where = { default = Vector3(0, 1, 0), replicated = true },
                alive = { type = "bool", default = true, replicated = true },
                notes = { type = "string", default = "n" },
            }

            function Counter:OnUpdate()
                self.score = self.score + 10
                self.speed = self.speed + 0.5
                self.notes = "live" .. self.score
                self.label = "tick" .. self.score
                self.alive = not self.alive
            end

            return Counter
        )";
        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    struct Rig
    {
        std::unique_ptr<Scene> scene;
        NetworkWorld world;
        LayoutCache layouts;
        std::vector<LuaComponent *> scripts;

        explicit Rig(const std::uint32_t objects)
        {
            scene = Scene::Create("LuaStateHash");
            for (std::uint32_t n = 1; n <= objects; ++n)
            {
                const GameObject::Ptr go = GameObject::Create("Object" + std::to_string(n));
                go->SetUUID(TestUuid(n));
                go->AddComponent<NetworkObject>();
                auto *script = go->AddComponent<LuaComponent>();
                script->SetScript(IO::ResourcePath("res://statehash/Counter.lua"));
                scripts.push_back(script);
                scene->AddRootGameObject(go);
            }
            EXPECT_TRUE(world.BindScene(*scene).has_value());
        }

        HashTree Tree()
        {
            auto tree = BuildStateHashTree(world, layouts, 3);
            EXPECT_TRUE(tree.has_value());
            return tree.value_or(HashTree());
        }
    };
};

TEST_F(LuaStateHashTest, ScriptsRunningTheSameGiveTheSameRoot)
{
    Rig a(3);
    Rig b(3);
    EXPECT_EQ(a.Tree().Root(), b.Tree().Root());
    for (int i = 0; i < 3; ++i)
    {
        for (LuaComponent *script : a.scripts)
        {
            script->OnUpdate();
        }
        for (LuaComponent *script : b.scripts)
        {
            script->OnUpdate();
        }
    }
    EXPECT_EQ(a.Tree().Root(), b.Tree().Root());
    // The live values moved on from the saved ones, and the tree followed
    Rig fresh(3);
    EXPECT_NE(a.Tree().Root(), fresh.Tree().Root());
}

TEST_F(LuaStateHashTest, ADivergentScriptIsLocatedByObjectAndField)
{
    Rig a(3);
    Rig b(3);
    for (LuaComponent *script : a.scripts)
    {
        script->OnUpdate();
    }
    for (LuaComponent *script : b.scripts)
    {
        script->OnUpdate();
    }
    ASSERT_EQ(a.Tree().Root(), b.Tree().Root());

    // One more update on object 2 only: every replicated field of that script differs; the lowest field is found
    b.scripts[1]->OnUpdate();
    HashTree local = a.Tree();
    HashTree remote = b.Tree();
    ASSERT_NE(local.Root(), remote.Root());
    const Divergence found = Bisect(local, remote);
    ASSERT_EQ(found.kind, Divergence::Kind::FieldDiffers);
    EXPECT_EQ(found.objectId, 2u);
    const std::string name = DescribeHashedField(a.world, a.layouts, found.objectId, found.fieldId);
    EXPECT_FALSE(name.empty());
    EXPECT_NE(name, "<meta>");
}
