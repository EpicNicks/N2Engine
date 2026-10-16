#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::Editor
{
    /**
     * Bounds the log lines that connections which never authenticate can make (audit F6, security-design Part 2 scope
     * item 9): a pre-auth worker posts its lines to the main thread's queue, which has no bound, so a peer that loops
     * connect + garbage must not write one line per connection.
     *
     * Each kind of outcome (a TLS handshake that failed, a Hello that never came, a refused Hello, ...) has a window. In
     * a window the first `burst` occurrences are written one by one, as before; the rest are only counted. When the
     * window has passed, the count is reported in one summary line (by TakeClosed, which the acceptor calls about once
     * a second, or by the next occurrence of the kind, whichever is first) and the kind starts afresh. So a kind writes
     * at most burst + 1 lines per window, whatever the number of connections or addresses (the kind is the key, not the
     * address: a spray from many addresses is bounded too).
     *
     * The time comes from an injected function, so its tests never sleep. A clock that goes backwards re-anchors the
     * window at the new time and keeps its counts: the window neither stalls until the old time returns nor ends early.
     *
     * Thread-safe: one mutex, the clock read before it is taken, nothing called out while it is held.
     */
    struct LogCoalescerOptions
    {
        /// How long a window lasts (clamped to at least 1 ms)
        std::chrono::milliseconds window{10000};
        /// How many lines of one kind a window writes one by one (clamped to at least 1)
        uint32_t burst = 10;
    };

    class LogCoalescer
    {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;
        using Clock = std::function<TimePoint()>;

        /// Lines of one kind that a window that has ended did not write
        struct Summary
        {
            std::string kind;
            uint64_t suppressed = 0;
            std::chrono::milliseconds window{0};
        };

        struct Decision
        {
            /// Write this occurrence's line(s) now
            bool write = false;
            /// The kind's previous window ended with lines unwritten: report it (before this occurrence's line)
            std::optional<Summary> closed;
        };

        /// An empty clock is std::chrono::steady_clock
        explicit LogCoalescer(LogCoalescerOptions options = {}, Clock clock = {});

        /// The options as clamped
        [[nodiscard]] const LogCoalescerOptions &Options() const { return _options; }

        /// One occurrence of this kind: whether its line is written, and a summary of the kind's last window if it ended
        [[nodiscard]] Decision Record(std::string_view kind);

        /// The windows that have ended with lines unwritten (those kinds start afresh); kinds whose window ended with
        /// nothing unwritten are forgotten
        [[nodiscard]] std::vector<Summary> TakeClosed();

        /// Everything unwritten, whatever the time (stopping); every kind starts afresh
        [[nodiscard]] std::vector<Summary> TakeAll();

        /// Occurrences not written one by one, since this object was made
        [[nodiscard]] uint64_t TotalSuppressed() const;

        /// The summary line: "N more pre-auth failure(s) of kind 'X' in the last T s were not logged one by one ..."
        [[nodiscard]] static std::string Describe(const Summary &summary, uint32_t burst);

    private:
        struct Window
        {
            TimePoint start{};
            uint32_t written = 0;
            uint64_t suppressed = 0;
        };

        [[nodiscard]] TimePoint Now() const;

        LogCoalescerOptions _options;
        Clock _clock;
        mutable std::mutex _mutex;
        std::map<std::string, Window, std::less<>> _windows;
        uint64_t _totalSuppressed = 0;
    };
}
