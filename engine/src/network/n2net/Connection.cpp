#include "engine/network/n2net/Connection.hpp"

#include <algorithm>

#include "engine/network/n2net/Sequence.hpp"

namespace N2Engine::N2Net
{
    namespace
    {
        constexpr std::uint64_t kMaxTimeMicros = 1ull << 40;

        /// The largest power of two that is at most `value` (at least 1): unit ids wrap at 65536, so a window that did
        /// not divide it would map the ids on either side of the wrap to colliding slots
        std::uint16_t FloorPowerOfTwo(std::uint16_t value) noexcept
        {
            std::uint16_t result = 1;
            while (static_cast<std::uint32_t>(result) * 2 <= value)
            {
                result = static_cast<std::uint16_t>(result * 2);
            }
            return result;
        }

        ConnectionConfig Sanitize(ConnectionConfig config) noexcept
        {
            config.maxMessageSize = std::clamp<std::uint32_t>(config.maxMessageSize, 1, kHardMaxMessageSize);
            config.maxSendQueueBytes = std::max(config.maxSendQueueBytes, config.maxMessageSize);
            config.reliableWindow =
                FloorPowerOfTwo(std::clamp<std::uint16_t>(config.reliableWindow, 1, kHardMaxReliableWindow));
            config.maxQueuedUnreliableMessages = std::max<std::uint16_t>(config.maxQueuedUnreliableMessages, 1);
            config.maxQueuedUnreliableBytes =
                std::max<std::uint32_t>(config.maxQueuedUnreliableBytes, static_cast<std::uint32_t>(kMaxUnreliablePayload));
            config.maxPacketsPerUpdate = std::max<std::uint32_t>(config.maxPacketsPerUpdate, 1);
            // Times are clamped to about 12 days so that no timeout arithmetic (the RTO's backoff shift, a deadline) can wrap
            config.initialRtoMicros = std::min(config.initialRtoMicros, kMaxTimeMicros);
            config.minRtoMicros = std::clamp<std::uint64_t>(config.minRtoMicros, 1, kMaxTimeMicros);
            config.maxRtoMicros = std::clamp(config.maxRtoMicros, config.minRtoMicros, kMaxTimeMicros);
            config.keepAliveMicros = std::clamp<std::uint64_t>(config.keepAliveMicros, 1, kMaxTimeMicros);
            config.idleTimeoutMicros = std::min(config.idleTimeoutMicros, kMaxTimeMicros);
            config.reassemblyTimeoutMicros = std::min(config.reassemblyTimeoutMicros, kMaxTimeMicros);
            return config;
        }

        constexpr std::uint32_t ReceivedMark(std::uint16_t sequence) noexcept
        {
            return 0x10000u | sequence;
        }

        void ReleaseBuffer(std::vector<std::uint8_t> &buffer) noexcept
        {
            std::vector<std::uint8_t>().swap(buffer);
        }
    } // namespace

    Connection::Connection(const ConnectionConfig &config, std::uint64_t nowMicros)
        : config_(Sanitize(config)),
          rtt_(config_.initialRtoMicros, config_.minRtoMicros, config_.maxRtoMicros),
          lastReceiveAt_(nowMicros),
          lastSendAt_(nowMicros),
          nextSequence_(config_.initialSequence),
          sendOldest_(config_.initialUnitId),
          sendNext_(config_.initialUnitId),
          receiveNext_(config_.initialUnitId)
    {
        if (!config_.reliableStream)
        {
            sendUnits_.resize(config_.reliableWindow);
        }
        receiveSlots_.resize(config_.reliableWindow);
    }

    // ---- sending ----

    SendResult Connection::SendReliable(std::span<const std::uint8_t> message)
    {
        if (IsFailed())
        {
            return SendResult::ConnectionFailed;
        }
        if (message.empty())
        {
            return SendResult::Empty;
        }
        if (message.size() > config_.maxMessageSize)
        {
            return SendResult::TooLarge;
        }
        if (sendQueueBytes_ + message.size() > config_.maxSendQueueBytes)
        {
            return SendResult::QueueFull;
        }
        sendQueue_.emplace_back(message.begin(), message.end());
        sendQueueBytes_ += message.size();
        ++stats_.reliableMessagesQueued;
        return SendResult::Ok;
    }

