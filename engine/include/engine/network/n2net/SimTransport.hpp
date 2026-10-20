#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "engine/network/n2net/SimRandom.hpp"
#include "engine/network/n2net/Transport.hpp"

namespace N2Engine::N2Net
{
    class SimNetwork;

    /// What one direction of a simulated link does to each datagram. All times are virtual microseconds, all
    /// probabilities are parts per million (1,000,000 = always), so the simulation is integer-only. The defaults are a
    /// perfect link.
    struct SimLinkConfig
    {
        /// Fixed one-way delay
        std::uint64_t latencyMicros = 0;
        /// Plus a uniform extra delay in [0, jitterMicros]
        std::uint64_t jitterMicros = 0;

        /// Independent loss
        std::uint32_t lossPpm = 0;
        /// Burst loss (a two-state model): while in the good state a datagram moves the link to the burst state with
        /// probability burstEnterPpm; while in the burst state it moves back with probability burstExitPpm and a
        /// datagram is lost with probability burstLossPpm instead of lossPpm. burstEnterPpm 0 never bursts.
        std::uint32_t burstEnterPpm = 0;
        std::uint32_t burstExitPpm = 0;
        std::uint32_t burstLossPpm = 1'000'000;

        /// With probability reorderPpm a datagram is delayed by a further 1 to reorderDelayMicros, so later datagrams
        /// can overtake it (a reorderDelayMicros of 0 delays by nothing: reordering then comes only from jitter)
        std::uint32_t reorderPpm = 0;
        std::uint64_t reorderDelayMicros = 0;

        /// With probability duplicatePpm a second copy is delivered, with its own jitter
        std::uint32_t duplicatePpm = 0;

        /// 0 = unlimited. Otherwise the link serialises datagrams at this rate: each waits for the ones before it
        /// (bytes * 1,000,000 / rate microseconds each), added to its delay
        std::uint64_t bandwidthBytesPerSecond = 0;
        /// With a bandwidth cap: a datagram that would find more than this many bytes already waiting is dropped
        /// (tail drop). 0 = unlimited.
        std::uint32_t queueLimitBytes = 0;
    };

    /// Counters of one direction of a link, since it was created
    struct SimLinkStats
    {
        std::uint64_t submitted = 0;
        std::uint64_t bytesSubmitted = 0;
        std::uint64_t droppedByLoss = 0;
        std::uint64_t droppedByQueueLimit = 0;
        /// Dropped at delivery because the link was removed while they were in flight
        std::uint64_t droppedLinkGone = 0;
        std::uint64_t duplicated = 0;
        std::uint64_t reordered = 0;
        std::uint64_t delivered = 0;
    };

    /// One endpoint of a SimNetwork: an ITransport whose peers are the other endpoints it is linked to (a PeerId is the
    /// other endpoint's id). Created and owned by the SimNetwork.
    class SimTransport final : public ITransport
    {
    public:
        /// This endpoint's own id: what the peers see as its PeerId
        [[nodiscard]] PeerId Id() const noexcept { return self_; }

        [[nodiscard]] std::size_t MaxPacketSize() const noexcept override { return kMaxPacketSize; }
        [[nodiscard]] bool IsReliableStream() const noexcept override { return false; }
        TransportSendResult Send(PeerId peer, std::span<const std::uint8_t> datagram) override;
        /// Delivers what is due at the network's virtual time (the network's Advance moves time; Poll never does)
        void Poll(EventHandler handler) override;
        void Disconnect(PeerId peer) override;

    private:
        friend class SimNetwork;
        SimTransport(SimNetwork *network, PeerId self) noexcept : network_(network), self_(self) {}

        struct Item
        {
            TransportEventType type = TransportEventType::Datagram;
            PeerId peer = kInvalidPeer;
            /// For datagrams: the link generation they were sent on (a re-linked pair is a new link)
            std::uint64_t generation = 0;
            std::vector<std::uint8_t> data;
        };

        SimNetwork *network_;
        PeerId self_;
        /// Keyed by (delivery time, global arrival counter): delivery order is fully determined, ties broken by the
        /// order the datagrams were sent
        std::map<std::pair<std::uint64_t, std::uint64_t>, Item> inbox_;
    };

    /// A deterministic in-memory network for tests: endpoints, links with impairments, and an explicit virtual clock.
    /// Nothing reads a wall clock and nothing is random except SimRandom streams derived from the seed, so the same
    /// seed and the same sequence of calls give byte-identical deliveries on every platform. Single-threaded.
    ///
    /// Time only moves in Advance. Datagrams are scheduled when sent and handed out by SimTransport::Poll once the
    /// clock has reached their delivery time. Every datagram consumes the same random draws whatever the config, so
    /// changing one impairment does not change what another decides for the same datagram.
    class SimNetwork
    {
    public:
        explicit SimNetwork(std::uint64_t seed);
        ~SimNetwork();
        SimNetwork(const SimNetwork &) = delete;
        SimNetwork &operator=(const SimNetwork &) = delete;

        /// A new endpoint, ids 1, 2, 3 ... in creation order. The reference stays valid for the network's life.
        SimTransport &AddEndpoint();
        [[nodiscard]] SimTransport *GetEndpoint(PeerId id) noexcept;

        /// Links two endpoints (each side gets a PeerConnected event). False if either does not exist, they are the
        /// same endpoint, or they are already linked.
        bool Link(PeerId a, PeerId b, const SimLinkConfig &aToB, const SimLinkConfig &bToA);
        bool Link(PeerId a, PeerId b, const SimLinkConfig &bothDirections);
        /// Removes the link (the other side is not told: use SimTransport::Disconnect for that). Datagrams in flight
        /// on it are dropped when due, even if the pair is linked again before then.
        bool Unlink(PeerId a, PeerId b);
        /// Replaces one direction's config; its state (burst, serialisation queue) and random stream continue
        bool SetLinkConfig(PeerId from, PeerId to, const SimLinkConfig &config);
        [[nodiscard]] const SimLinkStats *GetLinkStats(PeerId from, PeerId to) const noexcept;

        [[nodiscard]] std::uint64_t NowMicros() const noexcept { return now_; }
        void Advance(std::uint64_t micros) noexcept { now_ += micros; }

    private:
        friend class SimTransport;

        struct Direction
        {
            SimLinkConfig config;
            SimRandom random;
            bool active = false;
            std::uint64_t generation = 0;
            bool inBurst = false;
            std::uint64_t nextFreeAt = 0;
            SimLinkStats stats;
            Direction(const SimLinkConfig &cfg, SimRandom rng) : config(cfg), random(rng) {}
        };

        TransportSendResult Submit(PeerId from, PeerId to, std::span<const std::uint8_t> datagram);
        void Enqueue(PeerId to, std::uint64_t deliverAt, TransportEventType type, PeerId peer,
                     std::span<const std::uint8_t> data);

        std::uint64_t seed_;
        std::uint64_t now_ = 0;
        std::uint64_t arrivalCounter_ = 0;
        std::vector<std::unique_ptr<SimTransport>> endpoints_;
        std::map<std::pair<PeerId, PeerId>, Direction> directions_;
    };
} // namespace N2Engine::N2Net
