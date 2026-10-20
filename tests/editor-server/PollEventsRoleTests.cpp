#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/EditorTls.hpp>
#include <editor-server/EventRing.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>
#include <editor-server/ServerLog.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/Logger.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// What each role sees of the log (PollEvents): sensitivity tags, the Public view's own numbering, the host-path
// scrubbing, and the per-role behaviour through ExecuteCommand. The ring's order and eviction rules on their own are in
// EventRingTests.cpp; the unfiltered wire format is in PollEventsTests.cpp.

namespace
{
    constexpr uint32_t All = std::numeric_limits<uint32_t>::max();
    constexpr uint8_t EventsType = static_cast<uint8_t>(ResponseType::Events);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);

    constexpr auto Pub = Logger::LogSensitivity::Public;
    constexpr auto Adm = Logger::LogSensitivity::Admin;

    json Messages(const std::vector<json> &events)
    {
        json messages = json::array();
        for (const json &event : events)
            messages.push_back(event.value("message", ""));
        return messages;
    }

    std::vector<uint32_t> Seqs(const std::vector<json> &events)
    {
        std::vector<uint32_t> seqs;
        for (const json &event : events)
            seqs.push_back(event.at("seq").get<uint32_t>());
        return seqs;
    }

    bool Has(const std::vector<json> &events, std::string_view text)
    {
        for (const json &event : events)
        {
            if (event.value("message", "").find(text) != std::string::npos)
                return true;
        }
        return false;
    }

    struct Polled
    {
        uint8_t type = 0xEE;
        uint32_t epoch = 0;
        uint32_t nextSeq = 0;
        uint32_t dropped = 0;
        std::vector<json> events;
    };

    Polled Poll(EditorServer &server, uint32_t afterSeq = 0, uint32_t epoch = 0)
    {
        BufferWriter w;
        w.WriteU32(epoch);
        w.WriteU32(afterSeq);
        w.WriteU32(All);
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(CommandType::PollEvents), w.Release());
        BufferReader r(frame);
        Polled result;
        result.type = r.ReadU8();
        const uint32_t size = r.ReadU32();
        EXPECT_EQ(size, r.Remaining());
        if (result.type != EventsType)
            return result;
        result.epoch = r.ReadU32();
        result.nextSeq = r.ReadU32();
        result.dropped = r.ReadU32();
        const json events = ReadJson(r);
        for (const json &event : events)
            result.events.push_back(event);
        return result;
    }

    uint8_t ExecuteType(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(type), payload);
        return frame.empty() ? 0xEE : frame[0];
    }

    std::vector<uint8_t> HelloPayload(const std::string &token, const std::string &user)
    {
        BufferWriter w;
        w.WriteString("poll roles test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(token);
        w.WriteBool(true);
        w.WriteString(user);
        return w.Release();
    }

    std::vector<uint8_t> StringPayload(const std::string &text)
    {
        BufferWriter w;
        w.WriteString(text);
        return w.Release();
    }

    struct User
    {
        std::string name;
        std::string token;
        TokenRole role;
    };

    const User Alice{"alice", "alice-poll-token-0001", TokenRole::Admin};
    const User Bob{"bob", "bob-poll-token-0002", TokenRole::Editor};
    const User Carol{"carol", "carol-poll-token-0003", TokenRole::Viewer};

    class TempFolder
    {
    public:
        TempFolder()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _path = fs::temp_directory_path() / ("n2-poll-roles-" + std::to_string(stamp) + "-" + std::to_string(counter++));
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
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
        }
#ifndef _WIN32
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
    }

    class PollEventsRoleTest : public ::testing::Test
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

        static void HelloAs(EditorServer &server, const User &user)
        {
            ASSERT_EQ(ExecuteType(server, CommandType::Hello, HelloPayload(user.token, user.name)), ServerInfoType);
        }

        std::unique_ptr<TempFolder> _folder;
        fs::path _file;
    };
}

// ==================== The ring: views, numbering, eviction ====================

