#include <gtest/gtest.h>

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

// The Joint component through the editor server (#381 item 6, stage d): what GetComponentTypes tells an inspector about it
// (an enum dropdown, a GameObject picker, units in the tooltips), edits through SetComponentFields (the limits checked as
// a whole, and a refused edit leaving the joint as it was) and undo/redo of those edits. The inspector has no conditional
// visibility or per-component warnings, so every field is listed whatever the joint's type is.
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

    class EditorJointInspectorTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Joint Inspector Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            server.GetHistory().SetCoalesceWindow(std::chrono::milliseconds(-1));
            entity = Create("A");
            other = Create("B");
            const Frame added = Execute(server, CommandType::AddComponent, Strings({entity, "Joint"}));
            ASSERT_EQ(added.type, ComponentAddedType) << added.Text();
            BufferReader r(added.payload);
            joint = r.ReadString();
        }

        std::string Create(const std::string &name)
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString("");
            w.WriteI32(-1);
            w.WriteString("");
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            BufferReader r(created.payload);
            return r.ReadString();
        }

        Frame SetFields(const json &values)
        {
            BufferWriter w;
            w.WriteString(entity);
            w.WriteString(joint);
            WriteJson(w, values);
            return Execute(server, CommandType::SetComponentFields, w.Release());
        }

        json Get()
        {
            const Frame frame = Execute(server, CommandType::GetComponent, Strings({entity, joint}));
            EXPECT_EQ(frame.type, ComponentDataType) << frame.Text();
            BufferReader r(frame.payload);
            return ReadJson(r);
        }

        json Schema()
        {
            const Frame frame = Execute(server, CommandType::GetComponentTypes);
            EXPECT_EQ(frame.type, ComponentTypesType) << frame.Text();
            BufferReader r(frame.payload);
            for (const json &type : ReadJson(r))
            {
                if (type.at("typeName") == "Joint")
                    return type;
            }
            ADD_FAILURE() << "Joint is not in the component types";
            return json::object();
        }

        json Field(const std::string &name)
        {
            for (const json &field : Schema().at("fields"))
            {
                if (field.at("name") == name)
                    return field;
            }
            ADD_FAILURE() << "no field " << name;
            return json::object();
        }

        void Step(const CommandType type)
        {
            EXPECT_EQ(Execute(server, type).type, EditResultType);
        }

        EditorServer server;
        std::string entity;
        std::string other;
        std::string joint;
    };
}

TEST_F(EditorJointInspectorTest, TheSchemaGivesTheInspectorADropdownAPickerAndUnits)
{
    const json type = Field("_jointType");
    EXPECT_EQ(type.at("kind"), "Enum");
    EXPECT_EQ(type.at("enumOptions"), json::array({"Fixed", "Hinge", "Ball", "Slider", "Distance"}));
    EXPECT_FALSE(type.at("tooltip").get<std::string>().empty());

    EXPECT_EQ(Field("_connectedBody").at("kind"), "GameObjectRef");
    EXPECT_EQ(Field("_collideConnected").at("kind"), "Bool");
    EXPECT_EQ(Field("_useLimits").at("kind"), "Bool");
    EXPECT_EQ(Field("_anchor").at("kind"), "Vector3");
    EXPECT_EQ(Field("_axis").at("kind"), "Vector3");

    for (const char *name : {"_limitMin", "_limitMax"})
    {
        const json limit = Field(name);
        EXPECT_EQ(limit.at("kind"), "Float");
        const std::string tooltip = limit.at("tooltip");
        EXPECT_NE(tooltip.find("degrees"), std::string::npos) << tooltip;
        EXPECT_NE(tooltip.find("metres"), std::string::npos) << tooltip;
    }
    EXPECT_EQ(Field("_limitMin").at("displayName"), "Lower Limit");
    EXPECT_EQ(Field("_limitMax").at("displayName"), "Upper Limit");
}

