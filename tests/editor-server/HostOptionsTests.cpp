#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <editor-server/EditorServer.hpp>
#include <editor-server/EditorTls.hpp>
#include <editor-server/Host.hpp>
#include <editor-server/HostOptions.hpp>

using namespace N2Engine::Editor;

namespace
{
    /// False if the variable couldn't be set (callers ASSERT on it)
    [[nodiscard]] bool SetTestVariable(const char *name, const char *value)
    {
#ifdef _WIN32
        return _putenv_s(name, value) == 0;
#else
        return setenv(name, value, 1) == 0;
#endif
    }

    void UnsetTestVariable(const char *name)
    {
#ifdef _WIN32
        (void)_putenv_s(name, "");
#else
        (void)unsetenv(name);
#endif
    }
}

TEST(HostOptionsTest, ParsePortAcceptsTheWholeRangeIncludingZero)
{
    EXPECT_EQ(ParsePort("0"), 0);
    EXPECT_EQ(ParsePort("1"), 1);
    EXPECT_EQ(ParsePort("9999"), 9999);
    EXPECT_EQ(ParsePort("65535"), 65535);
}

TEST(HostOptionsTest, ParsePortRejectsAnythingButAPlainDecimalPort)
{
    EXPECT_FALSE(ParsePort(""));
    EXPECT_FALSE(ParsePort("65536"));
    EXPECT_FALSE(ParsePort("-1"));
    EXPECT_FALSE(ParsePort("-0"));
    EXPECT_FALSE(ParsePort("+80"));
    EXPECT_FALSE(ParsePort(" 80"));
    EXPECT_FALSE(ParsePort("80 "));
    EXPECT_FALSE(ParsePort("80x"));
    EXPECT_FALSE(ParsePort("0x50"));
    EXPECT_FALSE(ParsePort("99999999999"));
}

TEST(HostOptionsTest, NoArgumentsGiveTheDefaults)
{
    const auto parsed = ParseHostArguments({});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->port, HostOptions::DefaultPort);
    EXPECT_EQ(parsed->port, 9999);
    EXPECT_EQ(parsed->bindAddress, EditorServer::DefaultBindAddress);
    EXPECT_TRUE(parsed->projectPath.empty());
    EXPECT_EQ(parsed->renderer, HostRenderer::OpenGL);
    EXPECT_TRUE(parsed->tokenEnv.empty());
    EXPECT_FALSE(parsed->exitOnDisconnect);
    EXPECT_FALSE(parsed->exitOnStdinEof);
    EXPECT_FALSE(parsed->showHelp);
}

TEST(HostOptionsTest, ParsesEveryOption)
{
    const auto parsed = ParseHostArguments({"--port", "0", "--bind", "0.0.0.0", "--project", "C:/Games/My Game",
                                            "--renderer", "software", "--token-env", "N2_EDITOR_TOKEN",
                                            "--exit-on-disconnect", "--exit-on-stdin-eof", "--tls"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->port, 0);
    EXPECT_EQ(parsed->bindAddress, "0.0.0.0");
    EXPECT_EQ(parsed->projectPath, "C:/Games/My Game");
    EXPECT_EQ(parsed->renderer, HostRenderer::Software);
    EXPECT_EQ(parsed->tokenEnv, "N2_EDITOR_TOKEN");
    EXPECT_TRUE(parsed->exitOnDisconnect);
    EXPECT_TRUE(parsed->exitOnStdinEof);
    EXPECT_FALSE(parsed->showHelp);

    const auto shortPort = ParseHostArguments({"-p", "4000"});
    ASSERT_TRUE(shortPort) << shortPort.error();
    EXPECT_EQ(shortPort->port, 4000);
}

TEST(HostOptionsTest, HelpIsReportedNotActedOn)
{
    const auto longForm = ParseHostArguments({"--help"});
    ASSERT_TRUE(longForm);
    EXPECT_TRUE(longForm->showHelp);

    const auto shortForm = ParseHostArguments({"--port", "1234", "-h"});
    ASSERT_TRUE(shortForm);
    EXPECT_TRUE(shortForm->showHelp);
}

TEST(HostOptionsTest, HelpWinsOverParseErrors)
{
    for (const std::vector<std::string> &args : {std::vector<std::string>{"--port", "70000", "--help"},
                                                 std::vector<std::string>{"-h", "--project"},
                                                 std::vector<std::string>{"--project", "--port", "-h"}})
    {
        const auto parsed = ParseHostArguments(args);
        ASSERT_TRUE(parsed) << parsed.error();
        EXPECT_TRUE(parsed->showHelp);
    }
}

TEST(HostOptionsTest, AnOptionFollowedByAnotherOptionIsMissingItsValue)
{
    const auto parsed = ParseHostArguments({"--project", "--port", "0"});
    ASSERT_FALSE(parsed) << parsed->projectPath;
    EXPECT_EQ(parsed.error(), "--project is missing a value");

    const auto bind = ParseHostArguments({"--bind", "--project", "C:/game"});
    ASSERT_FALSE(bind);
    EXPECT_EQ(bind.error(), "--bind is missing a value");

    // A flag is an option too: not a variable called "--exit-on-disconnect"
    const auto tokenEnv = ParseHostArguments({"--token-env", "--exit-on-disconnect"});
    ASSERT_FALSE(tokenEnv) << tokenEnv->tokenEnv;
    EXPECT_EQ(tokenEnv.error(), "--token-env is missing a value");
}

TEST(HostOptionsTest, AnInvalidPortIsAnErrorNamingIt)
{
    const auto parsed = ParseHostArguments({"--port", "70000"});
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().find("70000"), std::string::npos) << parsed.error();
}

TEST(HostOptionsTest, AnOptionMissingItsValueIsAnError)
{
    for (const char *option : {"--port", "-p", "--bind", "--project", "--renderer", "--token-env"})
    {
        const auto parsed = ParseHostArguments({option});
        ASSERT_FALSE(parsed) << option;
        EXPECT_EQ(parsed.error(), std::string(option) + " is missing a value");
    }
}

TEST(HostOptionsTest, UnknownArgumentsAreIgnored)
{
    const auto parsed = ParseHostArguments({"--future-flag", "--port", "1234"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->port, 1234);
}

TEST(HostOptionsTest, TheReadyLineHasTheDocumentedFormat)
{
    // Launchers (the Electron editor, the CI smoke test) parse this exact text; see logging-and-editor.html
    EXPECT_EQ(FormatReadyLine(54321), "N2EditorHost ready port=54321");
    EXPECT_EQ(FormatReadyLine(9999).rfind(ReadyLinePrefix, 0), 0u);
}

TEST(HostOptionsTest, ParseRendererTakesExactlyTheTwoNames)
{
    EXPECT_EQ(ParseRenderer("opengl"), HostRenderer::OpenGL);
    EXPECT_EQ(ParseRenderer("software"), HostRenderer::Software);

    for (const std::string_view other : {"", "OpenGL", "Software", "vulkan", "gl", "software "})
    {
        EXPECT_FALSE(ParseRenderer(other)) << other;
    }
}

TEST(HostOptionsTest, AnInvalidRendererIsAnErrorNamingIt)
{
    const auto parsed = ParseHostArguments({"--renderer", "vulkan"});
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error(), "Invalid renderer: vulkan (expected opengl or software)");
}

