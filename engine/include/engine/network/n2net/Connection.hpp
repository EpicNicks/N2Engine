#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "engine/network/n2net/FunctionRef.hpp"
#include "engine/network/n2net/Packet.hpp"
#include "engine/network/n2net/RttEstimator.hpp"

namespace N2Engine::N2Net
{
    /// The most a configured message size may be, whatever the config says
    inline constexpr std::uint32_t kHardMaxMessageSize = 1u << 20;
    /// The most reliable units in flight (and buffered on the receiving side) a configured window may be
    inline constexpr std::uint16_t kHardMaxReliableWindow = 1024;

    struct ConnectionConfig
    {
        /// Identifies the protocol version and game build: a packet with any other id is dropped
        std::uint32_t protocolId = 0;
        /// This connection's id, the same on both sides (a later handshake will agree it): a packet with any other id
        /// is dropped
        std::uint32_t connectionId = 0;
        /// True over a stream transport (ITransport::IsReliableStream): no resends and no windows, units must arrive
        /// exactly in order
        bool reliableStream = false;

        /// The largest reliable message, sent or received. Clamped to [1, kHardMaxMessageSize].
        std::uint32_t maxMessageSize = 64 * 1024;
        /// The most reliable bytes queued and not yet packetised; SendReliable returns QueueFull beyond it
        std::uint32_t maxSendQueueBytes = 256 * 1024;
        /// The most units in flight (sender) and the units ahead of the next one the receiver will buffer. Clamped to
        /// [1, kHardMaxReliableWindow].
        std::uint16_t reliableWindow = 256;
        /// Unreliable messages waiting for the next Update: beyond either limit the oldest is dropped
        std::uint16_t maxQueuedUnreliableMessages = 64;
        std::uint32_t maxQueuedUnreliableBytes = 64 * 1024;
        /// The most packets one Update call produces
        std::uint32_t maxPacketsPerUpdate = 64;

        std::uint64_t initialRtoMicros = 200'000;
        std::uint64_t minRtoMicros = 50'000;
        std::uint64_t maxRtoMicros = 2'000'000;
        /// An empty packet is sent when nothing else was sent for this long
        std::uint64_t keepAliveMicros = 1'000'000;
        /// Nothing received for this long fails the connection (Timeout)
        std::uint64_t idleTimeoutMicros = 10'000'000;
        /// Reliable data partly received (a message with units missing, or units waiting behind a gap) and no progress
        /// for this long fails the connection (ReassemblyStall)
        std::uint64_t reassemblyTimeoutMicros = 5'000'000;

        /// The first packet sequence number sent (any value; wraparound tests start near 65535)
        std::uint16_t initialSequence = 0;
        /// The first reliable unit id. Must be the same on both sides.
        std::uint16_t initialUnitId = 0;
    };

    enum class MessageChannel : std::uint8_t
    {
        ReliableOrdered,
        UnreliableSequenced,
    };

    enum class SendResult : std::uint8_t
    {
        Ok,
        /// Zero bytes
        Empty,
        /// Over maxMessageSize (reliable) or kMaxUnreliablePayload (unreliable)
        TooLarge,
        /// The reliable send queue is full: try again after more has been sent
        QueueFull,
        ConnectionFailed,
    };

    /// What OnPacket did with one datagram. Only Ok means the datagram was processed.
    enum class PacketStatus : std::uint8_t
    {
        Ok,
        /// More than kMaxPacketSize bytes
        TooLarge,
        /// Shorter than the header, or a reserved header bit set
        BadHeader,
        WrongProtocol,
        WrongConnection,
        /// A block is invalid or bytes are left over (see Packet.hpp): nothing was applied
        Malformed,
        /// A packet number seen before
        DuplicatePacket,
        /// A packet number more than 255 behind the newest
        TooOld,
        ConnectionFailed,
    };

    enum class FailureReason : std::uint8_t
    {
        None,
        /// Nothing received for idleTimeoutMicros
        Timeout,
        /// Partly received reliable data made no progress for reassemblyTimeoutMicros
        ReassemblyStall,
        /// A received message grew past maxMessageSize
        MessageTooLarge,
        /// The peer sent reliable units that cannot be a legal stream (a continuation with no start, a start inside a
        /// message, or, over a stream transport, a unit out of order)
        ProtocolViolation,
    };