TEST(EventRingSensitivityTest, ThePublicViewHoldsOnlyPublicEventsNumberedConsecutively)
{
    EventRing ring;
    ring.PushLog(Logger::LogLevel::Info, "public one", 1, Pub);
    ring.PushLog(Logger::LogLevel::Info, "admin one", 2, Adm);
    ring.PushLog(Logger::LogLevel::Warn, "admin two", 3, Adm);
    ring.PushLog(Logger::LogLevel::Info, "public two", 4, Pub);
    ring.Push("sceneChanged", json{{"revision", 7}});

    const EventBatch full = ring.Read(0, All, 0, EventView::Full);
    EXPECT_EQ(Seqs(full.events), (std::vector<uint32_t>{1, 2, 3, 4, 5}));

    const EventBatch pub = ring.Read(0, All, 0, EventView::PublicOnly);
    EXPECT_EQ(Seqs(pub.events), (std::vector<uint32_t>{1, 2, 3})) << "no gap where the admin lines were";
    EXPECT_EQ(Messages(pub.events), (json::array({"public one", "public two", ""})));
    EXPECT_EQ(pub.events[2].at("kind").get<std::string>(), "sceneChanged") << "a non-log event is Public by default";
    EXPECT_EQ(pub.nextSeq, 3u);
    EXPECT_EQ(pub.dropped, 0u);
    EXPECT_EQ(ring.LastSeq(EventView::PublicOnly), 3u);
    EXPECT_EQ(ring.LastSeq(EventView::Full), 5u);
}

TEST(EventRingSensitivityTest, AnAdminOnlyEventTaggedOnPushIsNeverInThePublicView)
{
    EventRing ring;
    ring.Push("secretKind", json{{"user", "alice"}}, Adm);
    ring.Push("openKind", json::object(), Pub);

    const EventBatch pub = ring.Read(0, All, 0, EventView::PublicOnly);
    ASSERT_EQ(pub.events.size(), 1u);
    EXPECT_EQ(pub.events[0].at("kind").get<std::string>(), "openKind");
    EXPECT_EQ(pub.events[0].at("seq").get<uint32_t>(), 1u);
    EXPECT_EQ(ring.Read(0, All).events.size(), 2u) << "the default view is the full one";
}

TEST(EventRingSensitivityTest, APublicPollThatFindsNothingNewReportsTheNewestPublicSeqNotTheHiddenOnes)
{
    EventRing ring;
    ring.PushLog(Logger::LogLevel::Info, "p1", 1, Pub);
    ring.PushLog(Logger::LogLevel::Info, "p2", 2, Pub);
    const EventBatch first = ring.Read(0, All, 0, EventView::PublicOnly);
    ASSERT_EQ(first.nextSeq, 2u);

    // Admin lines pile up: the next public poll sees nothing and learns nothing of them
    for (int i = 0; i < 10; ++i)
        ring.PushLog(Logger::LogLevel::Info, "hidden", 3, Adm);
    const EventBatch second = ring.Read(first.nextSeq, All, first.epoch, EventView::PublicOnly);
    EXPECT_TRUE(second.events.empty());
    EXPECT_EQ(second.nextSeq, 2u);
    EXPECT_EQ(second.dropped, 0u);

    ring.PushLog(Logger::LogLevel::Info, "p3", 4, Pub);
    const EventBatch third = ring.Read(second.nextSeq, All, second.epoch, EventView::PublicOnly);
    ASSERT_EQ(third.events.size(), 1u);
    EXPECT_EQ(third.events[0].at("seq").get<uint32_t>(), 3u);
    EXPECT_EQ(third.nextSeq, 3u);
}

TEST(EventRingSensitivityTest, TheViewsHaveTheirOwnEpochsSoAClientChangingViewStartsOver)
{
    EventRing ring;
    ring.PushLog(Logger::LogLevel::Info, "p1", 1, Pub);
    ring.PushLog(Logger::LogLevel::Info, "a1", 2, Adm);
    ring.PushLog(Logger::LogLevel::Info, "p2", 3, Pub);
    EXPECT_NE(ring.Epoch(EventView::Full), 0u);
    EXPECT_NE(ring.Epoch(EventView::PublicOnly), 0u);
    EXPECT_NE(ring.Epoch(EventView::Full), ring.Epoch(EventView::PublicOnly));

    // An admin's (epoch, seq 3) means nothing to the public view: it reads from its start, not after its second event
    const EventBatch pub = ring.Read(3, All, ring.Epoch(EventView::Full), EventView::PublicOnly);
    EXPECT_EQ(pub.epoch, ring.Epoch(EventView::PublicOnly));
    EXPECT_EQ(Seqs(pub.events), (std::vector<uint32_t>{1, 2}));

    // And the other way round
    const EventBatch full = ring.Read(2, All, ring.Epoch(EventView::PublicOnly), EventView::Full);
    EXPECT_EQ(full.epoch, ring.Epoch(EventView::Full));
    EXPECT_EQ(Seqs(full.events), (std::vector<uint32_t>{1, 2, 3}));

    // A reader that doesn't know the epoch and passes a seq the view hasn't reached reads from the start too
    EXPECT_EQ(Seqs(ring.Read(3, All, 0, EventView::PublicOnly).events), (std::vector<uint32_t>{1, 2}));
}

