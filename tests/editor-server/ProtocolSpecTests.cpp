#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>

using namespace N2Engine::Editor;
using json = nlohmann::json;

// protocol.json is the spec the client generators read; the server's hand-written Protocol.hpp must
// agree with it, or a generated client talks past the server
namespace
{
    json LoadSpec()
    {
        std::ifstream file(N2_PROTOCOL_JSON);
        if (!file)
        {
            ADD_FAILURE() << "can't open " << N2_PROTOCOL_JSON;
            return json::object();
        }
        return json::parse(file);
    }

    int ParseId(const json &id)
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
}

TEST(ProtocolSpecTest, CommandIdsMatchTheSpec)
{
    const json spec = LoadSpec();
    ASSERT_TRUE(spec.contains("commands"));

    EXPECT_EQ(spec["commands"].size(), ServerCommands().size()) << "a command exists on only one side";
    for (const auto &[name, command] : spec["commands"].items())
    {
        const auto it = ServerCommands().find(name);
        ASSERT_NE(it, ServerCommands().end()) << "protocol.json command missing from Protocol.hpp: " << name;
        EXPECT_EQ(ParseId(command.at("id")), static_cast<int>(it->second)) << name;
    }
}

TEST(ProtocolSpecTest, ResponseIdsMatchTheSpec)
{
    const json spec = LoadSpec();
    ASSERT_TRUE(spec.contains("responses"));

    EXPECT_EQ(spec["responses"].size(), ServerResponses().size()) << "a response exists on only one side";
    for (const auto &[name, id] : spec["responses"].items())
    {
        const auto it = ServerResponses().find(name);
        ASSERT_NE(it, ServerResponses().end()) << "protocol.json response missing from Protocol.hpp: " << name;
        EXPECT_EQ(ParseId(id), static_cast<int>(it->second)) << name;
    }
}

TEST(ProtocolSpecTest, EntityRequestsCarryUuidStrings)
{
    const json spec = LoadSpec();
    for (const char *command : {"DestroyEntity", "SetEntityTransform", "GetEntityTransform"})
    {
        EXPECT_EQ(spec["commands"][command]["request"]["entityId"], "string") << command;
    }
}

TEST(ProtocolSpecTest, CommandDeserializersReadTheSpecFields)
{
    using namespace N2Engine::Editor::Protocol;
    const std::string uuid = "123e4567-e89b-12d3-a456-426614174000";

    BufferWriter destroy;
    destroy.WriteString(uuid);
    BufferReader destroyReader(destroy.Data());
    EXPECT_EQ(DestroyEntityCmd::Deserialize(destroyReader).entityId, uuid);

    BufferWriter get;
    get.WriteString(uuid);
    BufferReader getReader(get.Data());
    EXPECT_EQ(GetEntityTransformCmd::Deserialize(getReader).entityId, uuid);

    BufferWriter load;
    load.WriteString(R"({"name":"S","rootGameObjects":[]})");
    BufferReader loadReader(load.Data());
    EXPECT_EQ(LoadSceneCmd::Deserialize(loadReader).sceneJson, R"({"name":"S","rootGameObjects":[]})");

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
}