TEST_F(EditorJointInspectorTest, EveryFieldRoundTripsThroughSetComponentFields)
{
    const json values = {{"_jointType", "Slider"},
                         {"_connectedBody", other},
                         {"_collideConnected", true},
                         {"_useLimits", true},
                         {"_limitMin", -0.5},
                         {"_limitMax", 2.0},
                         {"_axis", {{"x", 0.0}, {"y", 1.0}, {"z", 0.0}}}};
    const Frame set = SetFields(values);
    ASSERT_NE(set.type, ErrorType) << set.Text();
    const json read = Get();
    EXPECT_EQ(read.at("_jointType"), "Slider");
    EXPECT_EQ(read.at("_connectedBody"), other);
    EXPECT_EQ(read.at("_collideConnected"), true);
    EXPECT_EQ(read.at("_useLimits"), true);
    EXPECT_FLOAT_EQ(read.at("_limitMin").get<float>(), -0.5f);
    EXPECT_FLOAT_EQ(read.at("_limitMax").get<float>(), 2.0f);
    EXPECT_EQ(read.at("_axis").at("y"), 1.0);
}

TEST_F(EditorJointInspectorTest, ABadJointTypeAndAnUnknownBodyAreRefused)
{
    EXPECT_EQ(SetFields({{"_jointType", "Spring"}}).type, ErrorType);
    EXPECT_EQ(SetFields({{"_connectedBody", "00000000-0000-0000-0000-00000000dead"}}).type, ErrorType);
    EXPECT_EQ(Get().at("_jointType"), "Fixed");
}

TEST_F(EditorJointInspectorTest, LimitsAreCheckedAsAWholeAndARefusedEditChangesNothing)
{
    ASSERT_NE(SetFields({{"_jointType", "Hinge"}}).type, ErrorType);
    ASSERT_NE(SetFields({{"_useLimits", true}, {"_limitMin", -45.0}, {"_limitMax", 90.0}}).type, ErrorType);

    // Lower above upper
    Frame refused = SetFields({{"_limitMin", 100.0}});
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("lower"), std::string::npos) << refused.Text();
    // A hinge range that does not hold 0
    refused = SetFields({{"_limitMin", 10.0}, {"_limitMax", 20.0}});
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("contain 0"), std::string::npos) << refused.Text();
    // A hinge is within +-180 degrees
    EXPECT_EQ(SetFields({{"_limitMax", 270.0}}).type, ErrorType);

    const json read = Get();
    EXPECT_FLOAT_EQ(read.at("_limitMin").get<float>(), -45.0f);
    EXPECT_FLOAT_EQ(read.at("_limitMax").get<float>(), 90.0f);

    // Metres for a distance joint: not negative, but not required to hold 0
    ASSERT_NE(SetFields({{"_jointType", "Distance"}}).type, ErrorType);
    EXPECT_EQ(SetFields({{"_limitMin", -1.0}, {"_limitMax", 3.0}}).type, ErrorType);
    EXPECT_NE(SetFields({{"_limitMin", 1.0}, {"_limitMax", 3.0}}).type, ErrorType);
    // Limits that are off are not checked
    EXPECT_NE(SetFields({{"_useLimits", false}, {"_limitMin", -5.0}}).type, ErrorType);
}

TEST_F(EditorJointInspectorTest, ChangingTheTypeIsNeverRefusedForLimitsLeftOverFromAnotherType)
{
    ASSERT_NE(SetFields({{"_jointType", "Distance"}}).type, ErrorType);
    ASSERT_NE(SetFields({{"_useLimits", true}, {"_limitMin", 1.0}, {"_limitMax", 3.0}}).type, ErrorType);
    EXPECT_NE(SetFields({{"_jointType", "Hinge"}}).type, ErrorType);
    EXPECT_EQ(Get().at("_jointType"), "Hinge");
    // The next limit edit is what is checked
    EXPECT_EQ(SetFields({{"_limitMax", 4.0}}).type, ErrorType);
}

TEST_F(EditorJointInspectorTest, EditsAreUndoneAndRedone)
{
    ASSERT_NE(SetFields({{"_jointType", "Ball"}}).type, ErrorType);
    ASSERT_NE(SetFields({{"_connectedBody", other}}).type, ErrorType);
    ASSERT_NE(SetFields({{"_collideConnected", true}}).type, ErrorType);

    Step(CommandType::Undo);
    EXPECT_EQ(Get().at("_collideConnected"), false);
    Step(CommandType::Undo);
    EXPECT_NE(Get().at("_connectedBody"), other);
    Step(CommandType::Undo);
    EXPECT_EQ(Get().at("_jointType"), "Fixed");

    Step(CommandType::Redo);
    Step(CommandType::Redo);
    Step(CommandType::Redo);
    const json read = Get();
    EXPECT_EQ(read.at("_jointType"), "Ball");
    EXPECT_EQ(read.at("_connectedBody"), other);
    EXPECT_EQ(read.at("_collideConnected"), true);
}