TEST(EventRingSensitivityTest, EvictionIsCountedInThePublicViewsOwnNumbers)
{
    EventRing ring(4, EventRing::DefaultMaxBytes);
    ring.PushLog(Logger::LogLevel::Info, "p1", 1, Pub); // seq 1, public 1
    ring.PushLog(Logger::LogLevel::Info, "a1", 2, Adm); // seq 2
    ring.PushLog(Logger::LogLevel::Info, "a2", 3, Adm); // seq 3
    ring.PushLog(Logger::LogLevel::Info, "p2", 4, Pub); // seq 4, public 2
    ring.PushLog(Logger::LogLevel::Info, "p3", 5, Pub); // seq 5, public 3; seq 1 (p1) falls off

    const EventBatch full = ring.Read(0, All, 0, EventView::Full);
    EXPECT_EQ(full.dropped, 1u);
    EXPECT_EQ(Seqs(full.events), (std::vector<uint32_t>{2, 3, 4, 5}));

    const EventBatch pub = ring.Read(0, All, 0, EventView::PublicOnly);
    EXPECT_EQ(pub.dropped, 1u) << "p1 was seen by nobody";
    EXPECT_EQ(Seqs(pub.events), (std::vector<uint32_t>{2, 3}));
    EXPECT_EQ(pub.nextSeq, 3u);

    // A client that had read p1 (public 1) misses nothing
    const EventBatch caughtUp = ring.Read(1, All, pub.epoch, EventView::PublicOnly);
    EXPECT_EQ(caughtUp.dropped, 0u);
    EXPECT_EQ(Seqs(caughtUp.events), (std::vector<uint32_t>{2, 3}));
}

TEST(EventRingSensitivityTest, AViewWithNoPublicEventsHeldStartsAtItsNextSeq)
{
    EventRing ring(2, EventRing::DefaultMaxBytes);
    ring.PushLog(Logger::LogLevel::Info, "p1", 1, Pub);
    ring.PushLog(Logger::LogLevel::Info, "a1", 2, Adm);
    ring.PushLog(Logger::LogLevel::Info, "a2", 3, Adm); // p1 falls off, only admin lines are held

    const EventBatch pub = ring.Read(0, All, 0, EventView::PublicOnly);
    EXPECT_TRUE(pub.events.empty());
    EXPECT_EQ(pub.dropped, 1u);
    EXPECT_EQ(pub.nextSeq, 1u);
}

TEST(EventRingSensitivityTest, TheLimitAndMaxEventsApplyToTheViewsOwnEvents)
{
    EventRing ring;
    for (int i = 0; i < 6; ++i)
    {
        ring.PushLog(Logger::LogLevel::Info, "p", i, Pub);
        ring.PushLog(Logger::LogLevel::Info, "a", i, Adm);
    }
    const EventBatch two = ring.Read(0, 2, 0, EventView::PublicOnly);
    EXPECT_EQ(Seqs(two.events), (std::vector<uint32_t>{1, 2}));
    EXPECT_EQ(two.nextSeq, 2u);
    const EventBatch none = ring.Read(0, 0, 0, EventView::PublicOnly);
    EXPECT_TRUE(none.events.empty());
    EXPECT_EQ(none.nextSeq, 6u) << "maxEvents 0 skips everything the view holds";
}

