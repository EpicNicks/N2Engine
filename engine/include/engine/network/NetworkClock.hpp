#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "engine/network/NetworkProtocol.hpp"

namespace N2Engine::Network
{
    // Clock synchronisation (netcode stage B3). A joined client pings the server on the UNRELIABLE channel, one message per
    // datagram; the server answers with its own tick. Little-endian, like every message (NetworkProtocol.hpp):
    //
    //   client to server
    //     0x85 ClockPing  u32 sequence (not 0)
    //   server to client
    //     0x12 ClockPong  u32 sequence (the ping's), u32 server tick (the fixed tick the server was on, modulo 2^32),
    //                     u32 microseconds since that tick began (at most 1000000 / tick rate), u16 tick rate (1 to 1000)
    //
    // The ping carries no time: the client keeps when it sent each ping and trusts nothing of its own clock to the
    // network. The pong names the ping it answers, so a pong for a sequence the client has no record of (a duplicate, a
    // very late one, or forged) is ignored. A server that is not given its tick (NetworkServer::SetCurrentTick) answers
    // with tick 0, which carries no information; the client's estimate is then of a clock that does not run.

    // The server also tells a client how far its stream of state has got when it has nothing to send. The server sends an
    // object only when it changed, so a client that hears nothing cannot tell an object that stopped from a snapshot that is
    // late; this message, sent in every state tick that has no StateUpdate for the client, settles it:
    //
    //   server to client
    //     0x13 StreamTick  u32 server tick (modulo 2^32): everything up to this tick that the server meant to send has been
    //                      sent (a StateUpdate carries the same promise in its own tick)

    /// The type bytes of the unreliable clock messages
    inline constexpr std::uint8_t kClockPingType = 0x85;
    inline constexpr std::uint8_t kClockPongType = 0x12;
    inline constexpr std::uint8_t kStreamTickType = 0x13;
    inline constexpr std::size_t kClockPingBytes = 5;
    inline constexpr std::size_t kClockPongBytes = 15;
    inline constexpr std::size_t kStreamTickBytes = 5;
    /// The highest tick rate a pong may name
    inline constexpr std::uint16_t kMaxClockTickRate = 1000;

    struct ClockPingMessage
    {
        std::uint32_t sequence = 0;
    };

    struct ClockPongMessage
    {
        std::uint32_t sequence = 0;
        std::uint32_t tick = 0;
        std::uint32_t tickMicros = 0;
        std::uint16_t tickRate = 0;
    };

    struct StreamTickMessage
    {
        std::uint32_t tick = 0;
    };

    [[nodiscard]] std::vector<std::uint8_t> Encode(const ClockPingMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const ClockPongMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const StreamTickMessage &message);
    /// The type byte and exactly 5 bytes (any tick is valid)
    [[nodiscard]] std::expected<StreamTickMessage, DecodeError> DecodeStreamTick(std::span<const std::uint8_t> bytes);

    /// The type byte, exactly 5 bytes, a sequence that is not 0
    [[nodiscard]] std::expected<ClockPingMessage, DecodeError> DecodeClockPing(std::span<const std::uint8_t> bytes);
    /// The type byte, exactly 15 bytes, a sequence that is not 0, a tick rate of 1 to kMaxClockTickRate and a tick
    /// fraction that fits in one tick (BadValue otherwise)
    [[nodiscard]] std::expected<ClockPongMessage, DecodeError> DecodeClockPong(std::span<const std::uint8_t> bytes);

    struct ClockSyncOptions
    {
        /// Samples kept (the newest); the estimate is taken from them. Clamped to 3 to 64.
        std::size_t sampleCount = 16;
        /// A sample whose round trip is longer than this is refused
        std::uint64_t maxRttMicros = 5'000'000;
        /// An estimate that is further than this from the running one replaces it at once (a forward jump; time still
        /// never runs backwards, it stands still until the new estimate catches up). Nearer ones are slewed.
        std::uint64_t snapThresholdMicros = 250'000;
        /// The most the running estimate moves toward a new one, as a fraction of the time that passed: 0.05 is a clock
        /// that runs at most 5 % fast or slow while it catches up. Clamped to 0.001 to 0.5.
        double maxSlewRate = 0.05;

        [[nodiscard]] ClockSyncOptions Clamped() const noexcept;
    };

