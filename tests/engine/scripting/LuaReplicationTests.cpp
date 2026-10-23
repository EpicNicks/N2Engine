#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaFieldDeclaration.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/serialization/FieldInfo.hpp"

// A Lua script field declared `replicated = true` (netcode stage B2): which declarations replicate, that the server reads
// the script's LIVE value and a client writes only the replicated fields of its own instance

using namespace N2Engine;
using namespace N2Engine::Scripting;
using json = nlohmann::json;
namespace fs = std::filesystem;

class LuaReplicationTest : public ::testing::Test
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

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_replication_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        WriteAsset("repl/Replicated.lua", R"(
            local Replicated = {}
            Replicated.__index = Replicated
            Replicated.SerializableFields = {
                score = { type = "int", default = 1, replicated = true },
                speed = { default = 2.5, replicated = true },
                label = { type = "string", default = "a", replicated = true },
                where = { default = Vector3(0, 1, 0), replicated = true },
                alive = { type = "bool", default = true, replicated = true },
                notes = { type = "string", default = "n" },
                refused = { default = 3, replicated = "yes" },
                off = { default = 4, replicated = false },
                target = { type = "GameObject", replicated = true },
                shorthand = 7,
            }

            -- Changes the script's live values; the saved defaults stay what they were
            function Replicated:OnUpdate()
                self.score = self.score + 10
                self.speed = self.speed + 0.5
                self.notes = "live" .. self.score
                self.label = "tick" .. self.score
                self.alive = not self.alive
            end

            function Replicated:OnFixedUpdate()
                self.gameObject:SetName(self.notes)
            end

            return Replicated
        )");

        IO::ResourceUUID::Initialize(Math::UUID::Random());
        IO::ResourceLoader::Instance().Initialize(s_projectRoot);
    }

    static void TearDownTestSuite()
    {
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);
    }

    static const FieldInfo *Find(const std::vector<FieldInfo> &fields, const std::string &name)
    {
        const auto found = std::ranges::find(fields, name, &FieldInfo::name);
        return found == fields.end() ? nullptr : &*found;
    }

    static LuaComponent *Attach(const GameObject::Ptr &object)
    {
        auto *component = object->AddComponent<LuaComponent>();
        component->SetScript(IO::ResourcePath("res://repl/Replicated.lua"));
        return component;
    }

    /// The replicated fields of a script component
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
};

TEST_F(LuaReplicationTest, OnlyAFieldDeclaredReplicatedTrueAndOfAReplicableKindReplicates)
{
    const auto object = GameObject::Create("Declared");
    const LuaComponent *component = Attach(object);
    ASSERT_FALSE(component->HasMissingScript());
    const std::vector<FieldInfo> fields = component->DescribeFields();

    for (const char *name : {"score", "speed", "label", "where", "alive"})
    {
        const FieldInfo *field = Find(fields, name);
        ASSERT_NE(field, nullptr) << name;
        EXPECT_TRUE(field->replicated) << name;
        EXPECT_EQ(field->container, "scriptData") << name;
    }
    // Not declared, declared false, a value that is not a boolean, a kind a script cannot replicate (a reference), the
    // shorthand (which has no table to say it in)
    for (const char *name : {"notes", "off", "refused", "target", "shorthand"})
    {
        const FieldInfo *field = Find(fields, name);
        ASSERT_NE(field, nullptr) << name;
        EXPECT_FALSE(field->replicated) << name;
    }
    EXPECT_FALSE(Find(fields, "scriptUUID")->replicated);

    // The kinds: bool, int, float, string and Vector3 only
    EXPECT_TRUE(IsLuaReplicableKind(FieldKind::Vector3));
    EXPECT_FALSE(IsLuaReplicableKind(FieldKind::GameObjectRef));
    EXPECT_FALSE(IsLuaReplicableKind(FieldKind::ComponentRef));
    EXPECT_FALSE(IsLuaReplicableKind(FieldKind::Json));
    EXPECT_FALSE(IsLuaReplicableKind(FieldKind::Quaternion));
}

TEST_F(LuaReplicationTest, TheMappingTakesTheFlagOnlyForAKindScriptsCanReplicate)
{
    EXPECT_TRUE(MakeLuaFieldInfo("a", "int", LuaDefaultKind::Other, true).replicated);
    EXPECT_FALSE(MakeLuaFieldInfo("a", "int", LuaDefaultKind::Other, false).replicated);
    EXPECT_TRUE(MakeLuaFieldInfo("a", "", LuaDefaultKind::Float, true).replicated);
    EXPECT_TRUE(MakeLuaFieldInfo("a", "string", LuaDefaultKind::Other, true).replicated);
    EXPECT_TRUE(MakeLuaFieldInfo("a", "Vector3", LuaDefaultKind::Other, true).replicated);
    EXPECT_FALSE(MakeLuaFieldInfo("a", "GameObject", LuaDefaultKind::Other, true).replicated);
    EXPECT_FALSE(MakeLuaFieldInfo("a", "", LuaDefaultKind::Other, true).replicated) << "Json";
    // Without the argument: nothing replicates (every existing caller)
    EXPECT_FALSE(MakeLuaFieldInfo("a", "int", LuaDefaultKind::Integer).replicated);
}

