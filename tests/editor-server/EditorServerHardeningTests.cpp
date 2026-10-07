#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/Logger.hpp>
#include <engine/sceneManagement/SceneManager.hpp>

#ifdef _WIN32
// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
namespace fs = std::filesystem;

namespace
{
    struct DecodedFrame
    {
        uint8_t type = 0xEE;
        std::string body;
    };

    DecodedFrame Decode(const std::vector<uint8_t> &frame)
    {
        BufferReader r(frame);
        DecodedFrame out;
        out.type = r.ReadU8();
        const uint32_t size = r.ReadU32();
        const auto body = r.ReadBytes(size);
        out.body.assign(body.begin(), body.end());
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the response payload";
        return out;
    }

    std::vector<uint8_t> ToVector(const BufferWriter &w)
    {
        return {w.Data().begin(), w.Data().end()};
    }

    std::vector<uint8_t> StringPayload(const std::string &s)
    {
        BufferWriter w;
        w.WriteString(s);
        return ToVector(w);
    }

    DecodedFrame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        return Decode(server.ExecuteCommand(static_cast<uint8_t>(type), payload));
    }

    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
}

// ==================== Payload and viewport limits ====================

TEST(EditorServerLimitsTest, PayloadCapIs64MiB)
{
    EXPECT_EQ(EditorServer::MaxPayloadBytes, 64u * 1024u * 1024u);
    EXPECT_TRUE(EditorServer::IsPayloadLengthAllowed(0));
    EXPECT_TRUE(EditorServer::IsPayloadLengthAllowed(EditorServer::MaxPayloadBytes));
    EXPECT_FALSE(EditorServer::IsPayloadLengthAllowed(EditorServer::MaxPayloadBytes + 1));
    EXPECT_FALSE(EditorServer::IsPayloadLengthAllowed(0xFFFFFFFFu));
}

TEST(EditorServerLimitsTest, ViewportSizeMustBePositiveAndCapped)
{
    EXPECT_TRUE(EditorServer::IsViewportSizeValid(1, 1));
    EXPECT_TRUE(EditorServer::IsViewportSizeValid(1280, 720));
    EXPECT_TRUE(EditorServer::IsViewportSizeValid(EditorServer::MaxViewportDimension, EditorServer::MaxViewportDimension));

    EXPECT_FALSE(EditorServer::IsViewportSizeValid(0, 720));
    EXPECT_FALSE(EditorServer::IsViewportSizeValid(1280, 0));
    EXPECT_FALSE(EditorServer::IsViewportSizeValid(-1, 720));
    EXPECT_FALSE(EditorServer::IsViewportSizeValid(EditorServer::MaxViewportDimension + 1, 720));
    EXPECT_FALSE(EditorServer::IsViewportSizeValid(1280, 0x7FFFFFFF));
}

TEST(EditorServerLimitsTest, SetViewportSizeRejectsInvalidSizes)
{
    EditorServer server;
    for (const auto [width, height] : {std::pair{0, 720}, std::pair{-5, -5}, std::pair{100000, 100000}})
    {
        BufferWriter w;
        w.WriteI32(width);
        w.WriteI32(height);
        EXPECT_EQ(Execute(server, CommandType::SetViewportSize, ToVector(w)).type, ErrorType)
            << width << "x" << height;
    }
}

// ==================== Handler failures become Error responses ====================

TEST(EditorServerErrorsTest, MalformedPayloadIsAnErrorResponse)
{
    EditorServer server;

    // Too short for three floats: the bounds-checked reader throws, which must not escape
    const DecodedFrame truncated = Execute(server, CommandType::SetCameraPosition, {1, 2});
    EXPECT_EQ(truncated.type, ErrorType);
    EXPECT_NE(truncated.body.find("Malformed payload"), std::string::npos) << truncated.body;

    // A string length far beyond the payload
    BufferWriter w;
    w.WriteU32(1000);
    EXPECT_EQ(Execute(server, CommandType::CreateEntity, ToVector(w)).type, ErrorType);

    EXPECT_EQ(Execute(server, CommandType::DestroyEntity).type, ErrorType);
}

TEST(EditorServerErrorsTest, InvalidSceneJsonIsAnErrorResponse)
{
    EditorServer server;
    EXPECT_EQ(Execute(server, CommandType::LoadScene, StringPayload("{ not json")).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::LoadScene, StringPayload("")).type, ErrorType);
}

