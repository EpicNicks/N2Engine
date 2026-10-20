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
                _sensitivities.push_back(Logger::CurrentSensitivity());
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

        /// Whether every line holding `text` was logged at Admin sensitivity (and there is one)
        [[nodiscard]] bool OnlyAdminHas(const std::string_view text) const
        {
            const std::lock_guard lock(_mutex);
            bool found = false;
            for (size_t i = 0; i < _lines.size(); ++i)
            {
                if (_lines[i].find(text) == std::string::npos)
                    continue;
                if (_sensitivities[i] != Logger::LogSensitivity::Admin)
                    return false;
                found = true;
            }
            return found;
        }

    private:
        mutable std::mutex _mutex;
        std::vector<std::string> _lines;
        std::vector<Logger::LogSensitivity> _sensitivities;
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

    /// `wrapper` with its "@@" replaced by `path`
    std::string Wrap(const std::string &wrapper, const std::string &path)
    {
        const size_t at = wrapper.find("@@");
        return wrapper.substr(0, at) + path + wrapper.substr(at + 2);
    }

    /// `text` with every `from` written as `to`
    std::string ReplaceAll(std::string text, const std::string &from, const std::string &to)
    {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        {
            text.replace(at, from.size(), to);
        }
        return text;
    }

    /// A '/' path as a host may print it: as it is, with backslashes, with doubled backslashes (a string escaped for
    /// JSON or C), mixed, and percent-encoded (an upper or lower case hex digit, a '\' or a '/', and a twice-encoded one)
    std::vector<std::string> Spellings(const std::string &generic)
    {
        std::string mixed = generic;
        bool back = true;
        for (char &c : mixed)
        {
            if (c == '/')
            {
                if (back)
                    c = '\\';
                back = !back;
            }
        }
        return {generic,
                ReplaceAll(generic, "/", "\\"),
                ReplaceAll(generic, "/", "\\\\"),
                mixed,
                ReplaceAll(ReplaceAll(generic, ":", "%3A"), "/", "%2F"),
                ReplaceAll(ReplaceAll(generic, ":", "%3a"), "/", "%2f"),
                ReplaceAll(ReplaceAll(generic, ":", "%3A"), "/", "%5C")};
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

TEST(ClientTextRedactTest, AFolderAtTheTopOfADriveOrTheFileSystemIsNeverReplaced)
{
    // A working folder of "C:\" (or "/") would otherwise leave "C:" as a root that matches everywhere
    const std::vector<ClientText::HostRoot> roots{
        {fs::path("C:\\"), "<host>"}, {fs::path("C:/"), "<host>"}, {fs::path("C:"), "<host>"}, {fs::path("/a"), "<a>"}};
    const std::string text = "C:/x and C:\\y\\z and /a/b and D:/a";
    EXPECT_EQ(ClientText::RedactHostPaths(text, roots), text);
}

TEST(ClientTextRedactTest, AFolderIsOnlyReplacedWhereItStartsAPath)
{
    const std::vector<ClientText::HostRoot> roots{{fs::path("/srv/game"), "<project>"}};
    // The client's own res:// and user:// paths, a deeper folder and another drive are not the host's /srv/game
    const std::string clientPaths = "res://srv/game/a.txt, user://srv/game/b.bin, /data/srv/game/c and D:/srv/game/d";
    EXPECT_EQ(ClientText::RedactHostPaths(clientPaths, roots), clientPaths);
    EXPECT_EQ(ClientText::RedactHostPaths("x/srv/game/e", roots), "x/srv/game/e");
    // After a space, a quote, a bracket or '=' it is the host's folder, and so is a file:// URL to it
    EXPECT_EQ(ClientText::RedactHostPaths("'/srv/game/a' [/srv/game/b] path=/srv/game/c", roots),
              "'<project>/a' [<project>/b] path=<project>/c");
    EXPECT_EQ(ClientText::RedactHostPaths("file:///srv/game/a.txt", roots), "file://<project>/a.txt");
}

TEST(ClientTextRedactTest, AHostFolderAfterAColonAFullStopOrAnyOtherPunctuationIsReplaced)
{
    // Only the client's own res:// and user:// paths protect a folder: these are the host's, however they are glued on
    const std::vector<ClientText::HostRoot> posix{{fs::path("/srv/game"), "<project>"}};
    EXPECT_EQ(ClientText::RedactHostPaths("key:/srv/game/x", posix), "key:<project>/x");
    EXPECT_EQ(ClientText::RedactHostPaths("open:/srv/game", posix), "open:<project>");
    EXPECT_EQ(ClientText::RedactHostPaths("failed./srv/game/x", posix), "failed.<project>/x");
    EXPECT_EQ(ClientText::RedactHostPaths("http:///srv/game/x", posix), "http://<project>/x");

    const std::vector<ClientText::HostRoot> windows{{fs::path("C:/proj/assets"), "res://"},
                                                    {fs::path("C:/proj"), "<project>"}};
    EXPECT_EQ(ClientText::RedactHostPaths("open:C:\\proj\\assets\\a.txt", windows), "open:res://a.txt");
    EXPECT_EQ(ClientText::RedactHostPaths("failed.C:\\proj\\x", windows), "failed.<project>/x");
    EXPECT_EQ(ClientText::RedactHostPaths("\"C:\\proj\\x\" (C:/proj) path=C:\\proj", windows),
              "\"<project>/x\" (<project>) path=<project>");
    // A Windows long-path prefix and a file:// URL are still the host's folder
    EXPECT_EQ(ClientText::RedactHostPaths("\\\\?\\C:\\proj\\x and file:///C:/proj/y", windows),
              "\\\\?\\<project>/x and file:///<project>/y");
}

TEST(ClientTextRedactTest, OnlyTheClientsOwnResAndUserPathsProtectAFolderAndNothingIsHiddenBehindThem)
{
    const std::vector<ClientText::HostRoot> roots{{fs::path("/srv/game"), "<project>"}};
    // The client's own paths, glued on however they are
    const std::string own = "open:res://srv/game/a.txt key=user://srv/game/b \"RES://srv/game/c\" [res://x/srv/game/d]";
    EXPECT_EQ(ClientText::RedactHostPaths(own, roots), own);
    // A drive and a relative path are not the host's folder
    const std::string other = "D:/srv/game/a open:D:/srv/game/b ./srv/game/c ../srv/game/d";
    EXPECT_EQ(ClientText::RedactHostPaths(other, roots), other);
    // But a host folder that follows one of them after a separator is replaced
    EXPECT_EQ(ClientText::RedactHostPaths("res://a.txt then /srv/game/x, user://bb:/srv/game/y", roots),
              "res://a.txt then <project>/x, user://bb:<project>/y");
}

TEST(ClientTextRedactTest, AnEscapedOrEncodedSpellingOfAFolderIsFoundAndWrittenAsTheSameLabel)
{
    const std::vector<ClientText::HostRoot> roots{{fs::path("C:/proj/assets"), "res://"},
                                                  {fs::path("C:/proj"), "<project>"}};
    // Doubled backslashes, as a JSON or C string holds a path
    EXPECT_EQ(ClientText::RedactHostPaths("{\"path\":\"C:\\\\proj\\\\assets\\\\a\\\\b.txt\"}", roots),
              "{\"path\":\"res://a/b.txt\"}");
    EXPECT_EQ(ClientText::RedactHostPaths("C:\\\\proj\\\\.n2", roots), "<project>/.n2");
    // Mixed slashes
    EXPECT_EQ(ClientText::RedactHostPaths("C:/proj\\assets/a.txt", roots), "res://a.txt");
    // Percent-encoded separators and drive colon, in either case, and a space
    EXPECT_EQ(ClientText::RedactHostPaths("C%3A%2Fproj%2Fassets%2Fa.txt", roots), "res://a.txt");
    EXPECT_EQ(ClientText::RedactHostPaths("C%3a%5cproj%5cassets%5ca.txt", roots), "res://a.txt");
    EXPECT_EQ(ClientText::RedactHostPaths("file:///C:/proj/a%20b.txt", roots), "file:///<project>/a%20b.txt");
    const std::vector<ClientText::HostRoot> spaced{{fs::path("/srv/my game"), "<project>"}};
    EXPECT_EQ(ClientText::RedactHostPaths("in %2Fsrv%2Fmy%20game%2Fx", spaced), "in <project>/x");
    // The text around it is as it was, and a name that only starts like the folder is still not it
    const std::string kept = "100%25 sure: C%3A%5Cproj2%5Cx and C:\\\\proj.bak";
    EXPECT_EQ(ClientText::RedactHostPaths(kept, roots), kept);
}

TEST(ClientTextRedactTest, TextWithNoHostFolderIsReturnedAsItIsWhateverItsEscapes)
{
    const std::vector<ClientText::HostRoot> roots{{fs::path("/srv/game"), "<project>"}, {fs::path("C:/proj"), "<p>"}};
    const std::string text = "res://a%20b/c%2Fd.txt, user://x\\\\y, 100%, %zz, %, %4, C:\\\\other\\\\z, a%00b, /data/srv/game";
    EXPECT_EQ(ClientText::RedactHostPaths(text, roots), text);
}

TEST(ClientTextRedactTest, NoConfiguredFolderSurvivesInAnyWrapperOrSpelling)
{
    // A corpus of the ways a message may hold a path: a folder ("zq" marks it) must never come out in any of them
    const std::vector<ClientText::HostRoot> roots{{fs::path("/zqhost/game/assets"), "res://"},
                                                  {fs::path("/zqhost/game"), "<project>"},
                                                  {fs::path("C:/zqproj/assets"), "res://"},
                                                  {fs::path("C:/zqproj"), "<project>"}};
    const std::vector<std::string> wrappers{"@@",
                                            "'@@'",
                                            "\"@@\"",
                                            "(@@)",
                                            "[@@]",
                                            "<@@>",
                                            "in @@",
                                            "file @@",
                                            "open:@@",
                                            "key=@@",
                                            "failed.@@",
                                            "@@: denied",
                                            "path:@@",
                                            "x: [@@]: y",
                                            "'a' @@ 'b'",
                                            "{\"path\":\"@@\"}",
                                            "file://@@",
                                            "file:///@@",
                                            "(\"@@\")",
                                            "error.@@.",
                                            "can't write @@ (is the disk full?)"};
    const std::vector<std::string> paths{"/zqhost/game",
                                         "/zqhost/game/assets/a/b.txt",
                                         "/zqhost/game/.n2/play/x.scene",
                                         "C:/zqproj",
                                         "C:/zqproj/assets/a/b.txt",
                                         "C:/zqproj/.n2/play/x.scene"};
    for (const std::string &path : paths)
    {
        for (const std::string &spelling : Spellings(path))
        {
            for (const std::string &wrapper : wrappers)
            {
                const std::string text = Wrap(wrapper, spelling);
                const std::string redacted = ClientText::RedactHostPaths(text, roots);
                EXPECT_EQ(redacted.find("zq"), std::string::npos) << "'" << text << "' came out as '" << redacted << "'";
            }
        }
    }
}

TEST(ClientTextRedactTest, AClientsOwnPathIsLeftAloneInEveryWrapper)
{
    const std::vector<ClientText::HostRoot> roots{{fs::path("/zqhost/game"), "<project>"},
                                                  {fs::path("C:/zqproj"), "<project>"}};
    const std::vector<std::string> wrappers{"@@", "'@@'", "(@@)", "open:@@", "key=@@", "in @@", "[@@]", "{\"path\":\"@@\"}"};
    for (const std::string own : {"res://zqhost/game/a.txt", "user://zqhost/game/b.bin", "res://C:/zqproj/c.txt"})
    {
        for (const std::string &wrapper : wrappers)
        {
            const std::string text = Wrap(wrapper, own);
            EXPECT_EQ(ClientText::RedactHostPaths(text, roots), text);
        }
    }
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
        EXPECT_TRUE(log.OnlyAdminHas("/srv/zz/assets/a.txt"))
            << "the detail names host paths: an Admin line (ServerLog), never a Public one a non-admin poller reads";
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
