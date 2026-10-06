#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <editor-server/EditorServer.hpp>
#include <editor-server/HostOptions.hpp>

using namespace N2Engine::Editor;

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
    EXPECT_FALSE(parsed->showHelp);
}

TEST(HostOptionsTest, ParsesEveryOption)
{
    const auto parsed = ParseHostArguments({"--port", "0", "--bind", "0.0.0.0", "--project", "C:/Games/My Game"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->port, 0);
    EXPECT_EQ(parsed->bindAddress, "0.0.0.0");
    EXPECT_EQ(parsed->projectPath, "C:/Games/My Game");
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
}

TEST(HostOptionsTest, AnInvalidPortIsAnErrorNamingIt)
{
    const auto parsed = ParseHostArguments({"--port", "70000"});
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().find("70000"), std::string::npos) << parsed.error();
}

TEST(HostOptionsTest, AnOptionMissingItsValueIsAnError)
{
    for (const char *option : {"--port", "-p", "--bind", "--project"})
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
