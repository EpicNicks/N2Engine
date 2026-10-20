#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <editor-server/ClientText.hpp>
#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/Logger.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
namespace fs = std::filesystem;

// What a client is told about a failure (security audit item 11, F10): ClientText's helpers, the definition of a local
// client, and the generic answer of ExecuteCommand to a command that throws. The commands that touch a project's files
// are in project/EditorSanitisedErrorsTests.cpp.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);

    class LogCapture
    {
    public:
        LogCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, Logger::LogLevel)
            {
                const std::lock_guard lock(_mutex);
                _lines.emplace_back(message);
            };
        }

        ~LogCapture() { Logger::logEvent -= _id; }

        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        [[nodiscard]] bool Has(const std::string_view text) const
        {
            const std::lock_guard lock(_mutex);
            for (const std::string &line : _lines)
            {
                if (line.find(text) != std::string::npos)
                    return true;
            }
            return false;
        }

    private:
        mutable std::mutex _mutex;
        std::vector<std::string> _lines;
        size_t _id = 0;
    };

    std::string ErrorText(EditorServer &server, const CommandType type, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(type), payload);
        BufferReader r(frame);
        EXPECT_EQ(r.ReadU8(), ErrorType);
        const auto body = r.ReadBytes(r.ReadU32());
        return {body.begin(), body.end()};
    }
}

// ==================== RedactHostPaths ====================

TEST(ClientTextRedactTest, TheProjectsFoldersBecomeTheirLabelsInEveryWayAPathIsSpelled)
{
    const std::vector<ClientText::HostRoot> roots{{fs::path("/srv/my game/assets"), "res://"},
                                                  {fs::path("/srv/my game"), "<project>"},
                                                  {fs::path("/srv/user data"), "user://"}};

    // Longest folder first: assets is res://, the rest of the project is <project>
    EXPECT_EQ(ClientText::RedactHostPaths("can't read /srv/my game/assets/a/b.txt", roots), "can't read res://a/b.txt");
    EXPECT_EQ(ClientText::RedactHostPaths("can't create a temporary file beside /srv/my game/.n2/play/x.scene", roots),
              "can't create a temporary file beside <project>/.n2/play/x.scene");
    EXPECT_EQ(ClientText::RedactHostPaths("/srv/user data/cache.bin: bad", roots), "user://cache.bin: bad");
    // The folder alone, and every occurrence
    EXPECT_EQ(ClientText::RedactHostPaths("in /srv/my game, not /srv/my game.", roots), "in <project>, not <project>.");
    // Text with none of them is returned as it is
    const std::string clean = "Couldn't write res://a.txt: permission denied";
    EXPECT_EQ(ClientText::RedactHostPaths(clean, roots), clean);
    EXPECT_EQ(ClientText::RedactHostPaths("", roots), "");
    EXPECT_EQ(ClientText::RedactHostPaths("/srv/my game/assets/a.txt", {}), "/srv/my game/assets/a.txt")
        << "no folders, nothing to replace";
}

TEST(ClientTextRedactTest, ABackslashPathIsFoundAndWhatFollowsIsWrittenWithSlashes)
{
    const std::vector<ClientText::HostRoot> roots{{fs::path("/srv/my game/assets"), "res://"},
                                                  {fs::path("/srv/my game"), "<project>"}};
    EXPECT_EQ(ClientText::RedactHostPaths("can't write \\srv\\my game\\assets\\a\\b.txt (is the disk full?)", roots),
              "can't write res://a/b.txt (is the disk full?)");
    EXPECT_EQ(ClientText::RedactHostPaths("\\srv/my game\\.n2\\play", roots), "<project>/.n2/play");
}

TEST(ClientTextRedactTest, AFolderThatOnlyStartsLikeTheProjectsIsLeftAlone)
{
    const std::vector<ClientText::HostRoot> roots{{fs::path("/srv/game"), "<project>"}};
    EXPECT_EQ(ClientText::RedactHostPaths("/srv/game2/x and /srv/game-old/y and /srv/game.bak", roots),
              "/srv/game2/x and /srv/game-old/y and /srv/game.bak");
    EXPECT_EQ(ClientText::RedactHostPaths("/srv/game/x", roots), "<project>/x");
}

TEST(ClientTextRedactTest, ARootThatIsNothingIsIgnored)
{
    // An empty path, or a drive root, would replace text everywhere
    const std::vector<ClientText::HostRoot> roots{{fs::path(), "<empty>"}, {fs::path("/"), "<root>"}};
    EXPECT_EQ(ClientText::RedactHostPaths("a/b and /c", roots), "a/b and /c");
}

TEST(ClientTextRedactTest, RedactDoesNothingForALocalClientAndAppliesTheScopesFoldersForARemoteOne)
{
    const std::string text = "can't read /srv/zz/assets/a.txt";
    {
        const ClientText::Scope local(true, {{fs::path("/srv/zz/assets"), "res://"}});
        EXPECT_TRUE(ClientText::IsLocal());
        EXPECT_EQ(ClientText::Redact(text), text);
    }
    {
        const ClientText::Scope remote(false, {{fs::path("/srv/zz/assets"), "res://"}});
        EXPECT_FALSE(ClientText::IsLocal());
        EXPECT_EQ(ClientText::Redact(text), "can't read res://a.txt");
    }
}

// ==================== Scope and Failure ====================

