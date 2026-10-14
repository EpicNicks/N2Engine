#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/EditorTls.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/Logger.hpp>

// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
#include "TestSockets.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
namespace fs = std::filesystem;

// Per-user tokens (--tokens-file) on the server: the role bound to a connection at Hello, what each role may run, the
// audit and Hello log lines, and revoking a user by rewriting the file. TokenTableTests.cpp covers the file format and
// the matching without a server. Hashing a token needs the TLS library (N2ENGINE_EDITOR_TLS), so the tests that use a
// tokens file skip themselves on a build without it; the ones that don't (the single --token-env token is one admin
// user, a tokens file needs TLS, the role rules) run on every leg.

namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);

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
        const auto body = r.ReadBytes(size);
        out.payload.assign(body.begin(), body.end());
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the response payload";
        return out;
    }

    struct Coded
    {
        std::string message;
        std::string code;
    };

    Coded DecodeCoded(const Frame &frame)
    {
        EXPECT_EQ(frame.type, ErrorCodedType) << frame.Text();
        BufferReader r(frame.payload);
        Coded coded;
        coded.message = r.ReadString();
        coded.code = r.ReadString();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after ErrorCoded";
        return coded;
    }

    bool IsForbidden(const Frame &frame)
    {
        if (frame.type != ErrorCodedType)
            return false;
        BufferReader r(frame.payload);
        (void)r.ReadString();
        return r.ReadString() == ErrorCodeForbidden;
    }

    struct Info
    {
        std::string protocolVersion;
        bool projectLoaded = false;
        bool readOnly = false;
        std::string user;
        std::string role;
    };

    Info DecodeServerInfo(const Frame &frame)
    {
        EXPECT_EQ(frame.type, ServerInfoType) << frame.Text();
        BufferReader r(frame.payload);
        Info info;
        info.protocolVersion = r.ReadString();
        (void)r.ReadString();
        (void)ReadJson(r);
        info.projectLoaded = r.ReadBool();
        info.readOnly = r.ReadBool();
        info.user = r.ReadString();
        info.role = r.ReadString();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after ServerInfo";
        return info;
    }

    /// A Hello as each minor version's client sends it: 1.10 has three fields, 1.11 adds errorCodes, 1.12 adds user
    std::vector<uint8_t> HelloPayload(const std::string &token, const std::optional<bool> errorCodes = true,
                                      const std::optional<std::string> &user = std::nullopt)
    {
        BufferWriter w;
        w.WriteString("tokens test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(token);
        if (errorCodes.has_value())
            w.WriteBool(*errorCodes);
        if (user.has_value())
            w.WriteString(*user);
        return w.Release();
    }

    Frame Execute(EditorServer &server, const uint8_t id, const std::vector<uint8_t> &payload = {})
    {
        return Decode(server.ExecuteCommand(id, payload));
    }

    Frame Execute(EditorServer &server, const CommandType type, const std::vector<uint8_t> &payload = {})
    {
        return Execute(server, static_cast<uint8_t>(type), payload);
    }

    std::vector<uint8_t> StringPayload(const std::string &text)
    {
        BufferWriter w;
        w.WriteString(text);
        return w.Release();
    }

    /// A command that passes the role gate and runs without an engine, a scene or a project, with a payload for it
    Frame RunPastTheGate(EditorServer &server, const CommandType type)
    {
        switch (type)
        {
        case CommandType::CreateScript: // answers with the script's text: a write that needs nothing
            return Execute(server, type, StringPayload("player"));
        case CommandType::CreateEntity: // "No scene loaded"
            return Execute(server, type, StringPayload("Cube"));
        default: // SendInput, SetPaused: the handler refuses the empty payload or the missing play host
            return Execute(server, type);
        }
    }

    /// The Logger's lines (level and text) while this object lives
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

        [[nodiscard]] std::vector<std::string> Lines() const
        {
            const std::lock_guard lock(_mutex);
            return _lines;
        }

        [[nodiscard]] bool Has(const std::string_view text) const
        {
            for (const std::string &line : Lines())
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

    class TempFolder
    {
    public:
        TempFolder()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _path = fs::temp_directory_path() /
                    ("n2-tokens-server-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(_path);
        }

        ~TempFolder()
        {
            std::error_code error;
            fs::remove_all(_path, error);
        }

        TempFolder(const TempFolder &) = delete;
        TempFolder &operator=(const TempFolder &) = delete;

        [[nodiscard]] const fs::path &Path() const { return _path; }

    private:
        fs::path _path;
    };

    struct User
    {
        std::string name;
        std::string token;
        TokenRole role;
    };

    const User Alice{"alice", "alice-secret-token-0001", TokenRole::Admin};
    const User Bob{"bob", "bob-secret-token-0002", TokenRole::Editor};
    const User Carol{"carol", "carol-secret-token-0003", TokenRole::Viewer};

    void WriteTokensText(const fs::path &file, const std::string &text)
    {
        static std::atomic<int> generation{0};
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
        }
#ifndef _WIN32
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
        fs::last_write_time(file, fs::file_time_type::clock::now() + std::chrono::seconds(2 * ++generation));
    }

    /// Writes a tokens file for these users, private to its owner (POSIX), and dated later than the last write, so a
    /// rewrite is seen whatever the file system's clock granularity
    void WriteTokensFile(const fs::path &file, const std::vector<User> &users)
    {
        std::string text = "[";
        for (size_t i = 0; i < users.size(); ++i)
        {
            const auto hash = HashToken(users[i].token);
            ASSERT_TRUE(hash.has_value());
            text += (i == 0 ? "" : ",\n ") + std::string("{\"user\":\"") + users[i].name + "\",\"role\":\"" +
                    std::string(TokenRoleName(users[i].role)) + "\",\"hash\":\"" + FormatTokenHash(*hash) + "\"}";
        }
        text += "]\n";
        WriteTokensText(file, text);
    }

    class TestClient
    {
    public:
        explicit TestClient(int port)
        {
            _socket = N2TestSockets::OpenTcp();
            if (_socket == N2TestSockets::InvalidHandle)
                return;
            N2TestSockets::SetReceiveTimeout(_socket, 10000); // a hung server fails the test instead of blocking it
            _connected = N2TestSockets::ConnectLoopback(_socket, port);
        }

        ~TestClient()
        {
            if (_socket != N2TestSockets::InvalidHandle)
                N2TestSockets::Close(_socket);
        }

        TestClient(const TestClient &) = delete;
        TestClient &operator=(const TestClient &) = delete;

        [[nodiscard]] bool IsConnected() const { return _connected; }

        bool SendRequest(CommandType type, const std::vector<uint8_t> &payload)
        {
            BufferWriter w;
            w.WriteU8(static_cast<uint8_t>(type));
            w.WriteU32(static_cast<uint32_t>(payload.size()));
            w.WriteBytes(payload);
            return N2TestSockets::SendAll(_socket, w.Data().data(), w.Size());
        }

        [[nodiscard]] size_t BytesAvailable() const { return N2TestSockets::BytesAvailable(_socket); }

        std::optional<std::vector<uint8_t>> ReadResponse()
        {
            std::vector<uint8_t> frame(5);
            if (!ReadExact(frame.data(), 5))
                return std::nullopt;
            uint32_t size = 0;
            std::memcpy(&size, frame.data() + 1, sizeof(size));
            frame.resize(5 + size);
            if (size > 0 && !ReadExact(frame.data() + 5, size))
                return std::nullopt;
            return frame;
        }

        /// True once the server has closed (or reset) the connection; a receive timeout doesn't count
        bool IsClosedByServer()
        {
            std::uint8_t byte = 0;
            const long long result = N2TestSockets::Receive(_socket, &byte, 1);
            return result == 0 || (result < 0 && !N2TestSockets::LastErrorWasTimeout());
        }

    private:
        bool ReadExact(uint8_t *out, size_t count)
        {
            size_t received = 0;
            while (received < count)
            {
                const long long result = N2TestSockets::Receive(_socket, out + received, count - received);
                if (result <= 0)
                    return false;
                received += static_cast<size_t>(result);
            }
            return true;
        }

        N2TestSockets::Runtime _runtime; // before the socket: Winsock must be started first and stopped last
        bool _connected = false;
        N2TestSockets::Handle _socket = N2TestSockets::InvalidHandle;
    };

    /// Sends a request and plays the host's main loop until its response arrives
    std::optional<Frame> Roundtrip(EditorServer &server, TestClient &client, const CommandType type,
                                   const std::vector<uint8_t> &payload = {})
    {
        if (!client.SendRequest(type, payload))
            return std::nullopt;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (client.BytesAvailable() < 5 && std::chrono::steady_clock::now() < deadline)
        {
            server.ProcessCommands(std::chrono::milliseconds(10));
        }
        const auto response = client.ReadResponse();
        if (!response.has_value())
            return std::nullopt;
        return Decode(*response);
    }

    /// Shutdown is answered on the network thread: nothing needs draining
    std::optional<Frame> SendShutdown(TestClient &client)
    {
        if (!client.SendRequest(CommandType::Shutdown, {}))
            return std::nullopt;
        const auto response = client.ReadResponse();
        if (!response.has_value())
            return std::nullopt;
        return Decode(*response);
    }

    /// Plays the host's main loop until the log has this text (the network thread posts its lines to it)
    bool PumpUntilLogged(EditorServer &server, const LogCapture &log, const std::string_view text)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!log.Has(text) && std::chrono::steady_clock::now() < deadline)
        {
            server.ProcessCommands(std::chrono::milliseconds(10));
        }
        return log.Has(text);
    }

    void StopWithin(EditorServer &server, std::chrono::seconds limit)
    {
        std::promise<void> stopped;
        std::future<void> stoppedFuture = stopped.get_future();
        std::thread stopper(
            [&]
            {
                server.Stop();
                stopped.set_value();
            });
        if (stoppedFuture.wait_for(limit) != std::future_status::ready)
        {
            std::fprintf(stderr, "EditorServer::Stop() hung with a client connected\n");
            std::abort(); // a hung thread can't be joined
        }
        stopper.join();
    }

    bool WaitUntilStopped(const EditorServer &server, std::chrono::seconds limit)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (server.IsRunning() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return !server.IsRunning();
    }

    /// A server with a tokens file of these users, in a folder of its own
    class TokensFileTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if (!EditorTlsServer::IsAvailable())
            {
                GTEST_SKIP() << "a tokens file needs the TLS library's SHA-256 (N2ENGINE_EDITOR_TLS)";
            }
            _folder = std::make_unique<TempFolder>();
            _file = _folder->Path() / "tokens.json";
            WriteTokensFile(_file, {Alice, Bob, Carol});
        }

        [[nodiscard]] const fs::path &File() const { return _file; }

        std::unique_ptr<TempFolder> _folder;
        fs::path _file;
    };
}

