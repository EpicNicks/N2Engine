#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/network/InputLayout.hpp"
#include "engine/network/LockstepProtocol.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/Lockstep.hpp"
#include "engine/network/n2net/Transport.hpp"

namespace N2Engine::Network
{
    /// What the relay does when a slot stalls (nothing from it for stallTimeoutMicros while the others wait) or its client
    /// leaves
    enum class StallPolicy : std::uint8_t
    {
        /// DropSlot: every peer synthesises that slot's input from the same tick on, and the session goes on
        Drop,
        /// Stop the session for everyone
        Stop,
    };

    [[nodiscard]] std::string_view ToString(StallPolicy policy) noexcept;

    struct LockstepRelayOptions
    {
        /// Identifies the game and its build in every packet (ConnectionConfig::protocolId)
        std::uint32_t protocolId = 0;
        /// Players the session waits for before it starts, 1 to kMaxRelaySlots (the replay format's cap)
        std::uint32_t slotCount = 2;
        /// The input layout every client must declare (its hash and payload size are checked at the join). Required: an
        /// empty layout is a session of zero-byte inputs, which is refused by the constructor (Valid() is false).
        InputLayout layout;
        std::uint32_t inputDelay = 2;
        std::uint32_t tickRate = 60;
        /// Ticks between the state-hash checkpoints the clients report
        std::uint32_t hashInterval = 30;
        N2Net::DropFill dropFill = N2Net::DropFill::Neutral;
        StallPolicy stallPolicy = StallPolicy::Drop;
        /// A slot the others wait for this long is stalled
        std::uint64_t stallTimeoutMicros = 5'000'000;
        /// A paused client (a hidden browser tab told the relay) is not stalled before this long
        std::uint64_t pausedGraceMicros = 30'000'000;
        /// A connected client that has not joined this long after it connected is dropped
        std::uint64_t joinTimeoutMicros = 10'000'000;
        /// After a Desync stop, how long the two peers have to bisect before the relay ends the session
        std::uint64_t desyncLingerMicros = 6'000'000;
        /// After any other stop, how long clients have to receive it
        std::uint64_t stopLingerMicros = 1'000'000;
        /// How often a client is sent the inputs it lacks
        std::uint64_t sendIntervalMicros = 4'000;
        /// More input bundles than this from one client in one second is a violation. A client sends one per
        /// sendInterval at most, so this is a ceiling, not a rate anyone needs.
        std::uint32_t maxBundlesPerSecond = 1024;
        /// More reliable messages than this from one client in one second is a violation (roots, pauses and forwards)
        std::uint32_t maxMessagesPerSecond = 256;
        /// Bisect messages forwarded in one session, in total
        std::uint32_t maxForwardMessages = 1024;
        /// Connections at once (joined or not); a connection beyond it is dropped at once
        std::uint32_t maxConnections = 16;
    };

    enum class RelayState : std::uint8_t
    {
        /// Waiting for the players
        Lobby,
        Running,
        /// A Stop was sent; the clients are finishing (a desync pair is bisecting)
        Stopping,
    };

    [[nodiscard]] std::string_view ToString(RelayState state) noexcept;

    struct LockstepRelayStats
    {
        std::uint64_t joined = 0;
        std::uint64_t refused = 0;
        std::uint64_t sessionsStarted = 0;
        std::uint64_t sessionsEnded = 0;
        std::uint64_t slotsDropped = 0;
        std::uint64_t clientsDropped = 0;
        std::uint64_t bundlesReceived = 0;
        std::uint64_t bundlesSent = 0;
        /// A bundle that did not decode, named another slot's inputs, or carried an input the layout cannot produce
        std::uint64_t bundlesRejected = 0;
        /// A second, different input for a slot and tick: the client is dropped
        std::uint64_t conflicts = 0;
        std::uint64_t rootsReceived = 0;
        /// A root for a tick the relay has not confirmed (or a dropped slot's): ignored
        std::uint64_t rootsIgnored = 0;
        std::uint64_t desyncs = 0;
        std::uint64_t forwarded = 0;
        std::uint64_t forwardsRefused = 0;
        std::uint64_t rateLimited = 0;
        std::uint64_t stalls = 0;
        std::uint64_t ticksConfirmed = 0;
    };

