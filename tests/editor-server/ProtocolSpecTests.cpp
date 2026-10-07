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

#include "ProtocolVectors.hpp"

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
            {"GetAudio", CommandType::GetAudio},
            {"Hello", CommandType::Hello},
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
            {"AudioSamples", ResponseType::AudioSamples},
            {"ServerInfo", ResponseType::ServerInfo},
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
            {"Hello", [](BufferReader &r) { (void)HelloCmd::Deserialize(r); }},
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
        else if (type == "json" || type.starts_with("json:"))
        {
            w.WriteString(R"({"value of":")" + name + R"("})");
        }
        else if (type == "uint8")
        {
            w.WriteU8(7);
        }
        else if (type == "quat")
        {
            for (int i = 0; i < 4; ++i)
            {
                w.WriteF32(0.5f);
            }
        }
        else if (type == "mat4")
        {
            for (int i = 0; i < 16; ++i)
            {
                w.WriteF32(static_cast<float>(i));
            }
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

namespace
{
    // The response a command answers with, as protocol.json declares it ({"type": ..., "fields": {...}})
    const spec_json *FindResponseFields(const spec_json &spec, const std::string &response)
    {
        for (const auto &[name, command] : spec.at("commands").items())
        {
            const spec_json &declared = command.at("response");
            if (declared.at("type").get<std::string>() == response && declared.contains("fields"))
                return &declared.at("fields");
        }
        return nullptr;
    }

    // Reads one field of the given spec type the way protocol.json's "encoding" section describes it (a client's
    // view of the payload); count holds the last uint32 named count, which an array's length comes from
    void ReadSpecField(const spec_json &spec, Protocol::BufferReader &r, const std::string &type,
                       const std::string &name, uint32_t &count)
    {
        // json first: a JSON shape can end in [] ("json:string[]") but is one string on the wire, not an array
        if (type == "json" || type.starts_with("json:"))
            EXPECT_FALSE(nlohmann::json::parse(r.ReadString(), nullptr, false).is_discarded()) << name << " isn't JSON";
        else if (type.ends_with("[]"))
        {
            const std::string element = type.substr(0, type.size() - 2);
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t nestedCount = 0;
                ReadSpecField(spec, r, element, name, nestedCount);
            }
        }
        else if (type == "string")
            (void)r.ReadString();
        else if (type == "uint8" || type == "bool")
            (void)r.ReadU8();
        else if (type == "uint32")
        {
            const uint32_t value = r.ReadU32();
            if (name == "count")
                count = value;
        }
        else if (type == "int32")
            (void)r.ReadI32();
        else if (type == "float32")
            (void)r.ReadF32();
        else if (type == "mat4")
            (void)r.ReadBytes(16 * sizeof(float));
        else if (type == "bytes")
            (void)r.ReadBytes(r.Remaining());
        else if (spec.at("types").contains(type))
        {
            for (const auto &[field, fieldType] : spec.at("types").at(type).items())
                ReadSpecField(spec, r, fieldType.get<std::string>(), field, count);
        }
        else
            ADD_FAILURE() << name << " has an unknown type: " << type;
    }

    bool IsDocumentedFieldType(const spec_json &spec, std::string type)
    {
        if (type.starts_with("json:"))
            type = "json";
        if (type.ends_with("[]"))
            type = type.substr(0, type.size() - 2);
        return spec.at("encoding").at("fieldTypes").contains(type) || spec.at("types").contains(type);
    }
}

TEST(ProtocolSpecTest, ProtocolVersionMatchesTheSpec)
{
    const spec_json spec = LoadSpec();
    EXPECT_EQ(spec.at("version").get<std::string>(), ProtocolVersion);
    EXPECT_TRUE(ParseProtocolVersion(ProtocolVersion).has_value());
}