// ==================== Roles, on every leg ====================

TEST(RoleTest, AnAdminMayRunEverythingAnEditorAllButTheHostsControlsAViewerOnlyReads)
{
    size_t reads = 0;
    size_t writes = 0;
    size_t admins = 0;
    for (int id = 0; id < 256; ++id)
    {
        const auto commandId = static_cast<uint8_t>(id);
        const auto commandClass = EditorServer::ClassifyCommand(commandId);
        EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, commandId)) << id << ": an admin may run anything";
        if (!commandClass.has_value())
        {
            // Default deny: a command nobody classified (or no command at all) is only an admin's
            EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Editor, commandId)) << id;
            EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, commandId)) << id;
            continue;
        }
        switch (*commandClass)
        {
        case EditorServer::CommandClass::Read:
            ++reads;
            EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Editor, commandId)) << EditorServer::CommandName(commandId);
            EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Viewer, commandId)) << EditorServer::CommandName(commandId);
            break;
        case EditorServer::CommandClass::Write:
            ++writes;
            EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Editor, commandId)) << EditorServer::CommandName(commandId);
            EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, commandId)) << EditorServer::CommandName(commandId);
            break;
        case EditorServer::CommandClass::Admin:
            ++admins;
            EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Editor, commandId)) << EditorServer::CommandName(commandId);
            EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, commandId)) << EditorServer::CommandName(commandId);
            break;
        }
    }
    EXPECT_GT(reads, 0u);
    EXPECT_GT(writes, 0u);
    EXPECT_GT(admins, 0u);
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, static_cast<uint8_t>(CommandType::Shutdown)));
    EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Editor, static_cast<uint8_t>(CommandType::Shutdown)));
}

