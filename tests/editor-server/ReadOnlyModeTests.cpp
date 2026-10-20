#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

// Read-only mode (--read-only) without a socket: the command classification (an allow list with default deny) and the
// refusal, through EditorServer::ExecuteCommand. EditorServerHardeningTests.cpp covers the same host over a socket
// (Shutdown, the connection staying usable, the access token). HostOptionsTests.cpp covers the option.

namespace
{
    using Class = EditorServer::CommandClass;

    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);

    // The test's own copy of the classification: a new command of protocol.json has to be added here AND to
    // EditorServer's table, and each side is checked against the other and against the spec. Read means the command
    // answers from what the host holds and changes nothing a client could save, run or lose.
    const std::map<std::string, Class> &ExpectedClasses()
    {
        static const std::map<std::string, Class> classes = {
            // Read
            {"RenderFrame", Class::Read}, {"SetViewportSize", Class::Read}, {"GetAudio", Class::Read},
            {"Hello", Class::Read}, {"PollEvents", Class::Read}, {"RenderFrameIfChanged", Class::Read},
            {"GetCameraPosition", Class::Read}, {"SetEditorCamera", Class::Read}, {"GetEditorCamera", Class::Read},
            {"PickEntity", Class::Read}, {"GetEntityBounds", Class::Read}, {"GetCurrentScene", Class::Read},
            {"GetHierarchy", Class::Read}, {"GetOpenScene", Class::Read}, {"GetEntityTransform", Class::Read},
            {"GetAllEntities", Class::Read}, {"GetEntity", Class::Read}, {"GetEngineHealth", Class::Read},
            {"GetComponentTypes", Class::Read}, {"GetComponent", Class::Read}, {"GetLuaFields", Class::Read},
            {"GetProjectInfo", Class::Read}, {"GetHistory", Class::Read}, {"GetAutosave", Class::Read},
            {"ListAssets", Class::Read}, {"GetAssetInfo", Class::Read}, {"ReadTextAsset", Class::Read},
            {"GetPlayState", Class::Read},
            // The extension lists answer from what was captured at load and run no Lua
            {"ListEditorExtensions", Class::Read}, {"GetEditorUi", Class::Read},
            // Write: the project, a scene, an asset, the game, or project code
            {"SetCameraPosition", Class::Write}, {"CreateScene", Class::Write}, {"LoadScene", Class::Write},
            {"SaveScene", Class::Write}, {"DeleteScene", Class::Write}, {"OpenScene", Class::Write},
            {"SaveSceneToFile", Class::Write}, {"NewScene", Class::Write}, {"CreateEntity", Class::Write},
            {"DestroyEntity", Class::Write}, {"SetEntityTransform", Class::Write}, {"CreateEntityEx", Class::Write},
            {"SetEntityParent", Class::Write}, {"SetEntityProperties", Class::Write},
            {"DuplicateEntity", Class::Write}, {"SetLocalTransform", Class::Write}, {"CreateScript", Class::Write},
            {"RescanAssets", Class::Write}, {"SetImportSettings", Class::Write}, {"WriteTextAsset", Class::Write},
            {"CreateScriptAsset", Class::Write}, {"CreateFolder", Class::Write}, {"AddComponent", Class::Write},
            {"RemoveComponent", Class::Write}, {"SetComponentFields", Class::Write},
            {"SetProjectSettings", Class::Write}, {"SetStartupScene", Class::Write}, {"Undo", Class::Write},
            {"Redo", Class::Write}, {"BeginEditGroup", Class::Write}, {"EndEditGroup", Class::Write},
            {"RestoreAutosave", Class::Write}, {"DiscardAutosave", Class::Write},
            {"WritePlaySnapshot", Class::Write}, {"SetPaused", Class::Write}, {"Step", Class::Write},
            {"SendInput", Class::Write},
            // Admin: the host itself
            {"Shutdown", Class::Admin}, {"ReloadEditorExtensions", Class::Admin},
        };
        return classes;
    }

    json LoadSpec()
    {
        std::ifstream file(N2_PROTOCOL_JSON);
        return json::parse(file);
    }

    uint8_t IdOf(const json &command)
    {
        return static_cast<uint8_t>(std::stoi(command.at("id").get<std::string>(), nullptr, 16));
    }

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return std::string(payload.begin(), payload.end()); }
    };

    Frame Decode(const std::vector<uint8_t> &frame)
    {
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const uint32_t size = r.ReadU32();
        const std::span<const uint8_t> body = r.ReadBytes(size);
        out.payload.assign(body.begin(), body.end());
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the response payload";
        return out;
    }

    Frame Execute(EditorServer &server, uint8_t id, const std::vector<uint8_t> &payload = {})
    {
        return Decode(server.ExecuteCommand(id, payload));
    }

    Frame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        return Execute(server, static_cast<uint8_t>(type), payload);
    }

    std::vector<uint8_t> HelloPayload(const bool sendErrorCodes, const bool withTrailingField = true)
    {
        BufferWriter w;
        w.WriteString("read-only test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString("");
        if (withTrailingField)
            w.WriteBool(sendErrorCodes);
        return w.Release();
    }

    struct Coded
    {
        std::string message;
        std::string code;
    };

    Coded DecodeErrorCoded(const Frame &frame)
    {
        EXPECT_EQ(frame.type, ErrorCodedType);
        BufferReader r(frame.payload);
        Coded coded;
        coded.message = r.ReadString();
        coded.code = r.ReadString();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after ErrorCoded";
        return coded;
    }

    bool IsDenial(const Frame &frame)
    {
        if (frame.type == ErrorCodedType)
            return true;
        return frame.type == ErrorType && frame.Text().starts_with("read-only host:");
    }
}

