#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "n2server/ServerOptions.hpp"

using namespace N2Engine;
using namespace N2Engine::Server;

// N2Server's command line as plain logic (no engine start): the defaults, every option, the fail-closed bind rule, and
// the errors. Server.cpp's RunServer is covered end to end by the smoke test (tests/smoke/n2server_smoke.py).

namespace
{
    std::expected<ServerOptions, std::string> Parse(std::vector<std::string> args)
    {
        return ParseServerArguments(args);
    }

    /// The arguments of a server that would serve on loopback
    std::vector<std::string> Serving(std::vector<std::string> more = {})
    {
        std::vector<std::string> args = {"--project", "game"};
        args.insert(args.end(), more.begin(), more.end());
        return args;
    }

    std::string ErrorOf(const std::vector<std::string> &args)
    {
        const auto parsed = ParseServerArguments(args);
        EXPECT_FALSE(parsed.has_value());
        return parsed ? std::string() : parsed.error();
    }
}

TEST(N2ServerOptionsTest, TheDefaultsAreLoopbackWithNoRelayAndNoSecrets)
{
    const auto parsed = Parse(Serving());
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->projectPath, "game");
    EXPECT_TRUE(parsed->scene.empty());
    EXPECT_EQ(parsed->bindAddress, "127.0.0.1");
    EXPECT_EQ(parsed->port, ServerOptions::DefaultPort);
    EXPECT_EQ(parsed->port, 7777);
    EXPECT_TRUE(parsed->tokensFile.empty());
    EXPECT_EQ(parsed->maxClients, ServerOptions::DefaultMaxClients);
    EXPECT_EQ(parsed->protocolId, ServerOptions::DefaultProtocolId);
    EXPECT_FALSE(parsed->insecureBind);
    EXPECT_FALSE(parsed->relay);
    EXPECT_FALSE(parsed->exitOnStdinEof);
    EXPECT_EQ(parsed->logLevel, Logger::LogLevel::Info);
    EXPECT_TRUE(parsed->logFile.empty());
    EXPECT_TRUE(parsed->newTokenUser.empty());
    EXPECT_FALSE(parsed->showHelp);
}

TEST(N2ServerOptionsTest, EveryOptionIsRead)
{
    const auto parsed = Parse({"--project", "games/arena", "--scene", "res://scenes/arena.scene", "--bind", "10.0.0.5",
                               "--port", "9000", "--tokens-file", "tokens.json", "--max-clients", "32",
                               "--protocol-id", "0xCAFE0001", "--log-level", "warn", "--log-file", "server.log",
                               "--exit-on-stdin-eof"});
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->projectPath, "games/arena");
    EXPECT_EQ(parsed->scene, "res://scenes/arena.scene");
    EXPECT_EQ(parsed->bindAddress, "10.0.0.5");
    EXPECT_EQ(parsed->port, 9000);
    EXPECT_EQ(parsed->tokensFile, "tokens.json");
    EXPECT_EQ(parsed->maxClients, 32);
    EXPECT_EQ(parsed->protocolId, 0xCAFE0001u);
    EXPECT_EQ(parsed->logLevel, Logger::LogLevel::Warn);
    EXPECT_EQ(parsed->logFile, "server.log");
    EXPECT_TRUE(parsed->exitOnStdinEof);
}

TEST(N2ServerOptionsTest, ShortPortAndPortZero)
{
    const auto shortForm = Parse(Serving({"-p", "8123"}));
    ASSERT_TRUE(shortForm.has_value()) << shortForm.error();
    EXPECT_EQ(shortForm->port, 8123);

    const auto any = Parse(Serving({"--port", "0"}));
    ASSERT_TRUE(any.has_value()) << any.error();
    EXPECT_EQ(any->port, 0);

    EXPECT_NE(ErrorOf(Serving({"--port", "65536"})).find("Invalid port"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--port", "-1"})).find("Invalid port"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--port", "80x"})).find("Invalid port"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--port", ""})).find("Invalid port"), std::string::npos);
}

TEST(N2ServerOptionsTest, HelpWinsOverEverythingElse)
{
    const auto parsed = Parse({"--port", "nonsense", "--bogus", "-h"});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->showHelp);
    EXPECT_FALSE(ServerUsage().empty());

    const auto longForm = Parse({"--help"});
    ASSERT_TRUE(longForm.has_value());
    EXPECT_TRUE(longForm->showHelp);

    // Every option the parser takes is in the usage text
    const std::string usage(ServerUsage());
    for (const char *option : {"--project", "--scene", "--bind", "--port", "--tokens-file", "--max-clients", "--protocol-id",
                               "--insecure-bind", "--relay", "--relay-players", "--relay-stall", "--relay-stall-ms", "--relay-layout", "--relay-spectators", "--relay-snapshot-max-mb", "--relay-snapshot-timeout-ms", "--exit-on-stdin-eof", "--log-level", "--log-file",
                               "--new-token"})
    {
        EXPECT_NE(usage.find(option), std::string::npos) << option;
    }
}