TEST(RoleTest, AViewerMayNotEnterPlayModeAnEditorMay)
{
    // Playing runs the project's Lua on the host: the play commands are writes, so a viewer (reads only) is refused them
    for (const CommandType play : {CommandType::SendInput, CommandType::WritePlaySnapshot, CommandType::SetPaused,
                                   CommandType::Step})
    {
        const auto id = static_cast<uint8_t>(play);
        EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, id)) << EditorServer::CommandName(id);
        EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Editor, id)) << EditorServer::CommandName(id);
        EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, id)) << EditorServer::CommandName(id);
    }
    // ... but it may watch: what the game is doing is a read
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Viewer, static_cast<uint8_t>(CommandType::GetPlayState)));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Viewer, static_cast<uint8_t>(CommandType::RenderFrame)));
    // Hello opens every role's session
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Viewer, static_cast<uint8_t>(CommandType::Hello)));
}

TEST(RoleTest, TheRefusalSaysWhichRoleMayNotRunWhichCommand)
{
    EXPECT_EQ(EditorServer::RoleDeniedMessage(TokenRole::Viewer, static_cast<uint8_t>(CommandType::CreateEntity)),
              "forbidden: role viewer is not allowed to run CreateEntity");
    EXPECT_EQ(EditorServer::RoleDeniedMessage(TokenRole::Editor, static_cast<uint8_t>(CommandType::Shutdown)),
              "forbidden: role editor is not allowed to run Shutdown");
    EXPECT_EQ(EditorServer::RoleDeniedMessage(TokenRole::Viewer, 0x7E), "forbidden: role viewer is not allowed to run command 0x7E");
    EXPECT_EQ(std::string{ErrorCodeForbidden}, "forbidden");
}

// ==================== The single token, on every leg ====================

TEST(TokenEnvTest, ASingleAccessTokenIsOneAdminUserCalledToken)
{
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("the-one-token"));

    const Info info = DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload("the-one-token")));
    EXPECT_EQ(info.user, "token");
    EXPECT_EQ(info.role, "admin");

    // Every command's gate is open to it: nothing it runs is refused as forbidden (a few run for real; the rest is the
    // role table, RoleTest above)
    for (const CommandType read : {CommandType::GetAllEntities, CommandType::GetPlayState, CommandType::GetEngineHealth})
    {
        EXPECT_FALSE(IsForbidden(Execute(server, read))) << static_cast<int>(read);
    }
    for (const CommandType write : {CommandType::CreateScript, CommandType::CreateEntity, CommandType::SendInput})
    {
        EXPECT_FALSE(IsForbidden(RunPastTheGate(server, write))) << static_cast<int>(write);
    }
    EXPECT_EQ(RunPastTheGate(server, CommandType::CreateScript).type, static_cast<uint8_t>(ResponseType::ScriptData));
}

TEST(TokenEnvTest, ATokenHostIgnoresTheUserAHelloNames)
{
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("the-one-token"));
    // A 1.12 client names a user whatever the host has: with the single token it makes no difference
    const Info info = DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload("the-one-token", false, "alice")));
    EXPECT_EQ(info.user, "token");
    EXPECT_EQ(info.role, "admin");
    EXPECT_EQ(Execute(server, CommandType::Hello, HelloPayload("wrong", false, "alice")).type, ErrorType);
}

TEST(TokenEnvTest, ACommandBeforeAHelloIsRefusedEvenWhenItReachesTheHandler)
{
    // The network thread never lets one through (EditorServerHardeningTests covers that over a socket); this is the
    // second line, for a caller that reaches ExecuteCommand some other way
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("the-one-token"));
    const Frame response = Execute(server, CommandType::GetAllEntities);
    EXPECT_EQ(response.type, ErrorType);
    EXPECT_EQ(response.Text(), EditorServer::HelloRequiredError);

    // And a failed Hello doesn't open it
    EXPECT_EQ(Execute(server, CommandType::Hello, HelloPayload("wrong")).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::GetAllEntities).Text(), EditorServer::HelloRequiredError);

    EXPECT_EQ(Execute(server, CommandType::Hello, HelloPayload("the-one-token")).type, ServerInfoType);
    EXPECT_NE(Execute(server, CommandType::GetAllEntities).Text(), EditorServer::HelloRequiredError);
}

