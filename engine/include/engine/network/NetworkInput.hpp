#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkProtocol.hpp"

namespace N2Engine::Network
{
    // Player input (netcode stage B4). The client samples its input once per fixed tick into an InputFrame and sends the
    // newest few unacknowledged frames in every datagram, on the UNRELIABLE channel, one message per datagram. The server
    // keeps a small buffer per client, keyed by tick, and acknowledges what it has seen. Little-endian, like every
    // message (NetworkProtocol.hpp):
    //
    //   client to server
    //     0x84 InputFrames  u8 frame count (1 to kMaxInputFramesPerMessage), then that many frames, oldest first:
    //                         u32 tick, u32 sequence (not 0), payload: exactly InputLayout::PayloadBytes() bytes
    //                       ticks and sequences must rise from one frame to the next of the same message
    //   server to client
    //     0x11 InputAck     u32 latest sequence received (not 0), u32 bits: bit i set means sequence (latest - 1 - i) was
    //                       received too
    //
    // The message names no object: the server applies a client's input to the objects THAT CLIENT OWNS (the owner it gave
    // them at spawn), whatever the client says. The length of a frame comes from the layout both ends agreed on in the
    // fingerprint, so a decoder knows every length before it reads and allocates at most kMaxInputFramesPerMessage frames of
    // at most kMaxInputPayloadBytes bytes.

    /// The type bytes of the unreliable input messages
    inline constexpr std::uint8_t kInputFramesType = 0x84;
    inline constexpr std::uint8_t kInputAckType = 0x11;
    /// The most frames in one InputFrames message
    inline constexpr std::size_t kMaxInputFramesPerMessage = 8;
    /// A frame's tick and sequence
    inline constexpr std::size_t kInputFrameHeaderBytes = 8;
    /// The largest InputFrames message there can be
    inline constexpr std::size_t kMaxInputMessageBytes =
        2 + kMaxInputFramesPerMessage * (kInputFrameHeaderBytes + kMaxInputPayloadBytes);
    /// The most unacknowledged frames a client keeps; the oldest is dropped beyond it
    inline constexpr std::size_t kMaxUnackedInputFrames = 64;

    /// Ticks the server remembers the resolved input of (a power of two)
    inline constexpr std::uint32_t kInputHistoryTicks = 64;
    /// Ticks a server's jitter buffer has slots for (a power of two, at least history + the largest future)
    inline constexpr std::uint32_t kInputWindowTicks = 256;
    /// The most a frame's tick may be ahead of the server's current tick (a ServerInputOptions limit)
    inline constexpr std::uint32_t kMaxInputFutureTicks = 64;
    inline constexpr std::uint32_t kMaxInputDelayTicks = 8;
    inline constexpr std::uint32_t kMaxInputRepeatTicks = 16;

    static_assert(kInputHistoryTicks + kMaxInputFutureTicks + kMaxInputDelayTicks <= kInputWindowTicks,
                  "every tick the buffer accepts must have its own slot");
    static_assert((kInputWindowTicks & (kInputWindowTicks - 1)) == 0 && (kInputHistoryTicks & (kInputHistoryTicks - 1)) == 0,
                  "slots are chosen by masking the tick");

    /// `a` minus `b` as a signed distance modulo 2^32: positive when `a` is later, and right across the wrap of the
    /// 32-bit counter as long as the two are less than 2^31 apart. Ticks and sequences are compared with this, never with <.
    [[nodiscard]] constexpr std::int32_t TickDifference(const std::uint32_t a, const std::uint32_t b) noexcept
    {
        return static_cast<std::int32_t>(a - b);
    }

    /// True when `a` is strictly before `b` (see TickDifference)
    [[nodiscard]] constexpr bool TickBefore(const std::uint32_t a, const std::uint32_t b) noexcept
    {
        return TickDifference(a, b) < 0;
    }

    /// One tick of one player's input: the (fixed) tick it is for, a sequence number that rises by one for each frame a
    /// client makes (it is what the acknowledgements name), and the layout's packed payload
    struct InputFrame
    {
        std::uint32_t tick = 0;
        std::uint32_t sequence = 0;
        std::vector<std::uint8_t> payload;
    };

