#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/Transport.hpp"

namespace N2Engine::Network
{
    /**
     * The session messages between a lockstep client and the relay (netcode C3, design 2.1.4). They share one n2net
     * Connection per client, and the first byte says what a message is:
     *
     *   n2net kinds (n2net/Lockstep.hpp, HashTree.hpp, BisectProtocol.hpp, the same bytes the peers use between them)
     *     1  InputBundle   unreliable, both ways: the client sends its own slot's frames, the relay sends everyone else's
     *                      (and, with no frames, the relay's ack of the client's own)
     *     2  RootMessage   reliable, client to relay: the state-hash root of a checkpoint tick
     *     3  BisectRequest and 4 BisectReply: only ever inside a Forward message
     *
     *   relay to client (0x40 to 0x5F), reliable
     *     0x40 Welcome   u16 version, u8 slot, u8 slotCount, u8 inputDelay, u16 tickRate, u16 hashInterval, u8 dropFill,
     *                    u8 payloadBytes, u64 layoutHash, payloadBytes of neutral input
     *     0x41 Start     (nothing): every slot has joined, tick 0 may run
     *     0x42 DropSlot  u8 slot, u32 fromTick, u8 reason: every peer applies it, so every peer synthesises that slot's
     *                    input from that tick on
     *     0x43 Stop      u8 reason, u32 tick, u8 slotA, u8 slotB, u64 rootA, u64 rootB: the session is over
     *     0x44 Refuse    u8 reason: the join was refused
     *     0x45 From      u8 sender slot, then a bisect request or reply of that slot
     *     0x46 SnapshotRequest  u32 request id: send the relay a serialised snapshot of a confirmed tick (version 2)
     *     0x47 SnapshotPart     the body of a SnapshotData, to the spectator it was asked for (version 2)
     *   client to relay (0x60 to 0x7F), reliable
     *     0x60 Join      u16 version, u8 payloadBytes, u64 layoutHash
     *     0x61 To        u8 target slot, then a bisect request or reply for that slot
     *     0x62 Pause     u8 paused (1 or 0): the page is hidden (a browser tab in the background), so a stall is not a drop yet
     *     0x63 Spectate  the body of a Join: join as a spectator, late or not (version 2). The Welcome's slot is
     *                    kSpectatorSlot.
     *     0x64 SnapshotData  u32 request id, u32 tick, u32 total bytes, u32 offset, then 1 to kMaxSnapshotPartBytes bytes of
     *                    a SerializeWorldSnapshot result, in order (version 2)
     *
     * Every decoder checks its lengths against the bytes present and its caps before it takes anything, refuses trailing
     * bytes, and allocates nothing a count asks for. The wire version of this protocol is separate from the world
     * protocol's kWireVersion: a lockstep session does not speak the world protocol at all. Version 2 (netcode C5/C6)
     * added the spectator and snapshot messages and the NoSnapshot refusal; a version 1 client is refused at its Join.
     */
    inline constexpr std::uint16_t kLockstepWireVersion = 2;
    /// The slot of a spectator's Welcome: it owns no slot and sends no input
    inline constexpr std::uint8_t kSpectatorSlot = 0xFF;
    /// The most snapshot bytes one SnapshotData or SnapshotPart carries (the message less its 17-byte head, with room)
    inline constexpr std::size_t kMaxSnapshotPartBytes = 3584;
    /// The largest session message: a bisect reply (kMaxBisectReplySize, 2569) plus the envelope, with room
    inline constexpr std::size_t kLockstepMaxMessageBytes = 4096;
    /// Slots the relay seats: the replay format's cap (a session is recorded as an n2replay file)
    inline constexpr std::uint32_t kMaxRelaySlots = 4;

    /// The connection id of every lockstep session Connection ("N2NL")
    inline constexpr std::uint32_t kLockstepConnectionId = 0x4E32'4E4C;

    enum class RelayMessage : std::uint8_t
    {
        Welcome = 0x40,
        Start = 0x41,
        DropSlot = 0x42,
        Stop = 0x43,
        Refuse = 0x44,
        From = 0x45,
        SnapshotRequest = 0x46,
        SnapshotPart = 0x47,
        Join = 0x60,
        To = 0x61,
        Pause = 0x62,
        Spectate = 0x63,
        SnapshotData = 0x64,
    };

    enum class RefuseReason : std::uint8_t
    {
        None = 0,
        WrongVersion = 1,
        /// The client's input layout is not the session's
        LayoutMismatch = 2,
        SessionFull = 3,
        /// The session already started (or is ending)
        SessionRunning = 4,
        /// A client sent a Join twice, or a malformed one
        Protocol = 5,
        /// A spectator could not be given a snapshot (no player sent one in time, the source left, it was too large)
        NoSnapshot = 6,
    };

    enum class DropReason : std::uint8_t
    {
        /// Nothing arrived from the slot within the stall timeout
        Stalled = 1,
        /// The client left or its connection failed
        Left = 2,
        /// The client broke the protocol (or sent a conflicting input) and was dropped
        Violation = 3,
    };

    enum class StopReason : std::uint8_t
    {
        /// Two peers' state-hash roots differ: slotA and slotB are the pair, rootA and rootB their roots
        Desync = 1,
        /// A slot stalled and the policy is to stop (slotA)
        Stalled = 2,
        /// A client left and the policy is to stop (slotA)
        PeerLeft = 3,
        /// The relay is shutting down
        Shutdown = 4,
        /// Every slot is gone
        Empty = 5,
    };