TEST(TokenEnvTest, ANewConnectionStartsWithoutTheLastOnesIdentity)
{
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("the-one-token"));
    ASSERT_TRUE(server.Start(0));
    {
        TestClient first(server.GetPort());
        ASSERT_TRUE(first.IsConnected());
        const auto hello = Roundtrip(server, first, CommandType::Hello, HelloPayload("the-one-token"));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(DecodeServerInfo(*hello).role, "admin");
    }
    // The second connection says nothing: it gets the Hello error, not the first one's rights
    TestClient second(server.GetPort());
    ASSERT_TRUE(second.IsConnected());
    ASSERT_TRUE(second.SendRequest(CommandType::GetAllEntities, {}));
    const auto response = second.ReadResponse();
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(Decode(*response).Text(), EditorServer::HelloRequiredError);
    EXPECT_TRUE(second.IsClosedByServer());
    // The host itself holds no identity between connections either: a command that reaches it with nobody's Hello is
    // refused, though the last connection was an admin
    EXPECT_EQ(Execute(server, CommandType::GetAllEntities).Text(), EditorServer::HelloRequiredError);
    StopWithin(server, std::chrono::seconds(10));
}

TEST(TokenEnvTest, TheAdminOfASingleTokenMayShutTheHostDown)
{
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("the-one-token"));
    ASSERT_TRUE(server.Start(0));
    TestClient client(server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload("the-one-token"));
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();

    const auto shutdown = SendShutdown(client);
    ASSERT_TRUE(shutdown.has_value());
    EXPECT_EQ(shutdown->type, OkType) << shutdown->Text();
    EXPECT_TRUE(WaitUntilStopped(server, std::chrono::seconds(10)));
    StopWithin(server, std::chrono::seconds(10));
}

// ==================== A tokens file and the other kinds of token ====================

TEST(TokensFileServerTest, ATokensFileAndASingleTokenAreExclusiveInBothOrders)
{
    TempFolder folder;
    const fs::path file = folder.Path() / "tokens.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << "[]";
    }

    {
        EditorServer server;
        ASSERT_TRUE(server.SetAccessToken("a-token"));
        const auto set = server.SetTokensFile(file);
        ASSERT_FALSE(set);
        EXPECT_NE(set.error().find("can't be combined"), std::string::npos) << set.error();
        EXPECT_FALSE(server.HasTokensFile());
        EXPECT_TRUE(server.RequiresAccessToken()) << "the token it had stays";
    }
    if (EditorTlsServer::IsAvailable())
    {
        EditorServer server;
#ifndef _WIN32
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
        ASSERT_TRUE(server.SetTokensFile(file));
        EXPECT_TRUE(server.HasTokensFile());
        EXPECT_FALSE(server.SetAccessToken("a-token")) << "a tokens file and a single token can't both gate Hello";
        EXPECT_TRUE(server.RequiresAccessToken());
    }
}

TEST(TokensFileServerTest, WithoutTheTlsLibraryATokensFileIsAStartupError)
{
    if (EditorTlsServer::IsAvailable())
        GTEST_SKIP() << "this build has the TLS library";

    TempFolder folder;
    const fs::path file = folder.Path() / "tokens.json";
    {
        std::ofstream out(file, std::ios::binary);
        out << "[]";
    }
    EditorServer server;
    const auto set = server.SetTokensFile(file);
    ASSERT_FALSE(set);
    EXPECT_NE(set.error().find("N2ENGINE_EDITOR_TLS"), std::string::npos) << set.error();
    EXPECT_NE(set.error().find("SHA-256"), std::string::npos) << set.error();
    // The host stays as it was: open, not silently gated by a file it can't check
    EXPECT_FALSE(server.HasTokensFile());
    EXPECT_FALSE(server.RequiresAccessToken());
}

TEST_F(TokensFileTest, TheFileMustLoadBeforeTheServerStarts)
{
    EditorServer server;
    const auto missing = server.SetTokensFile(_folder->Path() / "missing.json");
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().find("missing.json"), std::string::npos) << missing.error();
    EXPECT_FALSE(server.HasTokensFile());
    EXPECT_FALSE(server.RequiresAccessToken());

    // A malformed file: the error names the entry and never repeats a token or hash
    const fs::path bad = _folder->Path() / "bad.json";
    WriteTokensText(bad, "[{\"user\":\"alice\",\"role\":\"admin\",\"hash\":\"correct horse battery staple\"}]");
    const auto malformed = server.SetTokensFile(bad);
    ASSERT_FALSE(malformed);
    EXPECT_NE(malformed.error().find("entry 1 (alice)"), std::string::npos) << malformed.error();
    EXPECT_EQ(malformed.error().find("correct horse"), std::string::npos) << malformed.error();
    EXPECT_FALSE(server.HasTokensFile());

    WriteTokensText(bad, "{\"alice\":\"token\"}");
    EXPECT_FALSE(server.SetTokensFile(bad));

    // It takes the good one afterwards, and then not another while serving
    ASSERT_TRUE(server.SetTokensFile(File()));
    EXPECT_TRUE(server.HasTokensFile());
    EXPECT_TRUE(server.RequiresAccessToken());
    ASSERT_TRUE(server.Start(0));
    const auto late = server.SetTokensFile(File());
    ASSERT_FALSE(late);
    EXPECT_NE(late.error().find("running"), std::string::npos) << late.error();
    StopWithin(server, std::chrono::seconds(10));
}