    struct InputAckMessage
    {
        std::uint32_t latest = 0;
        std::uint32_t bits = 0;
    };

    /// The InputFrames message for `frames` (1 to kMaxInputFramesPerMessage of them, each with a payload of exactly
    /// `payloadBytes`); an empty vector when they do not fit those rules
    [[nodiscard]] std::vector<std::uint8_t> EncodeInputFrames(std::span<const InputFrame> frames, std::size_t payloadBytes);

    /// Decodes an InputFrames message whose frames carry `payloadBytes` bytes each (at most kMaxInputPayloadBytes).
    /// Checks the type byte (UnknownType), the count (BadValue unless 1 to kMaxInputFramesPerMessage), the exact length
    /// (Truncated, TrailingBytes) and that sequences are not 0 and ticks and sequences rise (BadValue). It does not look
    /// into a payload: the layout does (InputLayout::IsValidPayload). Allocates only the bounded frames.
    [[nodiscard]] std::expected<std::vector<InputFrame>, DecodeError> DecodeInputFrames(std::span<const std::uint8_t> bytes,
                                                                                        std::size_t payloadBytes);

    /// 9 bytes
    [[nodiscard]] std::vector<std::uint8_t> Encode(const InputAckMessage &message);
    /// The type byte, exactly 9 bytes, a latest sequence that is not 0
    [[nodiscard]] std::expected<InputAckMessage, DecodeError> DecodeInputAck(std::span<const std::uint8_t> bytes);

    // ---- The server's side -------------------------------------------------------------------------------------------

    struct ServerInputOptions
    {
        /// The input delay target. A frame stamped for tick T is applied on tick T + delayTicks: the cost is that many
        /// ticks more latency between a button press and its effect, the gain is that a frame still counts when it arrives
        /// up to that many ticks late. Clamped to 0 to kMaxInputDelayTicks.
        std::uint32_t delayTicks = 2;
        /// A frame whose apply tick is more than this many ticks ahead of the server's current tick is refused (not
        /// acknowledged: the client sends it again). Bounds the buffer in time. Clamped to 1 to kMaxInputFutureTicks.
        std::uint32_t maxFutureTicks = 32;
        /// When a tick has no frame: the input of the last frame is applied again for this many ticks in a row, then
        /// neutral input until a frame arrives. Clamped to 0 to kMaxInputRepeatTicks.
        std::uint32_t repeatLastTicks = 3;
        /// More input messages than this from one client in one second is a violation and the client is dropped. A client
        /// sends one per fixed tick at most (the network setting tickRate is at most 120), so this is a ceiling.
        std::uint32_t maxMessagesPerSecond = 512;

        [[nodiscard]] ServerInputOptions Clamped() const noexcept;
    };

    /// What one client's input buffer did. All data: nothing here throws.
    struct ServerInputStats
    {
        std::uint64_t framesReceived = 0;
        /// Put in the buffer
        std::uint64_t framesAccepted = 0;
        /// Seen before (a redundant copy, which is normal, or a second frame for a tick already held)
        std::uint64_t framesDuplicate = 0;
        /// Arrived after the tick it is for was already applied (the missing-input policy ran for it instead)
        std::uint64_t framesLate = 0;
        /// More than maxFutureTicks ahead of the server
        std::uint64_t framesTooFar = 0;
        /// A payload the layout does not produce, or sequence 0 (the server drops the client)
        std::uint64_t framesInvalid = 0;
        /// Ticks applied from a frame, by repeating the last input, with neutral input, and ticks nobody asked for
        /// (skipped when the game stopped reading)
        std::uint64_t ticksFromFrames = 0;
        std::uint64_t ticksRepeated = 0;
        std::uint64_t ticksNeutral = 0;
        std::uint64_t ticksSkipped = 0;

        void Add(const ServerInputStats &other) noexcept;
    };

