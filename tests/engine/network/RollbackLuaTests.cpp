#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "NetworkTestSupport.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/network/RollbackWorld.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/serialization/FieldInfo.hpp"

// Netcode C5: what a rollback restores of a Lua script. The documented scope (docs/netcode.html, "Lua state"): ONLY the
// fields declared `replicated = true`. A script's non-replicated fields, its module-level locals and its upvalues are
// not in a snapshot, so a Restore leaves them at their latest value. These tests pin that behaviour, in both directions,
// so a change to it is a deliberate one.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;
using namespace N2Engine::Scripting;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    class RollbackLuaTest : public ::testing::Test
    {
    protected:
        static inline fs::path s_projectRoot;

        static void SetUpTestSuite()
        {
            ASSERT_TRUE(LuaRuntime::Instance().Initialize());
            s_projectRoot = fs::temp_directory_path() / "n2engine_rollback_lua_tests";
            std::error_code ec;
            fs::remove_all(s_projectRoot, ec);
            const fs::path path = s_projectRoot / "assets" / "rb" / "Counter.lua";
            fs::create_directories(path.parent_path());
            // `hidden` is a module-level local: the kind of state the snapshot cannot see. `counter` and `shown` are
            // declared replicated; `notes` is a declared field that is not.
            std::ofstream(path) << R"(
                local hidden = 0
                local Counter = {}
                Counter.__index = Counter
                Counter.SerializableFields = {
                    counter = { type = "int", default = 0, replicated = true },
                    shown = { type = "int", default = 0, replicated = true },
                    notes = { type = "string", default = "n" },
                }

                function Counter:OnFixedUpdate()
                    hidden = hidden + 1
                    self.counter = self.counter + 1
                    self.shown = hidden
                    self.notes = "n" .. hidden
                    self.gameObject:SetName(self.notes)
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

        static std::vector<FieldInfo> ReplicatedFields(const LuaComponent &component)
        {
            std::vector<FieldInfo> replicated;
            for (const FieldInfo &field : component.DescribeFields())
            {
                if (field.replicated)
                {
                    replicated.push_back(field);
                }
            }
            return replicated;
        }

        void SetUp() override
        {
            scene = Scene::Create("RbLua");
            object = GameObject::Create("Counter");
            object->SetUUID(TestUuid(10));
            object->AddComponent<NetworkObject>();
            script = object->AddComponent<LuaComponent>();
            script->SetScript(IO::ResourcePath("res://rb/Counter.lua"));
            scene->AddRootGameObject(object);
            ASSERT_TRUE(world.BindScene(*scene).has_value());
            scene->ProcessAttachQueue();
            rollback = std::make_unique<RollbackWorld>(world, nullptr);
        }

        void TearDown() override
        {
            rollback.reset();
        }

        void Step(const int ticks)
        {
            for (int i = 0; i < ticks; ++i)
            {
                script->OnFixedUpdate();
            }
        }

        [[nodiscard]] int Counter() const { return Field("counter"); }
        [[nodiscard]] int Shown() const { return Field("shown"); }
        [[nodiscard]] int Field(const char *name) const
        {
            const json live = script->SerializeReplicated(ReplicatedFields(*script));
            return live["scriptData"][name].get<int>();
        }

        std::unique_ptr<Scene> scene;
        GameObject::Ptr object;
        LuaComponent *script = nullptr;
        NetworkWorld world;
        std::unique_ptr<RollbackWorld> rollback;
    };
}

TEST_F(RollbackLuaTest, AScriptsDeclaredReplicatedFieldsAreInTheSnapshotAndComeBack)
{
    ASSERT_EQ(ReplicatedFields(*script).size(), 2u) << "counter and shown replicate; notes does not";
    Step(5);
    ASSERT_EQ(Counter(), 5);
    WorldSnapshot at5;
    ASSERT_TRUE(rollback->Save(5, at5));
    Step(5);
    ASSERT_EQ(Counter(), 10);

    const auto restored = rollback->Restore(at5);
    ASSERT_TRUE(restored.has_value()) << restored.error();
    EXPECT_EQ(Counter(), 5) << "a replicated field is rewound";
    EXPECT_EQ(Shown(), 5);
}

TEST_F(RollbackLuaTest, ANonReplicatedFieldAndAModuleLocalAreNotRewoundWhichIsTheDocumentedScope)
{
    Step(5);
    WorldSnapshot at5;
    ASSERT_TRUE(rollback->Save(5, at5));
    Step(5);
    ASSERT_EQ(object->GetName(), "n10");

    ASSERT_TRUE(rollback->Restore(at5).has_value());
    // The replicated fields are the snapshot's ...
    EXPECT_EQ(Counter(), 5);
    // ... the non-replicated `notes` and the local `hidden` are not: the script does not know it was rolled back
    EXPECT_EQ(object->GetName(), "n10") << "notes was not touched by the restore";
    Step(1);
    EXPECT_EQ(Counter(), 6) << "the replicated counter resumes from the snapshot";
    EXPECT_EQ(Shown(), 11) << "`hidden` kept counting: a script that keeps simulation state in a local diverges after a rollback";
    EXPECT_EQ(object->GetName(), "n11");
}

TEST_F(RollbackLuaTest, ARollbackThroughAScriptThatKeepsStateInALocalEndsOffTheStraightRunsState)
{
    // The straight run: 8 ticks, never rolled back
    Step(8);
    const int straightCounter = Counter();
    const int straightShown = Shown();
    ASSERT_EQ(straightCounter, 8);
    ASSERT_EQ(straightShown, 8);

    // The same script (a fresh scene and instance, so a fresh local) rolled back from tick 6 to tick 3 and run on to 8
    SetUp();
    Step(3);
    WorldSnapshot at3;
    ASSERT_TRUE(rollback->Save(3, at3));
    Step(3);
    ASSERT_TRUE(rollback->Restore(at3).has_value());
    Step(5);
    EXPECT_EQ(Counter(), straightCounter) << "what the script declared replicated is the straight run's";
    EXPECT_NE(Shown(), straightShown) << "what it kept in a local is not: the hash tree would disagree with a peer that did not roll back";
}