TEST_F(TokensFileTest, AFileItsGroupOrOthersCanReadIsRefusedOnPosix)
{
#ifdef _WIN32
    GTEST_SKIP() << "file permissions aren't checked on Windows (ACLs)";
#else
    fs::permissions(File(), fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                                fs::perms::others_read, fs::perm_options::replace);
    EditorServer server;
    const auto set = server.SetTokensFile(File());
    ASSERT_FALSE(set);
    EXPECT_NE(set.error().find("chmod 600"), std::string::npos) << set.error();
    EXPECT_NE(set.error().find("tokens.json"), std::string::npos) << set.error();
    EXPECT_FALSE(server.RequiresAccessToken());

    fs::permissions(File(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    EXPECT_TRUE(server.SetTokensFile(File()));
#endif
}

// ==================== Hello with a tokens file ====================

TEST_F(TokensFileTest, HelloBindsTheMatchedUsersRoleAndReportsIt)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));

    for (const User &user : {Alice, Bob, Carol})
    {
        // A 1.12 client names its user; a 1.11 client sends errorCodes only; a 1.10 client sends neither: the same match
        for (const auto &payload : {HelloPayload(user.token, true, user.name), HelloPayload(user.token, true),
                                    HelloPayload(user.token, std::nullopt)})
        {
            const Info info = DecodeServerInfo(Execute(server, CommandType::Hello, payload));
            EXPECT_EQ(info.user, user.name);
            EXPECT_EQ(info.role, TokenRoleName(user.role));
            EXPECT_EQ(info.protocolVersion, ProtocolVersion);
        }
    }
}

TEST_F(TokensFileTest, AWrongTokenOrAWrongUserIsRefusedTheSameWay)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));

    struct Attempt
    {
        const char *what;
        std::vector<uint8_t> payload;
    };
    const std::vector<Attempt> attempts = {
        {"a token nobody has", HelloPayload("not-a-token", true, "alice")},
        {"no token", HelloPayload("", true, "alice")},
        {"no token and no user", HelloPayload("", true)},
        {"a token nobody has, no user", HelloPayload("not-a-token", true)},
        {"the end of a token", HelloPayload(Alice.token.substr(0, Alice.token.size() - 1), true, "alice")},
        {"a token and more", HelloPayload(Alice.token + "x", true, "alice")},
        {"bob's token as alice", HelloPayload(Bob.token, true, "alice")},
        {"alice's token as bob", HelloPayload(Alice.token, true, "bob")},
        {"a user the file doesn't have", HelloPayload(Alice.token, true, "mallory")},
        {"the user in another case", HelloPayload(Alice.token, true, "Alice")},
        {"the hash text as the token", HelloPayload(FormatTokenHash(*HashToken(Alice.token)), true, "alice")},
    };
    for (const Attempt &attempt : attempts)
    {
        const Frame response = Execute(server, CommandType::Hello, attempt.payload);
        EXPECT_EQ(response.type, ErrorType) << attempt.what;
        EXPECT_EQ(response.Text(), "Invalid access token") << attempt.what;
    }
    // A wrong user and a wrong token say nothing about which of them was wrong, or whether the user exists
    EXPECT_EQ(Execute(server, CommandType::Hello, HelloPayload(Alice.token, true, "mallory")).Text(),
              Execute(server, CommandType::Hello, HelloPayload("not-a-token", true, "alice")).Text());
}

TEST_F(TokensFileTest, TheTokenIsCheckedBeforeTheVersion)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    BufferWriter w;
    w.WriteString("tokens test");
    w.WriteString("999.0.0");
    w.WriteString("not-a-token");
    const Frame response = Execute(server, CommandType::Hello, w.Release());
    EXPECT_EQ(response.type, ErrorType);
    EXPECT_EQ(response.Text(), "Invalid access token");
}

TEST_F(TokensFileTest, ACommandBeforeAHelloOrAfterAFailedOneIsRefused)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    EXPECT_EQ(Execute(server, CommandType::GetAllEntities).Text(), EditorServer::HelloRequiredError);
    EXPECT_EQ(Execute(server, CommandType::Hello, HelloPayload("not-a-token")).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::GetAllEntities).Text(), EditorServer::HelloRequiredError);
    EXPECT_EQ(Execute(server, CommandType::Hello, HelloPayload(Carol.token)).type, ServerInfoType);
    EXPECT_NE(Execute(server, CommandType::GetAllEntities).Text(), EditorServer::HelloRequiredError);
}

// ==================== What each role may run ====================

TEST_F(TokensFileTest, AViewerIsRefusedEveryCommandThatIsNotAReadWithTheForbiddenCode)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_EQ(DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload(Carol.token, true, "carol"))).role,
              "viewer");

    size_t refused = 0;
    for (int id = 0; id < 256; ++id)
    {
        const auto commandId = static_cast<uint8_t>(id);
        const auto commandClass = EditorServer::ClassifyCommand(commandId);
        if (commandClass.has_value() && *commandClass == EditorServer::CommandClass::Read)
            continue;
        // Refused before any handler reads the payload: nothing changes, whatever the command would have done
        const Frame response = Execute(server, commandId);
        const Coded coded = DecodeCoded(response);
        EXPECT_EQ(coded.code, "forbidden") << id;
        EXPECT_EQ(coded.message, EditorServer::RoleDeniedMessage(TokenRole::Viewer, commandId)) << id;
        ++refused;
    }
    EXPECT_GT(refused, 40u) << "every write, the play commands and Shutdown, and the ids that are no command";

    // The reads still work, and the connection (the session) is as it was
    for (const CommandType read : {CommandType::GetAllEntities, CommandType::GetPlayState, CommandType::GetEngineHealth})
    {
        EXPECT_FALSE(IsForbidden(Execute(server, read))) << static_cast<int>(read);
    }
}

TEST_F(TokensFileTest, AViewerMayNotPlay)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_EQ(DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload(Carol.token, true, "carol"))).role,
              "viewer");
    for (const CommandType play : {CommandType::SendInput, CommandType::WritePlaySnapshot, CommandType::SetPaused,
                                   CommandType::Step})
    {
        const Coded coded = DecodeCoded(Execute(server, play));
        EXPECT_EQ(coded.code, "forbidden") << static_cast<int>(play);
        EXPECT_EQ(coded.message, EditorServer::RoleDeniedMessage(TokenRole::Viewer, static_cast<uint8_t>(play)));
    }
    EXPECT_FALSE(IsForbidden(Execute(server, CommandType::GetPlayState)));
}

