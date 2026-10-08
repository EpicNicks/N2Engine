#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/Logger.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;

// Hello and the access token, through ExecuteCommand (no socket). The per-connection gate on the network thread is
// tested over a socket in EditorServerHardeningTests.cpp (EditorServerSocketTest).
namespace
{
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
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the response payload";
        return out;
    }

    std::vector<uint8_t> HelloPayload(const std::string &protocolVersion, const std::string &token = "",
                                      const std::string &clientName = "session test")
    {
        BufferWriter w;
        w.WriteString(clientName);
        w.WriteString(protocolVersion);
        w.WriteString(token);
        return w.Release();
    }

    Frame Hello(EditorServer &server, const std::string &protocolVersion, const std::string &token = "")
    {
        return Execute(server, CommandType::Hello, HelloPayload(protocolVersion, token));
    }

    struct ServerInfo
    {
        std::string protocolVersion;
        std::string engineVersion;
        nlohmann::json capabilities;
        bool projectLoaded = false;
    };

    ServerInfo DecodeServerInfo(const Frame &frame)
    {
        BufferReader r(frame.payload);
        ServerInfo info;
        info.protocolVersion = r.ReadString();
        info.engineVersion = r.ReadString();
        info.capabilities = ReadJson(r);
        info.projectLoaded = r.ReadBool();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after ServerInfo";
        return info;
    }

    uint32_t OurMajorVersion()
    {
        return ParseProtocolVersion(ProtocolVersion).value().majorVersion;
    }

    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
}

TEST(EditorSessionTest, HelloAnswersWithTheServersVersionsAndCapabilities)
{
    EditorServer server;
    const Frame response = Hello(server, std::string{ProtocolVersion});
    ASSERT_EQ(response.type, ServerInfoType) << response.Text();

    const ServerInfo info = DecodeServerInfo(response);
    EXPECT_EQ(info.protocolVersion, ProtocolVersion);
    EXPECT_EQ(info.engineVersion, EditorServer::EngineVersion());
    EXPECT_FALSE(info.engineVersion.empty());
    EXPECT_TRUE(ParseProtocolVersion(info.engineVersion).has_value()) << info.engineVersion;
    EXPECT_EQ(info.capabilities, nlohmann::json::array()) << "no optional features are defined yet";
    EXPECT_FALSE(info.projectLoaded) << "a server without SetProject has no project";
}

TEST(EditorSessionTest, HelloAcceptsAnyMinorAndPatchVersionOfTheSameMajor)
{
    EditorServer server;
    const uint32_t major = OurMajorVersion();
    for (const std::string version : {std::format("{}.0.0", major), std::format("{}.999.42", major)})
    {
        EXPECT_EQ(Hello(server, version).type, ServerInfoType) << version;
    }
}

TEST(EditorSessionTest, HelloRefusesAnotherMajorVersion)
{
    EditorServer server;
    const uint32_t major = OurMajorVersion();
    for (const std::string version : {std::format("{}.0.0", major + 1), std::format("{}.1.0", major == 0 ? 7 : major - 1)})
    {
        const Frame response = Hello(server, version);
        EXPECT_EQ(response.type, ErrorType) << version;
        EXPECT_NE(response.Text().find("Protocol version mismatch"), std::string::npos) << response.Text();
        EXPECT_NE(response.Text().find(std::string{ProtocolVersion}), std::string::npos) << response.Text();
    }
}

TEST(EditorSessionTest, HelloRefusesAVersionThatIsntMajorMinorPatch)
{
    EditorServer server;
    for (const std::string version : {"", "1", "1.1", "latest", "1.1.1-beta"})
    {
        const Frame response = Hello(server, version);
        EXPECT_EQ(response.type, ErrorType) << version;
        EXPECT_NE(response.Text().find("Invalid protocol version"), std::string::npos) << response.Text();
    }
}

