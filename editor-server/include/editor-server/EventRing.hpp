#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
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

    /// Whose events a Read returns. Full is every event (an admin, or a host with no authentication, whose one local
    /// user has always seen everything). PublicOnly is what any role may see: the events tagged Public, with host paths
    /// scrubbed from their text, numbered as a view of their own (see EventRing).
    enum class EventView : uint8_t
    {
        Full,
        PublicOnly,
    };

    /// The editor's event ring: a bounded buffer of events ({seq, kind, ...}) that PollEvents reads.
    ///
    /// Every event gets the next seq, from 1, consecutive within an epoch. The epoch is a random non-zero number picked
    /// when the ring is made (so each host process has its own) and again if the seqs would pass the uint32 range: then
    /// the ring is emptied and numbering restarts at 1 under a new epoch. A reader passes the epoch and the last seq it
    /// has seen; afterSeq only means something within the epoch it came from, so Read treats it as 0 when the reader's
    /// epoch is non-zero and isn't the ring's (or when afterSeq is newer than any seq of this epoch).
    ///
    /// Sensitivity: every event is stored with the Logger's LogSensitivity tag (Push and PushLog take it, Public by
    /// default; the tag is not part of the event's JSON). The PublicOnly view has its own numbering, so it leaks nothing
    /// of the events it doesn't show: its seqs are consecutive over the Public events alone (a client never sees a gap
    /// that stands for a hidden line), and it has its own epoch, so a client that moves from one view to the other (a
    /// session taken over by another role) restarts instead of misreading a seq. What it can still observe is that the
    /// byte and count limits are shared: a burst of hidden lines evicts Public ones sooner, which shows as `dropped`.
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

        /// What replaces a host path that is not under a known root, in the text a PublicOnly reader gets
        static constexpr const char *HiddenPathText = "<host path>";

        explicit EventRing(size_t maxEvents = DefaultMaxEvents, size_t maxBytes = DefaultMaxBytes);

        EventRing(const EventRing &) = delete;
        EventRing &operator=(const EventRing &) = delete;

        /// Adds an event of this kind with these fields (a JSON object; anything else is kept under "value") and returns
        /// its seq. seq and kind are set here, over any fields of those names.
        uint32_t Push(std::string_view kind, nlohmann::json fields = nlohmann::json::object(),
                      Logger::LogSensitivity sensitivity = Logger::LogSensitivity::Public);

        /// Adds a log event: {seq, kind: "log", level: "info" | "warn" | "error", message, time (Unix milliseconds)}
        /// A PublicOnly reader gets the message with host paths scrubbed (ScrubHostPaths); an Admin-tagged line is not
        /// given to one at all.
        uint32_t PushLog(Logger::LogLevel level, std::string_view message, int64_t unixMilliseconds,
                         Logger::LogSensitivity sensitivity = Logger::LogSensitivity::Public);

        /// The events with a seq after afterSeq, oldest first, at most min(maxEvents, MaxEventsPerRead) of them.
        /// afterSeq reads as 0 when epoch is non-zero and isn't this ring's, or when it is newer than any seq of this
        /// epoch (a reader that doesn't know the epoch passes 0). maxEvents 0 returns none, with nextSeq the newest
        /// seq, which skips everything retained.
        /// afterSeq, epoch, nextSeq and dropped all belong to the view asked for.
        [[nodiscard]] EventBatch Read(uint32_t afterSeq, uint32_t maxEvents, uint32_t epoch = 0,
                                      EventView view = EventView::Full) const;

        /// The newest seq issued in this epoch (of the view: PublicOnly counts the Public events), 0 before its first
        [[nodiscard]] uint32_t LastSeq(EventView view = EventView::Full) const;
        /// The current epoch (never 0); each view has its own
        [[nodiscard]] uint32_t Epoch(EventView view = EventView::Full) const;

        /// The folders whose path prefix a PublicOnly reader sees as res:// (the open project's assets folder, which is res://). Replaces the
        /// earlier list; applies to lines pushed after it.
        void SetHostRoots(std::vector<std::string> roots);
        /// text with host paths made safe to show a non-admin: a path under a known root becomes res://..., any other
        /// absolute path (a drive letter, a UNC share, or two or more POSIX components after a word boundary) becomes
        /// HiddenPathText. A heuristic on top of the sensitivity tags, never instead of them.
        [[nodiscard]] std::string ScrubHostPaths(std::string_view text) const;
        /// The same with an explicit list of roots
        [[nodiscard]] static std::string ScrubHostPaths(std::string_view text, const std::vector<std::string> &roots);
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
            /// Position among the Public events (0 for one that isn't); what a PublicOnly reader sees as seq
            uint32_t publicSeq;
            nlohmann::json event;
            /// What a PublicOnly reader gets when it differs from `event` (a log line whose text was scrubbed)
            std::optional<nlohmann::json> publicEvent;
            size_t bytes;
            bool isPublic;
        };

        /// Gives the event the next seq (starting a new epoch past MaxSeq), stores it and drops the oldest beyond the
        /// limits. The seq is only taken once the event is stored, so a failed allocation leaves no gap.
        uint32_t Store(nlohmann::json event, size_t bytes, Logger::LogSensitivity sensitivity,
                       std::optional<nlohmann::json> publicEvent = std::nullopt);
        /// Requires the lock: fills `batch` as Read does for the PublicOnly view
        void ReadPublicLocked(EventBatch &batch, uint32_t afterSeq, uint32_t maxEvents, uint32_t epoch) const;
        /// A random epoch for the PublicOnly view: neither previous nor the full view's
        [[nodiscard]] static uint32_t NewPublicEpoch(uint32_t previous, uint32_t fullEpoch);
        /// Requires the lock: empties the ring
        void ClearLocked();

        const size_t _maxEvents;
        const size_t _maxBytes;

        mutable std::mutex _mutex;
        std::deque<Entry> _entries;
        size_t _bytes = 0;
        // 64 bits, so the step past MaxSeq is seen rather than wrapping; it never exceeds MaxSeq
        uint64_t _lastSeq = 0;
        // The same for the PublicOnly view: counts only the Public events, so never ahead of _lastSeq
        uint64_t _lastPublicSeq = 0;
        uint32_t _epoch;
        uint32_t _publicEpoch;
        uint64_t _droppedTotal = 0;

        // Its own lock: ScrubHostPaths runs before the ring's mutex is taken, and neither is held while the other is
        mutable std::mutex _rootsMutex;
        std::vector<std::string> _hostRoots;
    };
}
