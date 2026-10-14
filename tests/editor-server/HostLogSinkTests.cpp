#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/EditorTls.hpp>
#include <editor-server/EventRing.hpp>
#include <editor-server/HostOptions.hpp>
#include <editor-server/LogSink.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/RateLimiter.hpp>
#include <editor-server/Serialization.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/Logger.hpp>

// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
#include "TestSockets.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
namespace fs = std::filesystem;

// The host's log sink (LogSink, N2EditorHost's stderr and --log-file output): the line format and level filter, the file
// output, --log-level and --log-file, and, over real sockets, that the lines a refused Hello, an audited command and a
// rate-limit ban log reach a sink, with no token and no hash among them.

namespace
{
    // 2024-02-29T12:00:00.123Z
    constexpr int64_t FixedTime = 1'709'208'000'123;
    constexpr const char *FixedStamp = "2024-02-29T12:00:00.123Z";

    LogSink::Clock FixedClock()
    {
        return [] { return FixedTime; };
    }

    /// Every line handed to an output, newline included
    class CapturedLines
    {
    public:
        [[nodiscard]] LogOutput Output()
        {
            return [this](const std::string_view line)
            {
                const std::lock_guard lock(_mutex);
                _lines.emplace_back(line);
            };
        }

        [[nodiscard]] std::vector<std::string> Lines() const
        {
            const std::lock_guard lock(_mutex);
            return _lines;
        }

        [[nodiscard]] size_t Count(const std::string_view text) const
        {
            size_t count = 0;
            for (const std::string &line : Lines())
            {
                if (line.find(text) != std::string::npos)
                    ++count;
            }
            return count;
        }

        [[nodiscard]] bool Has(const std::string_view text) const { return Count(text) > 0; }

    private:
        mutable std::mutex _mutex;
        std::vector<std::string> _lines;
    };

