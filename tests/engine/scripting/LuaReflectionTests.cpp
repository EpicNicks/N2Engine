#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Model.hpp"
#include "engine/rendering/Texture.hpp"
#include "engine/sceneManagement/SceneFile.hpp"
#include "engine/text/Font.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/LuaScript.hpp"
#include "engine/serialization/FieldInfo.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

// A LuaComponent's editable fields (#77, E5): scriptUUID plus one field per entry of the script's SerializableFields,
// and the values SetEditorFields takes for them

using namespace N2Engine;
using namespace N2Engine::Scripting;
using json = nlohmann::json;
namespace fs = std::filesystem;

class LuaReflectionTest : public ::testing::Test
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

        s_projectRoot = fs::temp_directory_path() / "n2engine_lua_reflection_tests";
        std::error_code ec;
        fs::remove_all(s_projectRoot, ec);

        WriteAsset("reflect/Fields.lua", R"(
            local Fields = {}
            Fields.__index = Fields
            Fields.SerializableFields = {
                count = 3,
                ratio = 0.5,
                label = { type = "string", default = "hi" },
                enabled = true,
                offset = { default = Vector3(0, 1, 0) },
                target = { type = "GameObject" },
                body = { type = "Rigidbody" },
                steps = { type = "int", default = 2 },
            }
            return Fields
        )");
        WriteAsset("reflect/Other.lua", R"(
            local Other = {}
            Other.__index = Other
            Other.SerializableFields = {
                count = 9,
                brand = "new",
            }
            return Other
        )");

        // Fails to load until the global is set: a script that can be retried
        WriteAsset("reflect/Broken.lua", R"(
            if not BROKEN_FIXED then error("not yet") end
            local Broken = {}
            Broken.__index = Broken
            Broken.SerializableFields = { level = 1 }
            return Broken
        )");
        // A table keyed by something that isn't a name
        WriteAsset("reflect/Odd.lua", R"(
            local Odd = {}
            Odd.__index = Odd
            Odd.SerializableFields = { count = 3, [1] = "positional" }
            return Odd
        )");
        // Only their extensions matter: the metadata is made from them
        for (const char *file : {"types/a.png", "types/a.ttf", "types/a.mat", "types/a.gltf", "types/a.scene"})
        {
            WriteAsset(file, "x");
        }

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

    static std::string ScriptUuid(const char *path)
    {
        return IO::ResourceLoader::Instance().GetUUID(IO::ResourcePath(path)).ToString();
    }
};

TEST_F(LuaReflectionTest, AComponentWithoutAScriptOffersOnlyTheScript)
{
    const auto go = GameObject::Create("NoScript");
    const auto *script = go->AddComponent<LuaComponent>();
    const std::vector<FieldInfo> fields = script->DescribeFields();
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields[0].name, "scriptUUID");
    EXPECT_EQ(fields[0].kind, FieldKind::AssetRef);
    EXPECT_EQ(fields[0].assetType, "LuaScript");
    EXPECT_TRUE(fields[0].container.empty());
}

