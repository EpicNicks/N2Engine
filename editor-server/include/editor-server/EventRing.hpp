#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/Logger.hpp"

namespace N2Engine::Editor
{
    /// What PollEvents answers: the events after a client's afterSeq, oldest first
    struct EventBatch
    {
        /// The afterSeq for the client's next poll: the last returned event's seq, or, when none is returned, the
        /// newest seq issued (0 before the first event)
        uint32_t nextSeq = 0;
        /// Events after afterSeq that fell off the ring before this poll, so the client never saw them
        uint32_t dropped = 0;
        /// Each an EditorEvent (protocol.json's jsonTypes): {seq, kind, ...the kind's fields}
        std::vector<nlohmann::json> events;
    };

    /// The editor's event ring: a bounded buffer of events ({seq, kind, ...}) that PollEvents reads.
    ///
    /// Every event gets the next seq, from 1, never reused while the ring lives. The ring keeps the newest events
    /// within both limits (a count and an approximate byte size) and drops the oldest beyond them; the newest event is
    /// always kept. It keeps no per-client state: a reader passes the last seq it has seen (afterSeq), and what fell off
    /// in between is counted from that, as GetAudio counts droppedFrames.
    ///
    /// Thread-safe: Push may be called from any thread (the Logger's subscriber runs on whichever thread logged), Read
    /// from any other. Each holds the ring's own mutex only to copy, and never logs or waits on another thread while
    /// holding it: a subscriber holds the Logger's lock when it pushes, so a reader that logged under the ring's mutex
    /// could deadlock with it.
    class EventRing
    {
    public:
        static constexpr size_t DefaultMaxEvents = 4096;
        static constexpr size_t DefaultMaxBytes = 2u * 1024u * 1024u;
        /// The most events one Read returns, whatever maxEvents asks for (the rest come with the next poll)
        static constexpr uint32_t MaxEventsPerRead = 1024;
        /// A log message longer than this is cut (at a UTF-8 character boundary) and "..." appended
        static constexpr size_t MaxLogMessageBytes = 16u * 1024u;
        /// What an event costs against maxBytes beyond its text: its seq, kind, level and time, and the JSON around them
        static constexpr size_t EventOverheadBytes = 64;
        /// The kind of the events PushLog makes
        static constexpr const char *LogKind = "log";

        explicit EventRing(size_t maxEvents = DefaultMaxEvents, size_t maxBytes = DefaultMaxBytes);

        EventRing(const EventRing &) = delete;
        EventRing &operator=(const EventRing &) = delete;

        /// Adds an event of this kind with these fields (a JSON object; anything else is kept under "value") and returns
        /// its seq. seq and kind are set here, over any fields of those names.
        uint32_t Push(std::string_view kind, nlohmann::json fields = nlohmann::json::object());

        /// Adds a log event: {seq, kind: "log", level: "info" | "warn" | "error", message, time (Unix milliseconds)}
        uint32_t PushLog(Logger::LogLevel level, std::string_view message, int64_t unixMilliseconds);

        /// The events with a seq after afterSeq, oldest first, at most min(maxEvents, MaxEventsPerRead) of them.
        /// maxEvents 0 returns none, with nextSeq the newest seq, which skips everything retained. An afterSeq newer
        /// than any seq issued (a client of an earlier host process) reads as 0, so that client gets the backlog.
        [[nodiscard]] EventBatch Read(uint32_t afterSeq, uint32_t maxEvents) const;

        /// The newest seq issued, 0 before the first event
        [[nodiscard]] uint32_t LastSeq() const;
        /// How many events the ring holds now
        [[nodiscard]] size_t Size() const;
        /// How many events have fallen off since the ring was made
        [[nodiscard]] uint64_t DroppedTotal() const;

        /// "info", "warn" or "error"
        [[nodiscard]] static std::string_view LevelName(Logger::LogLevel level);
        /// text, or its first maxBytes bytes cut back to a UTF-8 character boundary with "..." appended
        [[nodiscard]] static std::string TruncateUtf8(std::string_view text, size_t maxBytes);
        /// The system clock now, in milliseconds since the Unix epoch (a log event's time)
        [[nodiscard]] static int64_t NowUnixMilliseconds();

    private:
        struct Entry
        {
            uint32_t seq;
            nlohmann::json event;
            size_t bytes;
        };

        /// Gives the event the next seq, stores it and drops the oldest beyond the limits
        uint32_t Store(nlohmann::json event, size_t bytes);

        const size_t _maxEvents;
        const size_t _maxBytes;

        mutable std::mutex _mutex;
        std::deque<Entry> _entries;
        size_t _bytes = 0;
        uint32_t _lastSeq = 0;
        uint64_t _droppedTotal = 0;
    };
}