    /// A folder of its own under the temp folder, removed with the object
    class TempFolder
    {
    public:
        TempFolder()
        {
            static std::atomic<int> counter{0};
            _path = fs::temp_directory_path() /
                    ("n2-log-sink-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                     std::to_string(counter++));
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

    std::vector<std::string> ReadLines(const fs::path &file)
    {
        std::ifstream in(file, std::ios::binary);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
        return lines;
    }
}

// ==================== Format ====================

TEST(LogSinkFormatTest, TheTimestampIsUtcIso8601WithMilliseconds)
{
    EXPECT_EQ(FormatLogTimestamp(0), "1970-01-01T00:00:00.000Z");
    EXPECT_EQ(FormatLogTimestamp(FixedTime), FixedStamp);
    EXPECT_EQ(FormatLogTimestamp(1'000'000'000'123), "2001-09-09T01:46:40.123Z");
    EXPECT_EQ(FormatLogTimestamp(2'147'483'647'000), "2038-01-19T03:14:07.000Z");
    // A leap day, and the end of a year
    EXPECT_EQ(FormatLogTimestamp(1'709'164'800'000), "2024-02-29T00:00:00.000Z");
    EXPECT_EQ(FormatLogTimestamp(1'735'689'599'999), "2024-12-31T23:59:59.999Z");
    // Before 1970 counts back from midnight
    EXPECT_EQ(FormatLogTimestamp(-1), "1969-12-31T23:59:59.999Z");
}

TEST(LogSinkFormatTest, ALineIsTimestampLevelAndMessage)
{
    EXPECT_EQ(FormatLogLine(FixedTime, Logger::LogLevel::Info, "audit: user=bob role=editor command=CreateEntity peer=10.0.0.5"),
              "2024-02-29T12:00:00.123Z INFO audit: user=bob role=editor command=CreateEntity peer=10.0.0.5");
    EXPECT_EQ(FormatLogLine(FixedTime, Logger::LogLevel::Warn, "w"), "2024-02-29T12:00:00.123Z WARN w");
    EXPECT_EQ(FormatLogLine(FixedTime, Logger::LogLevel::Error, "e"), "2024-02-29T12:00:00.123Z ERROR e");
    EXPECT_EQ(LogLevelName(Logger::LogLevel::Info), "INFO");
}

TEST(LogSinkFormatTest, ControlCharactersAreEscapedSoALineStaysOneLine)
{
    EXPECT_EQ(EscapeLogText("a\nb\r\tc"), "a\\nb\\r\\tc");
    EXPECT_EQ(EscapeLogText(std::string("x\x1b[31my\x7fz\0w", 11)), "x\\x1B[31my\\x7Fz\\x00w");
    // Text, UTF-8 and a backslash (a Windows path) stay as they are
    EXPECT_EQ(EscapeLogText("C:\\scenes\\\xC3\xA9.scene"), "C:\\scenes\\\xC3\xA9.scene");

    // A client-supplied name that tries to start a line of the host's own
    const std::string forged = "hello\n2024-02-29T12:00:00.123Z INFO audit: user=root role=admin command=Shutdown peer=127.0.0.1";
    const std::string line = FormatLogLine(FixedTime, Logger::LogLevel::Warn, forged);
    EXPECT_EQ(line.find('\n'), std::string::npos);
    EXPECT_EQ(line.find('\r'), std::string::npos);
    EXPECT_NE(line.find("hello\\n2024-02-29"), std::string::npos) << line;
}

TEST(LogSinkFormatTest, LevelNamesParseExactly)
{
    EXPECT_EQ(ParseLogLevel("info"), Logger::LogLevel::Info);
    EXPECT_EQ(ParseLogLevel("warn"), Logger::LogLevel::Warn);
    EXPECT_EQ(ParseLogLevel("error"), Logger::LogLevel::Error);
    // The Logger has no Debug level, and nothing else is a spelling
    EXPECT_FALSE(ParseLogLevel("debug").has_value());
    EXPECT_FALSE(ParseLogLevel("INFO").has_value());
    EXPECT_FALSE(ParseLogLevel("warning").has_value());
    EXPECT_FALSE(ParseLogLevel("").has_value());
}

// ==================== The sink ====================

TEST(LogSinkTest, WritesOneFormattedLineForEachLoggedLineAtOrAboveTheLevel)
{
    CapturedLines captured;
    {
        LogSink sink(Logger::LogLevel::Warn, {captured.Output()}, FixedClock());
        Logger::Info("log sink test: info");
        Logger::Warn("log sink test: warn");
        Logger::Error("log sink test: error");
    }
    const std::vector<std::string> lines = captured.Lines();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], std::string(FixedStamp) + " WARN log sink test: warn\n");
    EXPECT_EQ(lines[1], std::string(FixedStamp) + " ERROR log sink test: error\n");

    CapturedLines all;
    {
        LogSink sink(Logger::LogLevel::Info, {all.Output()}, FixedClock());
        Logger::Info("log sink test: info");
        Logger::Warn("log sink test: warn");
        Logger::Error("log sink test: error");
    }
    ASSERT_EQ(all.Lines().size(), 3u);
    EXPECT_EQ(all.Lines()[0], std::string(FixedStamp) + " INFO log sink test: info\n");
}

TEST(LogSinkTest, ALineWithANewlineIsStillOneLineInTheOutput)
{
    CapturedLines captured;
    {
        LogSink sink(Logger::LogLevel::Info, {captured.Output()}, FixedClock());
        Logger::Info("log sink test: first\naudit: user=root role=admin command=Shutdown");
    }
    const std::vector<std::string> lines = captured.Lines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].find('\n'), lines[0].size() - 1) << "the only newline is the one that ends the line";
    EXPECT_NE(lines[0].find("first\\naudit:"), std::string::npos) << lines[0];
}

TEST(LogSinkTest, StopsWritingWhenItIsDestroyedAndLeavesTheOtherSubscribersAlone)
{
    const size_t before = Logger::logEvent.GetSubscriberCount();
    CapturedLines captured;
    {
        LogSink sink(Logger::LogLevel::Error, {captured.Output()}, FixedClock());
        EXPECT_EQ(Logger::logEvent.GetSubscriberCount(), before + 1);
    }
    EXPECT_EQ(Logger::logEvent.GetSubscriberCount(), before);
    Logger::Error("log sink test: after the sink");
    EXPECT_FALSE(captured.Has("after the sink"));
}

