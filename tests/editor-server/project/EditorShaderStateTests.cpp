#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

// shaderState through the editor server (protocol 1.35.0, Hello capability "shaderState"): a Sprite2D's optional state member
// is settable by SetComponentFields although it is no field. What is refused (another shape, a string that is no UUID, a UUID
// that names no Shader asset, params that is not an object) changes nothing; null clears the shader, and the key stays out of
// GetComponentTypes' fields.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t ComponentTypesType = static_cast<uint8_t>(ResponseType::ComponentTypes);
    constexpr uint8_t ComponentAddedType = static_cast<uint8_t>(ResponseType::ComponentAdded);
    constexpr uint8_t ComponentDataType = static_cast<uint8_t>(ResponseType::ComponentData);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return {payload.begin(), payload.end()}; }
    };

    Frame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(type), payload);
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const auto body = r.ReadBytes(r.ReadU32());
        out.payload.assign(body.begin(), body.end());
        return out;
    }

    std::vector<uint8_t> Strings(std::initializer_list<std::string> values)
    {
        BufferWriter w;
        for (const std::string &value : values)
            w.WriteString(value);
        return w.Release();
    }

    class EditorShaderStateTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Shader State Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            BufferWriter w;
            w.WriteString("Sprite");
            w.WriteString("");
            w.WriteI32(-1);
            w.WriteString("");
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            ASSERT_EQ(created.type, EntityCreatedType) << created.Text();
            BufferReader e(created.payload);
            entity = e.ReadString();
            const Frame added = Execute(server, CommandType::AddComponent, Strings({entity, "Sprite2D"}));
            ASSERT_EQ(added.type, ComponentAddedType) << added.Text();
            BufferReader r(added.payload);
            sprite = r.ReadString();
        }

        Frame SetFields(const json &values)
        {
            BufferWriter w;
            w.WriteString(entity);
            w.WriteString(sprite);
            WriteJson(w, values);
            return Execute(server, CommandType::SetComponentFields, w.Release());
        }

        json Get()
        {
            const Frame frame = Execute(server, CommandType::GetComponent, Strings({entity, sprite}));
            EXPECT_EQ(frame.type, ComponentDataType) << frame.Text();
            BufferReader r(frame.payload);
            return ReadJson(r);
        }

        EditorServer server;
        std::string entity;
        std::string sprite;
    };
}

TEST_F(EditorShaderStateTest, TheHostSaysItTakesShaderState)
{
    const std::vector<std::string> capabilities = EditorServer::Capabilities();
    EXPECT_NE(std::ranges::find(capabilities, "shaderState"), capabilities.end());
    EXPECT_EQ(ProtocolVersion, "1.35.0");
}

TEST_F(EditorShaderStateTest, AWrongShaderStateIsRefusedAndChangesNothing)
{
    const json before = Get();
    EXPECT_FALSE(before.contains("shaderState")) << "no shader: the key is not saved";
    for (const json &bad : {json(5), json("text"), json::array(), json{{"params", json::object()}}, json{{"shader", "not-a-uuid"}},
                            json{{"shader", "6a1f0c2e-1b2d-4e3f-8a9b-0c1d2e3f4a5b"}},
                            json{{"shader", "6a1f0c2e-1b2d-4e3f-8a9b-0c1d2e3f4a5b"}, {"params", json::array()}}})
    {
        const Frame refused = SetFields(json{{"shaderState", bad}});
        EXPECT_EQ(refused.type, ErrorType) << bad.dump();
        EXPECT_NE(refused.Text().find("shaderState"), std::string::npos) << refused.Text();
    }
    EXPECT_EQ(Get(), before);
}

TEST_F(EditorShaderStateTest, NullClearsTheShaderAndTheKeyIsNoField)
{
    const Frame cleared = SetFields(json{{"shaderState", nullptr}});
    EXPECT_NE(cleared.type, ErrorType) << cleared.Text();
    EXPECT_FALSE(Get().contains("shaderState"));
    const Frame frame = Execute(server, CommandType::GetComponentTypes);
    ASSERT_EQ(frame.type, ComponentTypesType) << frame.Text();
    BufferReader r(frame.payload);
    for (const json &type : ReadJson(r))
    {
        if (type.at("typeName") != "Sprite2D")
            continue;
        for (const json &field : type.at("fields"))
            EXPECT_NE(field.at("name"), "shaderState") << "an optional state member is in no schema";
    }
    // A key that is neither a field nor an optional state member is still refused
    EXPECT_EQ(SetFields(json{{"notAField", 1}}).type, ErrorType);
}