// ==================== The classification ====================

// The guard that keeps a new command from shipping unclassified: every command id of protocol.json must have an
// explicit class in EditorServer (ClassifyCommand has a value), and the class must be the one this test lists. A command
// that is in the spec but in neither table fails here, by name.
TEST(CommandClassificationTest, EveryCommandOfTheSpecHasAnExplicitClass)
{
    const json spec = LoadSpec();
    for (const auto &[name, command] : spec.at("commands").items())
    {
        const uint8_t id = IdOf(command);

        const auto expected = ExpectedClasses().find(name);
        ASSERT_NE(expected, ExpectedClasses().end())
            << name << " is a command nobody has classified: add it to ExpectedClasses() here and to the table in "
            << "EditorServer.cpp as Read, Write or Admin (a read changes nothing a client could save, run or lose)";

        const std::optional<Class> actual = EditorServer::ClassifyCommand(id);
        ASSERT_TRUE(actual.has_value()) << name << " has no class in EditorServer's command table (so --read-only refuses it)";
        EXPECT_EQ(*actual, expected->second) << name;
        EXPECT_EQ(EditorServer::IsReadCommand(id), expected->second == Class::Read) << name;
    }

    for (const auto &[name, commandClass] : ExpectedClasses())
    {
        (void)commandClass;
        EXPECT_TRUE(spec.at("commands").contains(name)) << name << " is classified here but isn't in protocol.json";
    }
}

// Default deny: an id that is no command, or that nobody classified, is not a read
TEST(CommandClassificationTest, OnlyClassifiedReadsAreReads)
{
    const json spec = LoadSpec();
    std::set<uint8_t> known;
    for (const auto &[name, command] : spec.at("commands").items())
        known.insert(IdOf(command));

    size_t reads = 0;
    for (int value = 0; value <= 0xFF; ++value)
    {
        const auto id = static_cast<uint8_t>(value);
        if (known.contains(id))
        {
            if (EditorServer::IsReadCommand(id))
                ++reads;
            continue;
        }
        EXPECT_FALSE(EditorServer::ClassifyCommand(id).has_value()) << "0x" << std::hex << value;
        EXPECT_FALSE(EditorServer::IsReadCommand(id)) << "0x" << std::hex << value << " isn't a command, so it isn't a read";
    }

    size_t expectedReads = 0;
    for (const auto &[name, commandClass] : ExpectedClasses())
        expectedReads += commandClass == Class::Read ? 1 : 0;
    EXPECT_EQ(reads, expectedReads);
}

TEST(CommandClassificationTest, NothingThatChangesStateOrRunsCodeIsARead)
{
    for (const char *name : {"SendInput", "WritePlaySnapshot", "SetPaused", "Step", "Shutdown", "RescanAssets", "Undo",
                             "Redo", "SaveScene", "SaveSceneToFile", "SetImportSettings", "WriteTextAsset",
                             "CreateScriptAsset", "CreateFolder", "LoadScene", "OpenScene", "NewScene", "DeleteScene",
                             "SetProjectSettings", "SetStartupScene", "RestoreAutosave", "DiscardAutosave",
                             "SetCameraPosition", "CreateScript", "BeginEditGroup", "EndEditGroup"})
    {
        const auto found = ExpectedClasses().find(name);
        ASSERT_NE(found, ExpectedClasses().end()) << name;
        EXPECT_NE(found->second, Class::Read) << name;
    }
}