TEST(N2ServerOptionsTest, AProjectIsRequiredToServe)
{
    EXPECT_NE(ErrorOf({}).find("--project is required"), std::string::npos);
    EXPECT_NE(ErrorOf({"--port", "9000"}).find("--project is required"), std::string::npos);
}

TEST(N2ServerOptionsTest, AnOptionWithoutAValueIsAnError)
{
    EXPECT_NE(ErrorOf({"--project"}).find("--project is missing a value"), std::string::npos);
    // "--project --port 0" is a forgotten path, not a folder called "--port"
    EXPECT_NE(ErrorOf({"--project", "--port", "0"}).find("--project is missing a value"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--bind"})).find("--bind is missing a value"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--bind", ""})).find("--bind is missing a value"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--tokens-file", ""})).find("--tokens-file is missing a value"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--log-file", ""})).find("--log-file is missing a value"), std::string::npos);
    EXPECT_NE(ErrorOf({"--project", ""}).find("--project is missing a value"), std::string::npos);
}

TEST(N2ServerOptionsTest, UnknownOptionsAndStrayArgumentsAreErrorsNotIgnored)
{
    // Unlike N2EditorHost: a typo must not leave a server running with settings the operator thought they had changed
    const std::string typo = ErrorOf(Serving({"--tokens-flie", "tokens.json"}));
    EXPECT_NE(typo.find("Unknown option"), std::string::npos);
    EXPECT_NE(typo.find("--tokens-flie"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"stray"})).find("Unknown option"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--tls"})).find("Unknown option"), std::string::npos);

    // What an error repeats of the arguments has its control characters written out
    const std::string escaped = ErrorOf(Serving({std::string("--bad\nline")}));
    EXPECT_EQ(escaped.find('\n'), std::string::npos);
    EXPECT_NE(escaped.find("\\n"), std::string::npos);
}

TEST(N2ServerOptionsTest, MaxClientsIsAWholeNumberFromOneToTheLimit)
{
    for (const char *value : {"1", "16", "1024"})
    {
        const auto parsed = Parse(Serving({"--max-clients", value}));
        ASSERT_TRUE(parsed.has_value()) << value << ": " << parsed.error();
    }
    for (const char *value : {"0", "1025", "-3", "ten", "1.5", " 4", "4 ", ""})
    {
        EXPECT_NE(ErrorOf(Serving({"--max-clients", value})).find("Invalid --max-clients"), std::string::npos)
            << "[" << value << "]";
    }
}

TEST(N2ServerOptionsTest, ProtocolIdIsDecimalOrHexOfAtMost32Bits)
{
    EXPECT_EQ(ParseProtocolId("0"), std::optional<std::uint32_t>(0u));
    EXPECT_EQ(ParseProtocolId("1311919700"), std::optional<std::uint32_t>(0x4E324E54u));
    EXPECT_EQ(ParseProtocolId("4294967295"), std::optional<std::uint32_t>(0xFFFFFFFFu));
    EXPECT_EQ(ParseProtocolId("0x4E324E54"), std::optional<std::uint32_t>(0x4E324E54u));
    EXPECT_EQ(ParseProtocolId("0xffffffff"), std::optional<std::uint32_t>(0xFFFFFFFFu));
    EXPECT_EQ(ParseProtocolId("0x1"), std::optional<std::uint32_t>(1u));

    for (const char *text : {"", "0x", "4294967296", "0x100000000", "-1", "+1", "1e3", "0X10", "12 ", " 12", "0xg", "abc",
                             "0x-1"})
    {
        EXPECT_FALSE(ParseProtocolId(text).has_value()) << "[" << text << "]";
    }
    EXPECT_NE(ErrorOf(Serving({"--protocol-id", "banana"})).find("Invalid --protocol-id"), std::string::npos);
    EXPECT_EQ(ServerOptions::DefaultProtocolId, 0x4E324E54u);
}

TEST(N2ServerOptionsTest, SceneMustBeAResScenePath)
{
    EXPECT_TRUE(Parse(Serving({"--scene", "res://levels/one.scene"})).has_value());
    for (const char *value : {"levels/one.scene", "res://levels/one.png", "res://../one.scene", "user://one.scene",
                              "res://", "C:/game/one.scene"})
    {
        EXPECT_NE(ErrorOf(Serving({"--scene", value})).find("Invalid --scene"), std::string::npos) << value;
    }
}