    enum class InputReceive : std::uint8_t
    {
        Accepted,
        Duplicate,
        Late,
        TooFar,
        /// Not a payload the layout produces: the sender is broken or hostile
        Invalid
    };

    /// Where the input of a tick came from
    enum class InputSource : std::uint8_t
    {
        /// A frame stamped for the tick arrived in time
        Frame,
        /// None did: the last frame's input again (at most repeatLastTicks in a row)
        Repeated,
        /// None did and the repeats are used up (or there never was a frame): no input
        Neutral
    };

    [[nodiscard]] std::string_view ToString(InputSource source) noexcept;

    /// The input of one tick, as the server's game reads it
    struct ResolvedInput
    {
        std::uint32_t tick = 0;
        InputSource source = InputSource::Neutral;
        std::vector<std::uint8_t> payload;
    };

    using InputPayload = std::array<std::uint8_t, kMaxInputPayloadBytes>;

    /**
     * One client's jitter buffer on the server. No I/O and no clock: NetworkServer tells it the current tick (SetNow),
     * hands it the frames of each message (Receive) and asks for the input of a tick (Resolve).
     *
     * Memory is fixed: kInputWindowTicks slots and kInputHistoryTicks remembered ticks of at most kMaxInputPayloadBytes
     * each, whatever a client sends. A frame is kept only when its apply tick (tick + delayTicks) is not before the first
     * unresolved tick and not more than maxFutureTicks ahead of the current tick; one slot per tick, the first frame for a
     * tick wins. Everything is compared modulo 2^32 (TickDifference), so a counter that wraps changes nothing.
     *
     * Resolve(tick) hands out ticks in order: asking for tick T first resolves every unresolved tick before it, each
     * exactly once and in order, so the answer for a tick depends only on the frames that had arrived when it was
     * resolved. The same frames arriving in the same order give the same input to the last bit, which is what a replay
     * needs. A tick with no frame gets the missing-input policy (ServerInputOptions::repeatLastTicks).
     */
    class ServerInputBuffer
    {
    public:
        /// `layout` must outlive the buffer. `startTick` is the server's current tick when the client joined: nothing
        /// before it will ever be applied.
        ServerInputBuffer(const InputLayout &layout, const ServerInputOptions &options, std::uint32_t startTick);

        /// The server's current tick. Ticks that fell more than kInputHistoryTicks behind it without being asked for are
        /// skipped (counted as missing), so a game that never reads this client's input cannot make the buffer stall.
        void SetNow(std::uint32_t tick);

        [[nodiscard]] InputReceive Receive(const InputFrame &frame);

        /// The input for tick `tick`, resolving every tick up to it first. nullopt for a tick already forgotten (more than
        /// kInputHistoryTicks behind the newest resolved one).
        [[nodiscard]] std::optional<ResolvedInput> Resolve(std::uint32_t tick);

        [[nodiscard]] bool AckPending() const noexcept { return _ackPending; }
        /// The ack to send now; clears the pending flag
        [[nodiscard]] InputAckMessage TakeAck() noexcept
        {
            _ackPending = false;
            return InputAckMessage{_ackLatest, _ackBits};
        }

        /// The first tick not resolved yet
        [[nodiscard]] std::uint32_t NextTick() const noexcept { return _cursor; }
        /// Frames waiting for their tick
        [[nodiscard]] std::size_t BufferedFrames() const noexcept { return _buffered; }
        [[nodiscard]] const ServerInputOptions &Options() const noexcept { return _options; }
        [[nodiscard]] const ServerInputStats &Stats() const noexcept { return _stats; }

    private:
        struct Slot
        {
            bool used = false;
            std::uint32_t tick = 0;
            InputPayload payload{};
        };

        struct HistoryEntry
        {
            bool valid = false;
            std::uint32_t tick = 0;
            InputSource source = InputSource::Neutral;
            InputPayload payload{};
        };

