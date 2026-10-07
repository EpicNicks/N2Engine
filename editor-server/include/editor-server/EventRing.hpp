#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
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
        /// The ring's epoch: which numbering the seqs belong to (see EventRing). Never 0.
        uint32_t epoch = 0;
        /// The afterSeq for the client's next poll: the last returned event's seq, or, when none is returned, the
        /// newest seq issued in this epoch (0 before its first event)
        uint32_t nextSeq = 0;
        /// Events after afterSeq that fell off the ring before this poll, so the client never saw them
        uint32_t dropped = 0;
        /// Each an EditorEvent (protocol.json's jsonTypes): {seq, kind, ...the kind's fields}
        std::vector<nlohmann::json> events;
    };

    /// The editor's event ring: a bounded buffer of events ({seq, kind, ...}) that PollEvents reads.
    ///
    /// Every event gets the next seq, from 1, consecutive within an epoch. The epoch is a random non-zero number picked
    /// when the ring is made (so each host process has its own) and again if the seqs would pass the uint32 range: then
    /// the ring is emptied and numbering restarts at 1 under a new epoch. A reader passes the epoch and the last seq it
    /// has seen; afterSeq only means something within the epoch it came from, so Read treats it as 0 when the reader's
    /// epoch is non-zero and isn't the ring's (or when afterSeq is newer than any seq of this epoch).
    ///
    /// The ring keeps the newest events within both limits (a count and an approximate byte size) and drops the oldest
    /// beyond them; the newest event is always kept. It keeps no per-client state: what fell off after a reader's
    /// afterSeq is counted from that, as GetAudio counts droppedFrames.
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
        /// The last seq of an epoch: the next event starts a new epoch at seq 1
        static constexpr uint32_t MaxSeq = std::numeric_limits<uint32_t>::max();
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
        /// afterSeq reads as 0 when epoch is non-zero and isn't this ring's, or when it is newer than any seq of this
        /// epoch (a reader that doesn't know the epoch passes 0). maxEvents 0 returns none, with nextSeq the newest
        /// seq, which skips everything retained.
        [[nodiscard]] EventBatch Read(uint32_t afterSeq, uint32_t maxEvents, uint32_t epoch = 0) const;

        /// The newest seq issued in this epoch, 0 before its first event
        [[nodiscard]] uint32_t LastSeq() const;
        /// The current epoch (never 0)
        [[nodiscard]] uint32_t Epoch() const;
        /// How many events the ring holds now
        [[nodiscard]] size_t Size() const;
        /// How many events have fallen off (or been cleared by a new epoch) since the ring was made
        [[nodiscard]] uint64_t DroppedTotal() const;

        /// Tests only: drops every event and continues numbering after lastSeq, in the same epoch (so a test reaches
        /// MaxSeq without pushing four billion events)
        void SetLastSeqForTesting(uint32_t lastSeq);

        /// "info", "warn" or "error"
        [[nodiscard]] static std::string_view LevelName(Logger::LogLevel level);
        /// text, or its first maxBytes bytes cut back to a UTF-8 character boundary with "..." appended
        [[nodiscard]] static std::string TruncateUtf8(std::string_view text, size_t maxBytes);
        /// The system clock now, in milliseconds since the Unix epoch (a log event's time)
        [[nodiscard]] static int64_t NowUnixMilliseconds();
        /// A random epoch that is neither 0 nor previous
        [[nodiscard]] static uint32_t NewEpoch(uint32_t previous = 0);

    private:
        struct Entry
        {
            uint32_t seq;
            nlohmann::json event;
            size_t bytes;
        };

        /// Gives the event the next seq (starting a new epoch past MaxSeq), stores it and drops the oldest beyond the
        /// limits. The seq is only taken once the event is stored, so a failed allocation leaves no gap.
        uint32_t Store(nlohmann::json event, size_t bytes);
        /// Requires the lock: empties the ring
        void ClearLocked();

        const size_t _maxEvents;
        const size_t _maxBytes;

        mutable std::mutex _mutex;
        std::deque<Entry> _entries;
        size_t _bytes = 0;
        // 64 bits, so the step past MaxSeq is seen rather than wrapping; it never exceeds MaxSeq
        uint64_t _lastSeq = 0;
        uint32_t _epoch;
        uint64_t _droppedTotal = 0;
    };
}