TEST(N2ServerOptionsTest, LogLevelNamesAreExact)
{
    const std::vector<std::pair<std::string, Logger::LogLevel>> levels = {
        {"info", Logger::LogLevel::Info}, {"warn", Logger::LogLevel::Warn}, {"error", Logger::LogLevel::Error}};
    for (const auto &entry : levels)
    {
        const auto parsed = Parse(Serving({"--log-level", entry.first}));
        ASSERT_TRUE(parsed.has_value()) << entry.first;
        EXPECT_EQ(parsed->logLevel, entry.second) << entry.first;
    }
    for (const char *text : {"debug", "INFO", "warning", ""})
    {
        EXPECT_NE(ErrorOf(Serving({"--log-level", text})).find("Invalid --log-level"), std::string::npos) << text;
    }
}

TEST(N2ServerOptionsTest, ANonLoopbackBindIsRefusedWithoutATokensFileOrTheOverride)
{
    for (const char *address : {"0.0.0.0", "10.1.2.3", "192.168.0.7", "::", "2001:db8::1", "localhost", "example.com"})
    {
        const std::string error = ErrorOf(Serving({"--bind", address}));
        EXPECT_NE(error.find("Refusing to bind"), std::string::npos) << address;
        EXPECT_NE(error.find("--tokens-file"), std::string::npos) << address;
        EXPECT_NE(error.find("--insecure-bind"), std::string::npos) << address;

        // With a tokens file it is allowed
        const auto withTokens = Parse(Serving({"--bind", address, "--tokens-file", "tokens.json"}));
        EXPECT_TRUE(withTokens.has_value()) << address;
        // ...and so it is with the explicit override
        const auto overridden = Parse(Serving({"--bind", address, "--insecure-bind"}));
        ASSERT_TRUE(overridden.has_value()) << address;
        EXPECT_TRUE(overridden->insecureBind);
    }
}

TEST(N2ServerOptionsTest, LoopbackNeedsNoTokensFile)
{
    for (const char *address : {"127.0.0.1", "127.9.9.9", "::1", "::ffff:127.0.0.1"})
    {
        const auto parsed = Parse(Serving({"--bind", address}));
        ASSERT_TRUE(parsed.has_value()) << address << ": " << parsed.error();
        EXPECT_EQ(parsed->bindAddress, address);
    }
}

TEST(N2ServerOptionsTest, NoOptionTurnsTheDefaultIntoAWorldFacingBind)
{
    // Nothing but --bind changes the address: every other option leaves it on loopback
    const auto parsed = Parse(Serving({"--port", "0", "--max-clients", "4", "--tokens-file", "t.json", "--insecure-bind"}));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->bindAddress, "127.0.0.1");
}

TEST(N2ServerOptionsTest, RelayAndItsOptionsParse)
{
    const auto parsed = Parse(Serving({"--relay"}));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_TRUE(parsed->relay);
    EXPECT_EQ(parsed->relayPlayers, 2);
    EXPECT_FALSE(parsed->relayStallStops);
    EXPECT_EQ(parsed->relayStallMs, 5000);

    const auto tuned = Parse(Serving({"--relay", "--relay-players", "4", "--relay-stall", "stop", "--relay-stall-ms", "1500",
                                      "--relay-layout", "layout.json"}));
    ASSERT_TRUE(tuned.has_value()) << tuned.error();
    EXPECT_EQ(tuned->relayPlayers, 4);
    EXPECT_TRUE(tuned->relayStallStops);
    EXPECT_EQ(tuned->relayStallMs, 1500);
    EXPECT_EQ(tuned->relayLayoutFile, "layout.json");
}

TEST(N2ServerOptionsTest, RelayOptionsAreCheckedAndNeedRelay)
{
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-players", "0"})).find("--relay-players"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-players", "5"})).find("--relay-players"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-stall", "pause"})).find("--relay-stall"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-stall-ms", "5"})).find("--relay-stall-ms"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay-stall", "stop"})).find("only apply with --relay"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-layout"})).find("missing a value"), std::string::npos);
}

