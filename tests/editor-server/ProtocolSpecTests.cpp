#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>

using namespace N2Engine::Editor;
// ordered_json: a request's fields go on the wire in the order protocol.json lists them
using spec_json = nlohmann::ordered_json;

// protocol.json is the spec the client generators read; the server's hand-written Protocol.hpp and command
// deserializers must agree with it, or a generated client talks past the server
namespace
{
    spec_json LoadSpec()
    {
        std::ifstream file(N2_PROTOCOL_JSON);
        if (!file)
        {
            ADD_FAILURE() << "can't open " << N2_PROTOCOL_JSON;
            return spec_json::object();
        }
        return spec_json::parse(file);
    }

    int ParseId(const spec_json &id)
    {
        return std::stoi(id.get<std::string>(), nullptr, 16);
    }

    const std::map<std::string, CommandType> &ServerCommands()
    {
        static const std::map<std::string, CommandType> commands = {
            {"RenderFrame", CommandType::RenderFrame},
            {"SetViewportSize", CommandType::SetViewportSize},
            {"SetCameraPosition", CommandType::SetCameraPosition},
            {"GetCameraPosition", CommandType::GetCameraPosition},
            {"CreateScene", CommandType::CreateScene},
            {"LoadScene", CommandType::LoadScene},
            {"SaveScene", CommandType::SaveScene},
            {"DeleteScene", CommandType::DeleteScene},
            {"GetCurrentScene", CommandType::GetCurrentScene},
            {"CreateEntity", CommandType::CreateEntity},
            {"DestroyEntity", CommandType::DestroyEntity},
            {"SetEntityTransform", CommandType::SetEntityTransform},
            {"GetEntityTransform", CommandType::GetEntityTransform},
            {"GetAllEntities", CommandType::GetAllEntities},
            {"CreateScript", CommandType::CreateScript},
            {"RescanAssets", CommandType::RescanAssets},
            {"GetEngineHealth", CommandType::GetEngineHealth},
            {"Shutdown", CommandType::Shutdown},
        };
        return commands;
    }

    const std::map<std::string, ResponseType> &ServerResponses()
    {
        static const std::map<std::string, ResponseType> responses = {
            {"Ok", ResponseType::Ok},
            {"Error", ResponseType::Error},
            {"FrameData", ResponseType::FrameData},
            {"CameraPosition", ResponseType::CameraPosition},
            {"EntityTransform", ResponseType::EntityTransform},
            {"EntityList", ResponseType::EntityList},
            {"EntityCreated", ResponseType::EntityCreated},
            {"SceneData", ResponseType::SceneData},
            {"ScriptData", ResponseType::ScriptData},
            {"EngineHealth", ResponseType::EngineHealth},
        };
        return responses;
    }

    using Deserializer = std::function<void(Protocol::BufferReader &)>;

    // The server's deserializer for each command that has request fields in the spec
    const std::map<std::string, Deserializer> &ServerDeserializers()
    {
        using namespace Protocol;
        static const std::map<std::string, Deserializer> deserializers = {
            {"SetViewportSize", [](BufferReader &r) { (void)SetViewportSizeCmd::Deserialize(r); }},
            {"SetCameraPosition", [](BufferReader &r) { (void)SetCameraPositionCmd::Deserialize(r); }},
            {"CreateScene", [](BufferReader &r) { (void)CreateSceneCmd::Deserialize(r); }},
            {"LoadScene", [](BufferReader &r) { (void)LoadSceneCmd::Deserialize(r); }},
            {"DeleteScene", [](BufferReader &r) { (void)DeleteSceneCmd::Deserialize(r); }},
            {"CreateEntity", [](BufferReader &r) { (void)CreateEntityCmd::Deserialize(r); }},
            {"DestroyEntity", [](BufferReader &r) { (void)DestroyEntityCmd::Deserialize(r); }},
            {"SetEntityTransform", [](BufferReader &r) { (void)SetEntityTransformCmd::Deserialize(r); }},
            {"GetEntityTransform", [](BufferReader &r) { (void)GetEntityTransformCmd::Deserialize(r); }},
            {"CreateScript", [](BufferReader &r) { (void)CreateScriptCmd::Deserialize(r); }},
        };
        return deserializers;
    }