TEST(EditorSessionTest, WithoutAnAccessTokenHelloAcceptsAnyToken)
{
    EditorServer server;
    EXPECT_FALSE(server.RequiresAccessToken());
    EXPECT_EQ(Hello(server, std::string{ProtocolVersion}, "").type, ServerInfoType);
    EXPECT_EQ(Hello(server, std::string{ProtocolVersion}, "anything").type, ServerInfoType);
}

TEST(EditorSessionTest, WithAnAccessTokenHelloNeedsThatToken)
{
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("correct horse"));
    EXPECT_TRUE(server.RequiresAccessToken());

    for (const std::string token : {"", "correct", "correct horse ", "Correct horse", "wrong"})
    {
        const Frame response = Hello(server, std::string{ProtocolVersion}, token);
        EXPECT_EQ(response.type, ErrorType) << "'" << token << "'";
        EXPECT_EQ(response.Text(), "Invalid access token");
    }
    EXPECT_EQ(Hello(server, std::string{ProtocolVersion}, "correct horse").type, ServerInfoType);
}

TEST(EditorSessionTest, TheTokenIsCheckedBeforeTheVersion)
{
    // A client without the token learns nothing, not even which version the server speaks
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("token"));
    const Frame response = Hello(server, "999.0.0", "not the token");
    EXPECT_EQ(response.type, ErrorType);
    EXPECT_EQ(response.Text(), "Invalid access token");
}

TEST(EditorSessionTest, HelloNeverLogsTheToken)
{
    std::vector<std::string> lines;
    const size_t id = Logger::logEvent += [&lines](const std::string_view message, Logger::LogLevel)
    {
        lines.emplace_back(message);
    };

    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("the-access-token"));
    (void)Hello(server, std::string{ProtocolVersion}, "a-wrong-token");
    (void)Hello(server, std::string{ProtocolVersion}, "the-access-token");
    Logger::logEvent -= id;

    EXPECT_FALSE(lines.empty()) << "Hello logs who said it";
    for (const std::string &line : lines)
    {
        EXPECT_EQ(line.find("token-"), std::string::npos) << line;
        EXPECT_EQ(line.find("-token"), std::string::npos) << line;
    }
}

TEST(EditorSessionTest, AMalformedHelloIsAnError)
{
    EditorServer server;
    std::vector<uint8_t> payload = HelloPayload(std::string{ProtocolVersion});
    payload.pop_back(); // the token's last byte (an empty token is its length alone, so cut into that)
    EXPECT_EQ(Execute(server, CommandType::Hello, payload).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::Hello).type, ErrorType);
}

TEST(EditorSessionTest, TokensMatchOnlyWhenEqual)
{
    EXPECT_TRUE(EditorServer::TokensMatch("abc", "abc"));
    EXPECT_TRUE(EditorServer::TokensMatch("", ""));
    EXPECT_FALSE(EditorServer::TokensMatch("abc", "abd"));
    EXPECT_FALSE(EditorServer::TokensMatch("abc", "ab"));
    EXPECT_FALSE(EditorServer::TokensMatch("abc", "abcd"));
    EXPECT_FALSE(EditorServer::TokensMatch("abc", ""));
    EXPECT_FALSE(EditorServer::TokensMatch("", "abc"));
    EXPECT_FALSE(EditorServer::TokensMatch(std::string_view("a\0c", 3), "a"));
}

TEST(EditorSessionTest, OnlyHelloIsAllowedBeforeHello)
{
    EXPECT_TRUE(EditorServer::IsAllowedBeforeHello(static_cast<uint8_t>(CommandType::Hello)));
    for (const CommandType other : {CommandType::Shutdown, CommandType::GetEngineHealth, CommandType::RenderFrame,
                                    CommandType::DestroyEntity, CommandType::LoadScene})
    {
        EXPECT_FALSE(EditorServer::IsAllowedBeforeHello(static_cast<uint8_t>(other))) << static_cast<int>(other);
    }
    EXPECT_FALSE(EditorServer::IsAllowedBeforeHello(0x7E));
}

