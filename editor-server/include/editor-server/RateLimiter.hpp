#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace N2Engine::Editor
{
    /**
     * Failed-Hello rate limiting for a host that needs an access token: who may connect, and when, after Hellos with a
     * wrong token. Plain logic with no sockets and no real clock (the time comes from an injected function), so it is
     * unit tested on its own and its tests never sleep. EditorServer owns one. Every public method takes one mutex (they are short
     * and logic-only, and nothing is called out while it is held: the injected clock is read before it is taken), so the
     * pre-auth workers of a later change can share it.
     *
     * Three rules, each a table that is bounded so the limiter is not itself something to exhaust:
     *  - per address: after `freeFailures` failed Hellos from an address, it is backed off for `initialBackoff`,
     *    doubling with each further failure up to `maxBackoff`. A connection from a backed-off address is refused at
     *    accept (AdmitConnection), before any TLS handshake. A success clears the address; so does `idleReset` without a
     *    failure.
     *  - loopback (127.0.0.0/8, ::1) is exempt from the per-address rule, because a reverse proxy or an SSH tunnel that
     *    connects over loopback makes every peer loopback and one ban would lock everybody out (a proxy or container on
     *    another network presents its own address, which is limited like any other). Since PR S-4 it has NO backoff of its own either: the
     *    shared counter that used to refuse every loopback connection after ten failed Hellos in a minute (and so locked
     *    out a client with a CORRECT token) is gone. What bounds loopback is a cap on pre-auth concurrency and handshake
     *    rate (PreAuthSlots: loopback may use every slot; TokenBucket: the global bucket applies to loopback), which no
     *    failure of anybody changes, and the per-user backoff below, which a correct token passes.
     *  - per user: failed Hellos that name a user are counted against that name wherever they come from (the only
     *    handle on an attacker behind a tunnel), with a SHORT cap (`userMaxBackoff`) so an attacker can't keep a real
     *    user out for long. A success of that user clears it.
     * A connection that ends without a successful Hello, and without a wrong-token Hello (which is counted as such), is a
     * failure too: it timed out, closed early, sent another command or an oversized frame, or failed its TLS handshake
     * (RecordUnfinishedHello). Per non-loopback address only: loopback has nothing to count it in (see above) and
     * relies on the pre-Hello budget and the slot and handshake caps instead.
     * No trusted-proxy support: the peer address is the TCP peer, never a header or a PROXY line.
     */

    /// The other end of a TCP connection, as getpeername reports it
    class PeerAddress
    {
    public:
        enum class Family : uint8_t
        {
            /// getpeername failed, or the family is not IP
            Unknown,
            IPv4,
            IPv6,
        };

        PeerAddress() = default;

        [[nodiscard]] static PeerAddress IPv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
        /// 16 bytes in network order. An IPv4-mapped address (::ffff:a.b.c.d) becomes the IPv4 address, so one peer has
        /// one spelling whichever socket family saw it.
        [[nodiscard]] static PeerAddress IPv6(const std::array<uint8_t, 16> &bytes);

        [[nodiscard]] Family GetFamily() const { return _family; }
        /// 127.0.0.0/8 or ::1
        [[nodiscard]] bool IsLoopback() const;
        /// "203.0.113.7", "2001:db8::1" (lowercase, the longest run of zero groups as "::") or "unknown". Safe in a log
        /// line, and what the tables key on.
        [[nodiscard]] std::string ToString() const;

        [[nodiscard]] bool operator==(const PeerAddress &other) const = default;

    private:
        Family _family = Family::Unknown;
        // IPv4 uses the first four bytes
        std::array<uint8_t, 16> _bytes{};
    };

    struct RateLimitPolicy
    {
        /// Failed Hellos from one address before it is backed off; the failure after these starts at `initialBackoff`
        uint32_t freeFailures = 2;
        std::chrono::milliseconds initialBackoff{1000};
        std::chrono::milliseconds maxBackoff{5 * 60 * 1000};
        /// Without a failure for this long an address (or user, or loopback) is forgotten
        std::chrono::milliseconds idleReset{15 * 60 * 1000};
        /// Entries per table (addresses, users); past it the oldest idle one is evicted
        size_t maxEntries = 4096;
        /// Failed Hellos naming one user before that user is backed off (then doubling, capped at `userMaxBackoff`)
        uint32_t userFreeFailures = 2;
        std::chrono::milliseconds userMaxBackoff{30 * 1000};
        /// The TLS handshake budget and the Hello budget of an address with failures on record: each is the shorter of
        /// this and the host's own (EditorServer::DefaultTlsHandshakeTimeout, 5 s; DefaultHelloTimeout, 2 s). For the
        /// Hello it equals the default, so by default the distinction no longer applies there: a legitimate user who
        /// mistyped a token still needs the full 2 s for the retry over a slow link, and the pre-Hello failures this
        /// limiter now counts back every offender off anyway. The handshake is cut from 5 s to 2 s, as it was before the
        /// two budgets were split. An operator who raises the host's budgets gets this shorter one for addresses on
        /// record.
        std::chrono::milliseconds priorOffenderBudget{2000};
    };

    class RateLimiter
    {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;
        using Duration = std::chrono::milliseconds;
        using Clock = std::function<TimePoint()>;

        /// An empty clock is std::chrono::steady_clock
        explicit RateLimiter(RateLimitPolicy policy = {}, Clock clock = {});

        [[nodiscard]] const RateLimitPolicy &Policy() const { return _policy; }

        struct Admission
        {
            /// False: close the connection now, before the TLS handshake
            bool allowed = true;
            /// The address has failed Hellos on record: give it the short budget
            bool priorOffender = false;
            /// How much longer the backoff lasts (a refusal only)
            Duration retryAfter{0};
            /// Connections closed during a backoff that has now ended, reported once on the first connection after it
            uint32_t suppressed = 0;
        };

        /// At accept. A non-loopback address is held to its own backoff; loopback is always admitted (its caps are the
        /// pre-auth slots and the handshake rate, which the caller applies next).
        [[nodiscard]] Admission AdmitConnection(const PeerAddress &peer);
        /// Whether a Hello naming this user is refused for now (the user is backed off). Doesn't count as a failure.
        [[nodiscard]] bool IsUserBackedOff(std::string_view user);

        struct Backoff
        {
            /// How long from now
            Duration duration{0};
            /// The failures on record for this subject when it started
            uint32_t failures = 0;
        };
        /// A backoff this failure started (nullopt: none; one at most per rule)
        struct FailureOutcome
        {
            std::optional<Backoff> address;
            std::optional<Backoff> user;
        };

        /// A Hello with a wrong token. `user` is the user it named, when it named a valid one on a host with a tokens file.
        /// A loopback peer has no address entry (no per-address or shared backoff): only the user is counted.
        FailureOutcome RecordFailure(const PeerAddress &peer, std::optional<std::string_view> user);
        /// A connection that ended without a successful Hello (and was not counted as a wrong-token one): a failure for
        /// its address, naming no user, so the same backoff, one log line per backoff and suppressed count apply.
        /// Loopback is exempt (nothing is recorded, an empty outcome): see above.
        FailureOutcome RecordUnfinishedHello(const PeerAddress &peer);
        /// A Hello that succeeded: clears the address and the user.
        void RecordSuccess(const PeerAddress &peer, std::optional<std::string_view> user);

        [[nodiscard]] size_t AddressCount() const;
        [[nodiscard]] size_t UserCount() const;

    private:
        struct Entry
        {
            uint32_t failures = 0;
            TimePoint nextAllowed{};
            TimePoint lastSeen{};
            uint32_t suppressed = 0;
        };

        [[nodiscard]] TimePoint Now() const;
        /// failure number `failures` of a subject allowed `freeFailures`: zero while within the free ones, then
        /// initial, 2 x initial, ..., never past `cap`
        [[nodiscard]] Duration BackoffFor(uint32_t failures, uint32_t freeFailures, Duration cap) const;
        [[nodiscard]] static bool IsIdle(const Entry &entry, TimePoint now, Duration idleReset);
        /// The entry for a key, made (evicting if the table is full) when there is none, reset when it went idle
        Entry &Touch(std::map<std::string, Entry> &table, const std::string &key, TimePoint now);

        // Set at construction and never changed, so Policy() and Now() need no lock
        RateLimitPolicy _policy;
        Clock _clock;
        // Guards the two tables below; taken by every public method, never recursively (the private helpers assume it)
        mutable std::mutex _mutex;
        std::map<std::string, Entry> _addresses;
        std::map<std::string, Entry> _users;
    };
}