TEST_F(LuaReplicationTest, TheServerReadsTheLiveValuesNotTheSavedOnes)
{
    const auto object = GameObject::Create("Live");
    LuaComponent *component = Attach(object);
    const std::vector<FieldInfo> replicated = ReplicatedFields(*component);
    ASSERT_EQ(replicated.size(), 5u);

    json beforeRun = component->SerializeReplicated(replicated);
    ASSERT_TRUE(beforeRun.contains("scriptData"));
    EXPECT_EQ(beforeRun["scriptData"]["score"], 1);
    EXPECT_DOUBLE_EQ(beforeRun["scriptData"]["speed"].get<double>(), 2.5);
    EXPECT_EQ(beforeRun["scriptData"]["label"], "a");
    EXPECT_TRUE(beforeRun["scriptData"]["alive"].get<bool>());
    EXPECT_DOUBLE_EQ(beforeRun["scriptData"]["where"]["y"].get<double>(), 1.0);

    component->OnUpdate();
    component->OnUpdate();
    const json live = component->SerializeReplicated(replicated);
    EXPECT_EQ(live["scriptData"]["score"], 21);
    EXPECT_DOUBLE_EQ(live["scriptData"]["speed"].get<double>(), 3.5);
    EXPECT_EQ(live["scriptData"]["label"], "tick21");
    EXPECT_TRUE(live["scriptData"]["alive"].get<bool>()) << "toggled twice";
    // What the component saves is the saved data: the script's running state is not in it
    EXPECT_EQ(component->Serialize()["scriptData"]["score"], 1);
}

TEST_F(LuaReplicationTest, AClientWritesTheReplicatedFieldsOfItsInstanceAndNothingElse)
{
    const auto serverObject = GameObject::Create("Server");
    const auto clientObject = GameObject::Create("Client");
    LuaComponent *serverScript = Attach(serverObject);
    LuaComponent *clientScript = Attach(clientObject);

    serverScript->OnUpdate();
    serverScript->OnUpdate();
    serverScript->OnUpdate(); // score 31, speed 4.0, alive false, label tick31

    // The client's script runs too: its own live notes differ from the saved default
    clientScript->OnUpdate(); // score 11, notes "live11"

    // Server to client: the bytes the replication layer would send, applied to the client's object
    const Network::ObjectLayout serverLayout = Network::BuildObjectLayout(*serverObject);
    const Network::ObjectLayout clientLayout = Network::BuildObjectLayout(*clientObject);
    ASSERT_TRUE(serverLayout.usable);
    ASSERT_EQ(serverLayout.fields.size(), 5u);
    ASSERT_EQ(serverLayout.hash, clientLayout.hash);
    Network::NetworkWorld world; // not bound: no object has a net id, and a script field never holds a reference
    const Network::WorldReferences references(world);
    const Network::ObjectState state = Network::EncodeObjectState(serverLayout, *serverObject, &references);
    ASSERT_EQ(state.fields.size(), 5u);
    for (const std::vector<std::uint8_t> &bytes : state.fields)
    {
        EXPECT_FALSE(bytes.empty());
    }

    // The payload of a state entry that carries every field: the mask, then the fields
    std::vector<std::uint8_t> payload;
    for (std::size_t b = 0; b < serverLayout.MaskBytes(); ++b)
    {
        std::uint8_t byte = 0;
        for (std::size_t bit = 0; bit < 8; ++bit)
        {
            if (b * 8 + bit < serverLayout.fields.size())
            {
                byte = static_cast<std::uint8_t>(byte | (1u << bit));
            }
        }
        payload.push_back(byte);
    }
    for (const std::vector<std::uint8_t> &bytes : state.fields)
    {
        payload.insert(payload.end(), bytes.begin(), bytes.end());
    }
    N2Net::ByteReader reader(payload);
    std::size_t refused = 0;
    EXPECT_EQ(Network::ApplyEntry(clientLayout, *clientObject, reader, references, refused), Network::EntryResult::Applied);
    EXPECT_EQ(refused, 0u);

    // The client's script instance has the server's live values
    const json applied = clientScript->SerializeReplicated(ReplicatedFields(*clientScript));
    EXPECT_EQ(applied["scriptData"]["score"], 31);
    EXPECT_DOUBLE_EQ(applied["scriptData"]["speed"].get<double>(), 4.0);
    EXPECT_EQ(applied["scriptData"]["label"], "tick31");
    EXPECT_FALSE(applied["scriptData"]["alive"].get<bool>());
    EXPECT_DOUBLE_EQ(applied["scriptData"]["where"]["y"].get<double>(), 1.0);

    // A field that is not replicated is not written: the client's own live notes survive (writing every saved field
    // back to the instance would have reset them to the saved default "n")
    clientScript->OnFixedUpdate();
    EXPECT_EQ(clientObject->GetName(), "live11");

    // The script keeps running on the new values
    clientScript->OnUpdate();
    EXPECT_EQ(clientScript->SerializeReplicated(ReplicatedFields(*clientScript))["scriptData"]["score"], 41);
}

TEST_F(LuaReplicationTest, TheLayoutOfAScriptObjectComesFromItsDeclarations)
{
    const auto first = GameObject::Create("First");
    const auto second = GameObject::Create("Second");
    Attach(first);
    Attach(second);
    const Network::ObjectLayout a = Network::BuildObjectLayout(*first);
    const Network::ObjectLayout b = Network::BuildObjectLayout(*second);
    ASSERT_TRUE(a.usable);
    EXPECT_EQ(a.hash, b.hash) << "the same script, the same layout";
    std::vector<std::string> names;
    for (const FieldInfo &field : a.fields)
    {
        names.push_back(field.name);
    }
    // DescribeFields lists a script's fields sorted by name
    const std::vector<std::string> expected = {"alive", "label", "score", "speed", "where"};
    EXPECT_EQ(names, expected);
    for (const FieldInfo &field : a.fields)
    {
        EXPECT_EQ(field.container, "scriptData");
    }

    // An object with no script has no replicated fields
    const auto bare = GameObject::Create("Bare");
    bare->AddComponent<LuaComponent>();
    EXPECT_FALSE(Network::BuildObjectLayout(*bare).usable);
}