TEST_F(LuaReflectionTest, TheScriptsSerializableFieldsBecomeFields)
{
    const auto go = GameObject::Create("Fields");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://reflect/Fields.lua"));
    ASSERT_FALSE(script->HasMissingScript());

    const std::vector<FieldInfo> fields = script->DescribeFields();
    ASSERT_EQ(fields.size(), 9u); // the script, then the eight of the script, sorted by name
    EXPECT_EQ(fields[0].name, "scriptUUID");
    std::vector<std::string> names;
    for (std::size_t i = 1; i < fields.size(); ++i)
    {
        names.push_back(fields[i].name);
        EXPECT_EQ(fields[i].container, "scriptData") << fields[i].name;
    }
    EXPECT_EQ(names, (std::vector<std::string>{"body", "count", "enabled", "label", "offset", "ratio", "steps", "target"}));

    EXPECT_EQ(Find(fields, "count")->kind, FieldKind::Int);      // the shorthand `count = 3`
    EXPECT_EQ(Find(fields, "ratio")->kind, FieldKind::Float);    // `ratio = 0.5`
    EXPECT_EQ(Find(fields, "enabled")->kind, FieldKind::Bool);   // `enabled = true`
    EXPECT_EQ(Find(fields, "label")->kind, FieldKind::String);   // type = "string"
    EXPECT_EQ(Find(fields, "offset")->kind, FieldKind::Vector3); // a Vector3 default
    EXPECT_EQ(Find(fields, "steps")->kind, FieldKind::Int);      // type = "int"
    EXPECT_EQ(Find(fields, "target")->kind, FieldKind::GameObjectRef);
    EXPECT_EQ(Find(fields, "body")->kind, FieldKind::ComponentRef);
    EXPECT_EQ(Find(fields, "body")->typeName, "Rigidbody");
    EXPECT_EQ(Find(fields, "label")->displayName, "Label");
}

TEST_F(LuaReflectionTest, SetEditorFieldsMergesIntoTheScriptData)
{
    const auto go = GameObject::Create("Fields");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://reflect/Fields.lua"));

    script->SetEditorFields(json{{"scriptData", {{"count", 7}, {"label", "bye"}}}}, nullptr);
    const json &data = script->GetScriptData();
    EXPECT_EQ(data.at("count"), 7);
    EXPECT_EQ(data.at("label"), "bye");
    EXPECT_EQ(data.at("ratio"), 0.5);      // not mentioned: kept
    EXPECT_EQ(data.at("enabled"), true);
}

TEST_F(LuaReflectionTest, AnotherScriptKeepsOnlyTheFieldsItDeclares)
{
    const auto go = GameObject::Create("Fields");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://reflect/Fields.lua"));
    script->SetEditorFields(json{{"scriptData", {{"count", 7}}}}, nullptr);

    script->SetEditorFields(json{{"scriptUUID", ScriptUuid("res://reflect/Other.lua")}}, nullptr);
    EXPECT_EQ(script->GetScriptPath().ToString(), "res://reflect/Other.lua");
    const json &data = script->GetScriptData();
    EXPECT_EQ(data.at("count"), 7);         // declared by both: kept
    EXPECT_EQ(data.at("brand"), "new");     // declared by the new one: its default
    EXPECT_FALSE(data.contains("label"));   // only the old one's: dropped

    const std::vector<FieldInfo> fields = script->DescribeFields();
    EXPECT_NE(Find(fields, "brand"), nullptr);
    EXPECT_EQ(Find(fields, "label"), nullptr);
}

TEST_F(LuaReflectionTest, ChoosingTheScriptItAlreadyRunsReloadsNothing)
{
    const auto go = GameObject::Create("Fields");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://reflect/Fields.lua"));
    script->SetEditorFields(json{{"scriptData", {{"count", 7}}}}, nullptr);

    script->SetEditorFields(json{{"scriptUUID", ScriptUuid("res://reflect/Fields.lua")}}, nullptr);
    EXPECT_EQ(script->GetScriptData().at("count"), 7);
}

TEST_F(LuaReflectionTest, ABadScriptIsRefused)
{
    const auto go = GameObject::Create("Fields");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://reflect/Fields.lua"));

    EXPECT_THROW(script->SetEditorFields(json{{"scriptUUID", nullptr}}, nullptr), std::invalid_argument);
    EXPECT_THROW(script->SetEditorFields(json{{"scriptUUID", Math::UUID::Random().ToString()}}, nullptr),
                 std::invalid_argument);
    EXPECT_THROW(script->SetEditorFields(json{{"scriptUUID", "not a uuid"}}, nullptr), std::invalid_argument);
    EXPECT_EQ(script->GetScriptPath().ToString(), "res://reflect/Fields.lua");
}