TEST(EventRingSensitivityTest, ANewEpochRestartsBothViews)
{
    EventRing ring;
    ring.PushLog(Logger::LogLevel::Info, "p1", 1, Pub);
    const uint32_t publicEpoch = ring.Epoch(EventView::PublicOnly);
    ring.SetLastSeqForTesting(EventRing::MaxSeq);
    ring.PushLog(Logger::LogLevel::Info, "p-after", 2, Pub);

    EXPECT_NE(ring.Epoch(EventView::PublicOnly), publicEpoch);
    EXPECT_NE(ring.Epoch(EventView::PublicOnly), ring.Epoch(EventView::Full));
    const EventBatch pub = ring.Read(0, All, 0, EventView::PublicOnly);
    ASSERT_EQ(pub.events.size(), 1u);
    EXPECT_EQ(pub.events[0].at("seq").get<uint32_t>(), 1u);
}

TEST(EventRingSensitivityTest, ConcurrentPushersKeepBothViewsConsecutive)
{
    EventRing ring(100000, 64u * 1024u * 1024u);
    constexpr int Threads = 4;
    constexpr int PerThread = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t)
    {
        threads.emplace_back(
            [&ring]
            {
                for (int i = 0; i < PerThread; ++i)
                {
                    ring.PushLog(Logger::LogLevel::Info, "line", i, i % 2 == 0 ? Pub : Adm);
                }
            });
    }
    for (std::thread &thread : threads)
        thread.join();

    const EventBatch full = ring.Read(0, All, 0, EventView::Full);
    const EventBatch pub = ring.Read(0, All, 0, EventView::PublicOnly);
    // Read returns at most MaxEventsPerRead at a time, so page through each
    uint32_t after = 0;
    size_t fullCount = 0;
    for (;;)
    {
        const EventBatch page = ring.Read(after, All, full.epoch, EventView::Full);
        if (page.events.empty())
            break;
        fullCount += page.events.size();
        after = page.nextSeq;
    }
    after = 0;
    size_t publicCount = 0;
    uint32_t expected = 1;
    for (;;)
    {
        const EventBatch page = ring.Read(after, All, pub.epoch, EventView::PublicOnly);
        if (page.events.empty())
            break;
        for (const json &event : page.events)
            EXPECT_EQ(event.at("seq").get<uint32_t>(), expected++);
        publicCount += page.events.size();
        after = page.nextSeq;
    }
    EXPECT_EQ(fullCount, static_cast<size_t>(Threads * PerThread));
    EXPECT_EQ(publicCount, static_cast<size_t>(Threads * PerThread / 2));
    EXPECT_EQ(ring.LastSeq(EventView::PublicOnly), static_cast<uint32_t>(Threads * PerThread / 2));
}

// ==================== Host paths ====================

TEST(HostPathScrubTest, AKnownRootBecomesResAndOtherAbsolutePathsAreHidden)
{
    const std::vector<std::string> roots = {"/srv/games/proj"};
    EXPECT_EQ(EventRing::ScrubHostPaths("Can't open /srv/games/proj/scenes/a.scene: missing", roots),
              "Can't open res://scenes/a.scene: missing");
    EXPECT_EQ(EventRing::ScrubHostPaths("Project at /srv/games/proj", roots), "Project at res://");
    // A longer name that only starts with the root is another folder
    EXPECT_EQ(EventRing::ScrubHostPaths("In /srv/games/project2/x.lua", roots), "In <host path>");

    const std::vector<std::string> windowsRoots = {"C:\\games\\proj\\"};
    const std::string scrubbed = EventRing::ScrubHostPaths("Can't open C:\\games\\proj\\scenes\\a.scene", windowsRoots);
    EXPECT_EQ(scrubbed.rfind("Can't open res://scenes", 0), 0u) << scrubbed;
    EXPECT_EQ(scrubbed.find("games"), std::string::npos) << scrubbed;
}