    SendResult Connection::SendUnreliable(std::span<const std::uint8_t> message)
    {
        if (IsFailed())
        {
            return SendResult::ConnectionFailed;
        }
        if (message.empty())
        {
            return SendResult::Empty;
        }
        if (message.size() > kMaxUnreliablePayload)
        {
            return SendResult::TooLarge;
        }
        unreliableQueue_.emplace_back(message.begin(), message.end());
        unreliableQueueBytes_ += message.size();
        while (unreliableQueue_.size() > config_.maxQueuedUnreliableMessages ||
               unreliableQueueBytes_ > config_.maxQueuedUnreliableBytes)
        {
            unreliableQueueBytes_ -= unreliableQueue_.front().size();
            unreliableQueue_.pop_front();
            ++stats_.unreliableDropped;
        }
        return SendResult::Ok;
    }

    std::uint64_t Connection::DeadlineFor(std::uint64_t now, std::uint8_t retries) const noexcept
    {
        const std::uint64_t shift = std::min<std::uint64_t>(retries, kBackoffShiftLimit);
        const std::uint64_t base = rtt_.RtoMicros();
        const std::uint64_t backedOff = std::min(base << shift, std::max(base, config_.maxRtoMicros));
        return now + backedOff;
    }

    bool Connection::HeadUnitFits(std::size_t room, std::size_t &unitLength) const noexcept
    {
        const std::size_t remaining = sendQueue_.front().size() - headOffset_;
        unitLength = std::min(remaining, kMaxUnitPayload);
        return room >= kUnitHeaderSize + unitLength;
    }

    std::uint32_t Connection::Update(std::uint64_t nowMicros, PacketSender sender)
    {
        if (IsFailed())
        {
            return 0;
        }
        if (nowMicros > lastReceiveAt_ && nowMicros - lastReceiveAt_ > config_.idleTimeoutMicros)
        {
            Fail(FailureReason::Timeout);
            return 0;
        }
        if (stallActive_ && nowMicros > stallSince_ && nowMicros - stallSince_ > config_.reassemblyTimeoutMicros)
        {
            Fail(FailureReason::ReassemblyStall);
            return 0;
        }

        for (SentPacket &packet : sent_)
        {
            if (packet.state == SentState::Pending && nowMicros >= packet.deadline)
            {
                MarkLost(packet);
            }
        }

        std::array<std::uint8_t, kMaxPacketSize> buffer{};
        std::uint32_t produced = 0;
        while (produced < config_.maxPacketsPerUpdate)
        {
            std::size_t size = 0;
            if (!BuildPacket(nowMicros, buffer, size))
            {
                break;
            }
            sender(std::span<const std::uint8_t>(buffer.data(), size));
            ++produced;
        }
        return produced;
    }