TEST_F(TokensFileTest, ARefusalIsAPlainErrorForAClientThatDidNotAskForCodes)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    // A 1.10 client: three fields, never ErrorCoded; the message still says why
    ASSERT_EQ(DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload(Carol.token, std::nullopt))).role, "viewer");
    const Frame response = Execute(server, CommandType::CreateEntity, StringPayload("Cube"));
    EXPECT_EQ(response.type, ErrorType);
    EXPECT_EQ(response.Text(), "forbidden: role viewer is not allowed to run CreateEntity");
}

TEST_F(TokensFileTest, AnEditorMayWriteAndMayNotShutDown)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_EQ(DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload(Bob.token, true, "bob"))).role, "editor");

    // Past the role gate (the handler may still fail: there is no scene), for a write and for the play commands
    for (const CommandType write : {CommandType::CreateScript, CommandType::CreateEntity, CommandType::SendInput,
                                    CommandType::SetPaused})
    {
        EXPECT_FALSE(IsForbidden(RunPastTheGate(server, write))) << static_cast<int>(write);
    }
    EXPECT_EQ(RunPastTheGate(server, CommandType::CreateScript).type, static_cast<uint8_t>(ResponseType::ScriptData));
    // The Admin class is an admin's
    const Coded coded = DecodeCoded(Execute(server, CommandType::Shutdown));
    EXPECT_EQ(coded.code, "forbidden");
    EXPECT_EQ(coded.message, "forbidden: role editor is not allowed to run Shutdown");
}

TEST_F(TokensFileTest, AnAdminIsRefusedNothing)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_EQ(DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload(Alice.token, true, "alice"))).role, "admin");
    for (const CommandType write : {CommandType::CreateScript, CommandType::CreateEntity, CommandType::SendInput})
    {
        EXPECT_FALSE(IsForbidden(RunPastTheGate(server, write))) << static_cast<int>(write);
    }
    // An id that is no command is an unknown command, not forbidden
    const Frame unknown = Execute(server, uint8_t{0x7E});
    EXPECT_EQ(unknown.type, ErrorType);
    EXPECT_FALSE(IsForbidden(unknown));
}

TEST_F(TokensFileTest, TheRoleIsTheConnectionsAndAnotherHelloReplacesIt)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_EQ(DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload(Carol.token, true, "carol"))).role, "viewer");
    EXPECT_TRUE(IsForbidden(Execute(server, CommandType::CreateEntity, StringPayload("Cube"))));

    // A later Hello with another valid token is that user's session from then on
    ASSERT_EQ(DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload(Bob.token, true, "bob"))).role, "editor");
    EXPECT_FALSE(IsForbidden(Execute(server, CommandType::CreateEntity, StringPayload("Cube"))));

    // A failed one doesn't hand the old rights to anybody: it is an Error (and over a socket, the end of the connection)
    EXPECT_EQ(Execute(server, CommandType::Hello, HelloPayload("not-a-token", true, "bob")).type, ErrorType);
}

TEST_F(TokensFileTest, ReadOnlyStillAppliesOnTopOfTheRoles)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.SetReadOnly(true));
    ASSERT_EQ(DecodeServerInfo(Execute(server, CommandType::Hello, HelloPayload(Alice.token, true, "alice"))).role, "admin");

    // An admin of a --read-only host still can't write: that switch is the host's, and comes first
    const Coded coded = DecodeCoded(Execute(server, CommandType::CreateEntity, StringPayload("Cube")));
    EXPECT_EQ(coded.code, "readOnly");
    EXPECT_FALSE(IsForbidden(Execute(server, CommandType::GetAllEntities)));
}

// ==================== Log lines ====================