    struct ConnectionStats
    {
        std::uint64_t packetsSent = 0;
        std::uint64_t bytesSent = 0;
        std::uint64_t packetsReceived = 0;
        std::uint64_t bytesReceived = 0;
        /// Dropped by OnPacket for any reason other than Ok
        std::uint64_t packetsRejected = 0;
        std::uint64_t packetsMalformed = 0;
        std::uint64_t packetsDuplicate = 0;
        /// Our packets the peer acknowledged
        std::uint64_t packetsAcked = 0;
        /// Of those, the ones first declared lost and then acknowledged late (a wrong guess: reordering or a slow ack)
        std::uint64_t packetsAckedLate = 0;
        /// Our packets declared lost and not acknowledged since (no ack by the deadline, 3 or more newer packets acked
        /// first, or too old to ack). Counts only packets that carried blocks; the loss figure is
        /// packetsLost / (packetsLost + packetsAcked).
        std::uint64_t packetsLost = 0;
        std::uint64_t unitsSent = 0;
        std::uint64_t unitsResent = 0;
        std::uint64_t reliableMessagesQueued = 0;
        std::uint64_t reliableMessagesDelivered = 0;
        std::uint64_t unreliableMessagesDelivered = 0;
        /// Unreliable messages that arrived older than one already delivered (or equal)
        std::uint64_t unreliableStale = 0;
        /// Unreliable messages dropped from the send queue (full)
        std::uint64_t unreliableDropped = 0;
        /// Units received that were already delivered or already buffered
        std::uint64_t unitsDuplicate = 0;
        /// Units received outside the receive window
        std::uint64_t unitsOutOfWindow = 0;
    };

    /// One end of a connection: the packet layer over a transport. It owns the sequence numbers and acks, the
    /// reliable-ordered channel (units, resends, in-order delivery, fragmentation), the unreliable-sequenced channel
    /// and the RTT and loss statistics. It does no I/O and reads no clock: the caller passes datagrams in
    /// (OnPacket), asks for datagrams to send (Update) and supplies the time (any monotonic microsecond clock; the
    /// tests use SimNetwork's virtual one). Single-threaded, never throws (an allocation failure ends the process,
    /// as everywhere in the engine), no global state.
    ///
    /// Reliable messages are cut into units of at most kMaxUnitPayload bytes; the units form one numbered stream that
    /// is resent until acknowledged and delivered in order, so a message is delivered whole, exactly once, in the order
    /// it was sent. There is no congestion control: the unit window bounds what is in flight, nothing else slows a
    /// sender down.
    class Connection
    {
    public:
        using MessageHandler = FunctionRef<void(MessageChannel, std::span<const std::uint8_t>)>;
        using PacketSender = FunctionRef<void(std::span<const std::uint8_t>)>;

        /// `nowMicros` starts the idle and keep-alive clocks
        Connection(const ConnectionConfig &config, std::uint64_t nowMicros);

        /// Queues one reliable message (copied). It is sent by later Update calls.
        SendResult SendReliable(std::span<const std::uint8_t> message);
        /// Queues one unreliable message (copied); if it does not get out, or arrives behind a newer one, it is
        /// dropped, never resent
        SendResult SendUnreliable(std::span<const std::uint8_t> message);

        /// Processes one received datagram. Everything untrusted in it is checked first; a datagram with anything
        /// wrong is dropped whole. Delivers any messages it completes to `handler`, which may call SendReliable and
        /// SendUnreliable but not OnPacket or Update.
        PacketStatus OnPacket(std::uint64_t nowMicros, std::span<const std::uint8_t> datagram, MessageHandler handler);

        /// Resends what is due, sends what the window allows, acknowledges, keeps alive; calls `sender` once per packet
        /// (the span is valid only during the call). Returns the number of packets produced.
        std::uint32_t Update(std::uint64_t nowMicros, PacketSender sender);

        [[nodiscard]] bool IsFailed() const noexcept { return failure_ != FailureReason::None; }
        [[nodiscard]] FailureReason Failure() const noexcept { return failure_; }
        [[nodiscard]] const ConnectionStats &Stats() const noexcept { return stats_; }
        [[nodiscard]] const RttEstimator &Rtt() const noexcept { return rtt_; }
        [[nodiscard]] const ConnectionConfig &Config() const noexcept { return config_; }

        /// Our packets declared lost as a share of those settled (acked or lost), in parts per million
        [[nodiscard]] std::uint32_t LossRatePpm() const noexcept;

        /// Reliable messages queued or in flight that are not yet acknowledged, in bytes still to be sent or resent
        [[nodiscard]] std::size_t ReliableBacklogBytes() const noexcept;
        /// True if every reliable message queued so far was acknowledged
        [[nodiscard]] bool ReliableIdle() const noexcept;