    bool Connection::BuildPacket(std::uint64_t now, std::span<std::uint8_t> buffer, std::size_t &packetSize)
    {
        SentPacket &record = sent_[nextSequence_ % kSentRingSize];
        if (record.state == SentState::Pending)
        {
            // The ring slot is needed again and the packet in it is still unanswered: it is lost for good
            MarkLost(record);
        }

        ByteWriter blocks(buffer.data() + kHeaderSize, buffer.size() - kHeaderSize);
        std::size_t blockCount = 0;
        std::uint8_t unitCount = 0;
        std::array<std::uint16_t, kMaxUnitsPerPacket> units{};
        std::size_t unitsInPacket = 0;
        std::uint8_t maxRetries = 0;
        const bool stream = config_.reliableStream;

        // The ack bits cover the 32 packets before the newest ack, so a packet more than that behind it can never be
        // acknowledged. Keep every unanswered packet inside that span by sending no new data packet while the oldest
        // pending one is 32 or more packets back: at most 32 data packets per round trip. (Not needed on a stream
        // transport, which has its own flow control and no resends.)
        bool mayCarryData = true;
        if (!stream)
        {
            for (const SentPacket &pending : sent_)
            {
                if (pending.state == SentState::Pending &&
                    SequenceDistance(pending.sequence, nextSequence_) >= kMaxPacketsInFlight)
                {
                    mayCarryData = false;
                    break;
                }
            }
        }

        const auto writeUnit = [&blocks](std::uint16_t id, std::uint8_t flags, std::span<const std::uint8_t> data)
        {
            blocks.U8(kBlockReliableUnit);
            blocks.U16(id);
            blocks.U8(flags);
            blocks.U16(static_cast<std::uint16_t>(data.size()));
            blocks.Bytes(data);
        };

        // 1. units that need resending, oldest first
        if (!stream && mayCarryData)
        {
            for (std::uint16_t id = sendOldest_; id != sendNext_ && unitsInPacket < kMaxUnitsPerPacket; ++id)
            {
                SendUnit &unit = sendUnits_[id % Window()];
                if (!unit.needsResend || blocks.Remaining() < kUnitHeaderSize + unit.data.size())
                {
                    continue;
                }
                writeUnit(id, unit.flags, unit.data);
                unit.needsResend = false;
                unit.retries = static_cast<std::uint8_t>(std::min<unsigned>(unit.retries + 1u, 255u));
                maxRetries = std::max(maxRetries, unit.retries);
                units[unitCount++] = id;
                ++unitsInPacket;
                ++blockCount;
                ++stats_.unitsSent;
                ++stats_.unitsResent;
            }
        }

        // 2. new units, while the window has room
        while (mayCarryData && unitsInPacket < kMaxUnitsPerPacket && !sendQueue_.empty() &&
               (stream || SequenceDistance(sendOldest_, sendNext_) < Window()))
        {
            std::size_t length = 0;
            if (!HeadUnitFits(blocks.Remaining(), length))
            {
                break;
            }
            const std::vector<std::uint8_t> &head = sendQueue_.front();
            const bool first = headOffset_ == 0;
            const bool last = headOffset_ + length == head.size();
            const std::uint8_t flags = static_cast<std::uint8_t>((first ? kUnitFlagFirst : 0) | (last ? kUnitFlagLast : 0));
            const std::span<const std::uint8_t> data(head.data() + headOffset_, length);
            const std::uint16_t id = sendNext_;

            writeUnit(id, flags, data);
            if (!stream)
            {
                SendUnit &unit = sendUnits_[id % Window()];
                unit.inFlight = true;
                unit.acked = false;
                unit.needsResend = false;
                unit.retries = 0;
                unit.id = id;
                unit.flags = flags;
                unit.data.assign(data.begin(), data.end());
                units[unitCount++] = id;
            }
            ++sendNext_;
            if (stream)
            {
                // A stream transport delivers it: nothing is kept for a resend
                sendOldest_ = sendNext_;
            }
            ++unitsInPacket;
            ++blockCount;
            ++stats_.unitsSent;
            headOffset_ += length;
            if (last)
            {
                sendQueueBytes_ -= head.size();
                sendQueue_.pop_front();
                headOffset_ = 0;
            }
        }

        // 3. unreliable messages that fit
        while (mayCarryData && !unreliableQueue_.empty())
        {
            const std::vector<std::uint8_t> &message = unreliableQueue_.front();
            if (blocks.Remaining() < kUnreliableHeaderSize + message.size())
            {
                break;
            }
            blocks.U8(kBlockUnreliable);
            blocks.U16(nextUnreliableSequence_++);
            blocks.U16(static_cast<std::uint16_t>(message.size()));
            blocks.Bytes(message);
            unreliableQueueBytes_ -= message.size();
            unreliableQueue_.pop_front();
            ++blockCount;
        }

        if (blockCount == 0)
        {
            const bool keepAliveDue = now >= lastSendAt_ && now - lastSendAt_ >= config_.keepAliveMicros;
            if (!ackPending_ && !keepAliveDue)
            {
                return false;
            }
        }

        PacketHeader header;
        header.protocolId = config_.protocolId;
        header.connectionId = config_.connectionId;
        header.sequence = nextSequence_;
        header.hasAck = haveRemote_;
        if (haveRemote_)
        {
            header.ack = remoteSequence_;
            for (std::uint16_t bit = 0; bit < 32; ++bit)
            {
                const std::uint16_t sequence = static_cast<std::uint16_t>(remoteSequence_ - 1 - bit);
                if (receivedRing_[sequence % kReceivedRingSize] == ReceivedMark(sequence))
                {
                    header.ackBits |= 1u << bit;
                }
            }
        }
        ByteWriter headerWriter(buffer.data(), kHeaderSize);
        WriteHeader(headerWriter, header);
        packetSize = kHeaderSize + blocks.Size();

        // A packet with no blocks makes the peer send no ack of its own, so its ack may come late (with the peer's next
        // packet): it is not tracked, or the loss figures would count idle keep-alives as lost
        record.state = blockCount == 0 ? SentState::Acked : SentState::Pending;
        record.sequence = nextSequence_;
        record.sentAt = now;
        record.deadline = DeadlineFor(now, maxRetries);
        record.unitCount = unitCount;
        record.units = units;
        ++nextSequence_;

        ++stats_.packetsSent;
        stats_.bytesSent += packetSize;
        lastSendAt_ = now;
        ackPending_ = false;
        return true;
    }

