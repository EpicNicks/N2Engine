#include "engine/network/LockstepRelay.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "engine/network/LockstepInputMapping.hpp"
#include "engine/network/n2net/BisectProtocol.hpp"
#include "engine/network/n2net/HashTree.hpp"

namespace N2Engine::Network
{
    namespace
    {
        constexpr std::uint64_t kSecondMicros = 1'000'000;
        /// How long a refused client stays connected to receive the refusal
        constexpr std::uint64_t kRefuseLingerMicros = 300'000;
        /// An ack-only bundle is sent at least this often, so a client whose frames are all acknowledged still hears it
        constexpr std::uint64_t kAckIntervalMicros = 50'000;
        constexpr std::uint32_t kMaxRefusedForwards = 64;

        bool Ahead(const std::uint32_t a, const std::uint32_t b) noexcept
        {
            return static_cast<std::int32_t>(a - b) > 0;
        }
    }

    std::string_view ToString(const StallPolicy policy) noexcept
    {
        return policy == StallPolicy::Drop ? "drop" : "stop";
    }

    std::string_view ToString(const RelayState state) noexcept
    {
        switch (state)
        {
        case RelayState::Lobby:
            return "lobby";
        case RelayState::Running:
            return "running";
        case RelayState::Stopping:
            return "stopping";
        }
        return "unknown";
    }

    LockstepRelay::LockstepRelay(N2Net::ITransport &transport, LockstepRelayOptions options)
        : transport_(transport), options_(std::move(options))
    {
        options_.tickRate = std::clamp<std::uint32_t>(options_.tickRate, 1u, 1000u);
        options_.hashInterval = std::clamp<std::uint32_t>(options_.hashInterval, 1u, 65535u);
        options_.inputDelay = std::clamp<std::uint32_t>(options_.inputDelay, 1u, N2Net::kMaxInputDelay);
        options_.maxConnections = std::clamp<std::uint32_t>(options_.maxConnections, 1u, 256u);
        options_.sendIntervalMicros = std::max<std::uint64_t>(options_.sendIntervalMicros, 1000);
        options_.maxForwardMessages = std::min<std::uint32_t>(options_.maxForwardMessages, 65536u);
        layout_ = options_.layout;
        valid_ = !layout_.Empty() && options_.slotCount >= 1 && options_.slotCount <= kMaxRelaySlots &&
                 MakeLockstepLayoutInfo(layout_).has_value();
        if (valid_)
        {
            layoutHash_ = layout_.Hash();
            if (options_.maxSpectators > 0)
            {
                history_.resize(kSpectatorHistoryTicks);
            }
            EndSession();
            stats_.sessionsEnded = 0;
        }
    }