TEST(HostOptionsTest, ALaterRendererWins)
{
    const auto parsed = ParseHostArguments({"--renderer", "software", "--renderer", "opengl"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->renderer, HostRenderer::OpenGL);
}

TEST(HostOptionsTest, TheUsageNamesEveryOption)
{
    const std::string_view usage = HostUsage();
    for (const std::string_view option : {"--port", "-p,", "--bind", "--project", "--renderer", "--token-env",
                                          "--exit-on-disconnect", "--exit-on-stdin-eof", "--help", "-h,",
                                          "N2EditorHost ready port="})
    {
        EXPECT_NE(usage.find(option), std::string_view::npos) << option;
    }
}

TEST(HostOptionsTest, ExitOnStdinEofIsAFlagAndOffByDefault)
{
    // Not a value-taking option: the next argument is still parsed on its own
    const auto parsed = ParseHostArguments({"--exit-on-stdin-eof", "--port", "0"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_TRUE(parsed->exitOnStdinEof);
    EXPECT_FALSE(parsed->exitOnDisconnect);
    EXPECT_EQ(parsed->port, 0);

    const auto without = ParseHostArguments({"--exit-on-disconnect"});
    ASSERT_TRUE(without) << without.error();
    EXPECT_FALSE(without->exitOnStdinEof);
}

TEST(HostOptionsTest, DrainUntilEofReadsEverythingThenReturnsAtEndOfFile)
{
    int calls = 0;
    std::size_t total = 0;
    DrainUntilEof(
        [&](char *buffer, std::size_t size) -> std::ptrdiff_t
        {
            ++calls;
            EXPECT_NE(buffer, nullptr);
            EXPECT_GT(size, 0u);
            if (calls <= 3)
            {
                total += 10;
                return 10;
            }
            return 0;
        });
    EXPECT_EQ(calls, 4);
    EXPECT_EQ(total, 30u);
}

TEST(HostOptionsTest, DrainUntilEofReturnsOnAReadError)
{
    int calls = 0;
    DrainUntilEof(
        [&](char *, std::size_t) -> std::ptrdiff_t
        {
            ++calls;
            return calls == 1 ? 5 : -1;
        });
    EXPECT_EQ(calls, 2);
}

TEST(HostOptionsTest, ReadAccessTokenReadsTheVariableThenRemovesIt)
{
    constexpr const char *name = "N2_HOST_OPTIONS_TEST_TOKEN";
    ASSERT_TRUE(SetTestVariable(name, "s3cret token"));

    const auto token = ReadAccessToken(name);
    ASSERT_TRUE(token) << token.error();
    EXPECT_EQ(*token, "s3cret token");

    // Gone from the environment, so nothing the host runs can read it
    const auto again = ReadAccessToken(name);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error(), std::string("--token-env: the environment variable ") + name + " is not set");
}

TEST(HostOptionsTest, ReadAccessTokenRefusesAnUnsetVariable)
{
    constexpr const char *name = "N2_HOST_OPTIONS_TEST_UNSET";
    UnsetTestVariable(name);
    const auto token = ReadAccessToken(name);
    ASSERT_FALSE(token);
    EXPECT_NE(token.error().find(name), std::string::npos) << token.error();
}

#ifndef _WIN32
// On Windows, setting a variable to "" removes it, so an empty one can't exist there (that is the unset case)
TEST(HostOptionsTest, ReadAccessTokenRefusesAnEmptyVariable)
{
    constexpr const char *name = "N2_HOST_OPTIONS_TEST_EMPTY";
    ASSERT_TRUE(SetTestVariable(name, ""));
    const auto token = ReadAccessToken(name);
    ASSERT_FALSE(token);
    EXPECT_EQ(token.error(), std::string("--token-env: the environment variable ") + name + " is empty");
}
#endif

TEST(HostOptionsTest, ReadAccessTokenRefusesWhatIsntAVariableName)
{
    for (const std::string name : {"", "A=B", "="})
    {
        const auto token = ReadAccessToken(name);
        ASSERT_FALSE(token) << name;
        EXPECT_NE(token.error().find("is not an environment variable name"), std::string::npos) << token.error();
    }
}

// ==================== --create ====================

TEST(HostOptionsTest, CreateTakesAFolderANameAndAnId)
{
    const auto parsed = ParseHostArguments({"--create", "C:/Games/My Game", "--name", "My Game", "--project-id",
                                            "8E0C3A8E-0B1F-4F5E-9D0E-3F6F1C7D2A10"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->createPath, "C:/Games/My Game");
    EXPECT_EQ(parsed->projectName, "My Game");
    ASSERT_TRUE(parsed->projectId.has_value());
    EXPECT_EQ(parsed->projectId->ToString(), "8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10");
    EXPECT_FALSE(parsed->projectIdFromPath);
    EXPECT_TRUE(parsed->projectPath.empty());
}

TEST(HostOptionsTest, CreateDefaultsToTheFolderNameAndARandomId)
{
    const auto parsed = ParseHostArguments({"--create", "game"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->createPath, "game");
    EXPECT_TRUE(parsed->projectName.empty());
    EXPECT_FALSE(parsed->projectId.has_value());
    EXPECT_FALSE(parsed->projectIdFromPath);
}

TEST(HostOptionsTest, ProjectIdFromPathAsksForTheFoldersOldNamespace)
{
    const auto parsed = ParseHostArguments({"--create", "game", "--project-id", "from-path"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_TRUE(parsed->projectIdFromPath);
    EXPECT_FALSE(parsed->projectId.has_value());
}

TEST(HostOptionsTest, AnInvalidProjectIdIsAnError)
{
    for (const char *id : {"not-a-uuid", "00000000-0000-0000-0000-000000000000", "8e0c3a8e0b1f4f5e9d0e3f6f1c7d2a10",
                           "From-Path"})
    {
        const auto parsed = ParseHostArguments({"--create", "game", "--project-id", id});
        ASSERT_FALSE(parsed) << id;
        EXPECT_EQ(parsed.error().rfind("Invalid --project-id: ", 0), 0u) << parsed.error();
    }
}

TEST(HostOptionsTest, CreateAndProjectCantBeCombined)
{
    const auto parsed = ParseHostArguments({"--create", "a", "--project", "b"});
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().find("--create and --project"), std::string::npos) << parsed.error();
}

TEST(HostOptionsTest, NameAndProjectIdOnlyGoWithCreate)
{
    const auto name = ParseHostArguments({"--project", "b", "--name", "x"});
    ASSERT_FALSE(name);
    EXPECT_EQ(name.error(), "--name only goes with --create");

    const auto id = ParseHostArguments({"--project-id", "from-path"});
    ASSERT_FALSE(id);
    EXPECT_EQ(id.error(), "--project-id only goes with --create");
}

TEST(HostOptionsTest, CreateOptionsMissingTheirValueAreErrors)
{
    for (const char *option : {"--create", "--name", "--project-id"})
    {
        const auto parsed = ParseHostArguments({option});
        ASSERT_FALSE(parsed) << option;
        EXPECT_EQ(parsed.error(), std::string(option) + " is missing a value");
    }

    // An empty folder isn't "no --create": that would start a host instead
    const auto empty = ParseHostArguments({"--create", ""});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error(), "--create is missing a value");
}

TEST(HostOptionsTest, TheCreatedLineHasTheDocumentedFormat)
{
    // The Electron launcher parses this exact text; see editor.html#create
    const auto id = N2Engine::Math::UUID::FromString("8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10").value();
    EXPECT_EQ(FormatCreatedLine(id, "res://scenes/Main.scene"),
              "N2EditorHost created projectId=8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10 startupScene=res://scenes/Main.scene");
    EXPECT_EQ(FormatCreatedLine(id, ""), "N2EditorHost created projectId=8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10");
    // A value never holds a space: such a startup scene is left out rather than breaking the fields
    EXPECT_EQ(FormatCreatedLine(id, "res://my scenes/A.scene"),
              "N2EditorHost created projectId=8e0c3a8e-0b1f-4f5e-9d0e-3f6f1c7d2a10");
    EXPECT_EQ(FormatCreatedLine(id, "").rfind(CreatedLinePrefix, 0), 0u);
}

TEST(HostOptionsTest, TheUsageNamesTheCreateOptions)
{
    const std::string_view usage = HostUsage();
    for (const std::string_view option : {"--create", "--name", "--project-id", "from-path", "N2EditorHost created"})
    {
        EXPECT_NE(usage.find(option), std::string_view::npos) << option;
    }
}

TEST(HostOptionsTest, PlayTakesASnapshotFileAndIsOffByDefault)
{
    const auto none = ParseHostArguments({"--project", "p"});
    ASSERT_TRUE(none) << none.error();
    EXPECT_TRUE(none->playScene.empty());

    const auto parsed = ParseHostArguments({"--project", "C:/Games/My Game", "--play",
                                            "C:/Games/My Game/.n2/play/Main.scene", "--port", "0", "--renderer",
                                            "software"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->playScene, "C:/Games/My Game/.n2/play/Main.scene");
    EXPECT_EQ(parsed->projectPath, "C:/Games/My Game");
    EXPECT_EQ(parsed->renderer, HostRenderer::Software) << "the play host is told the renderer like any host";

    const auto resPath = ParseHostArguments({"--project", "p", "--play", "res://scenes/Main.scene"});
    ASSERT_TRUE(resPath) << resPath.error();
    EXPECT_EQ(resPath->playScene, "res://scenes/Main.scene");
}

TEST(HostOptionsTest, PlayMissingItsValueIsAnError)
{
    const auto atTheEnd = ParseHostArguments({"--project", "p", "--play"});
    ASSERT_FALSE(atTheEnd);
    EXPECT_NE(atTheEnd.error().find("--play"), std::string::npos) << atTheEnd.error();

    const auto beforeAnOption = ParseHostArguments({"--play", "--port", "0"});
    ASSERT_FALSE(beforeAnOption);
    EXPECT_NE(beforeAnOption.error().find("--play"), std::string::npos) << beforeAnOption.error();
}

TEST(HostOptionsTest, CreateAndPlayCantBeCombined)
{
    const auto parsed = ParseHostArguments({"--create", "a", "--play", "b.scene"});
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().find("--create and --play"), std::string::npos) << parsed.error();
}

TEST(HostOptionsTest, TheUsageNamesPlay)
{
    const std::string_view usage = HostUsage();
    EXPECT_NE(usage.find("--play"), std::string_view::npos);
    EXPECT_NE(usage.find("WritePlaySnapshot"), std::string_view::npos);
}

// ==================== --tls (editor TLS) ====================

TEST(HostOptionsTest, TlsIsAFlagAndOffByDefault)
{
    const auto defaults = ParseHostArguments({});
    ASSERT_TRUE(defaults) << defaults.error();
    EXPECT_FALSE(defaults->tls);

    // Not a value-taking option: the next argument is still parsed on its own
    // A non-loopback bind needs a token with --tls (or --insecure-bind), so this one has a token
    const auto parsed =
        ParseHostArguments({"--tls", "--port", "1234", "--bind", "0.0.0.0", "--token-env", "N2_EDITOR_TOKEN"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_TRUE(parsed->tls);
    EXPECT_EQ(parsed->port, 1234);
    EXPECT_EQ(parsed->bindAddress, "0.0.0.0");

    const auto last = ParseHostArguments({"--port", "1234", "--tls"});
    ASSERT_TRUE(last) << last.error();
    EXPECT_TRUE(last->tls);
}

// ==================== --no-rate-limit ====================

TEST(HostOptionsTest, NoRateLimitIsAFlagAndOffByDefault)
{
    const auto defaults = ParseHostArguments({});
    ASSERT_TRUE(defaults) << defaults.error();
    EXPECT_FALSE(defaults->noRateLimit) << "rate limiting is on unless it is turned off";

    const auto parsed = ParseHostArguments({"--no-rate-limit", "--port", "1234", "--token-env", "N2_TOKEN"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_TRUE(parsed->noRateLimit);
    EXPECT_EQ(parsed->port, 1234);
    EXPECT_EQ(parsed->tokenEnv, "N2_TOKEN");

    const auto last = ParseHostArguments({"--port", "1234", "--no-rate-limit"});
    ASSERT_TRUE(last) << last.error();
    EXPECT_TRUE(last->noRateLimit);

    EXPECT_NE(HostUsage().find("--no-rate-limit"), std::string_view::npos);
}

// ==================== --idle-timeout, --write-timeout, --max-session ====================

TEST(HostOptionsTest, ConnectionLimitsDefaultToTheServersDefaults)
{
    const auto parsed = ParseHostArguments({});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->idleTimeoutSeconds, 300);
    EXPECT_EQ(parsed->writeTimeoutSeconds, 30);
    EXPECT_EQ(parsed->maxSessionMinutes, 0) << "no maximum session age unless asked for";
    // HostOptions spells the defaults out (to keep EditorServer.hpp out of its header); they must stay the server's
    EXPECT_EQ(std::chrono::seconds(parsed->idleTimeoutSeconds), EditorServer::DefaultIdleTimeout);
    EXPECT_EQ(std::chrono::seconds(parsed->writeTimeoutSeconds), EditorServer::DefaultWriteTimeout);
    EXPECT_EQ(std::chrono::seconds(HostOptions::DefaultIdleTimeoutSeconds), EditorServer::DefaultIdleTimeout);
    EXPECT_EQ(std::chrono::seconds(HostOptions::DefaultWriteTimeoutSeconds), EditorServer::DefaultWriteTimeout);
}

TEST(HostOptionsTest, ParseWholeNumberTakesDigitsInsideTheRangeAndNothingElse)
{
    EXPECT_EQ(ParseWholeNumber("0", 0, 10), 0);
    EXPECT_EQ(ParseWholeNumber("10", 0, 10), 10);
    EXPECT_EQ(ParseWholeNumber("007", 0, 10), 7);
    EXPECT_FALSE(ParseWholeNumber("11", 0, 10));
    EXPECT_FALSE(ParseWholeNumber("0", 1, 10)) << "below the minimum";
    for (const char *bad : {"", "-1", "-0", "+5", " 5", "5 ", "5x", "1.5", "1e3", "0x5", "abc", "99999999999"})
    {
        EXPECT_FALSE(ParseWholeNumber(bad, 0, 1000000)) << "'" << bad << "'";
    }
}

TEST(HostOptionsTest, IdleTimeoutTakesWholeSecondsAndZeroTurnsItOff)
{
    const auto five = ParseHostArguments({"--idle-timeout", "5"});
    ASSERT_TRUE(five) << five.error();
    EXPECT_EQ(five->idleTimeoutSeconds, 5);

    const auto off = ParseHostArguments({"--idle-timeout", "0", "--port", "1234"});
    ASSERT_TRUE(off) << off.error();
    EXPECT_EQ(off->idleTimeoutSeconds, 0);
    EXPECT_EQ(off->port, 1234);

    const auto biggest = ParseHostArguments({"--idle-timeout", std::to_string(HostOptions::MaxIdleTimeoutSeconds)});
    ASSERT_TRUE(biggest) << biggest.error();
    EXPECT_EQ(biggest->idleTimeoutSeconds, HostOptions::MaxIdleTimeoutSeconds);

    const auto later = ParseHostArguments({"--idle-timeout", "5", "--idle-timeout", "9"});
    ASSERT_TRUE(later) << later.error();
    EXPECT_EQ(later->idleTimeoutSeconds, 9);
}

TEST(HostOptionsTest, WriteTimeoutTakesWholeSecondsOfAtLeastOne)
{
    const auto ten = ParseHostArguments({"--write-timeout", "10"});
    ASSERT_TRUE(ten) << ten.error();
    EXPECT_EQ(ten->writeTimeoutSeconds, 10);

    const auto one = ParseHostArguments({"--write-timeout", "1"});
    ASSERT_TRUE(one) << one.error();
    EXPECT_EQ(one->writeTimeoutSeconds, 1);

    // A write with no deadline is what the flag exists to prevent: there is no 0 for "off"
    const auto zero = ParseHostArguments({"--write-timeout", "0"});
    ASSERT_FALSE(zero);
    EXPECT_NE(zero.error().find("--write-timeout"), std::string::npos) << zero.error();
}

TEST(HostOptionsTest, MaxSessionTakesWholeMinutesAndZeroIsOff)
{
    const auto hour = ParseHostArguments({"--max-session", "60"});
    ASSERT_TRUE(hour) << hour.error();
    EXPECT_EQ(hour->maxSessionMinutes, 60);

    const auto off = ParseHostArguments({"--max-session", "0"});
    ASSERT_TRUE(off) << off.error();
    EXPECT_EQ(off->maxSessionMinutes, 0);
}

TEST(HostOptionsTest, ConnectionLimitsRefuseGarbageNegativeAndTooLargeValues)
{
    const std::vector<std::pair<std::string, int>> flags{{"--idle-timeout", HostOptions::MaxIdleTimeoutSeconds},
                                                         {"--write-timeout", HostOptions::MaxWriteTimeoutSeconds},
                                                         {"--max-session", HostOptions::MaxSessionMinutesLimit}};
    for (const auto &[flag, largest] : flags)
    {
        for (const std::string &bad : {std::string{"abc"}, std::string{"-1"}, std::string{"-0"}, std::string{"+5"},
                                       std::string{"1.5"}, std::string{" 5"}, std::string{"5 "}, std::string{"1e3"},
                                       std::string{""}, std::string{"99999999999"}, std::to_string(largest + 1)})
        {
            const auto parsed = ParseHostArguments({flag, bad});
            ASSERT_FALSE(parsed) << flag << " " << bad;
            EXPECT_NE(parsed.error().find(flag), std::string::npos) << parsed.error();
        }

        const auto missing = ParseHostArguments({flag});
        ASSERT_FALSE(missing) << flag;
        EXPECT_EQ(missing.error(), flag + " is missing a value");

        // Another option is not its value
        const auto swallowed = ParseHostArguments({flag, "--port", "0"});
        ASSERT_FALSE(swallowed) << flag;
        EXPECT_EQ(swallowed.error(), flag + " is missing a value");
    }
}

TEST(HostOptionsTest, TheUsageNamesTheConnectionLimits)
{
    const std::string_view usage = HostUsage();
    for (const char *option : {"--idle-timeout", "--write-timeout", "--max-session"})
    {
        EXPECT_NE(usage.find(option), std::string_view::npos) << option;
    }
    EXPECT_NE(usage.find("Default: 300"), std::string_view::npos) << "the idle default is documented";
    EXPECT_NE(usage.find("Default: 30;"), std::string_view::npos) << "the write default is documented";
}

// ==================== --preauth-slots ====================

TEST(HostOptionsTest, PreAuthSlotsDefaultToTheServersDefault)
{
    const auto parsed = ParseHostArguments({});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->preAuthSlots, 4);
    // HostOptions spells the numbers out (to keep EditorServer.hpp out of its header); they must stay the server's
    EXPECT_EQ(static_cast<size_t>(parsed->preAuthSlots), EditorServer::DefaultPreAuthSlots);
    EXPECT_EQ(static_cast<size_t>(HostOptions::DefaultPreAuthSlots), EditorServer::DefaultPreAuthSlots);
    EXPECT_EQ(static_cast<size_t>(HostOptions::MaxPreAuthSlots), EditorServer::MaxPreAuthSlots);
}

TEST(HostOptionsTest, PreAuthSlotsTakeAWholeNumberFromOneToSixteen)
{
    for (const int slots : {1, 2, 4, 15, 16})
    {
        const auto parsed = ParseHostArguments({"--preauth-slots", std::to_string(slots), "--port", "1234"});
        ASSERT_TRUE(parsed) << slots << ": " << parsed.error();
        EXPECT_EQ(parsed->preAuthSlots, slots);
        EXPECT_EQ(parsed->port, 1234) << "the next option is still parsed";
    }
    const auto later = ParseHostArguments({"--preauth-slots", "2", "--preauth-slots", "9"});
    ASSERT_TRUE(later) << later.error();
    EXPECT_EQ(later->preAuthSlots, 9);
}

TEST(HostOptionsTest, PreAuthSlotsRefuseGarbageAndValuesOutsideTheRange)
{
    for (const std::string &bad : {std::string{"0"}, std::string{"17"}, std::string{"-1"}, std::string{"+4"},
                                   std::string{"abc"}, std::string{"1.5"}, std::string{" 4"}, std::string{"4 "},
                                   std::string{""}, std::string{"99999999999"}})
    {
        const auto parsed = ParseHostArguments({"--preauth-slots", bad});
        ASSERT_FALSE(parsed) << "'" << bad << "'";
        EXPECT_NE(parsed.error().find("--preauth-slots"), std::string::npos) << parsed.error();
    }

    const auto missing = ParseHostArguments({"--preauth-slots"});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error(), "--preauth-slots is missing a value");
    const auto swallowed = ParseHostArguments({"--preauth-slots", "--port", "0"});
    ASSERT_FALSE(swallowed);
    EXPECT_EQ(swallowed.error(), "--preauth-slots is missing a value");
}

TEST(HostOptionsTest, TheUsageNamesThePreAuthSlots)
{
    const std::string_view usage = HostUsage();
    EXPECT_NE(usage.find("--preauth-slots <n>"), std::string_view::npos);
    EXPECT_NE(usage.find("Default: 4"), std::string_view::npos) << "the default is documented";
    EXPECT_NE(usage.find("The host is serving another session"), std::string_view::npos) << "the Busy answer is documented";
}

// ==================== --read-only ====================

TEST(HostOptionsTest, ReadOnlyIsAFlagAndOffByDefault)
{
    const auto defaults = ParseHostArguments({});
    ASSERT_TRUE(defaults) << defaults.error();
    EXPECT_FALSE(defaults->readOnly);

    // Not a value-taking option: the next argument is still parsed on its own, and it goes with a token or without
    const auto parsed = ParseHostArguments({"--read-only", "--port", "1234", "--token-env", "N2_TOKEN"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_TRUE(parsed->readOnly);
    EXPECT_EQ(parsed->port, 1234);
    EXPECT_EQ(parsed->tokenEnv, "N2_TOKEN");

    const auto last = ParseHostArguments({"--port", "1234", "--read-only"});
    ASSERT_TRUE(last) << last.error();
    EXPECT_TRUE(last->readOnly);
}

TEST(HostOptionsTest, ReadOnlyIsRejectedWithPlayAndWithCreate)
{
    // A play host runs the game and the project's Lua, which is not read-only, whatever the argument order
    const auto play = ParseHostArguments({"--read-only", "--play", "snapshot.json"});
    ASSERT_FALSE(play);
    EXPECT_NE(play.error().find("--read-only and --play can't be combined"), std::string::npos) << play.error();
    const auto playFirst = ParseHostArguments({"--play", "snapshot.json", "--port", "1234", "--read-only"});
    ASSERT_FALSE(playFirst);
    EXPECT_NE(playFirst.error().find("--read-only and --play"), std::string::npos) << playFirst.error();

    // --create writes a project
    const auto create = ParseHostArguments({"--create", "proj", "--read-only"});
    ASSERT_FALSE(create);
    EXPECT_NE(create.error().find("--read-only and --create can't be combined"), std::string::npos) << create.error();

    // Each alone still parses
    EXPECT_TRUE(ParseHostArguments({"--play", "snapshot.json"}));
    EXPECT_TRUE(ParseHostArguments({"--create", "proj"}));
    EXPECT_TRUE(ParseHostArguments({"--project", "proj", "--read-only"}));
}

TEST(HostOptionsTest, TheUsageNamesReadOnly)
{
    const std::string_view usage = HostUsage();
    EXPECT_NE(usage.find("--read-only"), std::string_view::npos);
    EXPECT_NE(usage.find("readOnly"), std::string_view::npos);
}

TEST(HostOptionsTest, TheUsageNamesTls)
{
    const std::string_view usage = HostUsage();
    EXPECT_NE(usage.find("--tls"), std::string_view::npos);
    EXPECT_NE(usage.find("fingerprint"), std::string_view::npos);
    EXPECT_NE(usage.find("N2ENGINE_EDITOR_TLS"), std::string_view::npos);
}

TEST(HostOptionsTest, TheReadyLineOfATlsHostHasTheFingerprint)
{
    const std::string fingerprint(64, 'a');
    EXPECT_EQ(FormatReadyLine(54321, fingerprint), "N2EditorHost ready port=54321 tls=1 fingerprint=" + fingerprint);
    // Still starts with the prefix and the port, so a launcher that ignores unknown keys is unaffected
    EXPECT_EQ(FormatReadyLine(54321, fingerprint).rfind("N2EditorHost ready port=54321 ", 0), 0u);
    // No fingerprint, no TLS fields: the line a plaintext host prints is exactly as it was
    EXPECT_EQ(FormatReadyLine(54321, {}), "N2EditorHost ready port=54321");
}

TEST(HostOptionsTest, ABuildWithoutTlsRefusesToServeIt)
{
#ifdef N2ENGINE_EDITOR_TLS
    EXPECT_TRUE(EditorTlsServer::IsAvailable());
#else
    EXPECT_FALSE(EditorTlsServer::IsAvailable());

    // The library-level refusal RunHost relies on: an error, and the server stays plaintext
    EditorServer server;
    const auto enabled = server.EnableTls(std::filesystem::temp_directory_path() / "n2-no-tls-build");
    ASSERT_FALSE(enabled);
    EXPECT_NE(enabled.error().find("no TLS support"), std::string::npos) << enabled.error();
    EXPECT_FALSE(server.IsTlsEnabled());
    EXPECT_TRUE(server.TlsFingerprint().empty());
#endif
}

// ==================== --tokens-file and --new-token ====================

TEST(HostOptionsTest, TokensFileTakesAPathAndIsOffByDefault)
{
    const auto defaults = ParseHostArguments({});
    ASSERT_TRUE(defaults) << defaults.error();
    EXPECT_TRUE(defaults->tokensFile.empty());
    EXPECT_TRUE(defaults->newTokenUser.empty());

    const auto parsed = ParseHostArguments({"--tokens-file", "/etc/n2editorhost/tokens.json", "--port", "1234"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->tokensFile, "/etc/n2editorhost/tokens.json");
    EXPECT_EQ(parsed->port, 1234);
    EXPECT_TRUE(parsed->tokenEnv.empty());

    // It goes with --read-only and --tls: the file says who, those say what and how
    const auto together = ParseHostArguments({"--tokens-file", "tokens.json", "--read-only", "--tls"});
    ASSERT_TRUE(together) << together.error();
    EXPECT_TRUE(together->readOnly);
    EXPECT_TRUE(together->tls);
}

TEST(HostOptionsTest, TokensFileMissingItsValueIsAnError)
{
    for (const std::vector<std::string> &args : {std::vector<std::string>{"--tokens-file"},
                                                 std::vector<std::string>{"--tokens-file", "--tls"},
                                                 std::vector<std::string>{"--tokens-file", ""}})
    {
        const auto parsed = ParseHostArguments(args);
        ASSERT_FALSE(parsed) << args.size();
        EXPECT_NE(parsed.error().find("--tokens-file is missing a value"), std::string::npos) << parsed.error();
    }
}

TEST(HostOptionsTest, TokensFileAndTokenEnvCantBeCombined)
{
    // One host has one kind of token, whatever the argument order: a file with --token-env would leave it unclear which
    // of them gates Hello
    for (const std::vector<std::string> &args :
         {std::vector<std::string>{"--tokens-file", "tokens.json", "--token-env", "N2_TOKEN"},
          std::vector<std::string>{"--token-env", "N2_TOKEN", "--tokens-file", "tokens.json"}})
    {
        const auto parsed = ParseHostArguments(args);
        ASSERT_FALSE(parsed);
        EXPECT_NE(parsed.error().find("--tokens-file and --token-env can't be combined"), std::string::npos)
            << parsed.error();
    }
    EXPECT_TRUE(ParseHostArguments({"--token-env", "N2_TOKEN"}));
    EXPECT_TRUE(ParseHostArguments({"--tokens-file", "tokens.json"}));
}

TEST(HostOptionsTest, NewTokenTakesAUserAndARole)
{
    for (const auto &[role, expected] : {std::pair{"admin", TokenRole::Admin}, std::pair{"editor", TokenRole::Editor},
                                         std::pair{"viewer", TokenRole::Viewer}})
    {
        const auto parsed = ParseHostArguments({"--new-token", "alice", role});
        ASSERT_TRUE(parsed) << parsed.error();
        EXPECT_EQ(parsed->newTokenUser, "alice");
        EXPECT_EQ(parsed->newTokenRole, expected);
    }
    // Other options around it still parse
    const auto parsed = ParseHostArguments({"--port", "1234", "--new-token", "ci-runner", "viewer", "--read-only"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->newTokenUser, "ci-runner");
    EXPECT_EQ(parsed->port, 1234);
}

TEST(HostOptionsTest, NewTokenNeedsAValidUserAndRole)
{
    // Too few values, or an option where a value should be
    for (const std::vector<std::string> &args : {std::vector<std::string>{"--new-token"},
                                                 std::vector<std::string>{"--new-token", "alice"},
                                                 std::vector<std::string>{"--new-token", "alice", "--tls"},
                                                 std::vector<std::string>{"--new-token", "--tls", "editor"}})
    {
        const auto parsed = ParseHostArguments(args);
        ASSERT_FALSE(parsed) << args.size();
        EXPECT_NE(parsed.error().find("--new-token needs a user and a role"), std::string::npos) << parsed.error();
    }
    const auto badUser = ParseHostArguments({"--new-token", "al ice", "editor"});
    ASSERT_FALSE(badUser);
    EXPECT_NE(badUser.error().find("--new-token: the user"), std::string::npos) << badUser.error();
    const auto badRole = ParseHostArguments({"--new-token", "alice", "root"});
    ASSERT_FALSE(badRole);
    EXPECT_NE(badRole.error().find("--new-token: the role must be admin, editor or viewer"), std::string::npos)
        << badRole.error();
    EXPECT_FALSE(ParseHostArguments({"--new-token", "alice", "Admin"}));
}

TEST(HostOptionsTest, NewTokenAndCreateCantBeCombined)
{
    const auto parsed = ParseHostArguments({"--new-token", "alice", "admin", "--create", "proj"});
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().find("--new-token and --create can't be combined"), std::string::npos) << parsed.error();
}

TEST(HostOptionsTest, TheUsageNamesTheTokenOptions)
{
    const std::string_view usage = HostUsage();
    for (const std::string_view text : {"--tokens-file <path>", "--new-token <user> <role>", "viewer", "editor", "admin",
                                        "sha256:", "SIGHUP", "chmod 600", "N2ENGINE_EDITOR_TLS"})
    {
        EXPECT_NE(usage.find(text), std::string_view::npos) << text;
    }
}

TEST(HostOptionsTest, ABuildWithoutTlsRefusesATokensFileAndCantMakeATokenButATlsBuildCan)
{
    HostOptions tokens;
    tokens.tokensFile = (std::filesystem::temp_directory_path() / "n2-no-such-tokens.json").string();
    HostOptions newToken;
    newToken.newTokenUser = "alice";
    newToken.newTokenRole = TokenRole::Editor;

    if (EditorTlsServer::IsAvailable())
    {
        // A host with a tokens file starts the engine, which this test doesn't: only the token maker runs here
        EXPECT_EQ(RunHost(newToken), 0);
        return;
    }
    // Before the engine starts, with one line on stderr: a host asked for per-user tokens it can't check must not start
    EXPECT_EQ(RunHost(tokens), 1);
    EXPECT_EQ(RunHost(newToken), 1);
    EXPECT_EQ(RunNewToken(newToken), 1);
}

// ==================== --insecure-bind and the fail-closed non-loopback bind (S3, audit F5) ====================

namespace
{
    constexpr const char *NonLoopbackBinds[] = {"0.0.0.0", "192.0.2.10"};

    [[nodiscard]] std::vector<std::string> BindArgs(const std::string &address, const std::vector<std::string> &more)
    {
        std::vector<std::string> args = {"--bind", address};
        args.insert(args.end(), more.begin(), more.end());
        return args;
    }
}

TEST(HostOptionsTest, NonLoopbackBindWithNoTokenAndNoTlsIsRefused)
{
    for (const char *address : NonLoopbackBinds)
    {
        const auto parsed = ParseHostArguments(BindArgs(address, {}));
        ASSERT_FALSE(parsed) << address;
        EXPECT_NE(parsed.error().find(address), std::string::npos);
        EXPECT_NE(parsed.error().find("missing an access token and --tls"), std::string::npos) << parsed.error();
        EXPECT_NE(parsed.error().find("--insecure-bind"), std::string::npos);
        EXPECT_NE(parsed.error().find("--token-env or --tokens-file"), std::string::npos);
    }
}

TEST(HostOptionsTest, NonLoopbackBindWithATokenEnvButNoTlsIsRefused)
{
    for (const char *address : NonLoopbackBinds)
    {
        const auto parsed = ParseHostArguments(BindArgs(address, {"--token-env", "N2_EDITOR_TOKEN"}));
        ASSERT_FALSE(parsed) << address;
        EXPECT_NE(parsed.error().find("missing --tls"), std::string::npos) << parsed.error();
        EXPECT_NE(parsed.error().find("--insecure-bind"), std::string::npos);
    }
}

TEST(HostOptionsTest, NonLoopbackBindWithATokensFileButNoTlsIsRefused)
{
    for (const char *address : NonLoopbackBinds)
    {
        const auto parsed = ParseHostArguments(BindArgs(address, {"--tokens-file", "tokens.json"}));
        ASSERT_FALSE(parsed) << address;
        EXPECT_NE(parsed.error().find("missing --tls"), std::string::npos) << parsed.error();
    }
}

TEST(HostOptionsTest, NonLoopbackBindWithTlsButNoTokenIsRefused)
{
    for (const char *address : NonLoopbackBinds)
    {
        const auto parsed = ParseHostArguments(BindArgs(address, {"--tls"}));
        ASSERT_FALSE(parsed) << address;
        EXPECT_NE(parsed.error().find("missing an access token:"), std::string::npos) << parsed.error();
    }
}

TEST(HostOptionsTest, NonLoopbackBindWithTlsAndATokenEnvIsAccepted)
{
    for (const char *address : NonLoopbackBinds)
    {
        const auto parsed = ParseHostArguments(BindArgs(address, {"--tls", "--token-env", "N2_EDITOR_TOKEN"}));
        ASSERT_TRUE(parsed) << address << ": " << parsed.error();
        EXPECT_EQ(parsed->bindAddress, address);
        EXPECT_FALSE(parsed->insecureBind);
    }
}

TEST(HostOptionsTest, NonLoopbackBindWithTlsAndATokensFileIsAccepted)
{
    for (const char *address : NonLoopbackBinds)
    {
        const auto parsed = ParseHostArguments(BindArgs(address, {"--tokens-file", "tokens.json", "--tls"}));
        ASSERT_TRUE(parsed) << address << ": " << parsed.error();
    }
}

TEST(HostOptionsTest, InsecureBindAcceptsEveryRefusedCombination)
{
    const std::vector<std::vector<std::string>> refused = {
        {}, {"--token-env", "N2_EDITOR_TOKEN"}, {"--tokens-file", "tokens.json"}, {"--tls"}};
    for (const char *address : NonLoopbackBinds)
    {
        for (const auto &more : refused)
        {
            auto args = BindArgs(address, more);
            ASSERT_FALSE(ParseHostArguments(args)) << address;
            // After the other options or before them: the order doesn't matter
            args.push_back("--insecure-bind");
            const auto after = ParseHostArguments(args);
            ASSERT_TRUE(after) << address << ": " << after.error();
            EXPECT_TRUE(after->insecureBind);
            args.pop_back();
            args.insert(args.begin(), "--insecure-bind");
            EXPECT_TRUE(ParseHostArguments(args));
        }
    }
}

TEST(HostOptionsTest, InsecureBindIsAFlagThatTakesNoValue)
{
    const auto defaults = ParseHostArguments({});
    ASSERT_TRUE(defaults);
    EXPECT_FALSE(defaults->insecureBind);

    const auto parsed = ParseHostArguments({"--insecure-bind", "--port", "1234", "--bind", "0.0.0.0"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_TRUE(parsed->insecureBind);
    EXPECT_EQ(parsed->port, 1234);
    EXPECT_EQ(parsed->bindAddress, "0.0.0.0");

    // The last argument too: nothing is swallowed as its value
    const auto last = ParseHostArguments({"--bind", "0.0.0.0", "--insecure-bind"});
    ASSERT_TRUE(last) << last.error();
    EXPECT_EQ(last->bindAddress, "0.0.0.0");
}

TEST(HostOptionsTest, ThePlayHostWithANonLoopbackBindNeedsInsecureBind)
{
    // A play host is always plaintext, so the rule refuses it; there is no separate --play rule
    for (const char *address : NonLoopbackBinds)
    {
        EXPECT_FALSE(ParseHostArguments(BindArgs(address, {"--play", "scene.json"}))) << address;
        EXPECT_FALSE(ParseHostArguments(BindArgs(address, {"--play", "scene.json", "--token-env", "T"}))) << address;
        const auto allowed = ParseHostArguments(BindArgs(address, {"--play", "scene.json", "--insecure-bind"}));
        ASSERT_TRUE(allowed) << address << ": " << allowed.error();
        EXPECT_EQ(allowed->playScene, "scene.json");
    }
    EXPECT_TRUE(ParseHostArguments({"--play", "scene.json"}));
}

TEST(HostOptionsTest, LoopbackBindsNeedNeitherATokenNorTls)
{
    EXPECT_TRUE(ParseHostArguments({}));
    for (const char *address : {"127.0.0.1", "127.1.2.3", "127.255.255.255"})
    {
        const auto parsed = ParseHostArguments(BindArgs(address, {}));
        ASSERT_TRUE(parsed) << address << ": " << parsed.error();
        EXPECT_FALSE(parsed->insecureBind);
    }
}

TEST(HostOptionsTest, CreateNewTokenAndHelpAreNotRefusedForANonLoopbackBind)
{
    // None of them serves
    EXPECT_TRUE(ParseHostArguments(BindArgs("0.0.0.0", {"--create", "C:/Games/New"})));
    EXPECT_TRUE(ParseHostArguments(BindArgs("0.0.0.0", {"--new-token", "alice", "editor"})));
    const auto help = ParseHostArguments(BindArgs("0.0.0.0", {"--help"}));
    ASSERT_TRUE(help);
    EXPECT_TRUE(help->showHelp);
}

TEST(HostOptionsTest, AnUnparsableBindAddressFollowsTheNonLoopbackRule)
{
    // Start still rejects these; here none of them may pass as loopback
    for (const char *address : {"", "localhost", "127.0.0.1 ", "127.0.0.1.", "127.0.0.1x", "127.0.0", "127.0.0.0.1",
                                "0127.0.0.1", "127.00.0.1", "127.0.0.256", "::1", "-127.0.0.1", "0x7f.0.0.1",
                                "2130706433", "127.0.0.1/8", "1.127.0.1", "0.0.0.0"})
    {
        EXPECT_FALSE(IsLoopbackBindAddress(address)) << '"' << address << '"';
        EXPECT_FALSE(ParseHostArguments(BindArgs(address, {}))) << '"' << address << '"';
        EXPECT_TRUE(ParseHostArguments(BindArgs(address, {"--insecure-bind"}))) << '"' << address << '"';
    }
    EXPECT_TRUE(IsLoopbackBindAddress("127.0.0.1"));
    EXPECT_TRUE(IsLoopbackBindAddress("127.1.2.3"));
    EXPECT_TRUE(IsLoopbackBindAddress("127.0.0.0"));
}

TEST(HostOptionsTest, TheLastBindIsTheOneJudged)
{
    EXPECT_FALSE(ParseHostArguments({"--bind", "127.0.0.1", "--bind", "0.0.0.0"}));
    EXPECT_TRUE(ParseHostArguments({"--bind", "0.0.0.0", "--bind", "127.0.0.1"}));
}

TEST(HostOptionsTest, RunHostExitsOneForARefusedBindBeforeTheEngineStarts)
{
    // The real entry point: the refusal is exit code 1 and returns at once (a host that got past it would start the
    // engine and serve, so this test would hang)
    char program[] = "N2EditorHost";
    char bind[] = "--bind";
    char address[] = "0.0.0.0";
    char *argv[] = {program, bind, address, nullptr};
    EXPECT_EQ(RunHost(3, argv), 1);
}

TEST(HostOptionsTest, TheUsageNamesInsecureBind)
{
    const std::string_view usage = HostUsage();
    EXPECT_NE(usage.find("--insecure-bind"), std::string_view::npos);
    EXPECT_NE(usage.find("127.0.0.0/8"), std::string_view::npos);
}

// ==================== --editor-extensions ====================

TEST(HostOptionsTest, EditorExtensionsIsAFlagAndOffByDefault)
{
    const auto defaults = ParseHostArguments({});
    ASSERT_TRUE(defaults) << defaults.error();
    EXPECT_FALSE(defaults->editorExtensions) << "project code on the host is never on unless the operator says so";

    // Not a value-taking option: the next argument is still parsed on its own, in either order
    const auto parsed = ParseHostArguments({"--editor-extensions", "--port", "1234", "--project", "proj"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_TRUE(parsed->editorExtensions);
    EXPECT_EQ(parsed->port, 1234);
    EXPECT_EQ(parsed->projectPath, "proj");

    const auto last = ParseHostArguments({"--project", "proj", "--editor-extensions"});
    ASSERT_TRUE(last) << last.error();
    EXPECT_TRUE(last->editorExtensions);
}

TEST(HostOptionsTest, EditorExtensionsIsRefusedWithPlayInEitherOrder)
{
    const auto play = ParseHostArguments({"--editor-extensions", "--play", "snapshot.json"});
    ASSERT_FALSE(play);
    EXPECT_NE(play.error().find("--editor-extensions and --play can't be combined"), std::string::npos) << play.error();
    const auto playFirst = ParseHostArguments({"--play", "snapshot.json", "--port", "1234", "--editor-extensions"});
    ASSERT_FALSE(playFirst);
    EXPECT_NE(playFirst.error().find("--editor-extensions and --play"), std::string::npos) << playFirst.error();

    // Each alone still parses
    EXPECT_TRUE(ParseHostArguments({"--play", "snapshot.json"}));
    EXPECT_TRUE(ParseHostArguments({"--editor-extensions"}));
}

TEST(HostOptionsTest, EditorExtensionsOnAnOpenBindNeedsATokensFile)
{
    // Not loopback and no tokens file: refused, and the message says what to add
    const auto open = ParseHostArguments({"--editor-extensions", "--bind", "0.0.0.0"});
    ASSERT_FALSE(open);
    EXPECT_NE(open.error().find("--editor-extensions needs --tokens-file"), std::string::npos) << open.error();

    // --insecure-bind lifts the fail-closed bind rule, not this one
    const auto insecure = ParseHostArguments({"--editor-extensions", "--bind", "0.0.0.0", "--insecure-bind"});
    ASSERT_FALSE(insecure);
    EXPECT_NE(insecure.error().find("--tokens-file"), std::string::npos) << insecure.error();

    // One admin token is no roles: --token-env does not satisfy it either
    const auto tokenEnv = ParseHostArguments({"--editor-extensions", "--bind", "0.0.0.0", "--token-env", "N2_TOKEN", "--tls"});
    ASSERT_FALSE(tokenEnv);
    EXPECT_NE(tokenEnv.error().find("--tokens-file"), std::string::npos) << tokenEnv.error();

    // The order of the arguments does not matter
    EXPECT_FALSE(ParseHostArguments({"--bind", "10.0.0.5", "--editor-extensions"}));

    // With a tokens file (and the rest the bind rule asks for) it is allowed
    const auto roles = ParseHostArguments(
        {"--editor-extensions", "--bind", "0.0.0.0", "--tokens-file", "tokens.json", "--tls"});
    ASSERT_TRUE(roles) << roles.error();
    EXPECT_TRUE(roles->editorExtensions);
    EXPECT_TRUE(ParseHostArguments(
        {"--editor-extensions", "--bind", "0.0.0.0", "--tokens-file", "tokens.json", "--insecure-bind"}));

    // On loopback no tokens are needed, as for every other flag
    EXPECT_TRUE(ParseHostArguments({"--editor-extensions", "--bind", "127.0.0.1"}));
    EXPECT_TRUE(ParseHostArguments({"--editor-extensions", "--bind", "127.5.5.5"}));
}

TEST(HostOptionsTest, EditorExtensionsIsAllowedWithReadOnly)
{
    const auto parsed = ParseHostArguments({"--editor-extensions", "--read-only", "--project", "proj"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_TRUE(parsed->editorExtensions);
    EXPECT_TRUE(parsed->readOnly);
}

TEST(HostOptionsTest, TheUsageNamesEditorExtensions)
{
    const std::string_view usage = HostUsage();
    EXPECT_NE(usage.find("--editor-extensions"), std::string_view::npos);
    EXPECT_NE(usage.find("<project>/editor/**/*.lua"), std::string_view::npos);
    EXPECT_NE(usage.find("OFF by"), std::string_view::npos) << "the default is documented";
    EXPECT_NE(usage.find("--play"), std::string_view::npos);
    EXPECT_NE(usage.find("--tokens-file"), std::string_view::npos);
}
