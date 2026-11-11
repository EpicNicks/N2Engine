#include <gtest/gtest.h>

#include <initializer_list>
#include <string_view>
#include <vector>

#include "PlayerOptions.hpp"

using namespace N2Engine::Player;

namespace
{
    auto Parse(const std::initializer_list<std::string_view> args)
    {
        const std::vector<std::string_view> list(args);
        return ParsePlayerArguments(list);
    }
}

TEST(PlayerOptionsTest, NoArgumentsMeansTheDefaults)
{
    const auto options = Parse({});
    ASSERT_TRUE(options.has_value());
    EXPECT_TRUE(options->projectDir.empty());
    EXPECT_EQ(options->renderBackend, RenderBackend::SOFTWARE);
    EXPECT_FALSE(options->consoleLog);
    EXPECT_FALSE(options->frames.has_value());
    EXPECT_FALSE(options->help);
}

TEST(PlayerOptionsTest, ReadsTheGameFolderAndEveryOption)
{
    const auto options = Parse({"some/game", "--renderer", "opengl", "--console-log", "--frames", "60"});
    ASSERT_TRUE(options.has_value()) << options.error();
    EXPECT_EQ(options->projectDir, std::filesystem::path("some/game"));
    EXPECT_EQ(options->renderBackend, RenderBackend::OPENGL);
    EXPECT_TRUE(options->consoleLog);
    ASSERT_TRUE(options->frames.has_value());
    EXPECT_EQ(*options->frames, 60);
}

TEST(PlayerOptionsTest, AcceptsTheEqualsForm)
{
    const auto options = Parse({"--renderer=vulkan", "--frames=3"});
    ASSERT_TRUE(options.has_value()) << options.error();
    EXPECT_EQ(options->renderBackend, RenderBackend::VULKAN);
    EXPECT_EQ(*options->frames, 3);
}

TEST(PlayerOptionsTest, Help)
{
    const auto options = Parse({"--help"});
    ASSERT_TRUE(options.has_value());
    EXPECT_TRUE(options->help);
    EXPECT_NE(PlayerUsage("n2player").find("--renderer"), std::string::npos);
}

TEST(PlayerOptionsTest, RejectsAnUnknownRenderer)
{
    const auto options = Parse({"--renderer", "metal"});
    ASSERT_FALSE(options.has_value());
    EXPECT_NE(options.error().find("metal"), std::string::npos);
}

TEST(PlayerOptionsTest, RejectsAMissingValue)
{
    EXPECT_FALSE(Parse({"--renderer"}).has_value());
    EXPECT_FALSE(Parse({"--frames"}).has_value());
}

TEST(PlayerOptionsTest, RejectsFramesThatAreNotAPositiveWholeNumber)
{
    for (const std::string_view bad : {"0", "-1", "ten", "5x", "", "99999999999"})
    {
        const std::vector<std::string_view> args{"--frames", bad};
        EXPECT_FALSE(ParsePlayerArguments(args).has_value()) << bad;
    }
}

TEST(PlayerOptionsTest, RejectsUnknownOptionsAndASecondFolder)
{
    EXPECT_FALSE(Parse({"--fullscreen"}).has_value());
    EXPECT_FALSE(Parse({"-x"}).has_value());
    const auto twoFolders = Parse({"a", "b"});
    ASSERT_FALSE(twoFolders.has_value());
    EXPECT_NE(twoFolders.error().find("More than one"), std::string::npos);
}

TEST(PlayerOptionsTest, RendererTitles)
{
    EXPECT_EQ(RenderBackendTitle(RenderBackend::OPENGL), "OpenGL");
    EXPECT_EQ(RenderBackendTitle(RenderBackend::VULKAN), "Vulkan");
    EXPECT_EQ(RenderBackendTitle(RenderBackend::SOFTWARE), "software");
}
