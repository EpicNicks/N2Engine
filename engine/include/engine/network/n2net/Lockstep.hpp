#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>


namespace N2Engine::N2Net
{
    /**
     * The lockstep input scheduler (netcode C3, design section 4.1). Pure logic: no transport, no clock, no allocation
     * after construction, and nothing a remote peer sends can make it allocate or index outside its fixed buffers.
     *
     * Every player slot produces one input per tick. The input sampled while the simulation is at tick T is for tick
     * T + inputDelay (default 2), so it has time to reach the others before they need it. A tick is runnable only when
     * every slot's input for it is known; until then the simulation waits. The first inputDelay ticks have no sender and
     * are pre-filled with neutral input for every slot, which is what lets a session start.
     *
     * Ticks are 32-bit and wrap: every comparison is a signed 32-bit difference, so the scheduler keeps working across
     * the 2^32 boundary. Inputs are accepted only for ticks in [CurrentTick, CurrentTick + kLockstepWindow): that bound
     * is the buffer, so a peer that sends a tick far in the future or the past is refused, not stored.
     *
     * Loss is handled by redundancy, not by a reliable channel: every datagram carries all frames the receiver has not
     * acknowledged (see InputBundle), and the acknowledgement is the sender's own CurrentTick (it can only have advanced
     * past T if it knows every slot's input for T). A duplicate with the same input is ignored; a second, different
     * input for the same slot and tick is a Conflict (a bug or a cheat) and the first one is kept.
     *
     * Missing inputs are never invented silently: the sim would diverge from a peer that did receive them. After
     * stallTimeoutPolls consecutive calls of TryAdvance without progress the status is TimedOut and MissingSlots says who
     * is late; the host then keeps waiting, or ends the session, or drops the slot with DropSlot. Dropping is a session
     * decision that every peer must apply at the same tick (the relay broadcasts "slot S dropped from tick T"): the
     * scheduler only provides the deterministic mechanism (from that tick on the slot's input is neutral, or the last
     * one it ran with, per DropFill).
     */
    inline constexpr int kMaxLockstepSlots = 8;
    /// Ticks of input buffered (and, after they ran, kept for resending): a power of two
    inline constexpr std::uint32_t kLockstepWindow = 128;
    inline constexpr std::uint32_t kMaxInputDelay = 16;
    /// Most frames in one InputBundle
    inline constexpr std::size_t kMaxBundleFrames = 32;

    /// The largest input of one slot for one tick: the largest networked InputFrame payload (Network::kMaxInputPayloadBytes,
    /// 32). n2net has no engine dependency, so the number is repeated here; LockstepInputMapping.cpp static_asserts it.
    inline constexpr std::size_t kMaxLockstepInputBytes = 32;

    /**
     * One slot's input for one tick: an opaque, fixed-capacity byte string, the packed payload of a networked InputFrame
     * (PR #232) under the session's input layout (engine/network/InputLayout.hpp; the mapping is in
     * engine/network/LockstepInputMapping.hpp). The scheduler never looks inside it: it only checks that its length is
     * the layout's payload size, stores it and hands it back, so two peers that submit the same bytes see the same
     * bytes. Unused bytes are always zero, so the defaulted comparison is a comparison of the payloads. A
     * default-constructed input is EMPTY (size 0), which is not the neutral input of a layout with controls: that is
     * the layout's NeutralPayload() (a signed axis at rest is its middle step, not zero bits), which the scheduler keeps
     * and uses for the ticks before the first possible input and for a dropped slot.
     */
    struct LockstepInput
    {
        std::array<std::uint8_t, kMaxLockstepInputBytes> bytes{};
        std::uint8_t size = 0;

        /// The payload, or nullopt when it is longer than kMaxLockstepInputBytes
        [[nodiscard]] static std::optional<LockstepInput> FromBytes(std::span<const std::uint8_t> payload) noexcept;

        [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept { return {bytes.data(), size}; }

        friend bool operator==(const LockstepInput &, const LockstepInput &) = default;
    };