TEST(LogSinkTest, ItChangesNothingAnotherSubscriberOrThePollEventsRingReceives)
{
    // What the editor's PollEvents ring and a Debug build's console get is the raw line at every level, whatever the sink's
    // level and escaping
    std::mutex mutex;
    std::vector<std::pair<std::string, Logger::LogLevel>> seen;
    const size_t observer = Logger::logEvent += [&](const std::string_view message, const Logger::LogLevel level)
    {
        const std::lock_guard lock(mutex);
        seen.emplace_back(std::string(message), level);
    };

    EditorServer server;
    CapturedLines captured;
    {
        LogSink sink(Logger::LogLevel::Error, {captured.Output()}, FixedClock());
        Logger::Info("log sink test: ring\nline");
        Logger::Warn("log sink test: ring warn");
    }
    Logger::logEvent -= observer;

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].first, "log sink test: ring\nline");
    EXPECT_EQ(seen[0].second, Logger::LogLevel::Info);
    EXPECT_EQ(seen[1].second, Logger::LogLevel::Warn);
    EXPECT_TRUE(captured.Lines().empty()) << "below the sink's level";

    const EventBatch batch = server.GetEvents().Read(0, std::numeric_limits<uint32_t>::max());
    const nlohmann::json events = nlohmann::json(batch.events);
    bool found = false;
    for (const nlohmann::json &event : events)
    {
        if (event.value("message", "") == "log sink test: ring\nline")
        {
            found = true;
            EXPECT_EQ(event.at("level").get<std::string>(), "info");
        }
    }
    EXPECT_TRUE(found) << events.dump();
}

TEST(LogSinkTest, AnOutputThatThrowsStopsNeitherTheNextOutputNorTheLogger)
{
    CapturedLines captured;
    {
        LogSink sink(Logger::LogLevel::Info,
                     {[](std::string_view) { throw std::runtime_error("disk on fire"); }, captured.Output()}, FixedClock());
        EXPECT_NO_THROW(Logger::Info("log sink test: survives"));
    }
    EXPECT_TRUE(captured.Has("log sink test: survives"));
}

TEST(LogSinkTest, TheStderrOutputWritesWithoutThrowing)
{
    // What it writes goes to this test's stderr; that it takes a line and returns is what is checked (the format is above)
    const LogOutput output = StderrLogOutput();
    EXPECT_NO_THROW(output(std::string(FixedStamp) + " INFO log sink test: a line on stderr\n"));
}

// ==================== The file ====================

TEST(LogFileTest, LinesAreAppendedAcrossOpens)
{
    const TempFolder folder;
    const std::string path = (folder.Path() / "host.log").string();

    {
        auto output = OpenLogFileOutput(path);
        ASSERT_TRUE(output.has_value()) << output.error();
        LogSink sink(Logger::LogLevel::Info, {*output}, FixedClock());
        Logger::Info("log file test: one");
        Logger::Warn("log file test: two");
    }
    {
        // A second start of the host adds to the same file
        auto output = OpenLogFileOutput(path);
        ASSERT_TRUE(output.has_value()) << output.error();
        LogSink sink(Logger::LogLevel::Info, {*output}, FixedClock());
        Logger::Error("log file test: three");
    }

    const std::vector<std::string> lines = ReadLines(path);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], std::string(FixedStamp) + " INFO log file test: one");
    EXPECT_EQ(lines[1], std::string(FixedStamp) + " WARN log file test: two");
    EXPECT_EQ(lines[2], std::string(FixedStamp) + " ERROR log file test: three");
}

TEST(LogFileTest, ALineIsOnDiskAsSoonAsItIsLogged)
{
    const TempFolder folder;
    const std::string path = (folder.Path() / "host.log").string();
    auto output = OpenLogFileOutput(path);
    ASSERT_TRUE(output.has_value()) << output.error();
    LogSink sink(Logger::LogLevel::Info, {*output}, FixedClock());
    Logger::Info("log file test: flushed");
    // Read while the file is still open: a crash right after the line must not lose it
    ASSERT_EQ(ReadLines(path).size(), 1u);
}