TEST(EditorSessionTest, TheAccessTokenCantChangeWhileServing)
{
    EditorServer server;
    ASSERT_TRUE(server.Start(0));
    EXPECT_FALSE(server.SetAccessToken("too late"));
    EXPECT_FALSE(server.RequiresAccessToken());
    server.Stop();
    EXPECT_TRUE(server.SetAccessToken("now"));
}

TEST(EditorSessionTest, AnotherMinorVersionIsAcceptedWithAWarning)
{
    std::vector<Logger::LogLevel> levels;
    std::vector<std::string> lines;
    const size_t id = Logger::logEvent += [&](const std::string_view message, const Logger::LogLevel level)
    {
        lines.emplace_back(message);
        levels.push_back(level);
    };

    EditorServer server;
    const ProtocolVersionNumber ours = ParseProtocolVersion(ProtocolVersion).value();
    const Frame response = Hello(server, std::format("{}.{}.0", ours.majorVersion, ours.minorVersion + 1));
    Logger::logEvent -= id;

    EXPECT_EQ(response.type, ServerInfoType) << response.Text();
    bool warned = false;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].find("said Hello with protocol") != std::string::npos)
            warned = levels[i] == Logger::LogLevel::Warn;
    }
    EXPECT_TRUE(warned) << "a client of another minor version is accepted, with a warning";
}

TEST(EditorSessionTest, AnOverlongVersionIsRefusedAndNotEchoedInFull)
{
    // from_chars accepts any number of leading zeros: such a version is refused, and neither the Error nor the log
    // carries all of it
    std::vector<std::string> lines;
    const size_t id = Logger::logEvent += [&lines](const std::string_view message, Logger::LogLevel)
    {
        lines.emplace_back(message);
    };

    EditorServer server;
    const std::string version = "1." + std::string(100000, '0') + "1.0";
    const Frame response = Hello(server, version);
    Logger::logEvent -= id;

    EXPECT_EQ(response.type, ErrorType);
    EXPECT_NE(response.Text().find("Invalid protocol version"), std::string::npos);
    EXPECT_LT(response.Text().size(), 200u) << "the Error echoes the whole version";
    for (const std::string &line : lines)
    {
        EXPECT_LT(line.size(), 400u) << "a log line carries the whole version";
    }
}

TEST(EditorSessionTest, ClientTextIsSanitisedForTheLog)
{
    EXPECT_EQ(EditorServer::SanitizeForLog("editor"), "editor");
    EXPECT_EQ(EditorServer::SanitizeForLog("two\nlines\r\tand\x1B[31m\x7F"), "two?lines??and?[31m?");
    EXPECT_EQ(EditorServer::SanitizeForLog("abcdef", 4), "abcd...");
    EXPECT_EQ(EditorServer::SanitizeForLog("abcd", 4), "abcd");

    // "a", then U+00E9 (2 bytes), then U+2713 (3 bytes): a cut never splits a character
    const std::string text = "a\xC3\xA9\xE2\x9C\x93";
    EXPECT_EQ(EditorServer::SanitizeForLog(text, 2), "a...");
    EXPECT_EQ(EditorServer::SanitizeForLog(text, 3), "a\xC3\xA9...");
    EXPECT_EQ(EditorServer::SanitizeForLog(text, 5), "a\xC3\xA9...");
    EXPECT_EQ(EditorServer::SanitizeForLog(text, 6), text);
}

TEST(EditorSessionTest, TheHelloTimeoutIsPositiveAndSetBeforeServing)
{
    EditorServer server;
    EXPECT_EQ(server.GetHelloTimeout(), EditorServer::DefaultHelloTimeout);
    EXPECT_FALSE(server.SetHelloTimeout(std::chrono::milliseconds::zero()));
    EXPECT_TRUE(server.SetHelloTimeout(std::chrono::milliseconds(250)));
    EXPECT_EQ(server.GetHelloTimeout(), std::chrono::milliseconds(250));

    ASSERT_TRUE(server.Start(0));
    EXPECT_FALSE(server.SetHelloTimeout(std::chrono::milliseconds(500)));
    server.Stop();
}