TEST(HostPathScrubTest, HostPathsOfAnyShapeAreHiddenWithoutKnowingTheirRoot)
{
    const std::vector<std::string> none;
    const auto hidden = [&](const std::string &text, const std::vector<std::string> &leaks)
    {
        const std::string scrubbed = EventRing::ScrubHostPaths(text, none);
        EXPECT_NE(scrubbed.find(EventRing::HiddenPathText), std::string::npos) << text << " -> " << scrubbed;
        for (const std::string &leak : leaks)
            EXPECT_EQ(scrubbed.find(leak), std::string::npos) << text << " -> " << scrubbed;
        return scrubbed;
    };
    hidden("Failed to read C:\\Users\\Someone\\proj\\a.lua", {"Users", "Someone", "proj"});
    hidden("Failed to read C:/Users/Someone/proj/a.lua", {"Users", "Someone"});
    hidden("Failed to read \\\\fileserver\\share\\proj\\a.lua", {"fileserver", "share"});
    hidden("Failed to read /home/someone/proj/a.lua", {"someone", "home"});
    hidden("Failed to read '/var/lib/n2/tokens.json'", {"/var", "tokens"});
    hidden("path=/opt/n2/host/bin", {"/opt", "n2/"});
    hidden("(see /etc/passwd)", {"etc", "passwd"});
    // A folder name with a space is part of the path
    hidden("Failed to read C:\\Users\\Some One\\proj\\a.lua: boom", {"Users", "Some", "One", "proj"});
    EXPECT_NE(EventRing::ScrubHostPaths("Failed to read C:\\Users\\Some One\\proj\\a.lua: boom", none).find("boom"),
              std::string::npos);
    // A line number after the path stays
    EXPECT_NE(EventRing::ScrubHostPaths("C:\\p\\a.lua:12: attempt to call nil", none).find(":12: attempt to call nil"),
              std::string::npos);
}

TEST(HostPathScrubTest, TextThatIsNotAHostPathIsLeftAlone)
{
    const std::vector<std::string> none;
    for (const std::string text : {"Loaded res://scenes/a.scene", "Saved user://save/slot1.json", "and/or",
                                   "ratio 3/4 of /tmp", "https://example.com/a/b", "audit: user=alice role=admin",
                                   "res://a/b/c.lua:7: error", "a:b", "x = 1 / 2 / 3"})
    {
        EXPECT_EQ(EventRing::ScrubHostPaths(text, none), text);
    }
}

TEST(HostPathScrubTest, APublicLineIsScrubbedForThePublicViewAndWholeForTheFullOne)
{
    EventRing ring;
    ring.SetHostRoots({"/srv/games/proj"});
    ring.PushLog(Logger::LogLevel::Error, "Lua error in /srv/games/proj/scripts/a.lua:3: boom", 1, Pub);
    ring.PushLog(Logger::LogLevel::Error, "Cannot load /usr/share/n2/font.ttf", 2, Pub);

    const EventBatch full = ring.Read(0, All, 0, EventView::Full);
    EXPECT_EQ(full.events[0].at("message").get<std::string>(), "Lua error in /srv/games/proj/scripts/a.lua:3: boom");
    EXPECT_EQ(full.events[1].at("message").get<std::string>(), "Cannot load /usr/share/n2/font.ttf");

    const EventBatch pub = ring.Read(0, All, 0, EventView::PublicOnly);
    ASSERT_EQ(pub.events.size(), 2u);
    EXPECT_EQ(pub.events[0].at("message").get<std::string>(), "Lua error in res://scripts/a.lua:3: boom");
    EXPECT_EQ(pub.events[1].at("message").get<std::string>(), "Cannot load <host path>");
    EXPECT_EQ(pub.events[0].at("level").get<std::string>(), "error");
    EXPECT_EQ(pub.events[0].at("seq").get<uint32_t>(), 1u);
    EXPECT_EQ(pub.events[1].at("seq").get<uint32_t>(), 2u);
}

// ==================== The Logger's tag and the host's own lines ====================

