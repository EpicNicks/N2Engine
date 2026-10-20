#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "editor-server/RateLimiter.hpp"

namespace N2Engine::Editor
{
    /**
     * Admission to the bounded set of pre-authentication slots (design: security-design Part 2). A connection holds a
     * slot from accept until it has authenticated or been closed, so the work an unauthenticated peer can cause is
     * bounded in threads and memory. Plain logic: no sockets, and the time comes from an injected function, so its tests
     * never sleep. NOT wired into EditorServer yet (PR S-3 does that).
     *
     *  - at most `totalSlots` slots at once (1-16, default 4);
     *  - at most `perAddressSlots` (default 2) for one non-loopback address;
     *  - loopback (127.0.0.0/8, ::1) has no per-address cap and may use every slot, because a proxy or tunnel that connects
     *    over loopback makes every peer loopback. All peers of an unknown family share one address ("unknown") and so its cap;
     *  - when nothing is free TryAcquire returns an empty handle and the caller closes the connection at once, with no
     *    TLS and no log line; the refusals are counted so a periodic summary line can be written (TakeCounters);
     *  - optional, OFF by default: `reservedSlots` of the total are kept for non-loopback addresses that authenticated
     *    within `recentAuthWindow` (RecordAuthenticated), so a flood cannot lock a known user out. Everybody else,
     *    loopback included, can use only `totalSlots - reservedSlots`. Useless behind a proxy.
     *
     * Thread-safe: one mutex, no call out while it is held except the injected clock, which is read before it is taken.
     * A handle must not outlive the PreAuthSlots.
     */
    struct PreAuthSlotsOptions
    {
        /// Clamped to 1-16
        size_t totalSlots = 4;
        /// Per non-loopback address; clamped to at least 1
        size_t perAddressSlots = 2;
        /// Keep some slots for recently authenticated non-loopback addresses (decision 6: off unless asked)
        bool reserveForRecentlyAuthenticated = false;
        /// How many; at most totalSlots - 1 (at least one slot stays open to everyone)
        size_t reservedSlots = 1;
        std::chrono::milliseconds recentAuthWindow{24 * 60 * 60 * 1000};
        /// Bound of the recently-authenticated table; past it the oldest entry goes
        size_t maxRecentAddresses = 256;
    };

    class PreAuthSlots
    {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;
        using Clock = std::function<TimePoint()>;

        /// A held slot; releasing it (or destroying the handle) frees the slot. Empty when admission was refused.
        class Handle
        {
        public:
            Handle() = default;
            Handle(const Handle &) = delete;
            Handle &operator=(const Handle &) = delete;
            Handle(Handle &&other) noexcept;
            Handle &operator=(Handle &&other) noexcept;
            ~Handle();

            [[nodiscard]] explicit operator bool() const { return _owner != nullptr; }
            /// Frees the slot now; safe to call twice
            void Release();

        private:
            friend class PreAuthSlots;
            Handle(PreAuthSlots *owner, std::string key, const bool loopback, const bool usedReserve)
                : _owner(owner), _key(std::move(key)), _loopback(loopback), _usedReserve(usedReserve)
            {
            }

            PreAuthSlots *_owner = nullptr;
            std::string _key;
            bool _loopback = false;
            bool _usedReserve = false;
        };

        struct Counters
        {
            uint64_t admitted = 0;
            /// Refused because every slot open to this peer was taken
            uint64_t refusedFull = 0;
            /// Refused because this address already held its share
            uint64_t refusedPerAddress = 0;
        };

        /// An empty clock is std::chrono::steady_clock
        explicit PreAuthSlots(PreAuthSlotsOptions options = {}, Clock clock = {});
        PreAuthSlots(const PreAuthSlots &) = delete;
        PreAuthSlots &operator=(const PreAuthSlots &) = delete;

        /// The options as clamped
        [[nodiscard]] const PreAuthSlotsOptions &Options() const { return _options; }

        /// An empty handle means: close the connection at once
        [[nodiscard]] Handle TryAcquire(const PeerAddress &peer);

        /// An address authenticated (a no-op for loopback and when the reserve is off): it may use a reserved slot for
        /// `recentAuthWindow`
        void RecordAuthenticated(const PeerAddress &peer);

        /// Counts since the last call (or since the start), then reset: the periodic summary line
        [[nodiscard]] Counters TakeCounters();
        /// Counts since the start
        [[nodiscard]] Counters TotalCounters() const;

        [[nodiscard]] size_t InUse() const;
        /// Slots held by one non-loopback address (loopback has no per-address count: 0)
        [[nodiscard]] size_t InUse(const PeerAddress &peer) const;
        [[nodiscard]] size_t RecentAddressCount() const;

    private:
        [[nodiscard]] TimePoint Now() const;
        [[nodiscard]] bool IsRecentLocked(const std::string &key, TimePoint now);
        void Release(const std::string &key, bool loopback, bool usedReserve);

        PreAuthSlotsOptions _options;
        size_t _reserved = 0;
        Clock _clock;
        mutable std::mutex _mutex;
        size_t _inUse = 0;
        std::map<std::string, size_t> _perAddress;
        std::map<std::string, TimePoint> _recent;
        Counters _sinceSummary;
        Counters _total;
    };
}