TEST(EditorServerErrorsTest, UnknownCommandIsAnErrorResponse)
{
    EditorServer server;
    EXPECT_EQ(Decode(server.ExecuteCommand(0x7E, {})).type, ErrorType);
}

TEST(EditorServerErrorsTest, ServerKeepsWorkingAfterAFailedCommand)
{
    EditorServer server;
    EXPECT_EQ(Execute(server, CommandType::SetCameraPosition, {1}).type, ErrorType);

    const DecodedFrame script = Execute(server, CommandType::CreateScript, StringPayload("player"));
    EXPECT_EQ(script.type, static_cast<uint8_t>(ResponseType::ScriptData));
    EXPECT_NE(script.body.find("Player"), std::string::npos);
}

// ==================== No scene loaded ====================

class EditorServerNoSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(SceneManager::GetCurScene(), nullptr) << "these tests need no scene loaded";
    }

    EditorServer server;
};

TEST_F(EditorServerNoSceneTest, DestroyEntityIsAnError)
{
    const DecodedFrame response = Execute(server, CommandType::DestroyEntity,
                                          StringPayload("00000000-0000-0000-0000-000000000000"));
    EXPECT_EQ(response.type, ErrorType);
    EXPECT_NE(response.body.find("No scene"), std::string::npos) << response.body;
}

TEST_F(EditorServerNoSceneTest, SetEntityTransformIsAnError)
{
    BufferWriter w;
    w.WriteString("00000000-0000-0000-0000-000000000000");
    for (int i = 0; i < 9; ++i)
    {
        w.WriteF32(1.0f);
    }
    const DecodedFrame response = Execute(server, CommandType::SetEntityTransform, ToVector(w));
    EXPECT_EQ(response.type, ErrorType);
    EXPECT_NE(response.body.find("No scene"), std::string::npos) << response.body;
}

TEST_F(EditorServerNoSceneTest, GetEntityTransformIsAnError)
{
    const DecodedFrame response = Execute(server, CommandType::GetEntityTransform,
                                          StringPayload("00000000-0000-0000-0000-000000000000"));
    EXPECT_EQ(response.type, ErrorType);
    EXPECT_NE(response.body.find("No scene"), std::string::npos) << response.body;
}

TEST_F(EditorServerNoSceneTest, SaveSceneAndCreateEntityAreErrors)
{
    EXPECT_EQ(Execute(server, CommandType::SaveScene).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::CreateEntity, StringPayload("Thing")).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::GetCurrentScene).type, ErrorType);
}

// ==================== Polled commands don't log per call ====================

namespace
{
    /// Collects every line logged while it lives
    class LogCapture
    {
    public:
        struct Line
        {
            std::string message;
            Logger::LogLevel level;
        };

        LogCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, const Logger::LogLevel level)
            {
                lines.push_back({std::string(message), level});
            };
        }
        ~LogCapture() { Logger::logEvent -= _id; }
        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        std::vector<Line> lines;

    private:
        size_t _id = 0;
    };
}

TEST(EditorServerLoggingTest, PolledCommandsAreTheOnesClientsPoll)
{
    for (const CommandType polled : {CommandType::RenderFrame, CommandType::GetAudio, CommandType::GetAllEntities,
                                     CommandType::GetEntityTransform, CommandType::GetCameraPosition,
                                     CommandType::GetEngineHealth})
    {
        EXPECT_TRUE(EditorServer::IsPolledCommand(static_cast<uint8_t>(polled))) << static_cast<int>(polled);
    }

    for (const CommandType notPolled : {CommandType::SetViewportSize, CommandType::SetCameraPosition,
                                        CommandType::CreateScene, CommandType::LoadScene, CommandType::SaveScene,
                                        CommandType::DeleteScene, CommandType::GetCurrentScene,
                                        CommandType::CreateEntity, CommandType::DestroyEntity,
                                        CommandType::SetEntityTransform, CommandType::CreateScript,
                                        CommandType::RescanAssets, CommandType::Hello, CommandType::Shutdown})
    {
        EXPECT_FALSE(EditorServer::IsPolledCommand(static_cast<uint8_t>(notPolled))) << static_cast<int>(notPolled);
    }
    EXPECT_FALSE(EditorServer::IsPolledCommand(0x7E)); // an unknown command still logs (and warns)
}

