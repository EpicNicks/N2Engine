#include "engine/network/n2net/SimTransport.hpp"

#include <algorithm>
#include <limits>

namespace N2Engine::N2Net
{
    namespace
    {
        constexpr std::uint32_t kPpmScale = 1'000'000;
        constexpr std::uint64_t kMicrosPerSecond = 1'000'000;

        /// NextBelow takes a 32-bit bound: a count of values that does not fit is clamped to what does
        std::uint32_t ClampBound(std::uint64_t count) noexcept
        {
            return count > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                                     : static_cast<std::uint32_t>(count);
        }

        /// The number of values in [0, maxInclusive], clamped
        std::uint32_t ValuesUpTo(std::uint64_t maxInclusive) noexcept
        {
            return ClampBound(std::min<std::uint64_t>(maxInclusive, std::numeric_limits<std::uint32_t>::max()) + 1);
        }
    } // namespace

    // ---- SimTransport ----

    TransportSendResult SimTransport::Send(PeerId peer, std::span<const std::uint8_t> datagram)
    {
        return network_->Submit(self_, peer, datagram);
    }

    void SimTransport::Poll(EventHandler handler)
    {
        while (!inbox_.empty())
        {
            auto first = inbox_.begin();
            if (first->first.first > network_->now_)
            {
                break;
            }
            // Take the item out before calling the handler: the handler may Send, which can add to this inbox
            Item item = std::move(first->second);
            inbox_.erase(first);

            if (item.type == TransportEventType::Datagram)
            {
                const auto direction = network_->directions_.find({item.peer, self_});
                if (direction == network_->directions_.end() || !direction->second.active ||
                    direction->second.generation != item.generation)
                {
                    if (direction != network_->directions_.end())
                    {
                        ++direction->second.stats.droppedLinkGone;
                    }
                    continue;
                }
                ++direction->second.stats.delivered;
            }

            TransportEvent event;
            event.type = item.type;
            event.peer = item.peer;
            event.data = std::span<const std::uint8_t>(item.data.data(), item.data.size());
            handler(event);
        }
    }

    void SimTransport::Disconnect(PeerId peer)
    {
        if (network_->Unlink(self_, peer))
        {
            network_->Enqueue(peer, network_->now_, TransportEventType::PeerDisconnected, self_, {});
        }
    }

    bool SimTransport::GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const
    {
        return peer != self_ && network_->EndpointAddress(peer, endpoint);
    }

    // ---- SimNetwork ----

    SimNetwork::SimNetwork(std::uint64_t seed) : seed_(seed) {}

    SimNetwork::~SimNetwork() = default;

    SimTransport &SimNetwork::AddEndpoint()
    {
        const PeerId id = static_cast<PeerId>(endpoints_.size() + 1);
        endpoints_.push_back(std::unique_ptr<SimTransport>(new SimTransport(this, id)));
        return *endpoints_.back();
    }

    bool SimNetwork::SetEndpointAddress(PeerId id, const PeerEndpoint &endpoint)
    {
        if (id == kInvalidPeer || id > endpoints_.size())
        {
            return false;
        }
        addresses_[id] = endpoint;
        return true;
    }

    bool SimNetwork::EndpointAddress(PeerId id, PeerEndpoint &endpoint) const
    {
        if (id == kInvalidPeer || id > endpoints_.size())
        {
            return false;
        }
        const auto found = addresses_.find(id);
        if (found != addresses_.end())
        {
            endpoint = found->second;
            return true;
        }
        endpoint = PeerEndpoint{};
        endpoint.ipv6 = false;
        endpoint.bytes[0] = 10;
        endpoint.bytes[1] = static_cast<std::uint8_t>((id >> 16) & 0xFF);
        endpoint.bytes[2] = static_cast<std::uint8_t>((id >> 8) & 0xFF);
        endpoint.bytes[3] = static_cast<std::uint8_t>(id & 0xFF);
        endpoint.port = 7777;
        return true;
    }

    SimTransport *SimNetwork::GetEndpoint(PeerId id) noexcept
    {
        if (id == kInvalidPeer || id > endpoints_.size())
        {
            return nullptr;
        }
        return endpoints_[id - 1].get();
    }

    bool SimNetwork::Link(PeerId a, PeerId b, const SimLinkConfig &aToB, const SimLinkConfig &bToA)
    {
        if (a == b || GetEndpoint(a) == nullptr || GetEndpoint(b) == nullptr)
        {
            return false;
        }
        const auto existing = directions_.find({a, b});
        if (existing != directions_.end() && existing->second.active)
        {
            return false;
        }

        const auto activate = [this](PeerId from, PeerId to, const SimLinkConfig &config)
        {
            const auto found = directions_.find({from, to});
            if (found == directions_.end())
            {
                Direction direction(config, SimRandom::ForStream(seed_, from, to));
                direction.active = true;
                direction.generation = 1;
                directions_.emplace(std::make_pair(from, to), std::move(direction));
            }
            else
            {
                found->second.config = config;
                found->second.active = true;
                ++found->second.generation;
                found->second.inBurst = false;
                found->second.nextFreeAt = 0;
            }
        };
        activate(a, b, aToB);
        activate(b, a, bToA);

        Enqueue(a, now_, TransportEventType::PeerConnected, b, {});
        Enqueue(b, now_, TransportEventType::PeerConnected, a, {});
        return true;
    }