TEST(ClientTextScopeTest, OutsideAScopeNobodyIsLocal)
{
    EXPECT_FALSE(ClientText::IsLocal()) << "fail closed: a message built where no command runs gives nothing away";
    EXPECT_EQ(ClientText::Failure("Couldn't write res://a.txt", "permission denied", "can't write /srv/zz/a.txt"),
              "Couldn't write res://a.txt: permission denied");
}

TEST(ClientTextScopeTest, AScopeNestsAndPutsBackTheOneBeforeIt)
{
    const ClientText::Scope outer(true, {});
    ASSERT_TRUE(ClientText::IsLocal());
    {
        const ClientText::Scope inner(false, {});
        EXPECT_FALSE(ClientText::IsLocal());
    }
    EXPECT_TRUE(ClientText::IsLocal());
}

TEST(ClientTextScopeTest, FailureGivesALocalClientTheDetailAndARemoteOneTheReasonAndLogsTheDetailForBoth)
{
    {
        LogCapture log;
        const ClientText::Scope local(true, {});
        EXPECT_EQ(ClientText::Failure("Couldn't write res://a.txt", "the file could not be written",
                                      "can't replace /srv/zz/assets/a.txt: Access is denied"),
                  "Couldn't write res://a.txt: can't replace /srv/zz/assets/a.txt: Access is denied");
        EXPECT_TRUE(log.Has("/srv/zz/assets/a.txt"));
        EXPECT_EQ(ClientText::Failure("", "the component refused the value", "boom"), "boom") << "no 'what': the detail alone";
    }
    {
        LogCapture log;
        const ClientText::Scope remote(false, {});
        EXPECT_EQ(ClientText::Failure("Couldn't write res://a.txt", "the file could not be written",
                                      "can't replace /srv/zz/assets/a.txt: Access is denied"),
                  "Couldn't write res://a.txt: the file could not be written");
        EXPECT_TRUE(log.Has("can't replace /srv/zz/assets/a.txt: Access is denied")) << "the detail is in the host's log";
        EXPECT_EQ(ClientText::Failure("", "the component refused the value", "boom"), "the component refused the value");
    }
}

TEST(ClientTextScopeTest, AFailureWithNoDetailIsTheReasonForALocalClientToo)
{
    const ClientText::Scope local(true, {});
    EXPECT_EQ(ClientText::Failure("Couldn't read res://a.txt", "permission denied", ""),
              "Couldn't read res://a.txt: permission denied");
}

TEST(ClientTextReasonTest, AFileSystemErrorIsAStableSentence)
{
    EXPECT_EQ(ClientText::Reason(std::make_error_code(std::errc::no_such_file_or_directory)), "file not found");
    EXPECT_EQ(ClientText::Reason(std::make_error_code(std::errc::permission_denied)), "permission denied");
    EXPECT_EQ(ClientText::Reason(std::make_error_code(std::errc::read_only_file_system)), "permission denied");
    EXPECT_EQ(ClientText::Reason(std::make_error_code(std::errc::no_space_on_device)), "the disk is full");
    EXPECT_EQ(ClientText::Reason(std::make_error_code(std::errc::io_error)), "the file system refused it");
    EXPECT_EQ(ClientText::Reason(std::error_code{}), "the file system refused it");
}

TEST(ClientTextRelativeTest, AFileBelowTheRootIsItsPathWithSlashes)
{
    const fs::path root = fs::path("/srv") / "my game";
    EXPECT_EQ(ClientText::RelativeTo(root / ".n2" / "play" / "Main-1a2b3c4d.scene", root), ".n2/play/Main-1a2b3c4d.scene");
    EXPECT_EQ(ClientText::RelativeTo(fs::path("/elsewhere/x.scene"), root), "") << "a file outside it is nothing to name";
}

// ==================== What a client is: local or remote ====================

TEST(ClientKindTest, ACallerInThisProcessOnAHostWithoutATokensFileIsLocal)
{
    EditorServer server;
    EXPECT_TRUE(server.IsLocalClient());
    server.SetCallerRemote(true);
    EXPECT_FALSE(server.IsLocalClient()) << "an embedder that forwards a remote client's commands says so";
    server.SetCallerRemote(false);
    EXPECT_TRUE(server.IsLocalClient());
}

TEST(ClientKindTest, TheSingleAccessTokenDoesNotMakeAHostMultiUser)
{
    // --token-env: one user, with every right, as before
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("the-single-token"));
    EXPECT_TRUE(server.IsLocalClient());
}

// ==================== A command that throws ====================

TEST(CommandFailureTest, ALocalClientIsToldWhatTheExceptionSaidAndARemoteOneIsNot)
{
    LogCapture log;
    EditorServer server;

    // A payload that is cut short: BufferReader throws
    const std::string local = ErrorText(server, CommandType::CreateEntity);
    EXPECT_TRUE(local.starts_with("Command failed: Malformed payload")) << local;

    server.SetCallerRemote(true);
    const std::string remote = ErrorText(server, CommandType::CreateEntity);
    EXPECT_EQ(remote, "Command failed: the host couldn't complete it (see the host's log)");
    EXPECT_TRUE(log.Has("Malformed payload")) << "the detail is in the host's log";
}

TEST(CommandFailureTest, ACommandThatProducesNoResponseIsTheSameForBoth)
{
    // An id that is no command is refused by the dispatcher with an Error, which both kinds are told as it is
    EditorServer server;
    const std::string local = ErrorText(server, static_cast<CommandType>(0xEF));
    server.SetCallerRemote(true);
    EXPECT_EQ(ErrorText(server, static_cast<CommandType>(0xEF)), local);
}