TEST_F(EditorServerNoSceneTest, PolledCommandsLogNothing)
{
    LogCapture capture;

    // Each answers normally (an empty list, an Error response for no scene, samples or "no audio stream")
    // without logging. RenderFrame, GetCameraPosition and GetEngineHealth aren't run here: they need an initialized
    // Application. PolledCommandsAreTheOnesClientsPoll covers the predicate for them.
    EXPECT_EQ(Execute(server, CommandType::GetAllEntities).type, static_cast<uint8_t>(ResponseType::EntityList));
    (void)Execute(server, CommandType::GetAudio);
    EXPECT_EQ(Execute(server, CommandType::GetEntityTransform,
                      StringPayload("00000000-0000-0000-0000-000000000000")).type, ErrorType);

    for (const auto &line : capture.lines)
    {
        ADD_FAILURE() << "a polled command logged: " << line.message;
    }
}

TEST_F(EditorServerNoSceneTest, OtherCommandsStillLogWhenIssued)
{
    LogCapture capture;

    EXPECT_EQ(Execute(server, CommandType::CreateScript, StringPayload("player")).type,
              static_cast<uint8_t>(ResponseType::ScriptData));

    bool issued = false;
    for (const auto &line : capture.lines)
    {
        if (line.message.find("Command Issued: 0x40") != std::string::npos)
            issued = true;
    }
    EXPECT_TRUE(issued) << "CreateScript (0x40) should log that it was issued";
}

TEST_F(EditorServerNoSceneTest, PolledCommandsStillLogFailures)
{
    LogCapture capture;

    // No entity id: the payload is malformed, so the handler throws and ExecuteCommand logs the failure
    EXPECT_EQ(Execute(server, CommandType::GetEntityTransform).type, ErrorType);

    ASSERT_EQ(capture.lines.size(), 1u);
    EXPECT_EQ(capture.lines[0].level, Logger::LogLevel::Error);
    EXPECT_NE(capture.lines[0].message.find("Command 0x33 failed"), std::string::npos) << capture.lines[0].message;
}

// ==================== DeleteScene path validation ====================

class DeleteSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _root = fs::temp_directory_path() /
                ("n2_editor_delete_scene_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        _scenes = _root / "scenes";
        fs::create_directories(_scenes / "sub");
        fs::create_directories(_scenes / "folder.json");

        Touch(_scenes / "Main.json");
        Touch(_scenes / "sub" / "Level.json");
        Touch(_scenes / "notes.txt");
        Touch(_root / "outside.json");
    }

    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(_root, error);
    }

    static void Touch(const fs::path &path)
    {
        std::ofstream(path) << "{}";
    }

    std::optional<fs::path> Resolve(const std::string &sceneName) const
    {
        return EditorServer::ResolveSceneFile(_scenes, sceneName);
    }

    fs::path _root;
    fs::path _scenes;
};

TEST_F(DeleteSceneTest, ResolvesSceneFilesInsideTheScenesDirectory)
{
    const auto byName = Resolve("Main");
    ASSERT_TRUE(byName.has_value());
    EXPECT_TRUE(fs::equivalent(*byName, _scenes / "Main.json"));

    const auto byFileName = Resolve("Main.json");
    ASSERT_TRUE(byFileName.has_value());
    EXPECT_TRUE(fs::equivalent(*byFileName, _scenes / "Main.json"));

    const auto nested = Resolve("sub/Level");
    ASSERT_TRUE(nested.has_value());
    EXPECT_TRUE(fs::equivalent(*nested, _scenes / "sub" / "Level.json"));

    // ".." that stays inside is fine
    const auto roundTrip = Resolve("sub/../Main.json");
    ASSERT_TRUE(roundTrip.has_value());
    EXPECT_TRUE(fs::equivalent(*roundTrip, _scenes / "Main.json"));
}

TEST_F(DeleteSceneTest, RejectsPathTraversal)
{
    EXPECT_FALSE(Resolve("../outside.json").has_value());
    EXPECT_FALSE(Resolve("../outside").has_value());
    EXPECT_FALSE(Resolve("sub/../../outside.json").has_value());
    EXPECT_FALSE(Resolve("..").has_value());
    EXPECT_FALSE(Resolve(".").has_value());
    EXPECT_FALSE(Resolve("../scenes").has_value());
}