TEST_F(TokensFileTest, HelloAndTheAuditLogNameTheUserAndNeverTheTokenOrItsHash)
{
    LogCapture log;
    {
        EditorServer server;
        ASSERT_TRUE(server.SetTokensFile(File()));

        // A failed Hello (the user the client claimed, no role), then each user's good one
        EXPECT_EQ(Execute(server, CommandType::Hello, HelloPayload("not-a-token", true, "mallory")).type, ErrorType);
        EXPECT_EQ(Execute(server, CommandType::Hello, HelloPayload(Alice.token, true, "bob")).type, ErrorType);
        ASSERT_EQ(Execute(server, CommandType::Hello, HelloPayload(Bob.token, true, "bob")).type, ServerInfoType);

        // Writes are accounted for by user; a polled command and a read are silent
        (void)RunPastTheGate(server, CommandType::CreateEntity);
        (void)RunPastTheGate(server, CommandType::SendInput);
        (void)Execute(server, CommandType::GetAllEntities);
        (void)Execute(server, CommandType::GetEngineHealth);
        // A refused one is logged as a refusal, not as a change
        (void)Execute(server, CommandType::Shutdown);

        ASSERT_EQ(Execute(server, CommandType::Hello, HelloPayload(Carol.token, true, "carol")).type, ServerInfoType);
        (void)Execute(server, CommandType::DestroyEntity, StringPayload("00000000-0000-0000-0000-000000000000"));
        ASSERT_EQ(Execute(server, CommandType::Hello, HelloPayload(Alice.token, false)).type, ServerInfoType);
        (void)RunPastTheGate(server, CommandType::CreateScript);
    }

    const std::vector<std::string> lines = log.Lines();
    EXPECT_TRUE(log.Has("Refused Hello from editor client 'tokens test' (user=mallory role=none): wrong access token"));
    EXPECT_TRUE(log.Has("Refused Hello from editor client 'tokens test' (user=bob role=none): wrong access token"));
    EXPECT_TRUE(log.Has("said Hello (protocol " + std::string{ProtocolVersion} + ") user=bob role=editor"));
    EXPECT_TRUE(log.Has("said Hello (protocol " + std::string{ProtocolVersion} + ") user=carol role=viewer"));
    EXPECT_TRUE(log.Has("said Hello (protocol " + std::string{ProtocolVersion} + ") user=alice role=admin"));

    EXPECT_TRUE(log.Has("audit: user=bob role=editor command=CreateEntity"));
    EXPECT_TRUE(log.Has("audit: user=alice role=admin command=CreateScript"));
    EXPECT_TRUE(log.Has("audit: denied user=bob role=editor command=Shutdown"));
    EXPECT_TRUE(log.Has("audit: denied user=carol role=viewer command=DestroyEntity"));
    EXPECT_FALSE(log.Has("audit: user=carol")) << "a refused command isn't a change";
    EXPECT_FALSE(log.Has("command=SendInput")) << "a polled command stays silent";
    EXPECT_FALSE(log.Has("command=GetAllEntities")) << "a read is nothing to account for";
    EXPECT_FALSE(log.Has("command=GetEngineHealth"));

    // Neither a token nor a hash, nor any part of a token, reaches a line
    std::set<std::string> secrets = {"not-a-token"};
    for (const User &user : {Alice, Bob, Carol})
    {
        secrets.insert(user.token);
        secrets.insert(user.token.substr(0, 12));
        const std::string hash = FormatTokenHash(*HashToken(user.token));
        secrets.insert(hash);
        secrets.insert(hash.substr(TokenHashPrefix.size(), 16));
    }
    for (const std::string &line : lines)
    {
        for (const std::string &secret : secrets)
        {
            EXPECT_EQ(line.find(secret), std::string::npos) << "'" << secret << "' in: " << line;
        }
        EXPECT_EQ(line.find("sha256"), std::string::npos) << line;
    }
}

// ==================== Over a socket: Shutdown, the user field, revocation ====================

TEST_F(TokensFileTest, ShutdownIsAnAdminsAloneOverASocketAndARefusalKeepsTheConnection)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.Start(0));

    for (const User &user : {Carol, Bob})
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(user.token, true, user.name));
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();

        // Answered on the network thread, which checks the role itself
        const auto shutdown = SendShutdown(client);
        ASSERT_TRUE(shutdown.has_value()) << user.name;
        EXPECT_EQ(DecodeCoded(*shutdown).code, "forbidden") << user.name;
        EXPECT_TRUE(server.IsRunning()) << user.name << " must not stop the host";

        // The connection goes on: a read still works after the refusal
        const auto read = Roundtrip(server, client, CommandType::GetPlayState);
        ASSERT_TRUE(read.has_value()) << user.name;
        EXPECT_FALSE(IsForbidden(*read)) << user.name;
    }

    TestClient admin(server.GetPort());
    ASSERT_TRUE(admin.IsConnected());
    const auto hello = Roundtrip(server, admin, CommandType::Hello, HelloPayload(Alice.token, true, "alice"));
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
    const auto shutdown = SendShutdown(admin);
    ASSERT_TRUE(shutdown.has_value());
    EXPECT_EQ(shutdown->type, OkType) << shutdown->Text();
    EXPECT_TRUE(WaitUntilStopped(server, std::chrono::seconds(10)));
    StopWithin(server, std::chrono::seconds(10));
}

TEST_F(TokensFileTest, AFailedHelloOverASocketClosesTheConnection)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.Start(0));

    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto response = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "alice"));
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->Text(), "Invalid access token");
        EXPECT_TRUE(client.IsClosedByServer());
    }
    {
        // A connection that sends a command first is refused and closed too
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        ASSERT_TRUE(client.SendRequest(CommandType::GetAllEntities, {}));
        const auto response = client.ReadResponse();
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(Decode(*response).Text(), EditorServer::HelloRequiredError);
        EXPECT_TRUE(client.IsClosedByServer());
    }
    StopWithin(server, std::chrono::seconds(10));
}

TEST_F(TokensFileTest, RevokingAUserByRewritingTheFileTakesEffectOnTheNextConnection)
{
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.Start(0));

    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
    }

    // Bob is removed (and a user added) while the host runs: no signal, no restart
    const User dave{"dave", "dave-secret-token-0004", TokenRole::Editor};
    WriteTokensFile(File(), {Alice, Carol, dave});

    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto refused = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(refused->Text(), "Invalid access token") << "bob's token is revoked";
        EXPECT_TRUE(client.IsClosedByServer());
    }
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(dave.token, true, "dave"));
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
        EXPECT_EQ(DecodeServerInfo(*hello).role, "editor");
    }
    {
        // Without a user too (an older client): dave's token is found across the entries
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(dave.token, std::nullopt));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(DecodeServerInfo(*hello).user, "dave");
    }
    EXPECT_TRUE(PumpUntilLogged(server, log, "Reloaded the tokens file: 3 user(s)"));

    // Everyone revoked: an empty file is a table with no users
    WriteTokensText(File(), "[]");
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto refused = Roundtrip(server, client, CommandType::Hello, HelloPayload(Alice.token, true, "alice"));
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(refused->type, ErrorType);
    }
    StopWithin(server, std::chrono::seconds(10));
}

TEST_F(TokensFileTest, ARoleChangeByRewritingTheFileTakesEffectOnTheNextConnection)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.Start(0));
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(DecodeServerInfo(*hello).role, "editor");
    }
    WriteTokensFile(File(), {Alice, User{"bob", Bob.token, TokenRole::Viewer}, Carol});
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(DecodeServerInfo(*hello).role, "viewer");
        const auto write = Roundtrip(server, client, CommandType::CreateEntity, StringPayload("Cube"));
        ASSERT_TRUE(write.has_value());
        EXPECT_TRUE(IsForbidden(*write));
    }
    StopWithin(server, std::chrono::seconds(10));
}