TEST(LogFileTest, ANewFileIsPrivateOnPosixAndAnExistingOneKeepsItsMode)
{
#ifdef _WIN32
    GTEST_SKIP() << "file permissions aren't checked on Windows (ACLs)";
#else
    const TempFolder folder;
    const fs::path created = folder.Path() / "new.log";
    {
        auto output = OpenLogFileOutput(created.string());
        ASSERT_TRUE(output.has_value()) << output.error();
    }
    const fs::perms mode = fs::status(created).permissions();
    EXPECT_EQ(mode & (fs::perms::group_all | fs::perms::others_all), fs::perms::none)
        << "an audit log is not readable by group or others";
    EXPECT_NE(mode & fs::perms::owner_write, fs::perms::none);

    const fs::path existing = folder.Path() / "existing.log";
    {
        std::ofstream(existing) << "old line\n";
    }
    const fs::perms shared = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
    fs::permissions(existing, shared, fs::perm_options::replace);
    {
        auto output = OpenLogFileOutput(existing.string());
        ASSERT_TRUE(output.has_value()) << output.error();
        LogSink sink(Logger::LogLevel::Info, {*output}, FixedClock());
        Logger::Info("log file test: appended");
    }
    EXPECT_EQ(fs::status(existing).permissions(), shared) << "an existing file's mode is the operator's choice";
    const std::vector<std::string> lines = ReadLines(existing);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "old line");
#endif
}

TEST(LogFileTest, APathThatCannotBeOpenedIsRefusedWithTheReason)
{
    const TempFolder folder;

    const std::string missingFolder = (folder.Path() / "no-such-folder" / "host.log").string();
    const auto missing = OpenLogFileOutput(missingFolder);
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().find(missingFolder), std::string::npos) << missing.error();
    EXPECT_NE(missing.error().find("can't be opened for appending"), std::string::npos) << missing.error();
    EXPECT_NE(missing.error().find("--log-file"), std::string::npos) << missing.error();
    EXPECT_FALSE(fs::exists(folder.Path() / "no-such-folder"));

    // A folder is not a file to append to
    const auto folderAsFile = OpenLogFileOutput(folder.Path().string());
    ASSERT_FALSE(folderAsFile.has_value());
    EXPECT_NE(folderAsFile.error().find("can't be opened for appending"), std::string::npos) << folderAsFile.error();

    EXPECT_FALSE(OpenLogFileOutput("").has_value());
}

// ==================== --log-level and --log-file ====================

TEST(LogOptionsTest, TheDefaultIsInfoOnStderrOnly)
{
    const auto parsed = ParseHostArguments({});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->logLevel, Logger::LogLevel::Info);
    EXPECT_TRUE(parsed->logFile.empty());
}

TEST(LogOptionsTest, TheFlagsAreParsed)
{
    const auto parsed = ParseHostArguments({"--log-level", "warn", "--log-file", "/var/log/n2editorhost/host.log"});
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->logLevel, Logger::LogLevel::Warn);
    EXPECT_EQ(parsed->logFile, "/var/log/n2editorhost/host.log");

    const auto error = ParseHostArguments({"--log-level", "error"});
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->logLevel, Logger::LogLevel::Error);
}

TEST(LogOptionsTest, ABadLevelAndAMissingOrEmptyValueAreErrors)
{
    for (const char *level : {"debug", "INFO", "verbose", ""})
    {
        const auto parsed = ParseHostArguments({"--log-level", level});
        ASSERT_FALSE(parsed.has_value()) << level;
        EXPECT_NE(parsed.error().find("expected info, warn or error"), std::string::npos) << parsed.error();
    }
    EXPECT_FALSE(ParseHostArguments({"--log-level"}).has_value());
    EXPECT_FALSE(ParseHostArguments({"--log-file"}).has_value());
    EXPECT_FALSE(ParseHostArguments({"--log-file", "--port", "0"}).has_value());
    const auto empty = ParseHostArguments({"--log-file", ""});
    ASSERT_FALSE(empty.has_value());
    EXPECT_NE(empty.error().find("--log-file"), std::string::npos);
}