// Not the play host's deny list: that one lists 13 commands and is applied only in play mode
TEST(CommandClassificationTest, TheReadListIsNotTheEditOnlyList)
{
    const json spec = LoadSpec();
    for (const auto &[name, commandClass] : ExpectedClasses())
    {
        (void)commandClass;
        const uint8_t id = IdOf(spec.at("commands").at(name));
        if (EditorServer::IsEditOnlyCommand(id))
        {
            EXPECT_FALSE(EditorServer::IsReadCommand(id)) << name << " writes project files, so it can't be a read";
        }
    }
}

// ==================== The refusal ====================

TEST(ReadOnlyHostTest, AHostServesWritesUnlessItIsReadOnly)
{
    EditorServer server;
    EXPECT_FALSE(server.IsReadOnly());
    // A write that needs no scene, project or engine: it runs, and nothing refuses it
    BufferWriter name;
    name.WriteString("player");
    EXPECT_EQ(Execute(server, CommandType::CreateScript, name.Release()).type,
              static_cast<uint8_t>(ResponseType::ScriptData));

    EXPECT_TRUE(server.SetReadOnly(true));
    EXPECT_TRUE(server.IsReadOnly());
    EXPECT_TRUE(server.SetReadOnly(false));
    EXPECT_FALSE(server.IsReadOnly());
}

// With no Hello, or a Hello of a client that doesn't know ErrorCoded (no trailing field), every refusal is a plain Error
// whose message says why, so an older client shows the reason
TEST(ReadOnlyHostTest, EveryNonReadIsRefusedWithAReadableErrorForAClientThatDidNotAskForCodes)
{
    EditorServer server;
    ASSERT_TRUE(server.SetReadOnly(true));
    const json spec = LoadSpec();

    for (const bool sentHello : {false, true})
    {
        if (sentHello)
        {
            ASSERT_EQ(Execute(server, CommandType::Hello, HelloPayload(false, false)).type,
                      static_cast<uint8_t>(ResponseType::ServerInfo));
        }

        for (const auto &[name, command] : spec.at("commands").items())
        {
            if (EditorServer::IsReadCommand(IdOf(command)))
                continue;
            const Frame response = Execute(server, IdOf(command));
            EXPECT_EQ(response.type, ErrorType) << name;
            EXPECT_EQ(response.Text(), "read-only host: " + name + " is not allowed") << name;
        }
    }
}

TEST(ReadOnlyHostTest, EveryNonReadIsRefusedWithTheReadOnlyCodeForAClientThatAsked)
{
    EditorServer server;
    ASSERT_TRUE(server.SetReadOnly(true));
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloPayload(true)).type, static_cast<uint8_t>(ResponseType::ServerInfo));
    const json spec = LoadSpec();

    size_t refused = 0;
    size_t reads = 0;
    for (const auto &[name, command] : spec.at("commands").items())
    {
        if (EditorServer::IsReadCommand(IdOf(command)))
        {
            ++reads;
            continue;
        }
        const Coded coded = DecodeErrorCoded(Execute(server, IdOf(command)));
        EXPECT_EQ(coded.code, std::string{ErrorCodeReadOnly}) << name;
        EXPECT_EQ(coded.message, "read-only host: " + name + " is not allowed") << name;
        ++refused;
    }
    EXPECT_GT(refused, 0u);
    EXPECT_EQ(refused + reads, spec.at("commands").size()) << "every command is a read or is refused";
}

// A refusal comes before any handler: a write with a payload that would change the host doesn't run
TEST(ReadOnlyHostTest, ARefusedWriteDoesNotRun)
{
    EditorServer server;
    ASSERT_TRUE(server.SetReadOnly(true));
    BufferWriter name;
    name.WriteString("player");
    const Frame refused = Execute(server, CommandType::CreateScript, name.Release());
    EXPECT_TRUE(IsDenial(refused)) << refused.Text();
}

TEST(ReadOnlyHostTest, AnUnknownCommandIsRefusedNotRun)
{
    EditorServer server;
    ASSERT_TRUE(server.SetReadOnly(true));
    const Frame legacy = Execute(server, uint8_t{0x7E});
    EXPECT_EQ(legacy.type, ErrorType);
    EXPECT_EQ(legacy.Text(), "read-only host: command 0x7E is not allowed");

    ASSERT_EQ(Execute(server, CommandType::Hello, HelloPayload(true)).type, static_cast<uint8_t>(ResponseType::ServerInfo));
    const Coded coded = DecodeErrorCoded(Execute(server, uint8_t{0x7E}));
    EXPECT_EQ(coded.code, std::string{ErrorCodeReadOnly});
}

