#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/EventRing.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>
#include <engine/Logger.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

// PollEvents through EditorServer::ExecuteCommand (no socket): the Logger subscription, the wire format, and that
// polling logs nothing. EventRingTests covers the ring's order, afterSeq and dropped rules on their own.

namespace
{
    constexpr uint32_t All = std::numeric_limits<uint32_t>::max();
    constexpr uint8_t EventsType = static_cast<uint8_t>(ResponseType::Events);

    struct PolledEvents
    {
        uint8_t type = 0xEE;
        uint32_t epoch = 0;
        uint32_t nextSeq = 0;
        uint32_t dropped = 0;
        json events;
        std::string error;
    };

    std::vector<uint8_t> PollPayload(uint32_t epoch, uint32_t afterSeq, uint32_t maxEvents)
    {
        BufferWriter w;
        w.WriteU32(epoch);
        w.WriteU32(afterSeq);
        w.WriteU32(maxEvents);
        return w.Release();
    }

    PolledEvents PollRaw(EditorServer &server, const std::vector<uint8_t> &payload)
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(CommandType::PollEvents), payload);
        BufferReader r(frame);
        PolledEvents result;
        result.type = r.ReadU8();
        const uint32_t size = r.ReadU32();
        EXPECT_EQ(size, r.Remaining());
        if (result.type != static_cast<uint8_t>(ResponseType::Events))
        {
            const auto bytes = r.ReadBytes(r.Remaining());
            result.error.assign(bytes.begin(), bytes.end());
            return result;
        }
        result.epoch = r.ReadU32();
        result.nextSeq = r.ReadU32();
        result.dropped = r.ReadU32();
        result.events = ReadJson(r);
        EXPECT_FALSE(r.HasData());
        return result;
    }

    /// A poll that doesn't know the epoch (0)
    PolledEvents Poll(EditorServer &server, uint32_t afterSeq, uint32_t maxEvents = All)
    {
        return PollRaw(server, PollPayload(0, afterSeq, maxEvents));
    }

    PolledEvents PollInEpoch(EditorServer &server, uint32_t epoch, uint32_t afterSeq)
    {
        return PollRaw(server, PollPayload(epoch, afterSeq, All));
    }

    const json *FindMessage(const json &events, std::string_view message)
    {
        for (const json &event : events)
        {
            if (event.value("message", "") == message)
                return &event;
        }
        return nullptr;
    }

    size_t CountContaining(const json &events, std::string_view text)
    {
        size_t count = 0;
        for (const json &event : events)
        {
            if (event.value("message", "").find(text) != std::string::npos)
                ++count;
        }
        return count;
    }
}

TEST(EditorServerEventsTest, LoggedLinesArePolledAsLogEvents)
{
    EditorServer server;
    Logger::Info("events test: an info line");
    Logger::Error("events test: an error line");

    const PolledEvents polled = Poll(server, 0);
    ASSERT_EQ(polled.type, EventsType) << polled.error;
    ASSERT_TRUE(polled.events.is_array());

    const json *info = FindMessage(polled.events, "events test: an info line");
    ASSERT_NE(info, nullptr) << polled.events.dump();
    EXPECT_EQ(info->at("kind").get<std::string>(), "log");
    EXPECT_EQ(info->at("level").get<std::string>(), "info");
    EXPECT_TRUE(info->at("time").is_number_integer());

    const json *error = FindMessage(polled.events, "events test: an error line");
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->at("level").get<std::string>(), "error");
    EXPECT_GT(error->at("seq").get<uint32_t>(), info->at("seq").get<uint32_t>());
    EXPECT_EQ(polled.nextSeq, server.GetEvents().LastSeq());
}

TEST(EditorServerEventsTest, AServerConstructedBeforeLoggingKeepsTheLines)
{
    // What the host relies on by constructing the server before Application::Init: no Start, no poll, the lines are
    // in the ring as they are logged
    EditorServer server;
    Logger::Info("events test: logged before Start");
    Logger::Warn("events test: also before Start");

    const EventBatch batch = server.GetEvents().Read(0, All);
    const json events = json(batch.events);
    EXPECT_NE(FindMessage(events, "events test: logged before Start"), nullptr) << events.dump();
    EXPECT_NE(FindMessage(events, "events test: also before Start"), nullptr) << events.dump();
}

TEST(EditorServerEventsTest, ALineLoggedOnAnotherThreadArrives)
{
    EditorServer server;
    std::thread([] { Logger::Warn("events test: from another thread"); }).join();

    const PolledEvents polled = Poll(server, 0);
    ASSERT_EQ(polled.type, EventsType) << polled.error;
    const json *line = FindMessage(polled.events, "events test: from another thread");
    ASSERT_NE(line, nullptr) << polled.events.dump();
    EXPECT_EQ(line->at("level").get<std::string>(), "warn");
}