TEST(LogOptionsTest, TheUsageDescribesBothFlags)
{
    const std::string_view usage = HostUsage();
    EXPECT_NE(usage.find("--log-level <level>"), std::string_view::npos);
    EXPECT_NE(usage.find("--log-file <path>"), std::string_view::npos);
    EXPECT_NE(usage.find("copytruncate"), std::string_view::npos);
}

// ==================== Over a socket ====================

namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t ScriptDataType = static_cast<uint8_t>(ResponseType::ScriptData);
    constexpr const char *RealToken = "sink-test-real-token";
    // Recognisable, so a copy of it in any output is found
    constexpr const char *FakeToken = "FAKE-TOKEN-7d41c9e2-must-never-be-logged";

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return std::string(payload.begin(), payload.end()); }
    };

    std::vector<uint8_t> HelloPayload(const std::string &token, const std::optional<std::string> &user = std::nullopt)
    {
        BufferWriter w;
        w.WriteString("sink test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(token);
        if (user.has_value())
        {
            w.WriteBool(true); // errorCodes: a user can only follow it
            w.WriteString(*user);
        }
        return w.Release();
    }

    std::vector<uint8_t> StringPayload(const std::string &text)
    {
        BufferWriter w;
        w.WriteString(text);
        return w.Release();
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

        std::optional<Frame> ReadResponse()
        {
            std::vector<uint8_t> header(5);
            if (!ReadExact(header.data(), 5))
                return std::nullopt;
            uint32_t size = 0;
            std::memcpy(&size, header.data() + 1, sizeof(size));
            Frame frame;
            frame.type = header[0];
            frame.payload.resize(size);
            if (size > 0 && !ReadExact(frame.payload.data(), size))
                return std::nullopt;
            return frame;
        }

        /// True once the server has closed (or reset) the connection without sending anything more
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
        return client.ReadResponse();
    }

    /// Plays the host's main loop until a captured line has this text (the network thread posts some lines to it)
    bool PumpUntilCaptured(EditorServer &server, const CapturedLines &captured, const std::string_view text)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!captured.Has(text) && std::chrono::steady_clock::now() < deadline)
        {
            server.ProcessCommands(std::chrono::milliseconds(10));
        }
        return captured.Has(text);
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

    /// The address the provider hands the network thread: the test sets it between connections
    class SharedPeer
    {
    public:
        explicit SharedPeer(const PeerAddress &initial) : _address(initial) {}

        void Set(const PeerAddress &address)
        {
            const std::lock_guard lock(_mutex);
            _address = address;
        }

        [[nodiscard]] PeerAddress Get() const
        {
            const std::lock_guard lock(_mutex);
            return _address;
        }

    private:
        mutable std::mutex _mutex;
        PeerAddress _address;
    };

    /// A server whose log lines go to a sink that captures them, with the peer address and the clock of the rate limiting
    /// chosen by the test (nothing here sleeps)
    struct SinkHost
    {
        SinkHost() : sink(Logger::LogLevel::Info, {captured.Output()}, FixedClock())
        {
            EXPECT_TRUE(server.SetRateLimiting(true));
            EXPECT_TRUE(server.SetHelloTimeout(std::chrono::seconds(30)));
            EXPECT_TRUE(server.SetRateLimitClock([this] { return RateLimiter::TimePoint(std::chrono::milliseconds(nowMs)); }));
            EXPECT_TRUE(server.SetPeerAddressProvider([this](int) { return peer.Get(); }));
        }

        ~SinkHost() { StopWithin(server, std::chrono::seconds(10)); }

        void Advance(const std::chrono::milliseconds by) { nowMs += by.count(); }

        /// A connection that sends a Hello and reads the answer
        std::optional<Frame> Hello(const std::string &token, const std::optional<std::string> &user = std::nullopt)
        {
            TestClient client(server.GetPort());
            if (!client.IsConnected())
                return std::nullopt;
            return Roundtrip(server, client, CommandType::Hello, HelloPayload(token, user));
        }

        /// Every line any output got must be one whole line and show none of these texts
        void ExpectNoSecrets(const std::vector<std::string> &secrets) const
        {
            for (const std::string &line : captured.Lines())
            {
                EXPECT_EQ(line.find('\n'), line.size() - 1) << "one line per event: " << line;
                EXPECT_EQ(line.rfind(FixedStamp, 0), 0u) << "a timestamp first: " << line;
                for (const std::string &secret : secrets)
                {
                    EXPECT_EQ(line.find(secret), std::string::npos) << "'" << secret << "' in: " << line;
                }
                EXPECT_EQ(line.find("sha256"), std::string::npos) << line;
            }
        }

        static inline const PeerAddress Attacker = PeerAddress::IPv4(203, 0, 113, 7);
        static inline const PeerAddress Friend = PeerAddress::IPv4(198, 51, 100, 20);

        // Before the sink and the server: the sink is destroyed last, and the server's thread reads these
        CapturedLines captured;
        SharedPeer peer{Attacker};
        std::atomic<int64_t> nowMs{1000};
        LogSink sink;
        EditorServer server;
    };

    /// Every text a token could leak as: itself, a part of it, its hash and a part of the hash
    void AddSecretsOf(std::vector<std::string> &secrets, const std::string &token)
    {
        secrets.push_back(token);
        secrets.push_back(token.substr(0, 12));
        if (const auto hash = HashToken(token))
        {
            const std::string text = FormatTokenHash(*hash);
            secrets.push_back(text);
            secrets.push_back(text.substr(TokenHashPrefix.size()));
            secrets.push_back(text.substr(TokenHashPrefix.size(), 16));
        }
    }
}