    // Writes one field of the given spec type; false for a type requests don't use
    bool WriteField(Protocol::BufferWriter &w, const std::string &type, const std::string &name)
    {
        if (type == "string")
        {
            w.WriteString("value of " + name);
        }
        else if (type == "int32")
        {
            w.WriteI32(7);
        }
        else if (type == "uint32")
        {
            w.WriteU32(7);
        }
        else if (type == "float32")
        {
            w.WriteF32(1.5f);
        }
        else if (type == "bool")
        {
            w.WriteBool(true);
        }
        else if (type == "vec3")
        {
            w.WriteF32(1.0f);
            w.WriteF32(2.0f);
            w.WriteF32(3.0f);
        }
        else
        {
            return false;
        }
        return true;
    }
}

TEST(ProtocolSpecTest, CommandIdsMatchTheSpec)
{
    const spec_json spec = LoadSpec();
    ASSERT_TRUE(spec.contains("commands"));
    const spec_json &commands = spec.at("commands");

    EXPECT_EQ(commands.size(), ServerCommands().size()) << "a command exists on only one side";
    for (const auto &[name, command] : commands.items())
    {
        const auto it = ServerCommands().find(name);
        ASSERT_NE(it, ServerCommands().end()) << "protocol.json command missing from Protocol.hpp: " << name;
        EXPECT_EQ(ParseId(command.at("id")), static_cast<int>(it->second)) << name;
    }
}

TEST(ProtocolSpecTest, ResponseIdsMatchTheSpec)
{
    const spec_json spec = LoadSpec();
    ASSERT_TRUE(spec.contains("responses"));
    const spec_json &responses = spec.at("responses");

    EXPECT_EQ(responses.size(), ServerResponses().size()) << "a response exists on only one side";
    for (const auto &[name, id] : responses.items())
    {
        const auto it = ServerResponses().find(name);
        ASSERT_NE(it, ServerResponses().end()) << "protocol.json response missing from Protocol.hpp: " << name;
        EXPECT_EQ(ParseId(id), static_cast<int>(it->second)) << name;
    }
}

TEST(ProtocolSpecTest, EntityRequestsCarryUuidStrings)
{
    const spec_json spec = LoadSpec();
    for (const char *command : {"DestroyEntity", "SetEntityTransform", "GetEntityTransform"})
    {
        EXPECT_EQ(spec.at("commands").at(command).at("request").at("entityId").get<std::string>(), "string")
            << command;
    }
}

// For every command with request fields: a payload built from the spec's field list (in order, by type)
// is exactly what the server's deserializer reads, no more and no less
TEST(ProtocolSpecTest, CommandDeserializersReadTheSpecFields)
{
    const spec_json spec = LoadSpec();
    ASSERT_TRUE(spec.contains("commands"));

    size_t checked = 0;
    for (const auto &[name, command] : spec.at("commands").items())
    {
        const spec_json &request = command.at("request");
        if (request.empty())
        {
            EXPECT_EQ(ServerDeserializers().count(name), 0u) << name << " has no request fields in the spec";
            continue;
        }

        const auto it = ServerDeserializers().find(name);
        ASSERT_NE(it, ServerDeserializers().end()) << "no server deserializer for " << name;

        Protocol::BufferWriter payload;
        for (const auto &[field, type] : request.items())
        {
            ASSERT_TRUE(WriteField(payload, type.get<std::string>(), field))
                << name << "." << field << " has an unexpected type";
        }

        Protocol::BufferReader reader(payload.Data());
        EXPECT_NO_THROW(it->second(reader)) << name << ": the server reads more than the spec's fields";
        EXPECT_EQ(reader.Remaining(), 0u) << name << ": the server reads less than the spec's fields";
        ++checked;
    }
    EXPECT_EQ(checked, ServerDeserializers().size()) << "a server deserializer has no request in the spec";
}

TEST(ProtocolSpecTest, CommandDeserializersKeepFieldValues)
{
    using namespace Protocol;
    const std::string uuid = "123e4567-e89b-12d3-a456-426614174000";

    BufferWriter set;
    set.WriteString(uuid);
    for (int i = 1; i <= 9; ++i)
    {
        set.WriteF32(static_cast<float>(i));
    }
    BufferReader setReader(set.Data());
    const SetEntityTransformCmd cmd = SetEntityTransformCmd::Deserialize(setReader);
    EXPECT_EQ(cmd.entityId, uuid);
    EXPECT_FLOAT_EQ(cmd.position.x, 1.0f);
    EXPECT_FLOAT_EQ(cmd.rotation.y, 5.0f);
    EXPECT_FLOAT_EQ(cmd.scale.z, 9.0f);

    BufferWriter load;
    load.WriteString(R"({"name":"S","rootGameObjects":[]})");
    BufferReader loadReader(load.Data());
    EXPECT_EQ(LoadSceneCmd::Deserialize(loadReader).sceneJson, R"({"name":"S","rootGameObjects":[]})");
}