    enum class ClockSample : std::uint8_t
    {
        Accepted,
        /// The pong came before the ping was sent
        BadTimes,
        /// A round trip over ClockSyncOptions::maxRttMicros
        RttTooLarge,
        /// A tick rate of 0 or over kMaxClockTickRate, or a tick fraction longer than one tick
        BadTick,
        /// The server's tick rate changed from the first sample's
        RateChanged,
    };

    [[nodiscard]] std::string_view ToString(ClockSample result) noexcept;

    struct ClockSyncStats
    {
        std::uint64_t samplesAccepted = 0;
        std::uint64_t samplesRejected = 0;
        /// Times the running estimate jumped to a new one (see ClockSyncOptions::snapThresholdMicros)
        std::uint64_t snaps = 0;
    };

    /**
     * The client's estimate of the server's clock. No I/O and no clock of its own: the caller hands it each ping/pong
     * (when the ping was sent and the pong arrived on the CLIENT's monotonic clock, and what the pong said) and asks for
     * the server's time at a client time.
     *
     * A sample gives the server's time when the pong arrived as (the pong's tick + half the round trip), so the offset
     * between the two clocks is that minus the client's clock. The newest `sampleCount` samples are kept; the estimate
     * is the median offset of the half of them with the shortest round trips (best-of-N: a sample delayed by queueing
     * has a long round trip and its offset is wrong by up to half the delay), so a few spikes of jitter or loss move
     * nothing. The round trip reported is the median of all kept, the jitter their mean absolute deviation from it.
     *
     * The estimate the caller reads (ServerTick) follows the target estimate at a bounded rate (maxSlewRate), which
     * handles drift without a visible jump, and is never smaller than a value it returned before: server time does not
     * run backwards. The first estimate is taken as it is. Everything is in ticks of the server's tick rate (a double),
     * which is exact for any realistic session: a tick counter of 2^32 at 1000 Hz is 50 days, far below 2^53. The 32-bit
     * tick of a pong is unwrapped against the first sample's, so the clock runs on across the wrap of the counter.
     */
    class ClockSync
    {
    public:
        explicit ClockSync(ClockSyncOptions options = {});

        /// One ping/pong. `sendMicros` and `receiveMicros` are the client's monotonic clock when the ping was sent and
        /// the pong arrived.
        ClockSample AddSample(std::uint64_t sendMicros, std::uint64_t receiveMicros, std::uint32_t serverTick,
                              std::uint32_t tickMicros, std::uint16_t tickRate);

        /// True once a sample was accepted
        [[nodiscard]] bool Synced() const noexcept { return _synced; }
        [[nodiscard]] std::size_t SampleCount() const noexcept { return _samples.size(); }
        /// The server's tick rate (0 before the first sample)
        [[nodiscard]] std::uint16_t TickRate() const noexcept { return _tickRate; }

        /// The server's tick count, fractional, when the client's clock reads `nowMicros`; 0 before the first sample.
        /// Never smaller than an earlier answer. Call with a client clock that does not run backwards (one that does is
        /// tolerated: the answer holds).
        [[nodiscard]] double ServerTick(std::uint64_t nowMicros);
        /// The same in microseconds of server time (tick * 1000000 / tick rate)
        [[nodiscard]] std::uint64_t ServerTimeMicros(std::uint64_t nowMicros);

        /// The round trip (median of the kept samples) and its jitter, in microseconds; 0 before the first sample
        [[nodiscard]] std::uint64_t RttMicros() const noexcept { return _rttMicros; }
        [[nodiscard]] std::uint64_t JitterMicros() const noexcept { return _jitterMicros; }

        [[nodiscard]] const ClockSyncStats &Stats() const noexcept { return _stats; }

    private:
        struct Sample
        {
            std::uint64_t rttMicros = 0;
            /// Server ticks minus client ticks (the client's microseconds scaled by the tick rate)
            double offsetTicks = 0.0;
        };

        void Recompute();

        ClockSyncOptions _options;
        std::deque<Sample> _samples;
        ClockSyncStats _stats;
        bool _synced = false;
        std::uint16_t _tickRate = 0;
        std::uint64_t _tickBase = 0;
        double _targetOffset = 0.0;
        double _offset = 0.0;
        bool _haveOffset = false;
        std::uint64_t _lastQueryMicros = 0;
        double _lastValue = 0.0;
        bool _haveValue = false;
        std::uint64_t _rttMicros = 0;
        std::uint64_t _jitterMicros = 0;
    };
}