        [[nodiscard]] bool SequenceSeen(std::uint32_t sequence) const noexcept;
        void MarkSeen(std::uint32_t sequence) noexcept;
        void SkipTo(std::uint32_t newCursor);
        void Resync(std::uint32_t now);
        void StepOne();
        [[nodiscard]] ResolvedInput ToResolved(const HistoryEntry &entry) const;

        const InputLayout *_layout;
        ServerInputOptions _options;
        std::size_t _payloadBytes;
        std::uint32_t _now;
        std::uint32_t _cursor;
        std::array<Slot, kInputWindowTicks> _slots;
        std::array<HistoryEntry, kInputHistoryTicks> _history;
        InputPayload _last{};
        InputPayload _neutral{};
        bool _haveLast = false;
        std::uint32_t _missingStreak = 0;
        std::size_t _buffered = 0;
        std::uint32_t _ackLatest = 0;
        std::uint32_t _ackBits = 0;
        bool _ackPending = false;
        ServerInputStats _stats;
    };

    // ---- The client's side -------------------------------------------------------------------------------------------

    struct ClientInputStats
    {
        std::uint64_t framesQueued = 0;
        /// Dropped unsent or unacknowledged because kMaxUnackedInputFrames were held
        std::uint64_t framesDroppedFull = 0;
        std::uint64_t messagesBuilt = 0;
        /// Frames written into messages (a frame sent in three messages counts three times)
        std::uint64_t framesSent = 0;
        std::uint64_t framesAcked = 0;
        /// Given up on: more than 32 sequences behind the newest acknowledged, which the acknowledgement cannot name
        std::uint64_t framesAbandoned = 0;
        /// An ack that names a sequence never sent
        std::uint64_t acksInvalid = 0;
        /// No frame fit the largest message the connection takes
        std::uint64_t messagesTooLarge = 0;
    };

    /**
     * The client's side of input: the frames it made that the server has not acknowledged, and the messages that carry
     * them. No I/O: NetworkClient queues a frame each fixed tick (Queue), builds one message per update that has a new
     * frame (BuildMessage: the newest `redundancy` unacknowledged frames, so a lost datagram loses no input as long as one
     * of the next few arrives) and passes the server's acks to OnAck, which trim the ring. The ring holds at most
     * kMaxUnackedInputFrames frames, whatever the game queues.
     */
    class ClientInputSender
    {
    public:
        ClientInputSender() = default;
        /// `redundancy` is clamped to 1 to kMaxInputFramesPerMessage (the network setting authoritative.inputRedundancy)
        ClientInputSender(InputLayout layout, std::uint32_t redundancy);

        /// Packs `values` (layout.ValueCount() of them) into the frame for `tick`, which must be later than the tick of the
        /// previous one. Returns the frame's sequence.
        [[nodiscard]] std::expected<std::uint32_t, std::string> Queue(std::uint32_t tick, std::span<const float> values);

        /// True when a frame was queued since the last BuildMessage
        [[nodiscard]] bool HasPending() const noexcept { return _pending; }

        /// The message for the newest unacknowledged frames, at most `maxMessageBytes` long; nullopt when there is none
        /// or not even one frame fits. Clears the pending flag.
        [[nodiscard]] std::optional<std::vector<std::uint8_t>> BuildMessage(std::size_t maxMessageBytes);

        /// Applies an acknowledgement. False when it names a sequence this sender never sent (nothing is applied).
        bool OnAck(const InputAckMessage &ack);

        [[nodiscard]] const InputLayout &Layout() const noexcept { return _layout; }
        [[nodiscard]] std::uint32_t Redundancy() const noexcept { return _redundancy; }
        [[nodiscard]] std::size_t Unacked() const noexcept { return _ring.size(); }
        [[nodiscard]] const ClientInputStats &Stats() const noexcept { return _stats; }

    private:
        InputLayout _layout;
        std::uint32_t _redundancy = 1;
        std::deque<InputFrame> _ring;
        std::uint32_t _nextSequence = 1;
        std::uint32_t _lastTick = 0;
        bool _haveTick = false;
        std::uint32_t _highestSent = 0;
        bool _pending = false;
        ClientInputStats _stats;
    };
}