TEST_F(TokensFileTest, AReloadThatRemovesTheOpenConnectionsUserClosesIt)
{
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.Start(0));

    TestClient client(server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
    ASSERT_TRUE(Roundtrip(server, client, CommandType::GetPlayState).has_value());

    // The file is rewritten without bob and the host is asked to look (what SIGHUP does): the open connection ends
    WriteTokensFile(File(), {Alice, Carol});
    server.RequestTokensReload();
    EXPECT_TRUE(client.IsClosedByServer()) << "bob's open connection must end when his entry is removed";
    EXPECT_TRUE(PumpUntilLogged(server, log, "Closing the connection of user=bob role=editor"));
    EXPECT_TRUE(server.IsRunning()) << "the host itself goes on";

    // And the host serves the others afterwards
    TestClient next(server.GetPort());
    ASSERT_TRUE(next.IsConnected());
    const auto carol = Roundtrip(server, next, CommandType::Hello, HelloPayload(Carol.token, true, "carol"));
    ASSERT_TRUE(carol.has_value());
    EXPECT_EQ(DecodeServerInfo(*carol).role, "viewer");
    StopWithin(server, std::chrono::seconds(10));
}

TEST_F(TokensFileTest, AReloadThatChangesTheOpenConnectionsRoleOrTokenClosesIt)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.Start(0));

    // A demotion
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
        WriteTokensFile(File(), {Alice, User{"bob", Bob.token, TokenRole::Viewer}, Carol});
        server.RequestTokensReload();
        EXPECT_TRUE(client.IsClosedByServer()) << "an editor demoted to viewer must reconnect to get the new role";
    }
    // A rotated token: the same user and role, another hash
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
        WriteTokensFile(File(), {Alice, User{"bob", "bob-rotated-token-0009", TokenRole::Viewer}, Carol});
        server.RequestTokensReload();
        EXPECT_TRUE(client.IsClosedByServer()) << "the old token must stop working on the open connection too";
    }
    StopWithin(server, std::chrono::seconds(10));
}

TEST_F(TokensFileTest, AReloadThatDoesNotTouchTheOpenConnectionsUserLeavesItOpen)
{
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.Start(0));

    TestClient client(server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Carol.token, true, "carol"));
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();

    // Bob goes, carol stays as she was
    WriteTokensFile(File(), {Alice, Carol});
    server.RequestTokensReload();
    EXPECT_TRUE(PumpUntilLogged(server, log, "Reloaded the tokens file: 2 user(s)"));

    const auto read = Roundtrip(server, client, CommandType::GetPlayState);
    ASSERT_TRUE(read.has_value()) << "carol's connection must survive a reload that didn't change her";
    EXPECT_FALSE(IsForbidden(*read));
    EXPECT_FALSE(log.Has("Closing the connection"));
    StopWithin(server, std::chrono::seconds(10));
}

TEST_F(TokensFileTest, AFailedReloadKeepsThePreviousTokensAndWarns)
{
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.Start(0));

    // A bad rewrite (a typo in a hash): the warning, and the old tokens stay in force
    WriteTokensText(File(), "[{\"user\":\"alice\",\"role\":\"admin\",\"hash\":\"sha256:nothex\"}]");
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(hello->type, ServerInfoType) << "bob is still a user: " << hello->Text();
    }
    EXPECT_TRUE(PumpUntilLogged(server, log, "Couldn't reload the tokens file, so the previous tokens stay in force"));
    EXPECT_TRUE(log.Has("entry 1 (alice)"));
    EXPECT_FALSE(log.Has("nothex")) << "the file's text isn't repeated";

    // Not JSON at all, and a missing file: the same
    WriteTokensText(File(), "this is not json");
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Carol.token, true, "carol"));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
    }
    std::error_code error;
    fs::remove(File(), error);
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Alice.token, true, "alice"));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(hello->type, ServerInfoType) << "a missing file keeps the tokens the host has";
    }

    // The file is fixed: the next connection loads it
    WriteTokensFile(File(), {Alice});
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto refused = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(refused->type, ErrorType) << "bob is gone once the file is good again";
    }
    StopWithin(server, std::chrono::seconds(10));
}

TEST_F(TokensFileTest, AReloadOfAFileTheOthersCanReadIsRefusedOnPosix)
{
#ifdef _WIN32
    GTEST_SKIP() << "file permissions aren't checked on Windows (ACLs)";
#else
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    ASSERT_TRUE(server.Start(0));

    // Rewritten with a new user, and left readable by everyone: not loaded, the old tokens stay
    const User dave{"dave", "dave-secret-token-0004", TokenRole::Editor};
    WriteTokensFile(File(), {Alice, Bob, Carol, dave});
    fs::permissions(File(), fs::perms::owner_read | fs::perms::owner_write | fs::perms::others_read,
                    fs::perm_options::replace);
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto refused = Roundtrip(server, client, CommandType::Hello, HelloPayload(dave.token, true, "dave"));
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(refused->type, ErrorType) << "dave is in a file the host won't read";
    }
    EXPECT_TRUE(PumpUntilLogged(server, log, "chmod 600"));
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Bob.token, true, "bob"));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(hello->type, ServerInfoType) << "the old tokens are still in force";
    }

    // Made private again (and touched, so the host looks): now it loads
    WriteTokensFile(File(), {Alice, Bob, Carol, dave});
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(dave.token, true, "dave"));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
    }
    StopWithin(server, std::chrono::seconds(10));
#endif
}