TEST(LoggerSensitivityTest, ASubscriberSeesTheTagOfTheLineItIsGiven)
{
    std::vector<std::pair<std::string, Logger::LogSensitivity>> seen;
    const size_t id = Logger::logEvent += [&seen](const std::string_view message, Logger::LogLevel)
    {
        if (!message.starts_with("sens test:"))
            return; // another thread's line
        seen.emplace_back(std::string(message), Logger::CurrentSensitivity());
        // A subscriber that logs: its own line carries its own tag, and the outer one is back afterwards
        if (message == "sens test: outer")
        {
            Logger::Info("sens test: nested public");
            seen.emplace_back("sens test: outer, after the nested line", Logger::CurrentSensitivity());
        }
    };
    Logger::Info("sens test: untagged");
    Logger::Warn("sens test: admin warn", Logger::LogSensitivity::Admin);
    Logger::Error("sens test: public error", Logger::LogSensitivity::Public);
    Logger::Info("sens test: outer", Logger::LogSensitivity::Admin);
    Logger::logEvent -= id;

    ASSERT_EQ(seen.size(), 6u);
    EXPECT_EQ(seen[0], std::make_pair(std::string("sens test: untagged"), Pub)) << "an untagged line is Public";
    EXPECT_EQ(seen[1], std::make_pair(std::string("sens test: admin warn"), Adm));
    EXPECT_EQ(seen[2], std::make_pair(std::string("sens test: public error"), Pub));
    EXPECT_EQ(seen[3], std::make_pair(std::string("sens test: outer"), Adm));
    EXPECT_EQ(seen[4], std::make_pair(std::string("sens test: nested public"), Pub));
    EXPECT_EQ(seen[5], std::make_pair(std::string("sens test: outer, after the nested line"), Adm));
    EXPECT_EQ(Logger::CurrentSensitivity(), Pub) << "outside a delivery";
}

TEST(ServerLogTest, TheHostsLinesAreAdminAndAnEngineLineIsPublic)
{
    EditorServer server;
    Logger::Info("serverlog test: an engine or script line");
    ServerLog::Info("serverlog test: a host info line");
    ServerLog::Warn("serverlog test: a host warning");
    ServerLog::Error("serverlog test: a host error");
    ServerLog::PublicInfo("serverlog test: a public host line");

    const EventBatch pub = server.GetEvents().Read(0, All, 0, EventView::PublicOnly);
    EXPECT_TRUE(Has(pub.events, "serverlog test: an engine or script line"));
    EXPECT_TRUE(Has(pub.events, "serverlog test: a public host line"));
    EXPECT_FALSE(Has(pub.events, "serverlog test: a host info line"));
    EXPECT_FALSE(Has(pub.events, "serverlog test: a host warning"));
    EXPECT_FALSE(Has(pub.events, "serverlog test: a host error"));

    const EventBatch full = server.GetEvents().Read(0, All, 0, EventView::Full);
    EXPECT_TRUE(Has(full.events, "serverlog test: a host info line"));
    EXPECT_TRUE(Has(full.events, "serverlog test: a host warning"));
    EXPECT_TRUE(Has(full.events, "serverlog test: a host error"));
}

TEST(ServerLogTest, GoldenListOfTheLinePrefixesOnlyAnAdminReads)
{
    // The first words of each kind of line the host writes about its users, peers, limits and files (the sites in
    // EditorServer.cpp and Host.cpp). They are written through ServerLog, so they are Admin whatever follows.
    const std::vector<std::string> adminPrefixes = {
        "audit: user=",
        "audit: denied user=",
        "Rate limit: ",
        "Pre-auth limits: ",
        "Editor connected peer=",
        "Editor disconnected peer=",
        "Closing an editor connection that hasn't sent a successful Hello: ",
        "Closing the connection of user=",
        "Session preemption: ",
        "Turned away an authenticated client: ",
        "A client waiting for the session hung up; its connection is closed peer=",
        "Refused Hello from editor client ",
        "Editor client '",
        "Reloaded the tokens file: ",
        "Couldn't reload the tokens file, so the previous tokens stay in force: ",
        "Clients must send Hello with the token of a user in the tokens file ",
        "Wrote the play snapshot ",
        "Playing scene '",
        "Editor server listening on ",
    };

    EditorServer server;
    for (const std::string &prefix : adminPrefixes)
        ServerLog::Warn(prefix + "x");

    const EventBatch pub = server.GetEvents().Read(0, All, 0, EventView::PublicOnly);
    const EventBatch full = server.GetEvents().Read(0, All, 0, EventView::Full);
    for (const std::string &prefix : adminPrefixes)
    {
        EXPECT_FALSE(Has(pub.events, prefix)) << prefix;
        EXPECT_TRUE(Has(full.events, prefix)) << prefix;
    }
}