TEST_F(DeleteSceneTest, RejectsAbsolutePaths)
{
    EXPECT_FALSE(Resolve((_root / "outside.json").string()).has_value());
    // Even one that points inside: the request names a scene, not a path
    EXPECT_FALSE(Resolve((_scenes / "Main.json").string()).has_value());
    EXPECT_FALSE(Resolve("/outside.json").has_value());
#ifdef _WIN32
    EXPECT_FALSE(Resolve("\\outside.json").has_value());
    EXPECT_FALSE(Resolve("C:outside.json").has_value());
    EXPECT_FALSE(Resolve("C:\\Windows\\win.ini").has_value());
#endif
}

TEST_F(DeleteSceneTest, OnlyEverNamesSceneFiles)
{
    // ".json" is appended unless the name already ends with it, so another extension can't be reached
    const auto notes = Resolve("notes.txt");
    ASSERT_TRUE(notes.has_value());
    EXPECT_EQ(notes->filename(), fs::path("notes.txt.json"));

    // Names may contain dots
    const auto dotted = Resolve("Level.1");
    ASSERT_TRUE(dotted.has_value());
    EXPECT_EQ(dotted->filename(), fs::path("Level.1.json"));

    // The extension matches in any case
    const auto upper = Resolve("Main.JSON");
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(upper->filename(), fs::path("Main.JSON"));
}

TEST_F(DeleteSceneTest, RejectsEmptyAndNulInput)
{
    EXPECT_FALSE(Resolve("").has_value());
    EXPECT_FALSE(Resolve(std::string("Main\0../../outside", 18)).has_value());
    EXPECT_FALSE(Resolve("sub/").has_value());
    EXPECT_FALSE(EditorServer::ResolveSceneFile({}, "Main").has_value());
}

TEST_F(DeleteSceneTest, DeletesASymlinkedSceneAsTheLinkNotItsTarget)
{
    std::error_code error;
    fs::create_symlink(_root / "outside.json", _scenes / "link.json", error);
    if (error)
    {
        GTEST_SKIP() << "can't create symlinks here: " << error.message();
    }

    EditorServer server;
    server.SetScenesDirectory(_scenes);
    EXPECT_EQ(Execute(server, CommandType::DeleteScene, StringPayload("link")).type, OkType);
    EXPECT_FALSE(fs::is_symlink(fs::symlink_status(_scenes / "link.json")));
    EXPECT_TRUE(fs::exists(_root / "outside.json"));
}

TEST_F(DeleteSceneTest, DeleteSceneOnlyDeletesInsideTheScenesDirectory)
{
    EditorServer server;
    server.SetScenesDirectory(_scenes);

    EXPECT_EQ(Execute(server, CommandType::DeleteScene, StringPayload("../outside.json")).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::DeleteScene, StringPayload((_root / "outside.json").string())).type,
              ErrorType);
    EXPECT_TRUE(fs::exists(_root / "outside.json"));

    EXPECT_EQ(Execute(server, CommandType::DeleteScene, StringPayload("notes.txt")).type, ErrorType);
    EXPECT_TRUE(fs::exists(_scenes / "notes.txt"));

    // A directory is not a scene file, even one named like one
    EXPECT_EQ(Execute(server, CommandType::DeleteScene, StringPayload("sub/")).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::DeleteScene, StringPayload("folder.json")).type, ErrorType);
    EXPECT_TRUE(fs::exists(_scenes / "sub" / "Level.json"));
    EXPECT_TRUE(fs::is_directory(_scenes / "folder.json"));

    EXPECT_EQ(Execute(server, CommandType::DeleteScene, StringPayload("Main")).type, OkType);
    EXPECT_FALSE(fs::exists(_scenes / "Main.json"));

    EXPECT_EQ(Execute(server, CommandType::DeleteScene, StringPayload("Main")).type, ErrorType) << "already deleted";
}

TEST_F(DeleteSceneTest, DeleteSceneIsDisabledWithoutAScenesDirectory)
{
    EditorServer server;
    EXPECT_EQ(Execute(server, CommandType::DeleteScene, StringPayload("Main")).type, ErrorType);
    EXPECT_TRUE(fs::exists(_scenes / "Main.json"));
}

// ==================== Over the socket ====================