    /// What the scheduler and the bundle codec need to know about the input layout every peer agreed on: how long a
    /// payload is, the layout's hash (InputLayout::Hash(), carried in every bundle so a peer with another layout is
    /// refused) and the neutral input. Build it from an InputLayout with MakeLockstepLayoutInfo (LockstepInputMapping.hpp).
    /// The default is a layout with no controls: zero-byte inputs.
    struct LockstepLayoutInfo
    {
        std::uint8_t payloadBytes = 0;
        std::uint64_t hash = 0;
        LockstepInput neutral;
    };

    enum class DropFill : std::uint8_t
    {
        Neutral,
        HoldLast,
    };

    struct LockstepConfig
    {
        std::uint32_t slotCount = 2;
        std::uint32_t inputDelay = 2;
        /// Consecutive TryAdvance calls that found the tick not runnable before TimedOut is reported
        std::uint32_t stallTimeoutPolls = 120;
        DropFill dropFill = DropFill::Neutral;
        /// The tick the session starts at (a test starts near the wrap)
        std::uint32_t startTick = 0;
        /// The input layout every peer agreed on
        LockstepLayoutInfo layout;
    };

    enum class SubmitResult : std::uint8_t
    {
        Accepted,
        /// Same slot, tick and input again
        Duplicate,
        /// A different input for a slot and tick that already has one: refused, the first stays
        Conflict,
        /// Before CurrentTick: the tick already ran
        TooOld,
        /// At or beyond CurrentTick + kLockstepWindow
        TooFar,
        BadSlot,
        /// The slot was dropped for that tick; the input is ignored
        SlotDropped,
        /// The input's length is not the layout's payload size
        BadPayload,
    };

    enum class AdvanceStatus : std::uint8_t
    {
        /// Every slot's input is known: `out` is filled and the scheduler moved to the next tick
        Runnable,
        /// Not yet
        Waiting,
        /// Not runnable for stallTimeoutPolls calls in a row (keeps being reported until progress)
        TimedOut,
    };

    struct TickInputs
    {
        std::uint32_t tick = 0;
        std::array<LockstepInput, kMaxLockstepSlots> inputs{};
    };

    class LockstepScheduler
    {
    public:
        /// An invalid config (slotCount outside [1, kMaxLockstepSlots], inputDelay above kMaxInputDelay) leaves the
        /// scheduler !Valid(): every call then refuses.
        explicit LockstepScheduler(const LockstepConfig &config);

        [[nodiscard]] bool Valid() const noexcept { return valid_; }
        [[nodiscard]] const LockstepConfig &Config() const noexcept { return config_; }
        /// The layout's payload size in bytes, and its hash (the value an InputBundle must carry)
        [[nodiscard]] std::size_t PayloadBytes() const noexcept { return payloadBytes_; }
        [[nodiscard]] std::uint64_t LayoutHash() const noexcept { return layoutHash_; }
        /// The layout's neutral input
        [[nodiscard]] const LockstepInput &Neutral() const noexcept { return neutral_; }
        /// The next tick to run
        [[nodiscard]] std::uint32_t CurrentTick() const noexcept { return current_; }

        /// The local slot's input as sampled now, for the slot's next local tick (CurrentTick + inputDelay when called
        /// once per tick). Call once per fixed tick the host wants to produce an input; the tick it was assigned is in
        /// `assignedTick`. TooFar once the slot is kLockstepWindow ticks ahead of the simulation (a long stall).
        SubmitResult SubmitLocal(std::uint32_t slot, const LockstepInput &input, std::uint32_t &assignedTick) noexcept;

        /// An input for (slot, tick), from the local player or a bundle
        SubmitResult Submit(std::uint32_t slot, std::uint32_t tick, const LockstepInput &input) noexcept;

        /// Moves to the next tick when every slot's input for it is known
        AdvanceStatus TryAdvance(TickInputs &out) noexcept;

        /// Bit s set when slot s has no input for CurrentTick yet
        [[nodiscard]] std::uint32_t MissingSlots() const noexcept;
        [[nodiscard]] std::uint32_t StalledPolls() const noexcept { return stalledPolls_; }

        /// The first tick of `slot` whose input is not known (every earlier one is, from CurrentTick on)
        [[nodiscard]] std::uint32_t NextNeeded(std::uint32_t slot) const noexcept;

        /// The input for (slot, tick) if it is still held (a tick that ran stays until a tick a window later replaces it)
        [[nodiscard]] std::optional<LockstepInput> Get(std::uint32_t slot, std::uint32_t tick) const noexcept;

