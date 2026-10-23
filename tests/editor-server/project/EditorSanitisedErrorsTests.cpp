#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/ClientText.hpp>
#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/EditorTls.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// Security audit item 11 (F10, MCP0b) through EditorServer::ExecuteCommand, against a project made by IO::CreateProject
// in a folder with an unusual absolute path: what a remote client is told (generic messages, res://-relative paths)
// and what a local one still is, and the admin-only determinism block of SetProjectSettings. Part of the
// editor_project_tests program, which owns ResourceLoader and SceneManager for its tests.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t ProjectInfoType = static_cast<uint8_t>(ResponseType::ProjectInfo);
    constexpr uint8_t PlaySnapshotType = static_cast<uint8_t>(ResponseType::PlaySnapshot);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);

    /// A word no real path holds: it must never come back to a remote client
    constexpr const char *RootMarker = "zzhostroot";

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        /// Everything in the frame as text: an Error's message, or an ErrorCoded's message and code
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

    std::vector<uint8_t> Strings(std::initializer_list<std::string> values)
    {
        BufferWriter w;
        for (const std::string &value : values)
            w.WriteString(value);
        return w.Release();
    }

    std::vector<uint8_t> JsonPayload(const json &value)
    {
        BufferWriter w;
        WriteJson(w, value);
        return w.Release();
    }

    /// An Error's message, or an ErrorCoded's; empty for any other frame
    std::string ErrorMessage(const Frame &frame)
    {
        if (frame.type == ErrorType)
            return frame.Text();
        if (frame.type == ErrorCodedType)
        {
            BufferReader r(frame.payload);
            return r.ReadString();
        }
        return {};
    }

    std::string ErrorCode(const Frame &frame)
    {
        if (frame.type != ErrorCodedType)
            return {};
        BufferReader r(frame.payload);
        (void)r.ReadString();
        return r.ReadString();
    }

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    std::string Utf8(const fs::path &path)
    {
        const std::u8string text = path.u8string();
        return {text.begin(), text.end()};
    }

    std::string GenericUtf8(const fs::path &path)
    {
        const std::u8string text = path.generic_u8string();
        return {text.begin(), text.end()};
    }

    /// Whether the text holds a path on a disk: a drive letter and a slash, or a folder of the system
    bool HasAbsolutePath(const std::string &text)
    {
        for (size_t i = 0; i + 2 < text.size(); ++i)
        {
            const bool drive = std::isalpha(static_cast<unsigned char>(text[i])) != 0 && text[i + 1] == ':' &&
                               (text[i + 2] == '/' || text[i + 2] == '\\') &&
                               (i == 0 || std::isalpha(static_cast<unsigned char>(text[i - 1])) == 0);
            if (drive)
                return true;
        }
        for (const char *folder : {"/tmp/", "/home/", "/Users/", "/var/", "/private/", "/usr/", "/opt/", "/mnt/"})
        {
            if (text.find(folder) != std::string::npos)
                return true;
        }
        return false;
    }

    class SanitisedErrorsTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            // An unusual absolute path: a marker word, spaces and parentheses
            _base = fs::temp_directory_path() / (std::string("n2-sanitised-") + info->name()) /
                    (std::string(RootMarker) + " (a game) & more");
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);

            const auto created = IO::CreateProject(_base / "My Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "My Game");

            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
            server.SetAssetPollInterval(std::chrono::milliseconds(0));
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        [[nodiscard]] fs::path Assets() const { return _root / "assets"; }

        void WriteAsset(const std::string &relative, const std::string &text)
        {
            const fs::path path = Assets() / IO::PathFromUtf8(relative);
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        /// Makes writing a play snapshot fail on a host that is not out of disk: its folder is a file
        void BlockThePlayFolder()
        {
            fs::create_directories(_root / ".n2");
            std::ofstream(_root / ".n2" / "play", std::ios::binary) << "in the way";
        }

        /// What the host's log says that a remote client mustn't be told
        [[nodiscard]] std::vector<std::string> ForbiddenIn(const std::string &text) const
        {
            std::vector<std::string> found;
            for (const std::string &needle :
                 {std::string(RootMarker), Utf8(_root), GenericUtf8(_root), Utf8(_base), GenericUtf8(_base)})
            {
                if (!needle.empty() && text.find(needle) != std::string::npos)
                    found.push_back(needle);
            }
            if (HasAbsolutePath(text))
                found.push_back("an absolute path");
            return found;
        }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };

    // ==================== A failing command, local and remote ====================

    /// The failures and the successes a client can provoke that touch the files of the project, for the scan
    std::vector<std::pair<CommandType, std::vector<uint8_t>>> Provocations()
    {
        return {
            {CommandType::ReadTextAsset, Strings({"res://missing.txt"})},
            {CommandType::ReadTextAsset, Strings({"res://dir.txt"})},
            {CommandType::ReadTextAsset, Strings({"res://a.bin"})},
            {CommandType::ReadTextAsset, Strings({"res://../project.n2proj"})},
            {CommandType::WriteTextAsset, Strings({"res://nofolder/a.txt", "x"})},
            {CommandType::WriteTextAsset, Strings({"res://../escape.txt", "x"})},
            {CommandType::WriteTextAsset, Strings({"res://dir.txt", "x"})},
            {CommandType::CreateScriptAsset, Strings({"res://existing.lua", ""})},
            {CommandType::CreateScriptAsset, Strings({"res://dir.txt/in.lua", ""})},
            {CommandType::CreateFolder, Strings({"res://existing.lua"})},
            {CommandType::CreateFolder, Strings({"res://existing.lua/inside"})},
            {CommandType::ListAssets, [] { BufferWriter w; w.WriteString("res://existing.lua"); w.WriteBool(false); return w.Release(); }()},
            {CommandType::GetAssetInfo, Strings({"res://nope.txt"})},
            {CommandType::OpenScene, Strings({"res://scenes/Missing.scene"})},
            {CommandType::OpenScene, Strings({"res://existing.lua"})},
            {CommandType::OpenScene, Strings({"res://scenes/Broken.scene"})},
            {CommandType::SaveSceneToFile, Strings({"res://../x.scene"})},
            {CommandType::SetStartupScene, Strings({"res://scenes/Missing.scene"})},
            {CommandType::WritePlaySnapshot, Strings({"res://scenes/Missing.scene"})},
            {CommandType::WritePlaySnapshot, Strings({"res://scenes/Broken.scene"})},
            {CommandType::WritePlaySnapshot, Strings({"res://scenes/Main.scene"})},
            {CommandType::GetAutosave, {}},
            {CommandType::RestoreAutosave, {}},
            {CommandType::Undo, {}},
            {CommandType::DeleteScene, Strings({"Missing"})},
            {CommandType::CreateEntity, {}},
            {CommandType::GetProjectInfo, {}},
        };
    }
}