    std::size_t LockstepRelay::SpectatorCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto &entry : peers_)
        {
            if (entry.second.spectator && !entry.second.leaving)
            {
                ++count;
            }
        }
        return count;
    }

    bool LockstepRelay::AwaitsSnapshotFrom(const N2Net::PeerId source) const
    {
        for (const auto &entry : peers_)
        {
            const Peer &peer = entry.second;
            if (peer.spectator && !peer.leaving && peer.snapshotRequested && peer.snapshotSource == source &&
                (!peer.streaming || peer.snapshotReceived < peer.snapshotTotal))
            {
                return true;
            }
        }
        return false;
    }

    void LockstepRelay::RefusePeer(Peer &peer, const RefuseReason reason, const std::string &why)
    {
        if (peer.leaving)
        {
            return;
        }
        ++stats_.refused;
        (void)Send(peer, EncodeRefuse(reason));
        Notify(RelayEvent::Type::Refused, 0, 0, why);
        // Stays connected a moment to receive it, then goes
        peer.leaveText = why;
        peer.leaveReason = DropReason::Left;
        peer.leaving = true;
        peer.lingerUntil = now_ + kRefuseLingerMicros;
    }

    std::uint32_t LockstepRelay::ConfirmedTick() const noexcept
    {
        return scheduler_ ? scheduler_->CurrentTick() : 0;
    }

    std::size_t LockstepRelay::JoinedCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto &entry : peers_)
        {
            if (entry.second.joined && !entry.second.leaving)
            {
                ++count;
            }
        }
        return count;
    }

    bool LockstepRelay::IsSlotDropped(const std::uint32_t slot) const noexcept
    {
        return slot < kMaxRelaySlots && slotDropped_[slot];
    }

    LockstepRelay::Peer *LockstepRelay::PeerOfSlot(const std::uint32_t slot)
    {
        for (auto &entry : peers_)
        {
            if (entry.second.joined && !entry.second.leaving && entry.second.slot == slot)
            {
                return &entry.second;
            }
        }
        return nullptr;
    }

    void LockstepRelay::Notify(const RelayEvent::Type type, const std::uint32_t slot, const std::uint32_t tick, std::string text)
    {
        if (observer_)
        {
            observer_(RelayEvent{type, slot, tick, std::move(text)});
        }
    }

    bool LockstepRelay::Send(Peer &peer, const std::vector<std::uint8_t> &message)
    {
        if (peer.leaving || message.empty())
        {
            return false;
        }
        if (peer.connection->SendReliable(message) != N2Net::SendResult::Ok)
        {
            Mark(peer, "its reliable send queue is full", DropReason::Violation);
            return false;
        }
        return true;
    }

    void LockstepRelay::Broadcast(const std::vector<std::uint8_t> &message, const std::uint32_t exceptSlot)
    {
        for (auto &entry : peers_)
        {
            Peer &peer = entry.second;
            if (peer.joined && !peer.leaving && peer.slot != exceptSlot)
            {
                (void)Send(peer, message);
            }
        }
    }

    void LockstepRelay::Mark(Peer &peer, std::string text, const DropReason reason)
    {
        if (peer.leaving)
        {
            return;
        }
        peer.leaving = true;
        peer.leaveReason = reason;
        peer.leaveText = std::move(text);
        if (reason == DropReason::Violation)
        {
            ++stats_.clientsDropped;
        }
    }

    bool LockstepRelay::AllowMessage(Peer &peer)
    {
        if (now_ - peer.messageWindowStart >= kSecondMicros)
        {
            peer.messageWindowStart = now_;
            peer.messageWindowCount = 0;
        }
        if (++peer.messageWindowCount > options_.maxMessagesPerSecond)
        {
            ++stats_.rateLimited;
            Mark(peer, "it sent more messages per second than allowed");
            return false;
        }
        return true;
    }

    bool LockstepRelay::AllowBundle(Peer &peer)
    {
        if (now_ - peer.bundleWindowStart >= kSecondMicros)
        {
            peer.bundleWindowStart = now_;
            peer.bundleWindowCount = 0;
        }
        if (++peer.bundleWindowCount > options_.maxBundlesPerSecond)
        {
            ++stats_.rateLimited;
            Mark(peer, "it sent more input bundles per second than allowed");
            return false;
        }
        return true;
    }

    void LockstepRelay::HandleEvent(const N2Net::TransportEvent &event)
    {
        switch (event.type)
        {
        case N2Net::TransportEventType::PeerConnected:
            OnConnected(event.peer);
            break;
        case N2Net::TransportEventType::PeerDisconnected:
            OnDisconnected(event.peer);
            break;
        case N2Net::TransportEventType::Datagram:
            OnDatagram(event.peer, event.data);
            break;
        }
    }

    void LockstepRelay::OnConnected(const N2Net::PeerId id)
    {
        if (peers_.contains(id))
        {
            return;
        }
        if (peers_.size() >= options_.maxConnections)
        {
            refusedPeers_.push_back(id);
            return;
        }
        auto config = MakeLockstepConnectionConfig(options_.protocolId, transport_);
        config.reliableStream = transport_.IsReliablePeer(id);
        config.maxPacketSize =
            static_cast<std::uint32_t>(std::min(transport_.MaxPeerPacketSize(id), N2Net::kMaxPacketSize));
        Peer peer;
        peer.id = id;
        peer.connection = std::make_unique<N2Net::Connection>(config, now_);
        peer.connectedAt = now_;
        peer.bundleWindowStart = now_;
        peer.messageWindowStart = now_;
        peers_.emplace(id, std::move(peer));
    }

    void LockstepRelay::OnDisconnected(const N2Net::PeerId id)
    {
        const auto found = peers_.find(id);
        if (found != peers_.end())
        {
            Mark(found->second, "disconnected", DropReason::Left);
        }
    }

    void LockstepRelay::OnDatagram(const N2Net::PeerId id, const std::span<const std::uint8_t> data)
    {
        const auto found = peers_.find(id);
        if (found == peers_.end() || found->second.leaving)
        {
            return;
        }
        Peer &peer = found->second;
        const auto handler = [this, &peer](const N2Net::MessageChannel channel, const std::span<const std::uint8_t> message)
        { OnMessage(peer, channel, message); };
        (void)peer.connection->OnPacket(now_, data, handler);
    }

    void LockstepRelay::OnMessage(Peer &peer, const N2Net::MessageChannel channel, const std::span<const std::uint8_t> message)
    {
        if (peer.leaving || message.empty())
        {
            return;
        }
        const std::uint8_t kind = message[0];
        if (channel == N2Net::MessageChannel::UnreliableSequenced)
        {
            if (kind == 1)
            {
                OnBundle(peer, message);
            }
            else
            {
                Mark(peer, "it sent an unexpected message on the unreliable channel");
            }
            return;
        }
        // A snapshot a spectator waits for comes in many parts: bounded by its size (maxSnapshotBytes), not by the rate
        const bool awaitedSnapshot =
            kind == static_cast<std::uint8_t>(RelayMessage::SnapshotData) && AwaitsSnapshotFrom(peer.id);
        if (!awaitedSnapshot && !AllowMessage(peer))
        {
            return;
        }
        switch (kind)
        {
        case static_cast<std::uint8_t>(RelayMessage::Join):
            OnJoin(peer, message);
            break;
        case static_cast<std::uint8_t>(RelayMessage::Spectate):
            OnSpectate(peer, message);
            break;
        case static_cast<std::uint8_t>(RelayMessage::SnapshotData):
            OnSnapshotData(peer, message);
            break;
        case 2:
            OnRoot(peer, message);
            break;
        case static_cast<std::uint8_t>(RelayMessage::To):
            OnForward(peer, message);
            break;
        case static_cast<std::uint8_t>(RelayMessage::Pause):
            OnPause(peer, message);
            break;
        default:
            Mark(peer, std::format("it sent a message the relay does not take (type 0x{:02X})", kind));
            break;
        }
    }

    void LockstepRelay::OnJoin(Peer &peer, const std::span<const std::uint8_t> message)
    {
        const auto refuse = [&](const RefuseReason reason, const std::string &why) { RefusePeer(peer, reason, why); };
        const auto join = DecodeJoin(message);
        if (!join || peer.joined || peer.spectator)
        {
            refuse(RefuseReason::Protocol, "a malformed or repeated join");
            return;
        }
        if (join->version != kLockstepWireVersion)
        {
            refuse(RefuseReason::WrongVersion, std::format("wire version {} (the relay speaks {})", join->version,
                                                           kLockstepWireVersion));
            return;
        }
        if (state_ != RelayState::Lobby)
        {
            refuse(RefuseReason::SessionRunning, "the session has started");
            return;
        }
        if (join->payloadBytes != layout_.PayloadBytes() || join->layoutHash != layoutHash_)
        {
            refuse(RefuseReason::LayoutMismatch, "its input layout is not the session's");
            return;
        }
        std::uint32_t slot = 0;
        while (slot < options_.slotCount && slotTaken_[slot])
        {
            ++slot;
        }
        if (slot >= options_.slotCount)
        {
            refuse(RefuseReason::SessionFull, "every slot is taken");
            return;
        }
        slotTaken_[slot] = true;
        peer.joined = true;
        peer.slot = slot;
        ++stats_.joined;

        WelcomeMessage welcome;
        welcome.slot = static_cast<std::uint8_t>(slot);
        welcome.slotCount = static_cast<std::uint8_t>(options_.slotCount);
        welcome.inputDelay = static_cast<std::uint8_t>(options_.inputDelay);
        welcome.tickRate = static_cast<std::uint16_t>(options_.tickRate);
        welcome.hashInterval = static_cast<std::uint16_t>(options_.hashInterval);
        welcome.dropFill = options_.dropFill == N2Net::DropFill::HoldLast ? 1 : 0;
        welcome.payloadBytes = static_cast<std::uint8_t>(layout_.PayloadBytes());
        welcome.layoutHash = layoutHash_;
        welcome.neutral = layout_.NeutralPayload();
        (void)Send(peer, Encode(welcome));
        Notify(RelayEvent::Type::Joined, slot, 0, "");

        if (JoinedCount() == options_.slotCount)
        {
            StartSession();
        }
    }

    void LockstepRelay::OnSpectate(Peer &peer, const std::span<const std::uint8_t> message)
    {
        const auto spectate = DecodeSpectate(message);
        if (!spectate || peer.joined || peer.spectator)
        {
            RefusePeer(peer, RefuseReason::Protocol, "a malformed or repeated spectate");
            return;
        }
        if (spectate->version != kLockstepWireVersion)
        {
            RefusePeer(peer, RefuseReason::WrongVersion,
                       std::format("wire version {} (the relay speaks {})", spectate->version, kLockstepWireVersion));
            return;
        }
        if (state_ == RelayState::Stopping)
        {
            RefusePeer(peer, RefuseReason::SessionRunning, "the session is ending");
            return;
        }
        if (spectate->payloadBytes != layout_.PayloadBytes() || spectate->layoutHash != layoutHash_)
        {
            RefusePeer(peer, RefuseReason::LayoutMismatch, "its input layout is not the session's");
            return;
        }
        if (SpectatorCount() >= options_.maxSpectators)
        {
            RefusePeer(peer, RefuseReason::SessionFull, "every spectator place is taken");
            return;
        }
        peer.spectator = true;
        ++stats_.spectatorsJoined;

        WelcomeMessage welcome;
        welcome.slot = kSpectatorSlot;
        welcome.slotCount = static_cast<std::uint8_t>(options_.slotCount);
        welcome.inputDelay = static_cast<std::uint8_t>(options_.inputDelay);
        welcome.tickRate = static_cast<std::uint16_t>(options_.tickRate);
        welcome.hashInterval = static_cast<std::uint16_t>(options_.hashInterval);
        welcome.dropFill = options_.dropFill == N2Net::DropFill::HoldLast ? 1 : 0;
        welcome.payloadBytes = static_cast<std::uint8_t>(layout_.PayloadBytes());
        welcome.layoutHash = layoutHash_;
        welcome.neutral = layout_.NeutralPayload();
        (void)Send(peer, Encode(welcome));
        Notify(RelayEvent::Type::Spectating, kSpectatorSlot, ConfirmedTick(), "");
    }

    void LockstepRelay::OnSnapshotData(Peer &peer, const std::span<const std::uint8_t> message)
    {
        const auto part = DecodeSnapshotPart(RelayMessage::SnapshotData, message);
        if (!part)
        {
            Mark(peer, "it sent a malformed snapshot part");
            return;
        }
        if (!peer.joined || state_ != RelayState::Running)
        {
            return;
        }
        Peer *target = nullptr;
        for (auto &entry : peers_)
        {
            Peer &candidate = entry.second;
            if (candidate.spectator && !candidate.leaving && candidate.snapshotRequested &&
                candidate.snapshotSource == peer.id && candidate.requestId == part->requestId)
            {
                target = &candidate;
                break;
            }
        }
        if (target == nullptr)
        {
            return; // a request that was given up (the spectator left, timed out, or asked someone else)
        }
        Peer &spectator = *target;
        if (!spectator.streaming)
        {
            // A player may be a little ahead of the relay (its own last frames are still on the way): the history
            // fills in up to that tick as the relay confirms it. Too far ahead, or already out of the history: refused.
            const std::uint32_t current = scheduler_->CurrentTick();
            const bool held = !Ahead(part->tick, current + N2Net::kLockstepWindow) &&
                              Ahead(part->tick + kSpectatorHistoryTicks, current);
            if (part->offset != 0 || part->totalBytes > options_.maxSnapshotBytes || !held)
            {
                RefusePeer(spectator, RefuseReason::NoSnapshot,
                           part->totalBytes > options_.maxSnapshotBytes
                               ? std::format("the snapshot ({} bytes) is larger than the relay passes on", part->totalBytes)
                               : std::string("the snapshot is not of a tick the relay holds"));
                return;
            }
            spectator.streaming = true;
            spectator.snapshotTick = part->tick;
            spectator.snapshotTotal = part->totalBytes;
            spectator.snapshotReceived = 0;
            spectator.snapshotForwarded = 0;
            spectator.ack = part->tick;
        }
        else if (part->offset != spectator.snapshotReceived || part->tick != spectator.snapshotTick ||
                 part->totalBytes != spectator.snapshotTotal)
        {
            Mark(peer, "it sent snapshot parts out of order");
            return;
        }
        spectator.snapshotReceived += static_cast<std::uint32_t>(part->data.size());
        spectator.snapshotQueue.push_back(Encode(RelayMessage::SnapshotPart, *part));
    }

    void LockstepRelay::ServeSpectators()
    {
        if (history_.empty())
        {
            return;
        }
        for (auto &entry : peers_)
        {
            Peer &spectator = entry.second;
            if (!spectator.spectator || spectator.leaving || spectator.snapshotDone)
            {
                continue;
            }
            // The source went away before the snapshot was whole: ask again, from the start (the spectator restarts at
            // a part with offset 0)
            const bool incomplete = !spectator.streaming || spectator.snapshotReceived < spectator.snapshotTotal;
            if (spectator.snapshotRequested && incomplete)
            {
                const auto source = peers_.find(spectator.snapshotSource);
                if (source == peers_.end() || source->second.leaving)
                {
                    spectator.snapshotRequested = false;
                    spectator.streaming = false;
                    spectator.snapshotQueue.clear();
                }
            }
            if (state_ != RelayState::Running)
            {
                continue;
            }
            if (!spectator.snapshotRequested)
            {
                Peer *source = nullptr;
                for (std::uint32_t slot = 0; slot < options_.slotCount && source == nullptr; ++slot)
                {
                    if (!slotDropped_[slot])
                    {
                        source = PeerOfSlot(slot);
                    }
                }
                if (source == nullptr)
                {
                    continue;
                }
                spectator.snapshotRequested = true;
                spectator.snapshotSource = source->id;
                spectator.requestId = ++nextRequestId_;
                spectator.requestedAt = now_;
                ++stats_.snapshotRequests;
                (void)Send(*source, EncodeSnapshotRequest(spectator.requestId));
                continue;
            }
            const std::uint64_t waited = now_ - spectator.requestedAt;
            if ((!spectator.streaming && waited >= options_.snapshotTimeoutMicros) ||
                waited >= 2 * options_.snapshotTimeoutMicros)
            {
                RefusePeer(spectator, RefuseReason::NoSnapshot, "no snapshot arrived in time");
                continue;
            }
            // Pass the parts on as the spectator's link takes them
            while (!spectator.snapshotQueue.empty() && spectator.connection->ReliableBacklogBytes() < 64u * 1024u)
            {
                const std::vector<std::uint8_t> part = std::move(spectator.snapshotQueue.front());
                spectator.snapshotQueue.pop_front();
                if (!Send(spectator, part))
                {
                    break;
                }
                spectator.snapshotForwarded += static_cast<std::uint32_t>(part.size() - 17);
            }
            if (spectator.streaming && spectator.snapshotForwarded == spectator.snapshotTotal &&
                spectator.snapshotQueue.empty())
            {
                spectator.snapshotDone = true;
                ++stats_.snapshotsRelayed;
                Notify(RelayEvent::Type::SnapshotRelayed, kSpectatorSlot, spectator.snapshotTick, "");
            }
        }
    }

    void LockstepRelay::SendSpectatorBundles(Peer &peer, const std::span<std::uint8_t> buffer, const std::size_t maxFrames)
    {
        const std::uint32_t current = scheduler_->CurrentTick();
        // ack + history > current: the oldest tick the spectator still needs is held
        if (!Ahead(peer.ack + kSpectatorHistoryTicks, current))
        {
            Mark(peer, "it fell behind the relay's input history", DropReason::Left);
            return;
        }
        const std::size_t payload = layout_.PayloadBytes();
        for (std::uint32_t slot = 0; slot < options_.slotCount; ++slot)
        {
            N2Net::InputBundle bundle;
            bundle.slot = static_cast<std::uint8_t>(slot);
            bundle.ackTick = current;
            bundle.firstTick = peer.ack;
            bundle.payloadBytes = static_cast<std::uint8_t>(payload);
            bundle.layoutHash = layoutHash_;
            for (std::uint32_t tick = peer.ack; Ahead(current, tick) && bundle.frames.size() < maxFrames; ++tick)
            {
                const HistoryTick &held = history_[tick % kSpectatorHistoryTicks];
                if (!held.valid || held.tick != tick)
                {
                    break;
                }
                bundle.frames.push_back(held.inputs[slot]);
            }
            if (bundle.frames.empty())
            {
                continue;
            }
            const std::size_t size = N2Net::EncodeInputBundle(bundle, buffer);
            if (size != 0 && peer.connection->SendUnreliable(std::span<const std::uint8_t>(buffer.data(), size)) ==
                                 N2Net::SendResult::Ok)
            {
                ++stats_.bundlesSent;
            }
        }
    }

    void LockstepRelay::StartSession()
    {
        state_ = RelayState::Running;
        progressAt_ = now_;
        ++stats_.sessionsStarted;
        Broadcast(EncodeStart());
        Notify(RelayEvent::Type::Started, 0, 0, "");
    }

    void LockstepRelay::OnBundle(Peer &peer, const std::span<const std::uint8_t> message)
    {
        if (peer.spectator)
        {
            // A spectator's bundles only acknowledge: the first tick it still needs
            if (state_ != RelayState::Running || !AllowBundle(peer))
            {
                return;
            }
            ++stats_.bundlesReceived;
            const auto bundle = DecodeInputBundleFor(layout_, message);
            if (!bundle || !bundle->frames.empty())
            {
                ++stats_.bundlesRejected;
                Mark(peer, "a spectator sent inputs");
                return;
            }
            if (peer.streaming && Ahead(bundle->ackTick, peer.ack) && !Ahead(bundle->ackTick, scheduler_->CurrentTick()))
            {
                peer.ack = bundle->ackTick;
            }
            return;
        }
        if (!peer.joined || state_ != RelayState::Running || !AllowBundle(peer))
        {
            return;
        }
        ++stats_.bundlesReceived;
        const auto bundle = DecodeInputBundleFor(layout_, message);
        if (!bundle || bundle->slot != peer.slot)
        {
            ++stats_.bundlesRejected;
            Mark(peer, bundle ? "it sent inputs for another slot" : "it sent an input bundle the session's layout cannot decode");
            return;
        }
        if (Ahead(bundle->ackTick, peer.ack))
        {
            peer.ack = bundle->ackTick;
        }
        std::uint32_t tick = bundle->firstTick;
        for (const N2Net::LockstepInput &input : bundle->frames)
        {
            const N2Net::SubmitResult result = scheduler_->Submit(peer.slot, tick, input);
            if (result == N2Net::SubmitResult::Conflict || result == N2Net::SubmitResult::BadPayload)
            {
                ++stats_.conflicts;
                Mark(peer, "it sent two different inputs for one tick");
                return;
            }
            ++tick;
        }
    }

    void LockstepRelay::OnRoot(Peer &peer, const std::span<const std::uint8_t> message)
    {
        const auto root = N2Net::DecodeRootMessage(message);
        if (!root)
        {
            Mark(peer, "it sent a malformed root message");
            return;
        }
        if (!peer.joined || state_ != RelayState::Running)
        {
            return;
        }
        ++stats_.rootsReceived;
        // A client reports the root after it ran `tick` ticks; it cannot have run a tick the relay has not confirmed
        if (root->tick == 0 || Ahead(root->tick, scheduler_->CurrentTick()))
        {
            ++stats_.rootsIgnored;
            return;
        }
        RootRecord *record = nullptr;
        RootRecord *oldest = &roots_[0];
        for (RootRecord &candidate : roots_)
        {
            if (candidate.valid && candidate.tick == root->tick)
            {
                record = &candidate;
                break;
            }
            if (!candidate.valid || (oldest->valid && candidate.order < oldest->order))
            {
                oldest = &candidate;
            }
        }
        if (record == nullptr)
        {
            record = oldest;
            *record = RootRecord{};
            record->valid = true;
            record->tick = root->tick;
            record->order = ++rootOrder_;
        }
        if ((record->mask & (1u << peer.slot)) != 0)
        {
            return; // reported already; the first stays
        }
        for (std::uint32_t other = 0; other < kMaxRelaySlots; ++other)
        {
            if ((record->mask & (1u << other)) != 0 && record->roots[other] != root->root)
            {
                BeginStop(StopReason::Desync, root->tick, other, peer.slot, record->roots[other], root->root);
                return;
            }
        }
        record->mask |= 1u << peer.slot;
        record->roots[peer.slot] = root->root;
    }

    void LockstepRelay::OnForward(Peer &peer, const std::span<const std::uint8_t> message)
    {
        const auto forward = DecodeForward(RelayMessage::To, message);
        if (!forward)
        {
            Mark(peer, "it sent a malformed forward message");
            return;
        }
        const bool allowed = state_ == RelayState::Stopping && stopReason_ == StopReason::Desync && peer.joined &&
                             (peer.slot == stopSlotA_ || peer.slot == stopSlotB_) &&
                             (forward->slot == stopSlotA_ || forward->slot == stopSlotB_) && forward->slot != peer.slot &&
                             forwardCount_ < options_.maxForwardMessages;
        Peer *const target = allowed ? PeerOfSlot(forward->slot) : nullptr;
        if (target == nullptr)
        {
            ++stats_.forwardsRefused;
            if (++peer.refusedForwards > kMaxRefusedForwards)
            {
                Mark(peer, "it keeps sending forward messages the relay refuses");
            }
            return;
        }
        ++forwardCount_;
        ++stats_.forwarded;
        (void)Send(*target, EncodeForward(RelayMessage::From, static_cast<std::uint8_t>(peer.slot), forward->inner));
    }

    void LockstepRelay::OnPause(Peer &peer, const std::span<const std::uint8_t> message)
    {
        const auto paused = DecodePause(message);
        if (!paused)
        {
            Mark(peer, "it sent a malformed pause message");
            return;
        }
        if (*paused && !peer.paused)
        {
            peer.pausedSince = now_;
        }
        peer.paused = *paused;
    }

    void LockstepRelay::AdvanceSession()
    {
        N2Net::TickInputs out;
        while (scheduler_->TryAdvance(out) == N2Net::AdvanceStatus::Runnable)
        {
            ++stats_.ticksConfirmed;
            progressAt_ = now_;
            if (!history_.empty())
            {
                HistoryTick &held = history_[out.tick % kSpectatorHistoryTicks];
                held.valid = true;
                held.tick = out.tick;
                std::copy_n(out.inputs.begin(), kMaxRelaySlots, held.inputs.begin());
            }
        }
    }

    void LockstepRelay::DropSlotNow(const std::uint32_t slot, const DropReason reason)
    {
        if (slot >= options_.slotCount || slotDropped_[slot])
        {
            return;
        }
        // The first tick this slot has not delivered. Every peer is at or before it (none can pass a tick without the
        // slot's input, and the relay forwards nothing from the slot at or after it), so all of them apply it in time.
        const std::uint32_t from = scheduler_->NextNeeded(slot);
        if (!scheduler_->DropSlot(slot, from))
        {
            return;
        }
        slotDropped_[slot] = true;
        dropTick_[slot] = from;
        ++stats_.slotsDropped;
        progressAt_ = now_;
        DropSlotMessage message;
        message.slot = static_cast<std::uint8_t>(slot);
        message.fromTick = from;
        message.reason = reason;
        Broadcast(Encode(message), slot);
        Notify(RelayEvent::Type::SlotDropped, slot, from, std::string(ToString(reason)));
    }

    void LockstepRelay::SlotGone(const std::uint32_t slot, const DropReason reason)
    {
        if (state_ != RelayState::Running || slot >= options_.slotCount || slotDropped_[slot])
        {
            return;
        }
        if (options_.stallPolicy == StallPolicy::Drop)
        {
            DropSlotNow(slot, reason);
        }
        else
        {
            BeginStop(reason == DropReason::Stalled ? StopReason::Stalled : StopReason::PeerLeft, scheduler_->CurrentTick(),
                      slot, slot, 0, 0);
        }
    }

    void LockstepRelay::BeginStop(const StopReason reason, const std::uint32_t tick, const std::uint32_t slotA,
                                  const std::uint32_t slotB, const std::uint64_t rootA, const std::uint64_t rootB)
    {
        if (state_ == RelayState::Stopping)
        {
            return;
        }
        state_ = RelayState::Stopping;
        stopAt_ = now_;
        stopReason_ = reason;
        stopSlotA_ = slotA;
        stopSlotB_ = slotB;
        forwardCount_ = 0;
        if (reason == StopReason::Desync)
        {
            ++stats_.desyncs;
        }
        StopMessage message;
        message.reason = reason;
        message.tick = tick;
        message.slotA = static_cast<std::uint8_t>(slotA);
        message.slotB = static_cast<std::uint8_t>(slotB);
        message.rootA = rootA;
        message.rootB = rootB;
        const std::vector<std::uint8_t> encoded = Encode(message);
        Broadcast(encoded);
        for (auto &entry : peers_)
        {
            if (entry.second.spectator && !entry.second.leaving)
            {
                (void)Send(entry.second, encoded);
            }
        }
        Notify(RelayEvent::Type::Stopped, slotA, tick, std::string(ToString(reason)));
    }

    void LockstepRelay::Shutdown()
    {
        if (state_ == RelayState::Running || state_ == RelayState::Lobby)
        {
            if (JoinedCount() == 0)
            {
                return;
            }
            BeginStop(StopReason::Shutdown, ConfirmedTick(), 0, 0, 0, 0);
        }
    }

    void LockstepRelay::EndSession()
    {
        for (const auto &entry : peers_)
        {
            transport_.Disconnect(entry.first);
        }
        peers_.clear();
        const bool wasActive = state_ != RelayState::Lobby || stats_.sessionsStarted > stats_.sessionsEnded;
        state_ = RelayState::Lobby;
        slotTaken_ = {};
        slotDropped_ = {};
        dropTick_ = {};
        roots_ = {};
        rootOrder_ = 0;
        forwardCount_ = 0;
        for (HistoryTick &held : history_)
        {
            held.valid = false;
        }
        N2Net::LockstepConfig config;
        config.slotCount = options_.slotCount;
        config.inputDelay = options_.inputDelay;
        config.stallTimeoutPolls = 0xFFFFFFFFu;
        config.dropFill = options_.dropFill;
        config.layout = *MakeLockstepLayoutInfo(layout_);
        scheduler_ = std::make_unique<N2Net::LockstepScheduler>(config);
        if (wasActive)
        {
            ++stats_.sessionsEnded;
            Notify(RelayEvent::Type::Reset, 0, 0, "");
        }
    }

    void LockstepRelay::RunTimers()
    {
        for (auto &entry : peers_)
        {
            Peer &peer = entry.second;
            if (peer.leaving)
            {
                continue;
            }
            if (!peer.joined && !peer.spectator && now_ - peer.connectedAt >= options_.joinTimeoutMicros)
            {
                Mark(peer, "it did not join in time", DropReason::Left);
            }
            else if (peer.connection->IsFailed())
            {
                Mark(peer, "its connection failed", DropReason::Left);
            }
        }

        if (state_ == RelayState::Running)
        {
            const std::uint32_t missing = scheduler_->MissingSlots();
            if (missing != 0 && now_ - progressAt_ >= options_.stallTimeoutMicros)
            {
                bool acted = false;
                for (std::uint32_t slot = 0; slot < options_.slotCount && state_ == RelayState::Running; ++slot)
                {
                    if ((missing & (1u << slot)) == 0 || slotDropped_[slot])
                    {
                        continue;
                    }
                    Peer *const peer = PeerOfSlot(slot);
                    if (peer != nullptr && peer->paused && now_ - peer->pausedSince < options_.pausedGraceMicros)
                    {
                        continue;
                    }
                    acted = true;
                    ++stats_.stalls;
                    SlotGone(slot, DropReason::Stalled);
                    if (peer != nullptr)
                    {
                        Mark(*peer, "it stalled", DropReason::Left);
                        peer->slotAccounted = true;
                    }
                }
                if (acted)
                {
                    progressAt_ = now_;
                }
            }
        }
        else if (state_ == RelayState::Stopping)
        {
            const std::uint64_t linger =
                stopReason_ == StopReason::Desync ? options_.desyncLingerMicros : options_.stopLingerMicros;
            if (now_ - stopAt_ >= linger || JoinedCount() == 0)
            {
                EndSession();
            }
        }
    }

    void LockstepRelay::SendBundles()
    {
        if (state_ != RelayState::Running)
        {
            return;
        }
        std::array<std::uint8_t, N2Net::kMaxPacketSize> buffer{};
        for (auto &entry : peers_)
        {
            Peer &peer = entry.second;
            if ((!peer.joined && !(peer.spectator && peer.streaming)) || peer.leaving ||
                now_ - peer.lastSendAt < options_.sendIntervalMicros)
            {
                continue;
            }
            peer.lastSendAt = now_;
            const std::size_t room = peer.connection->MaxUnreliableMessageSize();
            const std::size_t payload = layout_.PayloadBytes();
            const std::size_t overhead = N2Net::InputBundleSize(0, payload);
            const std::size_t fitFrames =
                payload == 0 || room < overhead ? 0 : (std::min(room, buffer.size()) - overhead) / payload;
            const std::size_t maxFrames = std::min<std::size_t>(fitFrames, N2Net::kMaxBundleFrames);
            if (room < overhead || maxFrames == 0)
            {
                continue;
            }
            if (peer.spectator)
            {
                SendSpectatorBundles(peer, buffer, maxFrames);
                continue;
            }
            const std::uint32_t current = scheduler_->CurrentTick();
            bool sentAny = false;
            for (std::uint32_t slot = 0; slot < options_.slotCount; ++slot)
            {
                if (slot == peer.slot)
                {
                    continue;
                }
                N2Net::InputBundle bundle = N2Net::BuildBundleFor(*scheduler_, slot, peer.ack);
                if (slotDropped_[slot])
                {
                    // Nothing at or after the drop tick is the slot's: every peer synthesises it
                    std::size_t keep = 0;
                    for (std::uint32_t tick = bundle.firstTick; keep < bundle.frames.size() && Ahead(dropTick_[slot], tick);
                         ++tick)
                    {
                        ++keep;
                    }
                    bundle.frames.resize(keep);
                }
                if (bundle.frames.size() > maxFrames)
                {
                    bundle.frames.resize(maxFrames);
                }
                if (bundle.frames.empty())
                {
                    continue;
                }
                const std::size_t size = N2Net::EncodeInputBundle(bundle, buffer);
                if (size != 0 && peer.connection->SendUnreliable(std::span<const std::uint8_t>(buffer.data(), size)) ==
                                     N2Net::SendResult::Ok)
                {
                    ++stats_.bundlesSent;
                    sentAny = true;
                }
            }
            // The ack of the client's own frames: its slot, no frames, our CurrentTick
            if (!peer.ackSent || peer.lastAckSent != current || now_ - peer.lastAckSendAt >= kAckIntervalMicros)
            {
                N2Net::InputBundle ack;
                ack.slot = static_cast<std::uint8_t>(peer.slot);
                ack.ackTick = current;
                ack.firstTick = current;
                ack.payloadBytes = static_cast<std::uint8_t>(payload);
                ack.layoutHash = layoutHash_;
                const std::size_t size = N2Net::EncodeInputBundle(ack, buffer);
                if (size != 0 && peer.connection->SendUnreliable(std::span<const std::uint8_t>(buffer.data(), size)) ==
                                     N2Net::SendResult::Ok)
                {
                    ++stats_.bundlesSent;
                    peer.ackSent = true;
                    peer.lastAckSent = current;
                    peer.lastAckSendAt = now_;
                }
            }
            (void)sentAny;
        }
    }

    void LockstepRelay::SendPackets()
    {
        for (auto &entry : peers_)
        {
            Peer &peer = entry.second;
            if (peer.connection->IsFailed())
            {
                continue;
            }
            const N2Net::PeerId id = entry.first;
            (void)peer.connection->Update(now_, [this, id](const std::span<const std::uint8_t> packet)
                                          { (void)transport_.Send(id, packet); });
        }
    }

    void LockstepRelay::ApplyLeavers()
    {
        for (const N2Net::PeerId id : refusedPeers_)
        {
            transport_.Disconnect(id);
        }
        refusedPeers_.clear();

        std::vector<N2Net::PeerId> gone;
        for (auto &entry : peers_)
        {
            Peer &peer = entry.second;
            if (peer.leaving && now_ >= peer.lingerUntil)
            {
                gone.push_back(entry.first);
            }
        }
        for (const N2Net::PeerId id : gone)
        {
            const auto found = peers_.find(id);
            if (found == peers_.end())
            {
                continue;
            }
            Peer &peer = found->second;
            if (peer.joined)
            {
                if (state_ == RelayState::Lobby)
                {
                    slotTaken_[peer.slot] = false;
                }
                else if (state_ == RelayState::Running && !peer.slotAccounted)
                {
                    peer.slotAccounted = true;
                    SlotGone(peer.slot, peer.leaveReason);
                }
            }
            Notify(RelayEvent::Type::ClientDropped, peer.joined ? peer.slot : 0, 0, peer.leaveText);
            transport_.Disconnect(id);
            peers_.erase(found);
        }

        if (state_ == RelayState::Running)
        {
            bool alive = false;
            for (std::uint32_t slot = 0; slot < options_.slotCount; ++slot)
            {
                alive = alive || !slotDropped_[slot];
            }
            if (!alive)
            {
                EndSession();
            }
        }
        else if (state_ == RelayState::Stopping && JoinedCount() == 0)
        {
            EndSession();
        }
    }

    void LockstepRelay::Update(const std::uint64_t nowMicros)
    {
        now_ = nowMicros;
        if (!valid_)
        {
            transport_.Poll([this](const N2Net::TransportEvent &event)
                            {
                                if (event.type == N2Net::TransportEventType::PeerConnected)
                                {
                                    refusedPeers_.push_back(event.peer);
                                }
                            });
            ApplyLeavers();
            return;
        }
        transport_.Poll([this](const N2Net::TransportEvent &event) { HandleEvent(event); });
        if (state_ == RelayState::Running)
        {
            AdvanceSession();
        }
        RunTimers();
        ServeSpectators();
        SendBundles();
        SendPackets();
        ApplyLeavers();
        // A Stop or a drop queued by the leavers goes out now, not a pass later
        SendPackets();
    }
}