TEST(EditorServerEventsTest, PollingNeverLogsSoAnIdleServerHasNoNewEvents)
{
    EditorServer server;
    Logger::Info("events test: before polling");
    const PolledEvents first = Poll(server, 0);
    ASSERT_EQ(first.type, EventsType) << first.error;

    // A poll that logged anything would find its own line on the next one. (Other threads may log meanwhile, so
    // this looks for lines about polling rather than for no lines at all.)
    uint32_t afterSeq = first.nextSeq;
    for (int i = 0; i < 5; ++i)
    {
        const PolledEvents again = Poll(server, afterSeq);
        ASSERT_EQ(again.type, EventsType) << again.error;
        for (const json &event : again.events)
        {
            const std::string message = event.value("message", "");
            EXPECT_EQ(message.find("PollEvents"), std::string::npos) << message;
            EXPECT_EQ(message.find("0x5"), std::string::npos) << message;
            EXPECT_EQ(message.find("oll"), std::string::npos) << message; // poll, Poll
        }
        afterSeq = again.nextSeq;
    }
}

TEST(EditorServerEventsTest, AMalformedPollIsAnErrorAndLoggedOnce)
{
    // As for every polled command, a failure is still logged, so the next poll finds it
    EditorServer server;
    const uint32_t before = server.GetEvents().LastSeq();

    const PolledEvents malformed = PollRaw(server, std::vector<uint8_t>{1, 2});
    EXPECT_EQ(malformed.type, static_cast<uint8_t>(ResponseType::Error));

    const PolledEvents polled = Poll(server, before);
    ASSERT_EQ(polled.type, EventsType) << polled.error;
    EXPECT_EQ(CountContaining(polled.events, "Command 0x5 failed"), 1u) << polled.events.dump();
    for (const json &event : polled.events)
    {
        if (event.value("message", "").find("Command 0x5 failed") != std::string::npos)
            EXPECT_EQ(event.at("level").get<std::string>(), "error");
    }
}

TEST(EditorServerEventsTest, TheEpochKeepsAReconnectingClientsPlace)
{
    EditorServer server;
    Logger::Info("events test: epoch one");
    const PolledEvents first = Poll(server, 0);
    ASSERT_EQ(first.type, EventsType) << first.error;
    EXPECT_EQ(first.epoch, server.GetEvents().Epoch());
    EXPECT_NE(first.epoch, 0u);

    Logger::Info("events test: epoch two");

    // The same host (the same epoch): it continues after nextSeq, so nothing is lost and nothing comes twice
    const PolledEvents same = PollInEpoch(server, first.epoch, first.nextSeq);
    ASSERT_EQ(same.type, EventsType) << same.error;
    EXPECT_EQ(FindMessage(same.events, "events test: epoch one"), nullptr) << same.events.dump();
    EXPECT_NE(FindMessage(same.events, "events test: epoch two"), nullptr) << same.events.dump();

    // Another host (another epoch): its afterSeq means nothing here, so it reads from the start
    const PolledEvents other = PollInEpoch(server, EventRing::NewEpoch(first.epoch), first.nextSeq);
    ASSERT_EQ(other.type, EventsType) << other.error;
    EXPECT_NE(FindMessage(other.events, "events test: epoch one"), nullptr) << other.events.dump();
    EXPECT_NE(FindMessage(other.events, "events test: epoch two"), nullptr) << other.events.dump();
    EXPECT_EQ(other.epoch, first.epoch);
}

TEST(EditorServerEventsTest, EventsAHandlerPushesArePolled)
{
    EditorServer server;
    const uint32_t before = server.GetEvents().LastSeq();
    server.GetEvents().Push("sceneChanged", json{{"revision", 3}});

    const PolledEvents polled = Poll(server, before);
    ASSERT_EQ(polled.type, EventsType) << polled.error;
    ASSERT_EQ(polled.events.size(), 1u);
    EXPECT_EQ(polled.events[0].at("kind").get<std::string>(), "sceneChanged");
    EXPECT_EQ(polled.events[0].at("revision").get<int>(), 3);
}

TEST(EditorServerEventsTest, TheServerUnsubscribesFromTheLoggerWhenDestroyed)
{
    const size_t subscribers = Logger::logEvent.GetSubscriberCount();
    {
        EditorServer server;
        EXPECT_EQ(Logger::logEvent.GetSubscriberCount(), subscribers + 1);
    }
    EXPECT_EQ(Logger::logEvent.GetSubscriberCount(), subscribers);
    // A line logged now reaches no destroyed server
    Logger::Info("events test: after the server is gone");
}