TEST(N2ServerOptionsTest, RelaySpectatorOptionsParseWithTheDocumentedDefaultsAndBounds)
{
    const auto defaults = Parse(Serving({"--relay"}));
    ASSERT_TRUE(defaults.has_value()) << defaults.error();
    EXPECT_EQ(defaults->relaySpectators, 4);
    EXPECT_EQ(defaults->relaySnapshotMaxMb, 16);
    EXPECT_EQ(defaults->relaySnapshotTimeoutMs, 10000);

    const auto tuned = Parse(Serving({"--relay", "--relay-spectators", "0", "--relay-snapshot-max-mb", "64",
                                      "--relay-snapshot-timeout-ms", "1000"}));
    ASSERT_TRUE(tuned.has_value()) << tuned.error();
    EXPECT_EQ(tuned->relaySpectators, 0);
    EXPECT_EQ(tuned->relaySnapshotMaxMb, 64);
    EXPECT_EQ(tuned->relaySnapshotTimeoutMs, 1000);
    const auto most = Parse(Serving({"--relay", "--relay-spectators", "16", "--relay-snapshot-max-mb", "1",
                                     "--relay-snapshot-timeout-ms", "120000"}));
    ASSERT_TRUE(most.has_value()) << most.error();
    EXPECT_EQ(most->relaySpectators, 16);

    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-spectators", "17"})).find("--relay-spectators"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-spectators", "-1"})).find("--relay-spectators"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-snapshot-max-mb", "0"})).find("--relay-snapshot-max-mb"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-snapshot-max-mb", "65"})).find("--relay-snapshot-max-mb"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-snapshot-timeout-ms", "999"})).find("--relay-snapshot-timeout-ms"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-snapshot-timeout-ms", "abc"})).find("--relay-snapshot-timeout-ms"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay", "--relay-spectators"})).find("missing a value"), std::string::npos);
    // Without --relay each one is an error
    EXPECT_NE(ErrorOf(Serving({"--relay-spectators", "0"})).find("only apply with --relay"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay-snapshot-max-mb", "8"})).find("only apply with --relay"), std::string::npos);
    EXPECT_NE(ErrorOf(Serving({"--relay-snapshot-timeout-ms", "2000"})).find("only apply with --relay"), std::string::npos);
}

TEST(N2ServerOptionsTest, NewTokenTakesAUserAndARoleAndNeedsATokensFile)
{
    const auto parsed = Parse({"--new-token", "alice", "player", "--tokens-file", "tokens.json"});
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->newTokenUser, "alice");
    EXPECT_EQ(parsed->newTokenRole, N2Net::JoinRole::Player);
    // No project needed: the server is not started
    EXPECT_TRUE(parsed->projectPath.empty());

    const auto spectator = Parse({"--tokens-file", "tokens.json", "--new-token", "bob@example.com", "spectator"});
    ASSERT_TRUE(spectator.has_value()) << spectator.error();
    EXPECT_EQ(spectator->newTokenRole, N2Net::JoinRole::Spectator);
    const auto admin = Parse({"--new-token", "root", "admin", "--tokens-file", "t.json"});
    ASSERT_TRUE(admin.has_value()) << admin.error();
    EXPECT_EQ(admin->newTokenRole, N2Net::JoinRole::Admin);

    EXPECT_NE(ErrorOf({"--new-token", "alice", "player"}).find("--new-token needs --tokens-file"), std::string::npos);
    EXPECT_NE(ErrorOf({"--new-token", "alice"}).find("--new-token needs a user and a role"), std::string::npos);
    EXPECT_NE(ErrorOf({"--new-token"}).find("--new-token needs a user and a role"), std::string::npos);
    // A value starting with "--" is the next option
    EXPECT_NE(ErrorOf({"--new-token", "alice", "--tokens-file", "t.json"}).find("--new-token needs a user and a role"),
              std::string::npos);
    EXPECT_NE(ErrorOf({"--new-token", "alice", "editor", "--tokens-file", "t.json"}).find("the role must be admin, player or spectator"),
              std::string::npos);
    EXPECT_NE(ErrorOf({"--new-token", "al ice", "player", "--tokens-file", "t.json"}).find("the user must be"),
              std::string::npos);
    EXPECT_NE(ErrorOf({"--new-token", "", "player", "--tokens-file", "t.json"}).find("the user must be"), std::string::npos);
}

TEST(N2ServerOptionsTest, TheReadyLineIsAPrefixAndKeyValueFields)
{
    EXPECT_EQ(FormatReadyLine(7777, "127.0.0.1"), "N2Server ready port=7777 bind=127.0.0.1");
    EXPECT_EQ(FormatReadyLine(0, "::1"), "N2Server ready port=0 bind=::1");
    EXPECT_TRUE(FormatReadyLine(9, "127.0.0.1").starts_with(ReadyLinePrefix));
    EXPECT_EQ(ReadyLinePrefix, "N2Server ready");
}


TEST(N2ServerOptionsTest, WebSocketListenerIsOptInAndValidatesPortAndOrigin)
{
    const auto parsed=ParseServerArguments({"--project","project","--websocket-port","0","--websocket-origin","https://game.example"});
    ASSERT_TRUE(parsed);EXPECT_EQ(parsed->webSocketPort,0);EXPECT_EQ(parsed->webSocketOrigin,"https://game.example");
    EXPECT_FALSE(ParseServerArguments({"--project","project","--websocket-port","65536"}));
    EXPECT_FALSE(ParseServerArguments({"--project","project","--websocket-origin","https://game.example\r\nInjected: yes"}));
}