#ifdef _WIN32
namespace
{
    // A loopback client for a server started on an OS-chosen port
    class TestClient
    {
    public:
        explicit TestClient(int port)
        {
            WSADATA wsaData;
            _wsaStarted = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;

            _socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (_socket == INVALID_SOCKET)
                return;

            DWORD timeoutMs = 10000; // a hung server fails the test instead of blocking it
            setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<u_short>(port));
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            _connected = connect(_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        }

        ~TestClient()
        {
            if (_socket != INVALID_SOCKET)
                closesocket(_socket);
            if (_wsaStarted)
                WSACleanup();
        }

        TestClient(const TestClient &) = delete;
        TestClient& operator=(const TestClient &) = delete;

        [[nodiscard]] bool IsConnected() const { return _connected; }

        bool SendRequest(CommandType type, const std::vector<uint8_t> &payload, uint32_t declaredLength)
        {
            BufferWriter w;
            w.WriteU8(static_cast<uint8_t>(type));
            w.WriteU32(declaredLength);
            w.WriteBytes(payload);
            const int size = static_cast<int>(w.Size());
            return send(_socket, reinterpret_cast<const char*>(w.Data().data()), size, 0) == size;
        }

        [[nodiscard]] size_t BytesAvailable() const
        {
            u_long available = 0;
            ioctlsocket(_socket, FIONREAD, &available);
            return available;
        }

        /// The next response frame; nullopt if the connection closed or timed out first
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
            char byte = 0;
            const int result = recv(_socket, &byte, 1, 0);
            return result == 0 || (result < 0 && WSAGetLastError() != WSAETIMEDOUT);
        }