    bool SimNetwork::Link(PeerId a, PeerId b, const SimLinkConfig &bothDirections)
    {
        return Link(a, b, bothDirections, bothDirections);
    }

    bool SimNetwork::Unlink(PeerId a, PeerId b)
    {
        bool removed = false;
        for (const auto &key : {std::make_pair(a, b), std::make_pair(b, a)})
        {
            const auto found = directions_.find(key);
            if (found != directions_.end() && found->second.active)
            {
                found->second.active = false;
                removed = true;
            }
        }
        return removed;
    }

    bool SimNetwork::SetLinkConfig(PeerId from, PeerId to, const SimLinkConfig &config)
    {
        const auto found = directions_.find({from, to});
        if (found == directions_.end() || !found->second.active)
        {
            return false;
        }
        found->second.config = config;
        return true;
    }

    const SimLinkStats *SimNetwork::GetLinkStats(PeerId from, PeerId to) const noexcept
    {
        const auto found = directions_.find({from, to});
        return found == directions_.end() ? nullptr : &found->second.stats;
    }

    void SimNetwork::Enqueue(PeerId to, std::uint64_t deliverAt, TransportEventType type, PeerId peer,
                             std::span<const std::uint8_t> data)
    {
        SimTransport *endpoint = GetEndpoint(to);
        if (endpoint == nullptr)
        {
            return;
        }
        SimTransport::Item item;
        item.type = type;
        item.peer = peer;
        if (type == TransportEventType::Datagram)
        {
            const auto link = directions_.find({peer, to});
            item.generation = link == directions_.end() ? 0 : link->second.generation;
        }
        item.data.assign(data.begin(), data.end());
        endpoint->inbox_.emplace(std::make_pair(deliverAt, arrivalCounter_++), std::move(item));
    }

    TransportSendResult SimNetwork::Submit(PeerId from, PeerId to, std::span<const std::uint8_t> datagram)
    {
        const auto found = directions_.find({from, to});
        if (found == directions_.end() || !found->second.active)
        {
            return TransportSendResult::UnknownPeer;
        }
        if (datagram.empty())
        {
            return TransportSendResult::EmptyPacket;
        }
        if (datagram.size() > kMaxPacketSize)
        {
            return TransportSendResult::PacketTooLarge;
        }

        Direction &direction = found->second;
        const SimLinkConfig &config = direction.config;
        ++direction.stats.submitted;
        direction.stats.bytesSubmitted += datagram.size();

        // Seven draws per datagram, always in this order, whatever the config and whatever the datagram's fate
        const std::uint32_t drawBurst = direction.random.NextBelow(kPpmScale);
        const std::uint32_t drawLoss = direction.random.NextBelow(kPpmScale);
        const std::uint32_t drawJitter = direction.random.NextBelow(ValuesUpTo(config.jitterMicros));
        const std::uint32_t drawReorder = direction.random.NextBelow(kPpmScale);
        const std::uint32_t drawReorderDelay = direction.random.NextBelow(ClampBound(config.reorderDelayMicros));
        const std::uint32_t drawDuplicate = direction.random.NextBelow(kPpmScale);
        const std::uint32_t drawDuplicateJitter = direction.random.NextBelow(ValuesUpTo(config.jitterMicros));

        // Bandwidth cap: the datagram waits for those before it, then takes bytes/rate to go out
        std::uint64_t leavesAt = now_;
        if (config.bandwidthBytesPerSecond > 0)
        {
            const std::uint64_t backlogMicros = direction.nextFreeAt > now_ ? direction.nextFreeAt - now_ : 0;
            if (config.queueLimitBytes > 0)
            {
                const std::uint64_t backlogBytes = backlogMicros * config.bandwidthBytesPerSecond / kMicrosPerSecond;
                if (backlogBytes + datagram.size() > config.queueLimitBytes)
                {
                    ++direction.stats.droppedByQueueLimit;
                    return TransportSendResult::Sent;
                }
            }
            const std::uint64_t start = std::max(now_, direction.nextFreeAt);
            const std::uint64_t serialisation =
                (datagram.size() * kMicrosPerSecond + config.bandwidthBytesPerSecond - 1) / config.bandwidthBytesPerSecond;
            direction.nextFreeAt = start + serialisation;
            leavesAt = direction.nextFreeAt;
        }

        // Loss: the burst state moves first, then the datagram meets the loss rate of the state it is in
        if (direction.inBurst)
        {
            if (drawBurst < config.burstExitPpm)
            {
                direction.inBurst = false;
            }
        }
        else if (drawBurst < config.burstEnterPpm)
        {
            direction.inBurst = true;
        }
        const std::uint32_t lossPpm = direction.inBurst ? config.burstLossPpm : config.lossPpm;
        if (drawLoss < lossPpm)
        {
            ++direction.stats.droppedByLoss;
            return TransportSendResult::Sent;
        }

        std::uint64_t delay = config.latencyMicros + drawJitter;
        if (config.reorderDelayMicros > 0 && drawReorder < config.reorderPpm)
        {
            delay += 1 + drawReorderDelay;
            ++direction.stats.reordered;
        }
        Enqueue(to, leavesAt + delay, TransportEventType::Datagram, from, datagram);

        if (drawDuplicate < config.duplicatePpm)
        {
            ++direction.stats.duplicated;
            Enqueue(to, leavesAt + config.latencyMicros + drawDuplicateJitter, TransportEventType::Datagram, from,
                    datagram);
        }
        return TransportSendResult::Sent;
    }
} // namespace N2Engine::N2Net