    /// What the relay tells its observer (a host that logs)
    struct RelayEvent
    {
        enum class Type : std::uint8_t
        {
            Joined,
            Refused,
            Started,
            SlotDropped,
            Stopped,
            /// A client was dropped: `text` is why
            ClientDropped,
            /// A session ended and the relay is waiting for the next one
            Reset,
        };

        Type type = Type::Joined;
        std::uint32_t slot = 0;
        std::uint32_t tick = 0;
        std::string text;
    };

    /**
     * The relay of the Deterministic preset (netcode C3, design 2.1.4): a star. Every client has one n2net Connection to
     * the relay and none to another client. The relay seats up to slotCount players, then starts the session; it
     * collects each slot's InputBundles into a LockstepScheduler of its own (so it knows which tick is CONFIRMED: every
     * slot's input for it has arrived), forwards every slot's frames to every other client, and acknowledges each client's
     * own frames with its CurrentTick. It does not simulate (optionally checking hashes by simulating is not done: it
     * compares the clients' reported roots instead).
     *
     * Authority. The relay decides, the clients obey:
     *  - a client may send inputs for its own slot only (a bundle naming another slot, a layout other than the session's,
     *    a payload the layout cannot produce, or a second different input for a tick it already sent drops the client);
     *  - a slot that does not deliver for stallTimeoutMicros while the others wait is stalled: with StallPolicy::Drop the
     *    relay runs DropSlot at the first tick the slot has not delivered and tells everyone (every peer applies it before
     *    it can pass that tick, because it cannot pass it without the slot's input), with StallPolicy::Stop it stops the
     *    session; a client that leaves or fails is handled the same way; a client that said it is paused (Pause) gets
     *    pausedGraceMicros first;
     *  - the clients report the state-hash root of every checkpoint tick; two different roots for one tick stop the
     *    session (StopReason::Desync) and name the pair, and only then, only between that pair, and only up to
     *    maxForwardMessages, does the relay forward bisect messages;
     *  - everything is bounded: connections, per-client message and bundle rates, forward budget, the 16 checkpoint
     *    records, the per-peer Connection memory. Nothing a client sends makes the relay allocate by a length it chose.
     *
     * Single-threaded; call Update every pass with a monotonic microsecond clock. After a session ends (every client
     * gone, or the linger over) the relay is back in the Lobby for the next one.
     */
    class LockstepRelay
    {
    public:
        LockstepRelay(N2Net::ITransport &transport, LockstepRelayOptions options);
        LockstepRelay(const LockstepRelay &) = delete;
        LockstepRelay &operator=(const LockstepRelay &) = delete;

        /// False when the options were refused (no controls in the layout, a slotCount outside range): Update then does
        /// nothing and every connection is dropped
        [[nodiscard]] bool Valid() const noexcept { return valid_; }

        void SetObserver(std::function<void(const RelayEvent &)> observer) { observer_ = std::move(observer); }

        void Update(std::uint64_t nowMicros);

        /// Stops a running session for everyone (StopReason::Shutdown); call Update a few more times to flush it
        void Shutdown();

        [[nodiscard]] RelayState State() const noexcept { return state_; }
        [[nodiscard]] const LockstepRelayStats &Stats() const noexcept { return stats_; }
        [[nodiscard]] const LockstepRelayOptions &Options() const noexcept { return options_; }
        /// The next tick the relay has not confirmed (every earlier one has every alive slot's input)
        [[nodiscard]] std::uint32_t ConfirmedTick() const noexcept;
        /// Clients that joined (seated in a slot)
        [[nodiscard]] std::size_t JoinedCount() const noexcept;
        [[nodiscard]] std::size_t ConnectionCount() const noexcept { return peers_.size(); }
        [[nodiscard]] bool IsSlotDropped(std::uint32_t slot) const noexcept;