TEST_F(LuaReflectionTest, AReferenceFieldIsResolvedThroughTheResolver)
{
    const auto go = GameObject::Create("Fields");
    const auto other = GameObject::Create("Other");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://reflect/Fields.lua"));

    ReferenceResolver resolver;
    resolver.RegisterGameObject(other->GetUUID(), other.get());
    script->SetEditorFields(json{{"scriptData", {{"target", {{"$ref", other->GetUUID().ToString()}}}}}}, &resolver);
    resolver.ResolveAll();

    EXPECT_FALSE(script->HasUnresolvedReferences());
    EXPECT_EQ(script->GetScriptData().at("target").at("$ref"), other->GetUUID().ToString());
}

TEST_F(LuaReflectionTest, AnIntegerFieldIsStoredAsAnInteger)
{
    const auto go = GameObject::Create("Fields");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://reflect/Fields.lua"));

    script->SetEditorFields(json{{"scriptData", {{"count", 7.0}, {"ratio", 2}}}}, nullptr);
    EXPECT_TRUE(script->GetScriptData().at("count").is_number_integer()) << script->GetScriptData().dump();
    EXPECT_EQ(script->GetScriptData().at("count"), 7);
    EXPECT_EQ(script->GetScriptData().at("ratio"), 2); // a float field keeps what it was given

    // Integers beyond 32 bits are kept: the field's limits are a 64-bit integer's
    const std::vector<FieldInfo> fields = script->DescribeFields();
    EXPECT_GT(Find(fields, "count")->limits.second, 3.0e9);
    EXPECT_FALSE(ValidateFieldValue(*Find(fields, "count"), 5000000000LL).has_value());
}

TEST_F(LuaReflectionTest, AScriptWhoseTableHasAPositionalKeyStillDescribesItsNamedFields)
{
    const auto go = GameObject::Create("Odd");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://reflect/Odd.lua"));
    const std::vector<FieldInfo> fields = script->DescribeFields();
    EXPECT_NE(Find(fields, "count"), nullptr);
    EXPECT_EQ(Find(fields, "1"), nullptr);
}

TEST_F(LuaReflectionTest, ChoosingTheScriptAgainRetriesOneThatFailedToLoad)
{
    sol::state &lua = LuaRuntime::Instance().GetState();
    lua["BROKEN_FIXED"] = false;
    const auto go = GameObject::Create("Broken");
    auto *script = go->AddComponent<LuaComponent>();
    script->SetScript(IO::ResourcePath("res://reflect/Broken.lua"));
    ASSERT_TRUE(script->HasMissingScript());

    lua["BROKEN_FIXED"] = true;
    script->SetEditorFields(json{{"scriptUUID", ScriptUuid("res://reflect/Broken.lua")}}, nullptr);
    EXPECT_FALSE(script->HasMissingScript());
    EXPECT_EQ(script->GetScriptData().at("level"), 1);
}

TEST_F(LuaReflectionTest, ResourceTypesInMetadataAreTheStaticNames)
{
    const auto typeOf = [](const char *path) -> std::string
    {
        const IO::AssetMetadata *meta = IO::ResourceLoader::Instance().GetMetadata(IO::ResourcePath(path));
        return meta != nullptr ? meta->resourceType : std::string("<no metadata>");
    };
    EXPECT_EQ(typeOf("res://reflect/Fields.lua"), LuaScript::ResourceTypeName);
    EXPECT_EQ(typeOf("res://types/a.png"), Rendering::Texture::ResourceTypeName);
    EXPECT_EQ(typeOf("res://types/a.ttf"), Text::Font::ResourceTypeName);
    EXPECT_EQ(typeOf("res://types/a.mat"), Rendering::Material::ResourceTypeName);
    EXPECT_EQ(typeOf("res://types/a.gltf"), Rendering::Model::ResourceTypeName);
    EXPECT_EQ(typeOf("res://types/a.scene"), SceneFile::ResourceTypeName);
}