    [[nodiscard]] std::string_view ToString(RefuseReason reason) noexcept;
    [[nodiscard]] std::string_view ToString(DropReason reason) noexcept;
    [[nodiscard]] std::string_view ToString(StopReason reason) noexcept;

    struct JoinMessage
    {
        std::uint16_t version = kLockstepWireVersion;
        std::uint8_t payloadBytes = 0;
        std::uint64_t layoutHash = 0;
    };

    struct WelcomeMessage
    {
        std::uint16_t version = kLockstepWireVersion;
        std::uint8_t slot = 0;
        std::uint8_t slotCount = 0;
        std::uint8_t inputDelay = 0;
        std::uint16_t tickRate = 60;
        std::uint16_t hashInterval = 30;
        /// 0 Neutral, 1 HoldLast
        std::uint8_t dropFill = 0;
        std::uint8_t payloadBytes = 0;
        std::uint64_t layoutHash = 0;
        std::vector<std::uint8_t> neutral;
    };

    struct DropSlotMessage
    {
        std::uint8_t slot = 0;
        std::uint32_t fromTick = 0;
        DropReason reason = DropReason::Stalled;
    };

    struct StopMessage
    {
        StopReason reason = StopReason::Desync;
        std::uint32_t tick = 0;
        std::uint8_t slotA = 0;
        std::uint8_t slotB = 0;
        std::uint64_t rootA = 0;
        std::uint64_t rootB = 0;
    };

    /// A piece of a serialised snapshot (SnapshotData from a player, SnapshotPart to a spectator)
    struct SnapshotPartMessage
    {
        std::uint32_t requestId = 0;
        /// The snapshot's tick: the state before it (every earlier tick ran on confirmed input)
        std::uint32_t tick = 0;
        std::uint32_t totalBytes = 0;
        std::uint32_t offset = 0;
        std::span<const std::uint8_t> data;
    };

    /// A forwarded bisect message and the slot at its other end (the target in To, the sender in From)
    struct ForwardedMessage
    {
        std::uint8_t slot = 0;
        std::span<const std::uint8_t> inner;
    };

    [[nodiscard]] std::vector<std::uint8_t> Encode(const JoinMessage &message);
    /// A Spectate: the bytes of a Join under type 0x63
    [[nodiscard]] std::vector<std::uint8_t> EncodeSpectate(const JoinMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> EncodeSnapshotRequest(std::uint32_t requestId);
    /// `type` is SnapshotData or SnapshotPart. Empty when the data is empty, over kMaxSnapshotPartBytes, or reaches past
    /// totalBytes.
    [[nodiscard]] std::vector<std::uint8_t> Encode(RelayMessage type, const SnapshotPartMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const WelcomeMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const DropSlotMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const StopMessage &message);
    [[nodiscard]] std::vector<std::uint8_t> EncodeStart();
    [[nodiscard]] std::vector<std::uint8_t> EncodeRefuse(RefuseReason reason);
    [[nodiscard]] std::vector<std::uint8_t> EncodePause(bool paused);
    /// Type byte, slot, inner. Empty when the result would be over kLockstepMaxMessageBytes or `inner` is empty.
    [[nodiscard]] std::vector<std::uint8_t> EncodeForward(RelayMessage type, std::uint8_t slot,
                                                          std::span<const std::uint8_t> inner);

    /// Each decoder takes the whole message, type byte included, and refuses a wrong type, a short or long message and a
    /// value out of range (slot at or over kMaxRelaySlots, payload over 32 bytes, an unknown reason). A Welcome's slot
    /// may also be kSpectatorSlot.
    [[nodiscard]] std::optional<JoinMessage> DecodeJoin(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] std::optional<JoinMessage> DecodeSpectate(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] std::optional<std::uint32_t> DecodeSnapshotRequest(std::span<const std::uint8_t> bytes) noexcept;
    /// `type` is SnapshotData or SnapshotPart: 1 to kMaxSnapshotPartBytes bytes of data, offset + size within totalBytes
    [[nodiscard]] std::optional<SnapshotPartMessage> DecodeSnapshotPart(RelayMessage type,
                                                                        std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] std::optional<WelcomeMessage> DecodeWelcome(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::optional<DropSlotMessage> DecodeDropSlot(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] std::optional<StopMessage> DecodeStop(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] std::optional<RefuseReason> DecodeRefuse(std::span<const std::uint8_t> bytes) noexcept;
    /// The paused flag of a Pause message
    [[nodiscard]] std::optional<bool> DecodePause(std::span<const std::uint8_t> bytes) noexcept;
    /// To or From (`type` says which): the slot and a view of the inner message, which must be a bisect request (To and
    /// From) or reply, kind 3 or 4, of at most kMaxBisectReplySize bytes
    [[nodiscard]] std::optional<ForwardedMessage> DecodeForward(RelayMessage type, std::span<const std::uint8_t> bytes) noexcept;

    /// The Connection settings both ends of a lockstep session use over `transport` (the same on both sides where the
    /// n2net layer needs it): the largest packet the transport takes and a reliable message size of
    /// kLockstepMaxMessageBytes
    [[nodiscard]] N2Net::ConnectionConfig MakeLockstepConnectionConfig(std::uint32_t protocolId,
                                                                       const N2Net::ITransport &transport);
}