    void Connection::MarkLost(SentPacket &packet)
    {
        packet.state = SentState::Lost;
        if (config_.reliableStream)
        {
            // Nothing is lost on a stream transport: an unanswered packet is only an ack that has not come yet
            return;
        }
        ++stats_.packetsLost;
        for (std::uint8_t i = 0; i < packet.unitCount; ++i)
        {
            const std::uint16_t id = packet.units[i];
            SendUnit &unit = sendUnits_[id % Window()];
            if (unit.inFlight && !unit.acked && unit.id == id)
            {
                unit.needsResend = true;
            }
        }
    }

    void Connection::AckUnits(const SentPacket &packet)
    {
        if (config_.reliableStream)
        {
            return;
        }
        for (std::uint8_t i = 0; i < packet.unitCount; ++i)
        {
            const std::uint16_t id = packet.units[i];
            SendUnit &unit = sendUnits_[id % Window()];
            if (unit.inFlight && !unit.acked && unit.id == id)
            {
                unit.acked = true;
                unit.needsResend = false;
                ReleaseBuffer(unit.data);
            }
        }
        while (sendOldest_ != sendNext_)
        {
            SendUnit &oldest = sendUnits_[sendOldest_ % Window()];
            if (!oldest.acked)
            {
                break;
            }
            oldest.inFlight = false;
            oldest.acked = false;
            ++sendOldest_;
        }
    }

    void Connection::ProcessAck(std::uint64_t now, std::uint16_t ack, std::uint32_t ackBits)
    {
        const auto settle = [this, now](std::uint16_t sequence, bool newest)
        {
            SentPacket &packet = sent_[sequence % kSentRingSize];
            if (packet.sequence != sequence)
            {
                return;
            }
            if (packet.state == SentState::Pending)
            {
                ++stats_.packetsAcked;
                if (newest && now >= packet.sentAt)
                {
                    rtt_.AddSample(now - packet.sentAt);
                }
                packet.state = SentState::Acked;
                AckUnits(packet);
            }
            else if (packet.state == SentState::Lost)
            {
                // Declared lost, and its ack was only late: it was not lost after all (the loss was a guess from a gap
                // in the acks or a timeout), and the units in it are delivered all the same
                packet.state = SentState::Acked;
                if (!config_.reliableStream)
                {
                    --stats_.packetsLost;
                    ++stats_.packetsAcked;
                    ++stats_.packetsAckedLate;
                }
                AckUnits(packet);
            }
        };

        settle(ack, true);
        for (std::uint16_t bit = 0; bit < 32; ++bit)
        {
            if ((ackBits >> bit) & 1u)
            {
                settle(static_cast<std::uint16_t>(ack - 1 - bit), false);
            }
        }

        // A packet 3 or more behind the newest ack and still unanswered is lost (the ack bits reach back 32, so past
        // that it can never be answered)
        for (SentPacket &packet : sent_)
        {
            if (packet.state != SentState::Pending)
            {
                continue;
            }
            const std::uint16_t behind = SequenceDistance(packet.sequence, ack);
            if (behind >= 3 && behind < 32768)
            {
                MarkLost(packet);
            }
        }
    }

    // ---- receiving ----