        /// The address the server side of this connection is bound to
        [[nodiscard]] std::string PeerAddress() const
        {
            sockaddr_in peer{};
            int size = sizeof(peer);
            if (getpeername(_socket, reinterpret_cast<sockaddr*>(&peer), &size) != 0)
                return {};
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text));
            return text;
        }

    private:
        bool ReadExact(uint8_t *out, size_t count)
        {
            size_t received = 0;
            while (received < count)
            {
                const int result = recv(_socket, reinterpret_cast<char*>(out + received),
                                        static_cast<int>(count - received), 0);
                if (result <= 0)
                    return false;
                received += static_cast<size_t>(result);
            }
            return true;
        }

        bool _wsaStarted = false;
        bool _connected = false;
        SOCKET _socket = INVALID_SOCKET;
    };

    /// Stop() must return even with a client connected; aborts rather than hanging the test run
    void StopWithin(EditorServer &server, std::chrono::seconds limit)
    {
        std::promise<void> stopped;
        std::future<void> stoppedFuture = stopped.get_future();
        std::thread stopper([&]
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
}

TEST(EditorServerSocketTest, ListensOnLoopbackAndRunsCommandsOnTheDrainingThread)
{
    EditorServer server;
    ASSERT_TRUE(server.Start(0));
    ASSERT_GT(server.GetPort(), 0);

    TestClient client(server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    EXPECT_EQ(client.PeerAddress(), "127.0.0.1");

    const std::vector<uint8_t> payload = StringPayload("00000000-0000-0000-0000-000000000000");
    ASSERT_TRUE(client.SendRequest(CommandType::DestroyEntity, payload, static_cast<uint32_t>(payload.size())));

    // Nothing answers the request until this thread drains the queue
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(client.BytesAvailable(), 0u);

    // This thread plays the host's main loop: the request is only answered once it drains the queue
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (client.BytesAvailable() < 5 && std::chrono::steady_clock::now() < deadline)
    {
        server.ProcessCommands(std::chrono::milliseconds(10));
    }

    const auto response = client.ReadResponse();
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(Decode(*response).type, ErrorType); // no scene loaded

    // The client is now idle in the server's recv(); Stop must still return
    StopWithin(server, std::chrono::seconds(10));
    EXPECT_FALSE(server.IsRunning());
    EXPECT_TRUE(client.IsClosedByServer());
}

TEST(EditorServerSocketTest, OversizedPayloadIsRefusedBeforeAllocating)
{
    EditorServer server;
    ASSERT_TRUE(server.Start(0));

    TestClient client(server.GetPort());
    ASSERT_TRUE(client.IsConnected());

    // Declares 4 GiB but sends nothing: the server must answer from the header alone
    ASSERT_TRUE(client.SendRequest(CommandType::LoadScene, {}, 0xFFFFFFFFu));

    const auto response = client.ReadResponse();
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(Decode(*response).type, ErrorType);
    EXPECT_TRUE(client.IsClosedByServer());

    StopWithin(server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, StopReturnsWithNoClient)
{
    EditorServer server;
    ASSERT_TRUE(server.Start(0));
    StopWithin(server, std::chrono::seconds(10));
    EXPECT_FALSE(server.IsRunning());

    // And it can serve again afterwards
    ASSERT_TRUE(server.Start(0));
    StopWithin(server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, RestartsOnTheSamePortAfterAClientShutdown)
{
    EditorServer server;
    ASSERT_TRUE(server.Start(0));
    const int port = server.GetPort();

    {
        TestClient client(port);
        ASSERT_TRUE(client.IsConnected());
        ASSERT_TRUE(client.SendRequest(CommandType::Shutdown, {}, 0));

        // Answered on the network thread, which then stops serving and closes the connection first
        const auto response = client.ReadResponse();
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(Decode(*response).type, OkType);
        EXPECT_TRUE(client.IsClosedByServer());
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (server.IsRunning() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_FALSE(server.IsRunning());

    // Start() without Stop() first: it must join the finished thread rather than terminate. The server
    // side of the old connection is in TIME_WAIT, which must not block binding the port again.
    ASSERT_TRUE(server.Start(port));
    EXPECT_EQ(server.GetPort(), port);
    {
        TestClient client(port);
        EXPECT_TRUE(client.IsConnected());
    }
    StopWithin(server, std::chrono::seconds(10));

    ASSERT_TRUE(server.Start(port));
    StopWithin(server, std::chrono::seconds(10));
}

namespace
{
    /// Sends a request and plays the host's main loop until its response arrives (a request the network thread
    /// answers itself needs no draining)
    std::optional<DecodedFrame> Roundtrip(EditorServer &server, TestClient &client, CommandType type,
                                          const std::vector<uint8_t> &payload = {})
    {
        if (!client.SendRequest(type, payload, static_cast<uint32_t>(payload.size())))
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

    std::vector<uint8_t> HelloPayload(const std::string &token)
    {
        BufferWriter w;
        w.WriteString("socket test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(token);
        return ToVector(w);
    }

    const std::vector<uint8_t> NoSuchEntity = StringPayload("00000000-0000-0000-0000-000000000000");
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
}

namespace
{
    /// A server with an access token, serving on an OS-picked port
    struct GatedServer
    {
        explicit GatedServer(std::chrono::milliseconds helloTimeout = EditorServer::DefaultHelloTimeout)
        {
            EXPECT_TRUE(server.SetAccessToken("socket-token"));
            EXPECT_TRUE(server.SetHelloTimeout(helloTimeout));
            started = server.Start(0);
        }

        EditorServer server;
        bool started = false;
    };
}

TEST(EditorServerSocketTest, WithAnAccessTokenACommandBeforeHelloIsRefusedAndTheConnectionClosed)
{
    GatedServer gated;
    ASSERT_TRUE(gated.started);

    // Refused on the network thread (answered with nothing draining the queue), then disconnected, so the one
    // connection the server serves at a time is free again
    for (const CommandType refused : {CommandType::DestroyEntity, CommandType::GetEngineHealth, CommandType::Shutdown})
    {
        TestClient client(gated.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const std::vector<uint8_t> payload = refused == CommandType::DestroyEntity ? NoSuchEntity : std::vector<uint8_t>{};
        ASSERT_TRUE(client.SendRequest(refused, payload, static_cast<uint32_t>(payload.size())));
        const auto response = client.ReadResponse();
        ASSERT_TRUE(response.has_value()) << static_cast<int>(refused);
        EXPECT_EQ(Decode(*response).type, ErrorType);
        EXPECT_EQ(Decode(*response).body, EditorServer::HelloRequiredError);
        EXPECT_TRUE(client.IsClosedByServer()) << static_cast<int>(refused);
    }
    EXPECT_TRUE(gated.server.IsRunning()) << "Shutdown before Hello must not stop the server";

    StopWithin(gated.server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, WithAnAccessTokenAFailedHelloClosesTheConnection)
{
    GatedServer gated;
    ASSERT_TRUE(gated.started);

    for (const std::string version : {std::string{ProtocolVersion}, std::string{"999.0.0"}})
    {
        TestClient client(gated.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        BufferWriter hello;
        hello.WriteString("socket test");
        hello.WriteString(version);
        hello.WriteString(version == ProtocolVersion ? "wrong" : "socket-token");
        const auto response = Roundtrip(gated.server, client, CommandType::Hello, ToVector(hello));
        ASSERT_TRUE(response.has_value()) << version;
        EXPECT_EQ(response->type, ErrorType) << version;
        EXPECT_TRUE(client.IsClosedByServer()) << version;
    }

    StopWithin(gated.server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, WithAnAccessTokenAHelloOpensTheSessionUntilAHelloFails)
{
    GatedServer gated;
    ASSERT_TRUE(gated.started);
    TestClient client(gated.server.GetPort());
    ASSERT_TRUE(client.IsConnected());

    auto response = Roundtrip(gated.server, client, CommandType::Hello, HelloPayload("socket-token"));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->type, ServerInfoType) << response->body;

    // The command now runs (and fails on its own terms)
    response = Roundtrip(gated.server, client, CommandType::DestroyEntity, NoSuchEntity);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->type, ErrorType);
    EXPECT_NE(response->body.find("No scene"), std::string::npos) << response->body;

    // A later Hello that fails ends the session and the connection
    response = Roundtrip(gated.server, client, CommandType::Hello, HelloPayload("wrong again"));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->body, "Invalid access token");
    EXPECT_TRUE(client.IsClosedByServer());

    StopWithin(gated.server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, WithAnAccessTokenAClientThatNeverSaysHelloIsDisconnected)
{
    GatedServer gated(std::chrono::milliseconds(300));
    ASSERT_TRUE(gated.started);

    {
        // Connects and sends nothing (the receive timeout is 10 s, so a close comes from the server)
        TestClient silent(gated.server.GetPort());
        ASSERT_TRUE(silent.IsConnected());
        const auto start = std::chrono::steady_clock::now();
        EXPECT_TRUE(silent.IsClosedByServer());
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
    }
    {
        // Sends half a header, then stalls
        TestClient stalled(gated.server.GetPort());
        ASSERT_TRUE(stalled.IsConnected());
        ASSERT_TRUE(stalled.SendRequest(CommandType::Hello, {}, 1000)); // promises a payload it never sends
        EXPECT_TRUE(stalled.IsClosedByServer());
    }

    // The server is free for the next client, whose Hello succeeds, and the deadline no longer applies to it
    TestClient client(gated.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    auto response = Roundtrip(gated.server, client, CommandType::Hello, HelloPayload("socket-token"));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->type, ServerInfoType) << response->body;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    response = Roundtrip(gated.server, client, CommandType::DestroyEntity, NoSuchEntity);
    ASSERT_TRUE(response.has_value());
    EXPECT_NE(response->body.find("No scene"), std::string::npos) << response->body;

    StopWithin(gated.server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, WithAnAccessTokenAPayloadBiggerThanAHelloIsRefusedBeforeHello)
{
    GatedServer gated;
    ASSERT_TRUE(gated.started);
    TestClient client(gated.server.GetPort());
    ASSERT_TRUE(client.IsConnected());

    // Declares more than MaxPayloadBytesBeforeHello and sends none of it: answered from the header alone
    ASSERT_TRUE(client.SendRequest(CommandType::Hello, {}, EditorServer::MaxPayloadBytesBeforeHello + 1));
    const auto response = client.ReadResponse();
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(Decode(*response).type, ErrorType);
    EXPECT_NE(Decode(*response).body.find("before Hello"), std::string::npos) << Decode(*response).body;
    EXPECT_TRUE(client.IsClosedByServer());

    StopWithin(gated.server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, ASessionEndsWithItsConnection)
{
    GatedServer gated;
    ASSERT_TRUE(gated.started);
    {
        TestClient client(gated.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(gated.server, client, CommandType::Hello, HelloPayload("socket-token"));
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->body;
    }

    // The same client reconnecting must say Hello again. The server notices the first connection closed (and
    // accepts this one) on its own; this thread only drains the queue meanwhile.
    TestClient reconnected(gated.server.GetPort());
    ASSERT_TRUE(reconnected.IsConnected());
    const auto response = Roundtrip(gated.server, reconnected, CommandType::DestroyEntity, NoSuchEntity);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->body, EditorServer::HelloRequiredError);

    StopWithin(gated.server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, WithoutAnAccessTokenHelloIsOptionalAndNothingTimesOut)
{
    EditorServer server;
    ASSERT_TRUE(server.SetHelloTimeout(std::chrono::milliseconds(200)));
    ASSERT_TRUE(server.Start(0));
    TestClient client(server.GetPort());
    ASSERT_TRUE(client.IsConnected());

    // Idle past the Hello timeout, which only applies with an access token, then a command without a Hello runs
    // (and fails on its own terms)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto response = Roundtrip(server, client, CommandType::DestroyEntity, NoSuchEntity);
    ASSERT_TRUE(response.has_value());
    EXPECT_NE(response->body.find("No scene"), std::string::npos) << response->body;

    // A failed Hello is answered, and the connection stays open
    BufferWriter hello;
    hello.WriteString("socket test");
    hello.WriteString("999.0.0");
    hello.WriteString("");
    response = Roundtrip(server, client, CommandType::Hello, ToVector(hello));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->type, ErrorType);

    // A Hello is still answered, whatever token it carries
    response = Roundtrip(server, client, CommandType::Hello, HelloPayload("any token"));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->type, ServerInfoType) << response->body;

    StopWithin(server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, RejectsAnInvalidBindAddress)
{
    EditorServer server;
    EXPECT_FALSE(server.Start(0, "not-an-address"));
    EXPECT_FALSE(server.IsRunning());
}

namespace
{
    /// Plays the host's main loop until the server stops itself; false if it is still running after `limit`
    bool WaitUntilStopped(EditorServer &server, std::chrono::seconds limit)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (server.IsRunning() && std::chrono::steady_clock::now() < deadline)
        {
            server.ProcessCommands(std::chrono::milliseconds(10));
        }
        return !server.IsRunning();
    }
}

TEST(EditorServerSocketTest, WithStopOnDisconnectTheServerStopsWhenItsClientLeaves)
{
    EditorServer server;
    EXPECT_FALSE(server.StopsOnDisconnect()) << "off by default";
    ASSERT_TRUE(server.SetStopOnDisconnect(true));
    ASSERT_TRUE(server.Start(0));
    EXPECT_FALSE(server.SetStopOnDisconnect(false)) << "it can't change while serving";
    EXPECT_TRUE(server.StopsOnDisconnect());

    {
        // Without an access token the connection is the session
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto response = Roundtrip(server, client, CommandType::DestroyEntity, NoSuchEntity);
        ASSERT_TRUE(response.has_value());
        EXPECT_TRUE(server.IsRunning());
    }

    EXPECT_TRUE(WaitUntilStopped(server, std::chrono::seconds(10)));
    StopWithin(server, std::chrono::seconds(10));

    // It can serve again, until its next client leaves
    ASSERT_TRUE(server.Start(0));
    EXPECT_TRUE(server.IsRunning());
    StopWithin(server, std::chrono::seconds(10));
}

TEST(EditorServerSocketTest, WithAnAccessTokenOnlyAnAuthenticatedSessionStopsTheServerOnDisconnect)
{
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("socket-token"));
    ASSERT_TRUE(server.SetStopOnDisconnect(true));
    ASSERT_TRUE(server.Start(0));
    const int port = server.GetPort();

    {
        // Connects and leaves without a Hello
        TestClient silent(port);
        ASSERT_TRUE(silent.IsConnected());
    }
    {
        // Its Hello is refused, and the server closes the connection
        TestClient refused(port);
        ASSERT_TRUE(refused.IsConnected());
        const auto response = Roundtrip(server, refused, CommandType::Hello, HelloPayload("wrong"));
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->type, ErrorType);
        EXPECT_TRUE(refused.IsClosedByServer());
    }

    {
        // Served one connection at a time, so this one is answered only once those two have ended: neither stopped
        // the server, or nothing would accept it
        TestClient client(port);
        ASSERT_TRUE(client.IsConnected());
        const auto response = Roundtrip(server, client, CommandType::Hello, HelloPayload("socket-token"));
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->type, ServerInfoType) << response->body;
        EXPECT_TRUE(server.IsRunning());
    }

    // That one had a session, so its leaving stops the server
    EXPECT_TRUE(WaitUntilStopped(server, std::chrono::seconds(10)));
    StopWithin(server, std::chrono::seconds(10));
}
#endif