TEST(ReadOnlyHostTest, ReadsStillRunAndTheHostKeepsServing)
{
    EditorServer server;
    ASSERT_TRUE(server.SetReadOnly(true));
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloPayload(true)).type, static_cast<uint8_t>(ResponseType::ServerInfo));

    // Reads that need no engine: they answer on their own terms (an Error for "No scene loaded" is not a refusal)
    EXPECT_EQ(Execute(server, CommandType::GetAllEntities).type, static_cast<uint8_t>(ResponseType::EntityList));
    EXPECT_EQ(Execute(server, CommandType::GetComponentTypes).type, static_cast<uint8_t>(ResponseType::ComponentTypes));
    EXPECT_FALSE(IsDenial(Execute(server, CommandType::GetHierarchy)));

    BufferWriter poll;
    poll.WriteU32(0);
    poll.WriteU32(0);
    poll.WriteU32(10);
    EXPECT_EQ(Execute(server, CommandType::PollEvents, poll.Release()).type, static_cast<uint8_t>(ResponseType::Events));

    // A refusal in between changes nothing: the next read still works
    EXPECT_EQ(Execute(server, CommandType::DestroyEntity).type, ErrorCodedType);
    EXPECT_EQ(Execute(server, CommandType::GetAllEntities).type, static_cast<uint8_t>(ResponseType::EntityList));
}

TEST(ReadOnlyHostTest, HelloSaysTheHostIsReadOnly)
{
    for (const bool readOnly : {false, true})
    {
        EditorServer server;
        ASSERT_TRUE(server.SetReadOnly(readOnly));
        const Frame info = Execute(server, CommandType::Hello, HelloPayload(false));
        ASSERT_EQ(info.type, static_cast<uint8_t>(ResponseType::ServerInfo));
        // readOnly, then the empty user (4 bytes) and the role "admin" (4 + 5 bytes) of a host with no tokens file
        ASSERT_GE(info.payload.size(), 14u);
        EXPECT_EQ(info.payload[info.payload.size() - 14], readOnly ? uint8_t{1} : uint8_t{0}) << "ServerInfo.readOnly";
    }
}

// ==================== The wire format ====================

TEST(ErrorCodedTest, TheFrameIsTheMessageThenTheCodeAsLengthPrefixedStrings)
{
    BufferWriter w;
    WriteErrorCoded(w, "no", ErrorCodeReadOnly);
    const std::vector<uint8_t> expected = {
        0x1C, 0x12, 0x00, 0x00, 0x00,  // ErrorCoded, 18 payload bytes
        0x02, 0x00, 0x00, 0x00, 'n', 'o',
        0x08, 0x00, 0x00, 0x00, 'r', 'e', 'a', 'd', 'O', 'n', 'l', 'y',
    };
    EXPECT_EQ(std::vector<uint8_t>(w.Data().begin(), w.Data().end()), expected);
}

TEST(ErrorCodedTest, ErrorKeepsItsRawPayload)
{
    // The reason ErrorCoded exists: Error's payload is the bare message, with no room for a field after it
    BufferWriter w;
    WriteError(w, "abc");
    const std::vector<uint8_t> expected = {0x01, 0x03, 0x00, 0x00, 0x00, 'a', 'b', 'c'};
    EXPECT_EQ(std::vector<uint8_t>(w.Data().begin(), w.Data().end()), expected);
}

TEST(ErrorCodedTest, TheCodesAreTheSpecsStableWords)
{
    const json spec = LoadSpec();
    std::set<std::string> codes;
    for (const auto &[code, description] : spec.at("errorCodes").items())
    {
        (void)description;
        codes.insert(code);
    }
    EXPECT_EQ(codes, (std::set<std::string>{std::string{ErrorCodeReadOnly}, std::string{ErrorCodeForbidden},
                                            std::string{ErrorCodeBusy}}));
    EXPECT_EQ(std::string{ErrorCodeReadOnly}, "readOnly");
    EXPECT_EQ(std::string{ErrorCodeForbidden}, "forbidden");
    EXPECT_EQ(std::string{ErrorCodeBusy}, "busy");
}

TEST(ErrorCodedTest, HelloTakesTheCapabilityAsATrailingFieldThatDefaultsToFalse)
{
    // A client of an older minor version sends three fields
    {
        const std::vector<uint8_t> payload = HelloPayload(false, false);
        BufferReader r(payload);
        const HelloCmd cmd = HelloCmd::Deserialize(r);
        EXPECT_EQ(cmd.clientName, "read-only test");
        EXPECT_FALSE(cmd.errorCodes);
        EXPECT_FALSE(r.HasData());
    }
    for (const bool asked : {false, true})
    {
        const std::vector<uint8_t> payload = HelloPayload(asked);
        BufferReader r(payload);
        const HelloCmd cmd = HelloCmd::Deserialize(r);
        EXPECT_EQ(cmd.errorCodes, asked);
        EXPECT_FALSE(r.HasData());
    }
}
