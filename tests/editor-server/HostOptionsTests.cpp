#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <editor-server/EditorServer.hpp>
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
                                            "--exit-on-disconnect", "--exit-on-stdin-eof"});
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
                                          "--exit-on-disconnect", "--exit-on-stdin-eof", "--help", "-h,", "N2EditorHost ready port="})
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