TEST_F(SanitisedErrorsTest, ALocalClientKeepsTheDetailOfAFailure)
{
    // The same failure as the golden test below: the folder of the play snapshots is a file
    BlockThePlayFolder();
    const Frame local = Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/Main.scene"}));
    ASSERT_EQ(local.type, ErrorType) << local.Text();
    EXPECT_TRUE(local.Text().starts_with("Couldn't write the play snapshot: ")) << local.Text();
    EXPECT_NE(local.Text().find(RootMarker), std::string::npos)
        << "the single local user is told which file: " << local.Text();

    // An exception's text: a payload that is cut short
    const Frame cut = Execute(server, CommandType::CreateEntity);
    ASSERT_EQ(cut.type, ErrorType);
    EXPECT_TRUE(cut.Text().starts_with("Command failed: Malformed payload")) << cut.Text();

    const Frame info = Execute(server, CommandType::GetProjectInfo);
    ASSERT_EQ(info.type, ProjectInfoType);
    BufferReader r(info.payload);
    EXPECT_EQ(r.ReadString(), Utf8(_root)) << "a local client gets the project's folder, as before";
}

TEST_F(SanitisedErrorsTest, ARemoteClientIsToldWhatFailedAndWhyButNeverWhere)
{
    server.SetCallerRemote(true);
    ASSERT_FALSE(server.IsLocalClient());
    BlockThePlayFolder();

    // WritePlaySnapshot: what failed and a stable reason; the detail is in the log
    const Frame snapshot = Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/Main.scene"}));
    EXPECT_EQ(snapshot.type, ErrorType);
    EXPECT_EQ(ErrorMessage(snapshot), "Couldn't write the play snapshot: the file could not be written");

    // An exception: the command failed, and nothing of what it said
    const Frame cut = Execute(server, CommandType::CreateEntity);
    EXPECT_EQ(ErrorMessage(cut), "Command failed: the host couldn't complete it (see the host's log)");

    // A file that isn't there: a res:// path, as for a local client
    EXPECT_EQ(ErrorMessage(Execute(server, CommandType::ReadTextAsset, Strings({"res://missing.txt"}))),
              "File not found: res://missing.txt");
    EXPECT_EQ(ErrorMessage(Execute(server, CommandType::ReadTextAsset, Strings({"res://a.bin"}))),
              "Not a text asset: res://a.bin (its extension isn't one ReadTextAsset reads)");
}