        /// From `fromTick` (not before CurrentTick) the slot's input is synthesised per DropFill. False for a bad slot,
        /// a tick in the past or a slot already dropped. Every peer must call this with the same arguments at the same
        /// point of the input stream (the relay decides).
        bool DropSlot(std::uint32_t slot, std::uint32_t fromTick) noexcept;
        [[nodiscard]] bool IsDropped(std::uint32_t slot, std::uint32_t tick) const noexcept;

    private:
        struct Entry
        {
            std::uint32_t tick = 0;
            bool valid = false;
            LockstepInput input;
        };

        struct Slot
        {
            std::array<Entry, kLockstepWindow> ring{};
            std::uint32_t nextLocalTick = 0;
            bool dropped = false;
            std::uint32_t droppedFrom = 0;
            LockstepInput lastInput;
        };

        [[nodiscard]] static bool Lookup(const Slot &slot, std::uint32_t tick, LockstepInput &out) noexcept;

        bool valid_ = false;
        LockstepConfig config_;
        std::size_t payloadBytes_ = 0;
        std::uint64_t layoutHash_ = 0;
        LockstepInput neutral_;
        std::uint32_t current_ = 0;
        std::uint32_t stalledPolls_ = 0;
        std::array<Slot, kMaxLockstepSlots> slots_{};
    };

    /**
     * The datagram that carries inputs: slot's frames [firstTick, firstTick + count), plus `ackTick`, the sender's
     * CurrentTick (it needs nothing before it), and the layout the frames are packed under: the payload size and the
     * layout hash (InputLayout::Hash). Layout: kind u8 (1), slot u8, ackTick u32, firstTick u32, count u8,
     * payloadBytes u8, layoutHash u64, then count * payloadBytes bytes. Decoding is bounded by kMaxBundleFrames, by
     * kMaxLockstepInputBytes and by the datagram's length; it fails on a short, over-long or over-counted datagram and
     * never allocates by a claimed length. A bundle packed under another layout is a LayoutMismatch, not an input.
     */
    struct InputBundle
    {
        std::uint8_t slot = 0;
        std::uint32_t ackTick = 0;
        std::uint32_t firstTick = 0;
        std::uint8_t payloadBytes = 0;
        std::uint64_t layoutHash = 0;
        std::vector<LockstepInput> frames;
    };

    enum class BundleError : std::uint8_t
    {
        /// Wrong kind, short, over-long, over-counted, payload size above the cap, or a payload the layout cannot produce
        Malformed,
        /// Well formed, but packed under a layout other than the receiver's (different hash or payload size)
        LayoutMismatch,
    };

    /// Encoded size of a bundle with `frameCount` frames of `payloadBytes` bytes
    [[nodiscard]] constexpr std::size_t InputBundleSize(std::size_t frameCount, std::size_t payloadBytes) noexcept
    {
        return 1 + 1 + 4 + 4 + 1 + 1 + 8 + frameCount * payloadBytes;
    }

    /// Writes the bundle into `out`; the number of bytes, or 0 when it does not fit, has too many frames, or a frame's
    /// size is not bundle.payloadBytes
    [[nodiscard]] std::size_t EncodeInputBundle(const InputBundle &bundle, std::span<std::uint8_t> out) noexcept;
    /// Decodes a datagram for a session that runs under `layout`: well formed (bounded counts) and packed under the same
    /// payload size and hash. It does not look inside the payloads: n2net does not know the layout's quantisation;
    /// DecodeInputBundle of LockstepInputMapping.hpp also checks that every payload is one the layout can produce.
    [[nodiscard]] std::expected<InputBundle, BundleError> DecodeInputBundle(std::span<const std::uint8_t> bytes,
                                                                            const LockstepLayoutInfo &layout);

    /// Builds the bundle for `slot` that a peer reporting `peerAck` (its CurrentTick) still needs: the frames from
    /// peerAck (clamped to what is still held) up to the first one not known, at most kMaxBundleFrames. No frames when
    /// it needs nothing.
    [[nodiscard]] InputBundle BuildBundleFor(const LockstepScheduler &scheduler, std::uint32_t slot, std::uint32_t peerAck);
}