TEST(ProtocolSpecTest, EveryFieldTypeIsDocumentedInTheSpec)
{
    const spec_json spec = LoadSpec();
    const auto check = [&](const spec_json &fields, const std::string &where)
    {
        for (const auto &[field, type] : fields.items())
            EXPECT_TRUE(IsDocumentedFieldType(spec, type.get<std::string>())) << where << "." << field << ": " << type;
    };
    for (const auto &[name, command] : spec.at("commands").items())
    {
        check(command.at("request"), name + ".request");
        if (command.at("response").contains("fields"))
            check(command.at("response").at("fields"), name + ".response");
    }
    for (const auto &[name, fields] : spec.at("types").items())
        check(fields, name);
}

TEST(ProtocolSpecTest, NoIdIsInTheRangeReservedForPushedEvents)
{
    const spec_json spec = LoadSpec();
    for (const auto &[name, command] : spec.at("commands").items())
    {
        const int id = ParseId(command.at("id"));
        EXPECT_FALSE(id >= 0xC0 && id <= 0xFE) << name;
    }
    for (const auto &[name, id] : spec.at("responses").items())
    {
        EXPECT_FALSE(ParseId(id) >= 0xC0 && ParseId(id) <= 0xFE) << name;
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

TEST(ProtocolSpecTest, ParseProtocolVersionTakesOnlyMajorMinorPatch)
{
    const auto version = ParseProtocolVersion("12.3.456");
    ASSERT_TRUE(version.has_value());
    EXPECT_EQ(version->majorVersion, 12u);
    EXPECT_EQ(version->minorVersion, 3u);
    EXPECT_EQ(version->patchVersion, 456u);

    for (const char *invalid : {"", "1", "1.1", "1.1.", "1.1.1.1", "v1.1.1", "1.-1.1", "1.+1.1", " 1.1.1", "1.1.1 ",
                                "1..1", "a.b.c", "99999999999.0.0", "4294967296.0.0", "00000000001.0.0",
                                "1.0.000000000000000000000000000000000000000000000000000000000000000001"})
    {
        EXPECT_FALSE(ParseProtocolVersion(invalid).has_value()) << invalid;
    }

    // Up to 10 digits a part, leading zeros included (from_chars alone would take any number of them)
    const auto padded = ParseProtocolVersion("0000000001.4294967295.0");
    ASSERT_TRUE(padded.has_value());
    EXPECT_EQ(padded->majorVersion, 1u);
    EXPECT_EQ(padded->minorVersion, 4294967295u);
}

// For every response with fields: the server's builder, given the golden vector's values, writes a frame of that
// response's id whose payload is exactly the spec's fields, in order, by type (read as protocol.json describes them)
TEST(ProtocolSpecTest, ResponseBuildersWriteTheSpecFields)
{
    const spec_json spec = LoadSpec();
    const auto vectors = ProtocolVectors::LoadResponses();
    ASSERT_FALSE(vectors.empty());

    size_t checked = 0;
    for (const auto &[name, id] : spec.at("responses").items())
    {
        const spec_json *fields = FindResponseFields(spec, name);
        if (fields == nullptr)
            continue; // Ok and Error: covered by ProtocolVectorTest.ServerBuildersWriteTheVectorResponses

        const auto vector = vectors.find(name);
        ASSERT_NE(vector, vectors.end()) << "no golden vector for " << name;
        const std::vector<uint8_t> frame = ProtocolVectors::BuildResponse(name, vector->second);
        ASSERT_GE(frame.size(), 5u) << name;

        Protocol::BufferReader r(frame);
        EXPECT_EQ(r.ReadU8(), ParseId(id)) << name;
        EXPECT_EQ(r.ReadU32(), frame.size() - 5) << name << ": the frame length isn't the payload's";

        uint32_t count = 0;
        for (const auto &[field, type] : fields->items())
        {
            ASSERT_NO_THROW(ReadSpecField(spec, r, type.get<std::string>(), field, count))
                << name << "." << field << ": the server writes less than the spec's fields";
        }
        EXPECT_EQ(r.Remaining(), 0u) << name << ": the server writes more than the spec's fields";
        ++checked;
    }
    EXPECT_EQ(checked + 2, spec.at("responses").size());
}