TEST_F(SanitisedErrorsTest, ReadTextAssetNamesTheResPathAndAStableReasonForAFileItCannotRead)
{
#ifdef _WIN32
    GTEST_SKIP() << "an unreadable file needs an ACL on Windows";
#else
    WriteAsset("locked.txt", "secret");
    fs::permissions(Assets() / "locked.txt", fs::perms::none, fs::perm_options::replace);
    if (std::ifstream(Assets() / "locked.txt"))
        GTEST_SKIP() << "this user reads files whatever their mode (root)";

    // The reason is the same for both kinds of client, and no path is in it
    const std::string expected = "Couldn't read res://locked.txt: permission denied";
    EXPECT_EQ(ErrorMessage(Execute(server, CommandType::ReadTextAsset, Strings({"res://locked.txt"}))), expected);
    server.SetCallerRemote(true);
    EXPECT_EQ(ErrorMessage(Execute(server, CommandType::ReadTextAsset, Strings({"res://locked.txt"}))), expected);
#endif
}

TEST_F(SanitisedErrorsTest, TheSnapshotPathIsTheHostsForALocalClientAndBelowTheProjectForARemoteOne)
{
    const auto snapshotOf = [this]
    {
        const Frame response = Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/Main.scene"}));
        EXPECT_EQ(response.type, PlaySnapshotType) << response.Text();
        BufferReader r(response.payload);
        return r.ReadString();
    };

    const std::string local = snapshotOf();
    EXPECT_TRUE(fs::path(std::u8string(local.begin(), local.end())).is_absolute()) << local;
    EXPECT_NE(local.find(RootMarker), std::string::npos) << local;

    server.SetCallerRemote(true);
    const std::string remote = snapshotOf();
    EXPECT_TRUE(remote.starts_with(".n2/play/Main-")) << remote;
    EXPECT_TRUE(remote.ends_with(".scene")) << remote;
    EXPECT_TRUE(ForbiddenIn(remote).empty()) << remote;
    // The same file: what a launcher on the host would pass --play
    EXPECT_TRUE(fs::equivalent(_root / fs::path(remote), fs::path(std::u8string(local.begin(), local.end()))));
}

TEST_F(SanitisedErrorsTest, TheProjectInfoOfARemoteClientHasNoFolderOfTheHost)
{
    server.SetCallerRemote(true);
    const Frame info = Execute(server, CommandType::GetProjectInfo);
    ASSERT_EQ(info.type, ProjectInfoType);
    BufferReader r(info.payload);
    EXPECT_EQ(r.ReadString(), "");
    EXPECT_EQ(r.ReadString(), "");
    const json project = ReadJson(r);
    EXPECT_EQ(project.at("name"), "My Game") << "the project file itself is the same";
    EXPECT_FALSE(r.HasData());
}