        /// The bytes of payload buffers this connection holds now (queues, unit tables, reassembly)
        [[nodiscard]] std::size_t MemoryInUseBytes() const noexcept;
        /// What MemoryInUseBytes can never exceed under this config, whatever the peer sends
        [[nodiscard]] std::size_t MemoryBoundBytes() const noexcept;

    private:
        static constexpr std::size_t kSentRingSize = 128;
        static constexpr std::size_t kReceivedRingSize = 256;
        static constexpr std::size_t kMaxUnitsPerPacket = 64;
        static constexpr std::uint64_t kBackoffShiftLimit = 4;
        /// The ack bits reach back 32 packets: no more than that many ack-eliciting packets are left unanswered
        static constexpr std::uint16_t kMaxPacketsInFlight = 32;

        enum class SentState : std::uint8_t
        {
            Empty,
            Pending,
            Acked,
            Lost,
        };

        struct SentPacket
        {
            SentState state = SentState::Empty;
            std::uint16_t sequence = 0;
            std::uint64_t sentAt = 0;
            std::uint64_t deadline = 0;
            std::uint8_t unitCount = 0;
            std::array<std::uint16_t, kMaxUnitsPerPacket> units{};
        };

        struct SendUnit
        {
            bool inFlight = false;
            bool acked = false;
            bool needsResend = false;
            std::uint8_t flags = 0;
            std::uint8_t retries = 0;
            std::uint16_t id = 0;
            std::vector<std::uint8_t> data;
        };

        struct ReceiveSlot
        {
            bool present = false;
            std::uint8_t flags = 0;
            std::vector<std::uint8_t> data;
        };

        void Fail(FailureReason reason) noexcept;
        bool BuildPacket(std::uint64_t now, std::span<std::uint8_t> buffer, std::size_t &packetSize);
        void ProcessAck(std::uint64_t now, std::uint16_t ack, std::uint32_t ackBits);
        void MarkLost(SentPacket &packet);
        void AckUnits(const SentPacket &packet);
        void OnUnit(std::uint64_t now, const PacketBlock &block, MessageHandler handler, bool &progress);
        void DeliverReady(std::uint64_t now, MessageHandler handler, bool &progress);
        bool ConsumeUnit(std::uint8_t flags, std::span<const std::uint8_t> data, std::uint64_t now,
                         MessageHandler handler);
        void OnUnreliable(const PacketBlock &block, MessageHandler handler);
        bool HeadUnitFits(std::size_t room, std::size_t &unitLength) const noexcept;
        [[nodiscard]] std::uint64_t DeadlineFor(std::uint64_t now, std::uint8_t retries) const noexcept;
        [[nodiscard]] std::uint16_t Window() const noexcept { return config_.reliableWindow; }

        ConnectionConfig config_;
        FailureReason failure_ = FailureReason::None;
        ConnectionStats stats_;
        RttEstimator rtt_;

        std::uint64_t lastReceiveAt_;
        std::uint64_t lastSendAt_;
        bool ackPending_ = false;

        // Receive side packet numbers
        bool haveRemote_ = false;
        std::uint16_t remoteSequence_ = 0;
        std::array<std::uint32_t, kReceivedRingSize> receivedRing_{};

        // Send side packet numbers
        std::uint16_t nextSequence_;
        std::array<SentPacket, kSentRingSize> sent_{};

        // Reliable send
        std::deque<std::vector<std::uint8_t>> sendQueue_;
        std::size_t sendQueueBytes_ = 0;
        std::size_t headOffset_ = 0;
        std::vector<SendUnit> sendUnits_;
        std::uint16_t sendOldest_;
        std::uint16_t sendNext_;

        // Reliable receive
        std::vector<ReceiveSlot> receiveSlots_;
        std::uint16_t receiveNext_;
        std::size_t receiveBufferedUnits_ = 0;
        std::vector<std::uint8_t> assembly_;
        bool assembling_ = false;
        bool stallActive_ = false;
        std::uint64_t stallSince_ = 0;

        // Unreliable
        std::deque<std::vector<std::uint8_t>> unreliableQueue_;
        std::size_t unreliableQueueBytes_ = 0;
        std::uint16_t nextUnreliableSequence_ = 0;
        bool haveUnreliable_ = false;
        std::uint16_t lastUnreliableSequence_ = 0;
    };
} // namespace N2Engine::N2Net
