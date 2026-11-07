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
#include "engine/network/RollbackRuntime.hpp"
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
            // For the session lint (design 9.2): every way of keeping tick state outside the replicated fields that the
            // lint follows, and the same in callbacks a tick does not run (not reported)
            std::ofstream(path.parent_path() / "Leaky.lua") << R"(
                local state = { n = 0 }
                local total = 0
                local Leaky = {}
                Leaky.__index = Leaky
                Leaky.SerializableFields = {
                    hp = { type = "int", default = 0, replicated = true },
                }

                local function bump()
                    total = total + 1
                end

                function Leaky:Advance()
                    self.timer = (self.timer or 0) + 1
                end

                function Leaky:OnFixedUpdate()
                    state.n = state.n + 1
                    bump()
                    self:Advance()
                    leakyGlobal = 1
                    local twice = function() total = total + 2 end
                    twice()
                    self.hp = self.hp + 1
                end

                function Leaky:OnCollisionEnter(collision)
                    Leaky.hits = (Leaky.hits or 0) + 1
                end

                function Leaky:OnUpdate()
                    total = 0
                    self.frame = 1
                end

                return Leaky
            )";
            // Reads a module-level constant, writes a table of its own and only its replicated field in a tick, and keeps
            // a non-replicated field set once at attach: nothing to report on a networked object
            std::ofstream(path.parent_path() / "Clean.lua") << R"(
                local SPEED = 3
                local Clean = {}
                Clean.__index = Clean
                Clean.SerializableFields = {
                    pos = { type = "int", default = 0, replicated = true },
                }

                local function step(value)
                    return value + SPEED
                end

                function Clean:OnAttach()
                    self.cache = 1
                end

                function Clean:OnFixedUpdate()
                    local scratch = {}
                    scratch.x = 1
                    self.pos = step(self.pos)
                end

                return Clean
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

// ===== The session lint (design 9.2, option A: the contract is kept and a script that breaks it is named) =====

namespace
{
    std::string Joined(const std::vector<std::string> &lines)
    {
        std::string text;
        for (const std::string &line : lines)
        {
            text += line + "\n";
        }
        return text;
    }
}

TEST_F(RollbackLuaTest, TheSessionLintNamesTheModuleLocalAndTheNonReplicatedFieldATickWrites)
{
    const std::vector<std::string> warnings = RollbackRuntime::LuaStateWarnings(*scene);
    ASSERT_EQ(warnings.size(), 1u) << Joined(warnings);
    const std::string &line = warnings[0];
    EXPECT_NE(line.find("res://rb/Counter.lua"), std::string::npos) << line;
    EXPECT_NE(line.find("OnFixedUpdate assigns 'hidden'"), std::string::npos) << line;
    EXPECT_NE(line.find("OnFixedUpdate writes self.notes, which is not a replicated field"), std::string::npos) << line;
    EXPECT_EQ(line.find("self.counter"), std::string::npos) << "counter is replicated: " << line;
    EXPECT_EQ(line.find("self.shown"), std::string::npos) << "shown is replicated: " << line;
    EXPECT_EQ(line.find("NetworkObject"), std::string::npos) << line;
}

TEST_F(RollbackLuaTest, TheSessionLintFollowsHelpersAndNamesGlobalsAndModuleTablesButNotOtherCallbacks)
{
    std::unique_ptr<Scene> lintScene = Scene::Create("RbLint");
    std::uint32_t next = 20;
    const auto add = [&](const char *name, const char *path, const bool networked)
    {
        GameObject::Ptr gameObject = GameObject::Create(name);
        gameObject->SetUUID(TestUuid(next++));
        if (networked)
        {
            gameObject->AddComponent<NetworkObject>();
        }
        gameObject->AddComponent<LuaComponent>()->SetScript(IO::ResourcePath(path));
        lintScene->AddRootGameObject(gameObject);
        lintScene->ProcessAttachQueue();
    };
    add("Leaky", "res://rb/Leaky.lua", true);
    add("Clean", "res://rb/Clean.lua", true);
    add("CleanAgain", "res://rb/Clean.lua", true);

    std::vector<std::string> warnings = RollbackRuntime::LuaStateWarnings(*lintScene);
    ASSERT_EQ(warnings.size(), 1u) << "the clean script says nothing: " << Joined(warnings);
    const std::string &leaky = warnings[0];
    EXPECT_NE(leaky.find("res://rb/Leaky.lua"), std::string::npos) << leaky;
    EXPECT_NE(leaky.find("OnFixedUpdate writes the field 'n' of 'state'"), std::string::npos) << leaky;
    EXPECT_NE(leaky.find("OnFixedUpdate (through the local function 'bump') assigns 'total'"), std::string::npos) << leaky;
    EXPECT_NE(leaky.find("OnFixedUpdate (through self:Advance) writes self.timer"), std::string::npos) << leaky;
    EXPECT_NE(leaky.find("OnFixedUpdate writes the global 'leakyGlobal'"), std::string::npos) << leaky;
    EXPECT_NE(leaky.find("OnFixedUpdate assigns 'total'"), std::string::npos) << "the inner function's write: " << leaky;
    EXPECT_NE(leaky.find("OnCollisionEnter writes the field 'hits' of 'Leaky'"), std::string::npos) << leaky;
    EXPECT_EQ(leaky.find("self.hp"), std::string::npos) << "hp is replicated: " << leaky;
    EXPECT_EQ(leaky.find("OnUpdate"), std::string::npos) << "OnUpdate does not run in a tick: " << leaky;
    EXPECT_EQ(leaky.find("self.frame"), std::string::npos) << leaky;

    // The same clean script on an object without a NetworkObject: none of its fields is in a snapshot
    add("Loose", "res://rb/Clean.lua", false);
    warnings = RollbackRuntime::LuaStateWarnings(*lintScene);
    ASSERT_EQ(warnings.size(), 2u) << Joined(warnings);
    const auto loose = std::find_if(warnings.begin(), warnings.end(),
                                    [](const std::string &line) { return line.find("Clean.lua") != std::string::npos; });
    ASSERT_NE(loose, warnings.end()) << Joined(warnings);
    EXPECT_NE(loose->find("without a NetworkObject"), std::string::npos) << *loose;
    EXPECT_NE(loose->find("OnFixedUpdate writes self.pos"), std::string::npos) << *loose;
    EXPECT_EQ(loose->find("self.cache"), std::string::npos) << "set at attach, not in a tick: " << *loose;
}