    PacketStatus Connection::OnPacket(std::uint64_t now, std::span<const std::uint8_t> datagram, MessageHandler handler)
    {
        const auto reject = [this](PacketStatus status)
        {
            ++stats_.packetsRejected;
            return status;
        };

        if (IsFailed())
        {
            return reject(PacketStatus::ConnectionFailed);
        }
        if (datagram.size() > kMaxPacketSize)
        {
            return reject(PacketStatus::TooLarge);
        }
        ByteReader reader(datagram);
        PacketHeader header;
        if (!ReadHeader(reader, header))
        {
            return reject(PacketStatus::BadHeader);
        }
        if (header.protocolId != config_.protocolId)
        {
            return reject(PacketStatus::WrongProtocol);
        }
        if (header.connectionId != config_.connectionId)
        {
            return reject(PacketStatus::WrongConnection);
        }
        const std::span<const std::uint8_t> body = datagram.subspan(kHeaderSize);
        std::size_t blockCount = 0;
        if (!ValidateBlocks(body, blockCount))
        {
            ++stats_.packetsMalformed;
            return reject(PacketStatus::Malformed);
        }

        // Packet numbers: duplicates and the too old are dropped, the rest are remembered for the ack bits
        const std::uint16_t sequence = header.sequence;
        if (!haveRemote_)
        {
            haveRemote_ = true;
            remoteSequence_ = sequence;
            receivedRing_.fill(0);
            receivedRing_[sequence % kReceivedRingSize] = ReceivedMark(sequence);
        }
        else if (SequenceMoreRecent(sequence, remoteSequence_))
        {
            const std::uint16_t advance = SequenceDistance(remoteSequence_, sequence);
            if (advance >= kReceivedRingSize)
            {
                receivedRing_.fill(0);
            }
            else
            {
                for (std::uint16_t step = 1; step < advance; ++step)
                {
                    receivedRing_[static_cast<std::uint16_t>(remoteSequence_ + step) % kReceivedRingSize] = 0;
                }
            }
            remoteSequence_ = sequence;
            receivedRing_[sequence % kReceivedRingSize] = ReceivedMark(sequence);
        }
        else
        {
            const std::uint16_t behind = SequenceDistance(sequence, remoteSequence_);
            if (behind >= kReceivedRingSize)
            {
                return reject(PacketStatus::TooOld);
            }
            if (receivedRing_[sequence % kReceivedRingSize] == ReceivedMark(sequence))
            {
                ++stats_.packetsDuplicate;
                if (blockCount > 0)
                {
                    // The peer is resending: our ack of it was probably lost
                    ackPending_ = true;
                }
                return reject(PacketStatus::DuplicatePacket);
            }
            receivedRing_[sequence % kReceivedRingSize] = ReceivedMark(sequence);
        }

        ++stats_.packetsReceived;
        stats_.bytesReceived += datagram.size();
        lastReceiveAt_ = now;
        if (blockCount > 0)
        {
            ackPending_ = true;
        }
        if (header.hasAck)
        {
            ProcessAck(now, header.ack, header.ackBits);
        }

        bool progress = false;
        ByteReader blockReader(body);
        PacketBlock block;
        while (!IsFailed() && ReadBlock(blockReader, block) == BlockResult::Block)
        {
            if (block.type == kBlockReliableUnit)
            {
                OnUnit(now, block, handler, progress);
            }
            else
            {
                OnUnreliable(block, handler);
            }
        }

        if (!IsFailed())
        {
            const bool partial = assembling_ || receiveBufferedUnits_ > 0;
            if (!partial)
            {
                stallActive_ = false;
            }
            else if (!stallActive_ || progress)
            {
                stallActive_ = true;
                stallSince_ = now;
            }
        }
        return PacketStatus::Ok;
    }

    void Connection::OnUnit(std::uint64_t now, const PacketBlock &block, MessageHandler handler, bool &progress)
    {
        const std::uint16_t ahead = SequenceDistance(receiveNext_, block.id);
        if (ahead >= 32768)
        {
            ++stats_.unitsDuplicate; // already delivered
            return;
        }
        if (config_.reliableStream)
        {
            if (ahead != 0)
            {
                Fail(FailureReason::ProtocolViolation);
                return;
            }
            if (ConsumeUnit(block.flags, block.data, now, handler))
            {
                ++receiveNext_;
                progress = true;
            }
            return;
        }
        if (ahead >= Window())
        {
            ++stats_.unitsOutOfWindow;
            return;
        }
        ReceiveSlot &slot = receiveSlots_[block.id % Window()];
        if (slot.present)
        {
            ++stats_.unitsDuplicate;
            return;
        }
        slot.present = true;
        slot.flags = block.flags;
        slot.data.assign(block.data.begin(), block.data.end());
        ++receiveBufferedUnits_;
        DeliverReady(now, handler, progress);
    }

    void Connection::DeliverReady(std::uint64_t now, MessageHandler handler, bool &progress)
    {
        while (!IsFailed())
        {
            ReceiveSlot &slot = receiveSlots_[receiveNext_ % Window()];
            if (!slot.present)
            {
                break;
            }
            const bool ok = ConsumeUnit(slot.flags, slot.data, now, handler);
            slot.present = false;
            ReleaseBuffer(slot.data);
            --receiveBufferedUnits_;
            ++receiveNext_;
            progress = true;
            if (!ok)
            {
                break;
            }
        }
    }

