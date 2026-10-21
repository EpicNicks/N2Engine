#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "engine/security/RateLimiter.hpp"

// n2security: socket-free admission policy shared by the editor host (N2EditorServer) and the netcode's handshake
// (N2Net::SecureTransport). Moved here unchanged from editor-server/ (only the namespace changed; editor-server/'s header of
// the same name re-exports it into N2Engine::Editor). The comments describe it in the editor host's terms: a "Hello" is the
// editor's authentication message; for the netcode it is the handshake's connect response (see docs/netcode.html).
namespace N2Engine::Security
{
    /**
     * The handshake rate limit (design: security-design Part 2, "Handshake rate"): TLS CPU is a DoS lever, so the
     * acceptor takes a token before it starts a handshake and closes the connection when there is none. One global
     * bucket and one bucket per non-loopback address. Integer arithmetic only (milli-tokens), and the time comes from an
     * injected function, so its tests never sleep. NOT wired into EditorServer yet (PR S-3 does that).
     *
     *  - a token is taken from BOTH buckets or from neither, so an address that is out of tokens does not drain the
     *    global bucket by trying;
     *  - loopback has no per-address bucket (a proxy or tunnel that connects over loopback makes every peer loopback and one bucket would
     *    throttle everybody): only the global one applies to it;
     *  - the per-address table is bounded: past `maxAddresses` the buckets that have refilled completely are dropped
     *    (they are indistinguishable from a new one), then the least recently used;
     *  - a clock that goes backwards refills nothing and re-anchors the bucket at the new time, so the bucket neither
     *    stalls until the old time returns nor refills from a negative elapsed time.
     *
     * Thread-safe: one mutex, and the clock is read before it is taken.
     */
    struct TokenBucketOptions
    {
        /// Handshakes per second across all addresses (clamped to at least 1)
        uint32_t globalPerSecond = 20;
        /// How many may arrive at once after a quiet spell (0: one second's worth)
        uint32_t globalBurst = 0;
        /// Handshakes per second per non-loopback address (clamped to at least 1)
        uint32_t perAddressPerSecond = 5;
        /// 0: one second's worth
        uint32_t perAddressBurst = 0;
        /// Bound of the per-address table (clamped to at least 1)
        size_t maxAddresses = 1024;
    };

    class TokenBucket
    {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;
        using Clock = std::function<TimePoint()>;

        struct Counters
        {
            uint64_t allowed = 0;
            uint64_t refusedGlobal = 0;
            uint64_t refusedAddress = 0;
        };

        /// An empty clock is std::chrono::steady_clock
        explicit TokenBucket(TokenBucketOptions options = {}, Clock clock = {});

        /// The options as clamped
        [[nodiscard]] const TokenBucketOptions &Options() const { return _options; }

        /// Whether this handshake may start; false: close the connection before TLS
        [[nodiscard]] bool TryTake(const PeerAddress &peer);

        [[nodiscard]] Counters TotalCounters() const;
        [[nodiscard]] size_t AddressCount() const;

    private:
        /// One bucket in milli-tokens (1000 = one handshake)
        struct Bucket
        {
            int64_t milliTokens = 0;
            TimePoint last{};
            bool anchored = false;
        };

        [[nodiscard]] TimePoint Now() const;
        static void Refill(Bucket &bucket, TimePoint now, int64_t capacityMilli, int64_t perSecond);
        void MakeRoomLocked(TimePoint now);

        TokenBucketOptions _options;
        int64_t _globalCapacityMilli = 0;
        int64_t _addressCapacityMilli = 0;
        Clock _clock;
        mutable std::mutex _mutex;
        Bucket _global;
        std::map<std::string, Bucket> _addresses;
        Counters _counters;
    };
}