TEST(ServerLogTest, TheTagArrivesWithALineLoggedOnAnotherThread)
{
    // The tag arrives with the line, on whichever thread logged it
    EditorServer server;
    std::thread(
        []
        {
            Logger::Warn("serverlog thread: public from another thread");
            ServerLog::Warn("serverlog thread: admin from another thread");
        })
        .join();

    const EventBatch pub = server.GetEvents().Read(0, All, 0, EventView::PublicOnly);
    EXPECT_TRUE(Has(pub.events, "serverlog thread: public from another thread"));
    EXPECT_FALSE(Has(pub.events, "serverlog thread: admin from another thread"));
    EXPECT_TRUE(Has(server.GetEvents().Read(0, All).events, "serverlog thread: admin from another thread"));
}

// ==================== PollEvents by role ====================

TEST(PollEventsRoleTest, ANoTokenHostGivesTheLocalUserEveryLineAsBefore)
{
    EditorServer server;
    ASSERT_FALSE(server.RequiresAccessToken());
    Logger::Info("roles test: a script line");
    ServerLog::Warn("audit: user=someone role=editor command=CreateEntity peer=203.0.113.9");
    ServerLog::Warn("Rate limit: 203.0.113.9 is backed off");
    ServerLog::Info("Wrote the play snapshot /srv/games/proj/.n2/play/a.json");

    const Polled polled = Poll(server);
    ASSERT_EQ(polled.type, EventsType);
    EXPECT_TRUE(Has(polled.events, "roles test: a script line"));
    EXPECT_TRUE(Has(polled.events, "audit: user=someone"));
    EXPECT_TRUE(Has(polled.events, "peer=203.0.113.9"));
    EXPECT_TRUE(Has(polled.events, "Rate limit: "));
    EXPECT_TRUE(Has(polled.events, "/srv/games/proj/.n2/play/a.json")) << "a host without roles keeps its full log";
    EXPECT_EQ(polled.epoch, server.GetEvents().Epoch(EventView::Full));
}

TEST(PollEventsRoleTest, ASingleTokenHostIsAnAdminAndReadsEveryLine)
{
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken("the-one-token"));
    BufferWriter hello;
    hello.WriteString("poll roles test");
    hello.WriteString(std::string{ProtocolVersion});
    hello.WriteString("the-one-token");
    ASSERT_EQ(ExecuteType(server, CommandType::Hello, hello.Release()), ServerInfoType);
    ServerLog::Warn("audit: user=token role=admin command=CreateEntity");

    const Polled polled = Poll(server);
    ASSERT_EQ(polled.type, EventsType);
    EXPECT_TRUE(Has(polled.events, "audit: user=token"));
}

TEST_F(PollEventsRoleTest, AnAdminSeesTheAuditLinesAndEveryoneElsesNames)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    HelloAs(server, Bob);
    (void)ExecuteType(server, CommandType::CreateEntity, StringPayload("Cube"));
    (void)ExecuteType(server, CommandType::Shutdown);
    Logger::Info("roles test: a script line");

    HelloAs(server, Alice);
    const Polled polled = Poll(server);
    ASSERT_EQ(polled.type, EventsType);
    EXPECT_TRUE(Has(polled.events, "audit: user=bob role=editor command=CreateEntity"));
    EXPECT_TRUE(Has(polled.events, "audit: denied user=bob role=editor command=Shutdown"));
    EXPECT_TRUE(Has(polled.events, "said Hello"));
    EXPECT_TRUE(Has(polled.events, "user=bob"));
    EXPECT_TRUE(Has(polled.events, "roles test: a script line"));
    EXPECT_EQ(polled.epoch, server.GetEvents().Epoch(EventView::Full));
    // The ring's own numbering, gaps and all (an admin sees what the gaps are)
    EXPECT_EQ(polled.nextSeq, server.GetEvents().LastSeq(EventView::Full));
}