TEST(LogSinkServerTest, ARefusedHelloAnAuditedCommandAndABanReachTheSinkAndNoTokenDoes)
{
    SinkHost host;
    ASSERT_TRUE(host.server.SetAccessToken(RealToken));
    ASSERT_TRUE(host.server.Start(0));

    // Three failed Hellos with a recognisable fake token: each is a refused-Hello line, and the third starts a ban
    for (int i = 0; i < 3; ++i)
    {
        const auto refused = host.Hello(FakeToken);
        ASSERT_TRUE(refused.has_value()) << i;
        EXPECT_EQ(refused->type, ErrorType) << i;
        EXPECT_EQ(refused->Text(), "Invalid access token") << i;
    }
    EXPECT_TRUE(host.captured.Has(std::string(FixedStamp) +
                                  " WARN Refused Hello from editor client 'sink test' (user=- role=none): wrong access "
                                  "token peer=203.0.113.7\n"));
    EXPECT_EQ(host.captured.Count("Refused Hello"), 3u);
    ASSERT_TRUE(PumpUntilCaptured(host.server, host.captured,
                                  "Rate limit: peer=203.0.113.7 is backed off for 1000 ms after 3 failed Hellos"));
    EXPECT_TRUE(host.captured.Has(" WARN Rate limit: peer=203.0.113.7 is backed off"));

    // The ban ends with the clock; a Hello with the real token from another address is served, and a write it makes is an
    // audit line naming the user
    host.peer.Set(SinkHost::Friend);
    TestClient client(host.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    const auto hello = Roundtrip(host.server, client, CommandType::Hello, HelloPayload(RealToken));
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
    const auto script = Roundtrip(host.server, client, CommandType::CreateScript, StringPayload("player"));
    ASSERT_TRUE(script.has_value());
    EXPECT_EQ(script->type, ScriptDataType);
    // A read is not accounted for
    const auto health = Roundtrip(host.server, client, CommandType::GetEngineHealth);
    ASSERT_TRUE(health.has_value());

    EXPECT_TRUE(host.captured.Has(std::string(FixedStamp) +
                                  " INFO audit: user=token role=admin command=CreateScript peer=198.51.100.20\n"));
    EXPECT_FALSE(host.captured.Has("command=GetEngineHealth"));
    EXPECT_TRUE(host.captured.Has(" INFO Editor client 'sink test' said Hello (protocol " + std::string{ProtocolVersion} +
                                  ") user=token role=admin peer=198.51.100.20"));

    std::vector<std::string> secrets;
    AddSecretsOf(secrets, FakeToken);
    AddSecretsOf(secrets, RealToken);
    secrets.emplace_back("FAKE-TOKEN");
    host.ExpectNoSecrets(secrets);
}

TEST(LogSinkServerTest, AClientThatPutsATokenInTheUserFieldDoesNotSeeItInTheLog)
{
    SinkHost host;
    ASSERT_TRUE(host.server.SetAccessToken(RealToken));
    ASSERT_TRUE(host.server.Start(0));

    // The user field is client text: a client that sent its token there (or a forged line) gets "user=-", never the text
    const auto refused = host.Hello(FakeToken, std::string(FakeToken));
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->type, ErrorType) << refused->Text();
    const auto forged = host.Hello("wrong", std::string("alice\n2024-02-29T12:00:00.123Z INFO audit: user=root role=admin"));
    ASSERT_TRUE(forged.has_value());

    EXPECT_EQ(host.captured.Count("Refused Hello"), 2u);
    EXPECT_EQ(host.captured.Count("(user=- role=none)"), 2u);
    std::vector<std::string> secrets;
    AddSecretsOf(secrets, FakeToken);
    AddSecretsOf(secrets, RealToken);
    secrets.emplace_back("user=root");
    host.ExpectNoSecrets(secrets);
}