    private:
        struct Peer
        {
            N2Net::PeerId id = N2Net::kInvalidPeer;
            std::unique_ptr<N2Net::Connection> connection;
            std::uint64_t connectedAt = 0;
            bool joined = false;
            std::uint32_t slot = 0;
            /// The client's CurrentTick as of its latest bundle: it needs nothing before it
            std::uint32_t ack = 0;
            std::uint64_t lastSendAt = 0;
            std::uint64_t lastAckSendAt = 0;
            std::uint32_t lastAckSent = 0;
            bool ackSent = false;
            bool paused = false;
            std::uint64_t pausedSince = 0;
            std::uint64_t bundleWindowStart = 0;
            std::uint32_t bundleWindowCount = 0;
            std::uint64_t messageWindowStart = 0;
            std::uint32_t messageWindowCount = 0;
            std::uint32_t refusedForwards = 0;
            bool leaving = false;
            bool slotAccounted = false;
            /// A client being dropped stays connected until then (a refusal needs a moment to be delivered)
            std::uint64_t lingerUntil = 0;
            DropReason leaveReason = DropReason::Left;
            std::string leaveText;
        };

        struct RootRecord
        {
            bool valid = false;
            std::uint32_t tick = 0;
            std::uint64_t order = 0;
            std::uint32_t mask = 0;
            std::array<std::uint64_t, kMaxRelaySlots> roots{};
        };

        void HandleEvent(const N2Net::TransportEvent &event);
        void OnConnected(N2Net::PeerId id);
        void OnDisconnected(N2Net::PeerId id);
        void OnDatagram(N2Net::PeerId id, std::span<const std::uint8_t> data);
        void OnMessage(Peer &peer, N2Net::MessageChannel channel, std::span<const std::uint8_t> message);
        void OnJoin(Peer &peer, std::span<const std::uint8_t> message);
        void OnBundle(Peer &peer, std::span<const std::uint8_t> message);
        void OnRoot(Peer &peer, std::span<const std::uint8_t> message);
        void OnForward(Peer &peer, std::span<const std::uint8_t> message);
        void OnPause(Peer &peer, std::span<const std::uint8_t> message);
        void StartSession();
        void AdvanceSession();
        void RunTimers();
        void SendBundles();
        void SendPackets();
        void ApplyLeavers();
        void SlotGone(std::uint32_t slot, DropReason reason);
        void DropSlotNow(std::uint32_t slot, DropReason reason);
        void BeginStop(StopReason reason, std::uint32_t tick, std::uint32_t slotA, std::uint32_t slotB, std::uint64_t rootA,
                       std::uint64_t rootB);
        void EndSession();
        void Mark(Peer &peer, std::string text, DropReason reason = DropReason::Violation);
        bool Send(Peer &peer, const std::vector<std::uint8_t> &message);
        void Broadcast(const std::vector<std::uint8_t> &message, std::uint32_t exceptSlot = 0xFFFFFFFFu);
        bool AllowMessage(Peer &peer);
        bool AllowBundle(Peer &peer);
        Peer *PeerOfSlot(std::uint32_t slot);
        void Notify(RelayEvent::Type type, std::uint32_t slot, std::uint32_t tick, std::string text);

        N2Net::ITransport &transport_;
        LockstepRelayOptions options_;
        bool valid_ = false;
        InputLayout layout_;
        std::uint64_t layoutHash_ = 0;
        RelayState state_ = RelayState::Lobby;
        std::unique_ptr<N2Net::LockstepScheduler> scheduler_;
        std::map<N2Net::PeerId, Peer> peers_;
        std::vector<N2Net::PeerId> refusedPeers_;
        std::array<bool, kMaxRelaySlots> slotTaken_{};
        std::array<bool, kMaxRelaySlots> slotDropped_{};
        std::array<std::uint32_t, kMaxRelaySlots> dropTick_{};
        std::array<RootRecord, 16> roots_{};
        std::uint64_t rootOrder_ = 0;
        std::uint64_t now_ = 0;
        std::uint64_t progressAt_ = 0;
        std::uint64_t stopAt_ = 0;
        StopReason stopReason_ = StopReason::Empty;
        std::uint32_t stopSlotA_ = 0;
        std::uint32_t stopSlotB_ = 0;
        std::uint32_t forwardCount_ = 0;
        LockstepRelayStats stats_;
        std::function<void(const RelayEvent &)> observer_;
    };
}