TEST_F(SanitisedErrorsTest, NoResponseToARemoteClientHoldsAPathOnTheHostsDisk)
{
    // Things to fail on, and the file the play snapshot fails to be written beside
    WriteAsset("dir.txt/inner.txt", "x");
    WriteAsset("existing.lua", "return {}");
    WriteAsset("a.bin", "x");
    WriteAsset("scenes/Broken.scene", "this is not json");
    BlockThePlayFolder();

    // The scan sees what it is looking for: a local client's answers do hold the folder
    bool localHoldsThePath = false;
    for (const auto &[type, payload] : Provocations())
    {
        const Frame frame = Execute(server, type, payload);
        if (!ForbiddenIn(frame.Text()).empty())
            localHoldsThePath = true;
    }
    EXPECT_TRUE(localHoldsThePath) << "a scan that finds nothing in a local client's answers proves nothing";

    server.SetCallerRemote(true);
    size_t index = 0;
    for (const auto &[type, payload] : Provocations())
    {
        const Frame frame = Execute(server, type, payload);
        const std::vector<std::string> found = ForbiddenIn(frame.Text());
        EXPECT_TRUE(found.empty()) << "request " << index << " (command 0x" << std::hex << static_cast<int>(type)
                                   << std::dec << ") told a remote client " << (found.empty() ? "" : found.front())
                                   << ": " << frame.Text();
        ++index;
    }
}

TEST(ClientTextRedactionTest, AnErrorThatNamesTheProjectIsRewrittenForARemoteClient)
{
    // The net under the sites: a message built anywhere that holds the project's folder never leaves it
    const std::vector<ClientText::HostRoot> roots{{fs::path("/srv/zzhostroot/game/assets"), "res://"},
                                                  {fs::path("/srv/zzhostroot/game"), "<project>"}};
    EXPECT_EQ(ClientText::RedactHostPaths("can't write /srv/zzhostroot/game/assets/a/b.txt: disk full", roots),
              "can't write res://a/b.txt: disk full");
    EXPECT_EQ(ClientText::RedactHostPaths("can't replace /srv/zzhostroot/game/.n2/play/x.scene", roots),
              "can't replace <project>/.n2/play/x.scene");
}

// ==================== SetProjectSettings: the determinism block is an admin's ====================

namespace
{
    constexpr const char *AliceToken = "alice-secret-token-0001";
    constexpr const char *BobToken = "bob-secret-token-0002";
    constexpr const char *CarolToken = "carol-secret-token-0003";