TEST(LogSinkServerTest, ATokensFileHostLogsTheUserAndTheReloadAndNeitherTokenNorHash)
{
    if (!EditorTlsServer::IsAvailable())
    {
        GTEST_SKIP() << "a tokens file needs the TLS library's SHA-256 (N2ENGINE_EDITOR_TLS)";
    }
    const TempFolder folder;
    const fs::path file = folder.Path() / "tokens.json";
    const auto alice = GenerateToken("alice", TokenRole::Admin);
    ASSERT_TRUE(alice.has_value()) << alice.error();
    {
        std::ofstream out(file, std::ios::binary);
        out << "[\n" << alice->entryLine << "\n]\n";
    }
#ifndef _WIN32
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif

    SinkHost host;
    ASSERT_TRUE(host.server.SetTokensFile(file.string()));
    ASSERT_TRUE(host.server.Start(0));

    // A wrong token for a user that exists: refused, naming the user (a valid name) and no more
    for (int i = 0; i < 3; ++i)
    {
        const auto refused = host.Hello(FakeToken, std::string("alice"));
        ASSERT_TRUE(refused.has_value()) << i;
        EXPECT_EQ(refused->type, ErrorType) << i;
    }
    EXPECT_EQ(host.captured.Count("Refused Hello from editor client 'sink test' (user=alice role=none): wrong access token peer=203.0.113.7"), 3u);
    ASSERT_TRUE(PumpUntilCaptured(host.server, host.captured, "Rate limit: user=alice is backed off for"));

    // The right one, from another address, once the backoffs are over
    host.peer.Set(SinkHost::Friend);
    host.Advance(std::chrono::minutes(10));
    {
        TestClient client(host.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(host.server, client, CommandType::Hello, HelloPayload(alice->token, std::string("alice")));
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
        ASSERT_TRUE(Roundtrip(host.server, client, CommandType::CreateScript, StringPayload("player")).has_value());
    }
    EXPECT_TRUE(host.captured.Has(" INFO audit: user=alice role=admin command=CreateScript peer=198.51.100.20"));

    // Revoke everybody in place: the next connection re-reads the file, which is one line, and refuses the token
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "[]\n";
    }
    host.Advance(std::chrono::minutes(10));
    const auto revoked = host.Hello(alice->token, std::string("alice"));
    ASSERT_TRUE(revoked.has_value());
    EXPECT_EQ(revoked->type, ErrorType) << revoked->Text();
    ASSERT_TRUE(PumpUntilCaptured(host.server, host.captured, "Reloaded the tokens file: 0 user(s)"));
    EXPECT_TRUE(host.captured.Has(" INFO Reloaded the tokens file: 0 user(s)"));

    std::vector<std::string> secrets;
    AddSecretsOf(secrets, FakeToken);
    AddSecretsOf(secrets, alice->token);
    secrets.emplace_back("FAKE-TOKEN");
    host.ExpectNoSecrets(secrets);
}
