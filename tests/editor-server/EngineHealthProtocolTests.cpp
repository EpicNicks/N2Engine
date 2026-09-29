#include <gtest/gtest.h>

#include <editor-server/Commands.hpp>
#include <engine/Health.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;

namespace
{
    struct DecodedStatus
    {
        std::string name, state, detail;
    };

    struct DecodedHealth
    {
        uint8_t responseType = 0;
        uint32_t payloadSize = 0;
        size_t payloadBytesRemaining = 0;
        bool healthy = false;
        std::vector<DecodedStatus> subsystems;
    };

    // Decodes the response the way a client would, following protocol.json
    DecodedHealth Decode(const BufferWriter &w)
    {
        BufferReader r(w.Data());
        DecodedHealth out;
        out.responseType = r.ReadU8();
        out.payloadSize = r.ReadU32();
        out.payloadBytesRemaining = r.Remaining();
        out.healthy = r.ReadBool();

        const uint32_t count = r.ReadU32();
        for (uint32_t i = 0; i < count; ++i)
        {
            DecodedStatus s;
            s.name = r.ReadString();
            s.state = r.ReadString();
            s.detail = r.ReadString();
            out.subsystems.push_back(std::move(s));
        }

        EXPECT_FALSE(r.HasData()) << "trailing bytes after the last subsystem";
        return out;
    }
}

TEST(EngineHealthProtocolTest, UsesProtocolIds)
{
    EXPECT_EQ(static_cast<uint8_t>(CommandType::GetEngineHealth), 0x50);
    EXPECT_EQ(static_cast<uint8_t>(ResponseType::EngineHealth), 0x09);
}

TEST(EngineHealthProtocolTest, EncodesEmptyHealth)
{
    BufferWriter w;
    WriteEngineHealth(w, EngineHealth{});

    const DecodedHealth decoded = Decode(w);

    EXPECT_EQ(decoded.responseType, static_cast<uint8_t>(ResponseType::EngineHealth));
    EXPECT_EQ(decoded.payloadSize, decoded.payloadBytesRemaining);
    EXPECT_TRUE(decoded.healthy);
    EXPECT_TRUE(decoded.subsystems.empty());
}

TEST(EngineHealthProtocolTest, EncodesEachSubsystem)
{
    const EngineHealth health{.subsystems = {
        {.name = "Window", .state = SubsystemState::Running},
        {.name = "Renderer", .state = SubsystemState::Running, .detail = "OpenGL"},
        {.name = "Audio", .state = SubsystemState::Disabled, .detail = "Headless mode"},
        {.name = "Physics", .state = SubsystemState::Failed, .detail = "PhysX failed"},
        {.name = "Scripting", .state = SubsystemState::NotStarted},
    }};

    BufferWriter w;
    WriteEngineHealth(w, health);
    const DecodedHealth decoded = Decode(w);

    EXPECT_EQ(decoded.payloadSize, decoded.payloadBytesRemaining);
    EXPECT_FALSE(decoded.healthy);
    ASSERT_EQ(decoded.subsystems.size(), 5u);

    EXPECT_EQ(decoded.subsystems[0].name, "Window");
    EXPECT_EQ(decoded.subsystems[0].state, "Running");
    EXPECT_EQ(decoded.subsystems[0].detail, "");

    EXPECT_EQ(decoded.subsystems[1].detail, "OpenGL");
    EXPECT_EQ(decoded.subsystems[2].state, "Disabled");

    EXPECT_EQ(decoded.subsystems[3].name, "Physics");
    EXPECT_EQ(decoded.subsystems[3].state, "Failed");
    EXPECT_EQ(decoded.subsystems[3].detail, "PhysX failed");

    EXPECT_EQ(decoded.subsystems[4].state, "NotStarted");
}

TEST(EngineHealthProtocolTest, HealthyFlagMatchesEngineHealth)
{
    const EngineHealth health{.subsystems = {
        {.name = "Audio", .state = SubsystemState::Disabled},
    }};

    BufferWriter w;
    WriteEngineHealth(w, health);

    EXPECT_TRUE(Decode(w).healthy);
}