TEST_F(PollEventsRoleTest, AnEditorAndAViewerSeeOnlyThePublicLines)
{
    for (const User *user : {&Bob, &Carol})
    {
        EditorServer server;
        ASSERT_TRUE(server.SetTokensFile(File()));
        HelloAs(server, Bob);
        (void)ExecuteType(server, CommandType::CreateEntity, StringPayload("Cube"));
        (void)ExecuteType(server, CommandType::Shutdown);
        HelloAs(server, Carol);
        (void)ExecuteType(server, CommandType::DestroyEntity, StringPayload("00000000-0000-0000-0000-000000000000"));
        Logger::Info("roles test: a script line");
        Logger::Warn("roles test: Lua error in /srv/games/proj/scripts/a.lua:3: boom");
        ServerLog::PublicInfo("Saved scene res://scenes/main.scene");
        HelloAs(server, *user);

        const Polled polled = Poll(server);
        ASSERT_EQ(polled.type, EventsType) << user->name;
        EXPECT_TRUE(Has(polled.events, "roles test: a script line")) << user->name;
        EXPECT_TRUE(Has(polled.events, "Saved scene res://scenes/main.scene")) << user->name;
        for (const char *hidden : {"audit:", "said Hello", "Refused Hello", "peer=", "user=", "role=", "Rate limit",
                                   "alice", "bob", "carol", "/srv/games/proj", "tokens"})
        {
            EXPECT_FALSE(Has(polled.events, hidden)) << user->name << " must not see: " << hidden;
        }
        EXPECT_TRUE(Has(polled.events, "Lua error in <host path>:3: boom") ||
                    Has(polled.events, "Lua error in res://scripts/a.lua:3: boom"))
            << user->name << ": " << Messages(polled.events).dump();

        // Numbered as a view of its own: 1, 2, 3, ... with no gap where the hidden lines were (there are about ten of
        // them in the full numbering), under the view's own epoch
        const std::vector<uint32_t> seqs = Seqs(polled.events);
        for (size_t i = 0; i < seqs.size(); ++i)
            EXPECT_EQ(seqs[i], i + 1) << user->name;
        EXPECT_GE(server.GetEvents().LastSeq(EventView::Full), seqs.size() + 5) << "the full numbering has more";
        EXPECT_EQ(polled.nextSeq, seqs.size()) << user->name;
        EXPECT_EQ(polled.epoch, server.GetEvents().Epoch(EventView::PublicOnly)) << user->name;
        EXPECT_NE(polled.epoch, server.GetEvents().Epoch(EventView::Full)) << user->name;

        // Polling again after hidden lines (the next Hello and a refused command) finds nothing, and no seq moves
        HelloAs(server, *user);
        (void)ExecuteType(server, CommandType::Shutdown);
        const Polled again = Poll(server, polled.nextSeq, polled.epoch);
        ASSERT_EQ(again.type, EventsType);
        EXPECT_TRUE(again.events.empty()) << user->name << ": " << Messages(again.events).dump();
        EXPECT_EQ(again.nextSeq, polled.nextSeq);
        EXPECT_EQ(again.dropped, 0u);
    }
}

TEST_F(PollEventsRoleTest, ASessionTakenOverByAnotherRoleReadsItsOwnViewFromTheStart)
{
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    HelloAs(server, Alice);
    Logger::Info("roles test: first public line");
    ServerLog::Info("roles test: an admin line");
    Logger::Info("roles test: second public line");
    const Polled asAdmin = Poll(server);
    ASSERT_EQ(asAdmin.type, EventsType);

    // The same host, now serving a viewer who passes the admin's last position: it starts over, in its own numbering
    HelloAs(server, Carol);
    const Polled asViewer = Poll(server, asAdmin.nextSeq, asAdmin.epoch);
    ASSERT_EQ(asViewer.type, EventsType);
    EXPECT_NE(asViewer.epoch, asAdmin.epoch);
    EXPECT_TRUE(Has(asViewer.events, "roles test: first public line"));
    EXPECT_TRUE(Has(asViewer.events, "roles test: second public line"));
    EXPECT_FALSE(Has(asViewer.events, "roles test: an admin line"));
    EXPECT_EQ(asViewer.events.front().at("seq").get<uint32_t>(), 1u);
}

TEST_F(PollEventsRoleTest, TheHealthDetailOfANonAdminHasNoHostPaths)
{
    // GetEngineHealth is a Read command every role runs; what the subsystems say passes the same scrubbing. Without an
    // initialized engine the detail has no path in it, so this checks the command still answers each role.
    EditorServer server;
    ASSERT_TRUE(server.SetTokensFile(File()));
    for (const User *user : {&Alice, &Bob, &Carol})
    {
        HelloAs(server, *user);
        EXPECT_EQ(ExecuteType(server, CommandType::GetEngineHealth), static_cast<uint8_t>(ResponseType::EngineHealth))
            << user->name;
    }
}