    bool Connection::ConsumeUnit(std::uint8_t flags, std::span<const std::uint8_t> data, std::uint64_t /*now*/,
                                 MessageHandler handler)
    {
        const bool first = (flags & kUnitFlagFirst) != 0;
        const bool last = (flags & kUnitFlagLast) != 0;
        if (first)
        {
            if (assembling_)
            {
                Fail(FailureReason::ProtocolViolation);
                return false;
            }
            if (last)
            {
                if (data.size() > config_.maxMessageSize)
                {
                    Fail(FailureReason::MessageTooLarge);
                    return false;
                }
                ++stats_.reliableMessagesDelivered;
                handler(MessageChannel::ReliableOrdered, data);
                return true;
            }
            assembling_ = true;
            assembly_.clear();
        }
        else if (!assembling_)
        {
            Fail(FailureReason::ProtocolViolation);
            return false;
        }

        if (assembly_.size() + data.size() > config_.maxMessageSize)
        {
            Fail(FailureReason::MessageTooLarge);
            return false;
        }
        assembly_.insert(assembly_.end(), data.begin(), data.end());
        if (last)
        {
            assembling_ = false;
            ++stats_.reliableMessagesDelivered;
            handler(MessageChannel::ReliableOrdered, std::span<const std::uint8_t>(assembly_.data(), assembly_.size()));
            ReleaseBuffer(assembly_);
        }
        return true;
    }

    void Connection::OnUnreliable(const PacketBlock &block, MessageHandler handler)
    {
        if (haveUnreliable_ && !SequenceMoreRecent(block.id, lastUnreliableSequence_))
        {
            ++stats_.unreliableStale;
            return;
        }
        haveUnreliable_ = true;
        lastUnreliableSequence_ = block.id;
        ++stats_.unreliableMessagesDelivered;
        handler(MessageChannel::UnreliableSequenced, block.data);
    }

    void Connection::Fail(FailureReason reason) noexcept
    {
        if (failure_ != FailureReason::None)
        {
            return;
        }
        failure_ = reason;
        // Nothing more will be sent or delivered: give the buffers back
        sendQueue_.clear();
        sendQueueBytes_ = 0;
        unreliableQueue_.clear();
        unreliableQueueBytes_ = 0;
        for (SendUnit &unit : sendUnits_)
        {
            ReleaseBuffer(unit.data);
        }
        for (ReceiveSlot &slot : receiveSlots_)
        {
            slot.present = false;
            ReleaseBuffer(slot.data);
        }
        receiveBufferedUnits_ = 0;
        ReleaseBuffer(assembly_);
    }

    // ---- statistics ----

    std::uint32_t Connection::LossRatePpm() const noexcept
    {
        const std::uint64_t settled = stats_.packetsAcked + stats_.packetsLost;
        if (settled == 0)
        {
            return 0;
        }
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(stats_.packetsLost * 1'000'000 / settled, 1'000'000));
    }

    std::size_t Connection::ReliableBacklogBytes() const noexcept
    {
        std::size_t bytes = sendQueueBytes_ - headOffset_;
        for (const SendUnit &unit : sendUnits_)
        {
            if (unit.inFlight && !unit.acked)
            {
                bytes += unit.data.size();
            }
        }
        return bytes;
    }

    bool Connection::ReliableIdle() const noexcept
    {
        return sendQueue_.empty() && sendOldest_ == sendNext_;
    }

    std::size_t Connection::MemoryInUseBytes() const noexcept
    {
        std::size_t bytes = sendQueueBytes_ + unreliableQueueBytes_ + assembly_.capacity();
        for (const SendUnit &unit : sendUnits_)
        {
            bytes += unit.data.capacity();
        }
        for (const ReceiveSlot &slot : receiveSlots_)
        {
            bytes += slot.data.capacity();
        }
        return bytes;
    }

    std::size_t Connection::MemoryBoundBytes() const noexcept
    {
        // Each buffer family may hold up to twice what it needs in capacity (a vector's growth slack)
        const std::size_t units = static_cast<std::size_t>(Window()) * kMaxUnitPayload;
        return config_.maxSendQueueBytes + config_.maxQueuedUnreliableBytes + 2 * units + 2 * units +
               2 * static_cast<std::size_t>(config_.maxMessageSize);
    }
} // namespace N2Engine::N2Net