    std::vector<uint8_t> HelloPayload(const std::string &token, const std::string &user)
    {
        BufferWriter w;
        w.WriteString("sanitised errors test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(token);
        w.WriteBool(true);
        w.WriteString(user);
        return w.Release();
    }

    /// A project and a tokens file with an admin, an editor and a viewer. A tokens file needs the TLS library (it hashes
    /// the tokens with its SHA-256), so these tests skip themselves without it.
    class DeterminismRoleTest : public SanitisedErrorsTest
    {
    protected:
        void SetUp() override
        {
            SanitisedErrorsTest::SetUp();
            if (!EditorTlsServer::IsAvailable())
                GTEST_SKIP() << "a tokens file needs the TLS library (N2ENGINE_EDITOR_TLS)";

            std::string text = "[";
            const struct
            {
                const char *user;
                const char *role;
                const char *token;
            } users[] = {{"alice", "admin", AliceToken}, {"bob", "editor", BobToken}, {"carol", "viewer", CarolToken}};
            bool first = true;
            for (const auto &user : users)
            {
                const auto hash = HashToken(user.token);
                ASSERT_TRUE(hash.has_value());
                text += std::string(first ? "" : ",\n ") + "{\"user\":\"" + user.user + "\",\"role\":\"" + user.role +
                        "\",\"hash\":\"" + FormatTokenHash(*hash) + "\"}";
                first = false;
            }
            text += "]\n";
            _tokensFile = _base.parent_path() / "tokens.json";
            {
                std::ofstream out(_tokensFile, std::ios::binary | std::ios::trunc);
                out << text;
            }
#ifndef _WIN32
            fs::permissions(_tokensFile, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
            ASSERT_TRUE(server.SetTokensFile(_tokensFile));
        }

        void SignIn(const char *token, const char *user)
        {
            const Frame info = Execute(server, CommandType::Hello, HelloPayload(token, user));
            ASSERT_EQ(info.type, ServerInfoType) << info.Text();
        }

        Frame SetSettings(const json &patch) { return Execute(server, CommandType::SetProjectSettings, JsonPayload(patch)); }

        /// The settings project.n2proj holds on disk
        [[nodiscard]] json SavedSettings() const
        {
            const auto saved = IO::ProjectFile::Load(_root);
            EXPECT_TRUE(saved.has_value());
            return saved ? saved->settings : json::object();
        }

        fs::path _tokensFile;
    };
}

TEST_F(DeterminismRoleTest, AnEditorMayNotSetTheLuaSeedAndNothingIsWritten)
{
    SignIn(BobToken, "bob");
    const std::string before = ReadFile(IO::ProjectFile::PathIn(_root));

    const Frame refused = SetSettings({{"determinism", {{"luaSeed", 12345}}}});
    EXPECT_EQ(refused.type, ErrorCodedType) << refused.Text();
    EXPECT_EQ(ErrorCode(refused), "forbidden");
    EXPECT_TRUE(ErrorMessage(refused).starts_with("forbidden: role editor is not allowed to change the project's 'determinism' "
                                                  "settings"))
        << ErrorMessage(refused);
    EXPECT_NE(ErrorMessage(refused).find("determinism.luaSeed"), std::string::npos);

    EXPECT_EQ(ReadFile(IO::ProjectFile::PathIn(_root)), before) << "the project file is untouched";
    EXPECT_FALSE(SavedSettings().contains("determinism"));
}

TEST_F(DeterminismRoleTest, AnEditorMayNotRemoveTheBlockOrSmuggleItInWithAnotherChange)
{
    SignIn(BobToken, "bob");
    const std::string before = ReadFile(IO::ProjectFile::PathIn(_root));

    // Removing is a change to the block (a merge patch's null)
    EXPECT_EQ(ErrorCode(SetSettings({{"determinism", nullptr}})), "forbidden");
    // A patch with another block too is refused whole: the other block isn't written either
    EXPECT_EQ(ErrorCode(SetSettings({{"window", {{"title", "T"}}}, {"determinism", {{"luaSeed", 1}}}})), "forbidden");
    // A value that is no block is still a change to the block's key, and refused before the subsystem is asked
    EXPECT_EQ(ErrorCode(SetSettings({{"determinism", "nope"}})), "forbidden");

    EXPECT_EQ(ReadFile(IO::ProjectFile::PathIn(_root)), before);
}

TEST_F(DeterminismRoleTest, AnEditorMayStillChangeTheOtherSettings)
{
    SignIn(BobToken, "bob");
    const Frame saved = SetSettings({{"window", {{"title", "Edited by Bob"}}}});
    EXPECT_EQ(saved.type, ProjectInfoType) << saved.Text();
    EXPECT_EQ(SavedSettings().at("window").at("title"), "Edited by Bob");
}

TEST_F(DeterminismRoleTest, AnAdminMaySetTheLuaSeed)
{
    SignIn(AliceToken, "alice");
    const Frame saved = SetSettings({{"determinism", {{"luaSeed", 12345}}}});
    EXPECT_EQ(saved.type, ProjectInfoType) << saved.Text();
    EXPECT_EQ(SavedSettings().at("determinism").at("luaSeed"), 12345);

    // And what is not a seed is refused as before, by the subsystem, not by the role
    const Frame notASeed = SetSettings({{"determinism", {{"luaSeed", -1}}}});
    EXPECT_EQ(notASeed.type, ErrorType) << notASeed.Text();
    EXPECT_NE(notASeed.Text().find("luaSeed"), std::string::npos);
}

TEST_F(DeterminismRoleTest, AViewerIsRefusedAsBefore)
{
    SignIn(CarolToken, "carol");
    const Frame refused = SetSettings({{"determinism", {{"luaSeed", 5}}}});
    EXPECT_EQ(ErrorCode(refused), "forbidden");
    EXPECT_EQ(ErrorMessage(refused), EditorServer::RoleDeniedMessage(TokenRole::Viewer, static_cast<uint8_t>(CommandType::SetProjectSettings)));
    EXPECT_FALSE(SavedSettings().contains("determinism"));
}

TEST_F(DeterminismRoleTest, NoOtherCommandWritesTheProjectFileAnEditorMayRun)
{
    SignIn(BobToken, "bob");
    const std::string before = ReadFile(IO::ProjectFile::PathIn(_root));

    // The project file is above the assets folder, which is all a text asset can be written in
    for (const std::string &path : {"res://../project.n2proj", "res://..\\project.n2proj", "res://scenes/../../project.n2proj",
                                    "project.n2proj"})
    {
        const Frame refused = Execute(server, CommandType::WriteTextAsset,
                                      Strings({path, "{\"settings\":{\"determinism\":{\"luaSeed\":9}}}"}));
        EXPECT_EQ(refused.type, ErrorType) << path << ": " << refused.Text();
    }
    // A file of that name in the assets folder is an asset like any other, not the project's
    EXPECT_EQ(Execute(server, CommandType::WriteTextAsset, Strings({"res://project.n2proj", "x"})).type, ErrorType)
        << "its extension isn't a text asset's";

    EXPECT_EQ(ReadFile(IO::ProjectFile::PathIn(_root)), before);
    EXPECT_FALSE(SavedSettings().contains("determinism"));
}

TEST_F(SanitisedErrorsTest, AHostWithoutAuthenticationHasOneAdminUser)
{
    // No tokens: the single local user's SetProjectSettings is what it was
    const Frame saved = Execute(server, CommandType::SetProjectSettings,
                                JsonPayload({{"determinism", {{"luaSeed", 77}}}}));
    EXPECT_EQ(saved.type, ProjectInfoType) << saved.Text();
    const auto onDisk = IO::ProjectFile::Load(_root);
    ASSERT_TRUE(onDisk.has_value());
    EXPECT_EQ(onDisk->settings.at("determinism").at("luaSeed"), 77);
}

TEST_F(SanitisedErrorsTest, TheSingleTokenIsAnAdminAndMaySetTheLuaSeed)
{
    ASSERT_TRUE(server.SetAccessToken("the-single-token"));
    BufferWriter hello;
    hello.WriteString("test");
    hello.WriteString(std::string{ProtocolVersion});
    hello.WriteString("the-single-token");
    ASSERT_EQ(Execute(server, CommandType::Hello, hello.Release()).type, ServerInfoType);

    const Frame saved = Execute(server, CommandType::SetProjectSettings, JsonPayload({{"determinism", {{"luaSeed", 5}}}}));
    EXPECT_EQ(saved.type, ProjectInfoType) << saved.Text();
}

TEST(AdminOnlySettingsTest, OnlyTheDeterminismBlockIsAnAdminsAlone)
{
    EXPECT_TRUE(EditorServer::IsAdminOnlySettingsBlock("determinism"));
    for (const char *block : {"layers", "input", "physics", "math", "rendering", "window", "future", "", "Determinism"})
    {
        EXPECT_FALSE(EditorServer::IsAdminOnlySettingsBlock(block)) << block;
    }
}
