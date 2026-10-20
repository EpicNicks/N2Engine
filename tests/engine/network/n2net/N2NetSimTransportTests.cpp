#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "N2NetTestUtil.hpp"
#include "engine/network/n2net/SimTransport.hpp"

// The deterministic network simulator (net-a3): delivery timing, jitter, independent and burst loss, reordering,
// duplication, the bandwidth cap and queue limit, the transport interface's results and events, and determinism: the same
// seed and calls give the same bytes everywhere (a golden trace hashed and compared to a constant).

using namespace N2Engine::N2Net;
using namespace N2Engine::N2Net::Testing;

namespace
{
    struct Delivery
    {
        std::uint64_t at = 0;
        TransportEventType type = TransportEventType::Datagram;
        PeerId peer = kInvalidPeer;
        std::vector<std::uint8_t> data;
    };

    std::vector<Delivery> Drain(SimNetwork &network, SimTransport &endpoint)
    {
        std::vector<Delivery> out;
        const std::uint64_t now = network.NowMicros();
        endpoint.Poll(
            [&](const TransportEvent &event)
            {
                Delivery delivery;
                delivery.at = now;
                delivery.type = event.type;
                delivery.peer = event.peer;
                delivery.data.assign(event.data.begin(), event.data.end());
                out.push_back(std::move(delivery));
            });
        return out;
    }

    std::vector<Delivery> Datagrams(const std::vector<Delivery> &all)
    {
        std::vector<Delivery> out;
        for (const Delivery &delivery : all)
        {
            if (delivery.type == TransportEventType::Datagram)
            {
                out.push_back(delivery);
            }
        }
        return out;
    }

    /// Sends `count` datagrams a -> b, one per `spacingMicros`, each carrying its index; returns what b received, with
    /// the times, after the network has been run long enough to drain
    std::vector<Delivery> SendStream(SimNetwork &network, SimTransport &a, SimTransport &b, std::uint32_t count,
                                     std::uint64_t spacingMicros, std::uint64_t drainMicros)
    {
        std::vector<Delivery> received;
        const auto step = [&]()
        {
            for (const Delivery &delivery : Drain(network, b))
            {
                if (delivery.type == TransportEventType::Datagram)
                {
                    received.push_back(delivery);
                }
            }
        };
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::vector<std::uint8_t> payload = MakeMessage(i, 8);
            EXPECT_EQ(a.Send(b.Id(), payload), TransportSendResult::Sent);
            network.Advance(spacingMicros);
            step();
        }
        for (std::uint64_t elapsed = 0; elapsed < drainMicros; elapsed += 1000)
        {
            network.Advance(1000);
            step();
        }
        return received;
    }

    std::uint64_t ScenarioHash(std::uint64_t seed, bool withExtraLink)
    {
        SimNetwork network(seed);
        SimTransport &a = network.AddEndpoint();
        SimTransport &b = network.AddEndpoint();
        SimLinkConfig noisy;
        noisy.latencyMicros = 25'000;
        noisy.jitterMicros = 12'000;
        noisy.lossPpm = 80'000;
        noisy.burstEnterPpm = 10'000;
        noisy.burstExitPpm = 150'000;
        noisy.burstLossPpm = 900'000;
        noisy.reorderPpm = 60'000;
        noisy.reorderDelayMicros = 40'000;
        noisy.duplicatePpm = 30'000;
        noisy.bandwidthBytesPerSecond = 200'000;
        noisy.queueLimitBytes = 6000;
        SimLinkConfig quiet;
        quiet.latencyMicros = 5'000;
        quiet.jitterMicros = 2'000;
        network.Link(a.Id(), b.Id(), noisy, quiet);
        SimTransport *c = nullptr;
        if (withExtraLink)
        {
            c = &network.AddEndpoint();
            network.Link(a.Id(), c->Id(), noisy);
        }

        std::uint64_t hash = kFnvOffset;
        const auto observe = [&](SimTransport &endpoint)
        {
            const std::uint64_t now = network.NowMicros();
            endpoint.Poll(
                [&](const TransportEvent &event)
                {
                    // the extra link's traffic is not part of the hash: only a <-> b is compared
                    if (event.peer == 3 || endpoint.Id() == 3)
                    {
                        return;
                    }
                    hash = HashU64(hash, now);
                    hash = HashU64(hash, endpoint.Id());
                    hash = HashU64(hash, static_cast<std::uint64_t>(event.type));
                    hash = HashU64(hash, event.peer);
                    hash = HashBytes(hash, event.data);
                });
        };
        for (std::uint32_t i = 0; i < 3000; ++i)
        {
            const std::size_t size = 1 + (i * 37u) % 1200u;
            const std::vector<std::uint8_t> payload = MakeMessage(i, size);
            a.Send(b.Id(), payload);
            if (i % 3 == 0)
            {
                b.Send(a.Id(), MakeMessage(i + 100000, 1 + (i * 11u) % 300u));
            }
            if (c != nullptr)
            {
                a.Send(c->Id(), payload);
            }
            network.Advance(1000);
            observe(a);
            observe(b);
            if (c != nullptr)
            {
                observe(*c);
            }
        }
        for (int i = 0; i < 400; ++i)
        {
            network.Advance(1000);
            observe(a);
            observe(b);
            if (c != nullptr)
            {
                observe(*c);
            }
        }
        const SimLinkStats *stats = network.GetLinkStats(a.Id(), b.Id());
        hash = HashU64(hash, stats->submitted);
        hash = HashU64(hash, stats->droppedByLoss);
        hash = HashU64(hash, stats->droppedByQueueLimit);
        hash = HashU64(hash, stats->duplicated);
        hash = HashU64(hash, stats->reordered);
        hash = HashU64(hash, stats->delivered);
        return hash;
    }
} // namespace

TEST(N2NetSimTransportTest, PerfectLinkDeliversExactlyAtTheLatency)
{
    SimNetwork network(1);
    SimTransport &a = network.AddEndpoint();
    SimTransport &b = network.AddEndpoint();
    SimLinkConfig link;
    link.latencyMicros = 30'000;
    ASSERT_TRUE(network.Link(a.Id(), b.Id(), link));

    // Both sides are told about the link, at once
    const std::vector<Delivery> connectedA = Drain(network, a);
    const std::vector<Delivery> connectedB = Drain(network, b);
    ASSERT_EQ(connectedA.size(), 1u);
    ASSERT_EQ(connectedB.size(), 1u);
    EXPECT_EQ(connectedA[0].type, TransportEventType::PeerConnected);
    EXPECT_EQ(connectedA[0].peer, b.Id());
    EXPECT_EQ(connectedB[0].type, TransportEventType::PeerConnected);
    EXPECT_EQ(connectedB[0].peer, a.Id());

    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5};
    ASSERT_EQ(a.Send(b.Id(), payload), TransportSendResult::Sent);
    network.Advance(29'999);
    EXPECT_TRUE(Drain(network, b).empty());
    network.Advance(1);
    const std::vector<Delivery> arrived = Drain(network, b);
    ASSERT_EQ(arrived.size(), 1u);
    EXPECT_EQ(arrived[0].type, TransportEventType::Datagram);
    EXPECT_EQ(arrived[0].peer, a.Id());
    EXPECT_EQ(arrived[0].data, payload);
    EXPECT_EQ(arrived[0].at, 30'000u);
    // the sender hears nothing
    EXPECT_TRUE(Drain(network, a).empty());
}

TEST(N2NetSimTransportTest, SendResultsAndLimits)
{
    SimNetwork network(1);
    SimTransport &a = network.AddEndpoint();
    SimTransport &b = network.AddEndpoint();
    SimTransport &stranger = network.AddEndpoint();
    network.Link(a.Id(), b.Id(), SimLinkConfig{});

    EXPECT_EQ(a.MaxPacketSize(), 1200u);
    EXPECT_FALSE(a.IsReliableStream());
    const std::vector<std::uint8_t> full(1200, 7);
    const std::vector<std::uint8_t> over(1201, 7);
    EXPECT_EQ(a.Send(b.Id(), full), TransportSendResult::Sent);
    EXPECT_EQ(a.Send(b.Id(), over), TransportSendResult::PacketTooLarge);
    EXPECT_EQ(a.Send(b.Id(), std::span<const std::uint8_t>()), TransportSendResult::EmptyPacket);
    EXPECT_EQ(a.Send(stranger.Id(), full), TransportSendResult::UnknownPeer);
    EXPECT_EQ(a.Send(kInvalidPeer, full), TransportSendResult::UnknownPeer);
    EXPECT_EQ(a.Send(999, full), TransportSendResult::UnknownPeer);

    EXPECT_FALSE(network.Link(a.Id(), a.Id(), SimLinkConfig{})); // not with itself
    EXPECT_FALSE(network.Link(a.Id(), b.Id(), SimLinkConfig{})); // not twice
    EXPECT_FALSE(network.Link(a.Id(), 999, SimLinkConfig{}));    // not with nothing
    EXPECT_EQ(network.GetEndpoint(0), nullptr);
    EXPECT_EQ(network.GetEndpoint(a.Id()), &a);
}

TEST(N2NetSimTransportTest, DisconnectTellsThePeerAndDropsWhatIsInFlight)
{
    SimNetwork network(1);
    SimTransport &a = network.AddEndpoint();
    SimTransport &b = network.AddEndpoint();
    SimLinkConfig link;
    link.latencyMicros = 10'000;
    network.Link(a.Id(), b.Id(), link);
    Drain(network, a);
    Drain(network, b);

    const std::vector<std::uint8_t> payload{9};
    ASSERT_EQ(a.Send(b.Id(), payload), TransportSendResult::Sent);
    a.Disconnect(b.Id());
    EXPECT_EQ(a.Send(b.Id(), payload), TransportSendResult::UnknownPeer);
    EXPECT_EQ(b.Send(a.Id(), payload), TransportSendResult::UnknownPeer);

    network.Advance(20'000);
    const std::vector<Delivery> events = Drain(network, b);
    ASSERT_EQ(events.size(), 1u); // the disconnect, not the datagram that was in flight
    EXPECT_EQ(events[0].type, TransportEventType::PeerDisconnected);
    EXPECT_EQ(events[0].peer, a.Id());
    EXPECT_EQ(network.GetLinkStats(a.Id(), b.Id())->droppedLinkGone, 1u);

    a.Disconnect(b.Id()); // nothing left to drop: no second event
    EXPECT_TRUE(Drain(network, b).empty());

    // the pair can be linked again
    EXPECT_TRUE(network.Link(a.Id(), b.Id(), link));
    EXPECT_EQ(a.Send(b.Id(), payload), TransportSendResult::Sent);
}

TEST(N2NetSimTransportTest, JitterStaysInsideItsBound)
{
    SimNetwork network(11);
    SimTransport &a = network.AddEndpoint();
    SimTransport &b = network.AddEndpoint();
    SimLinkConfig link;
    link.latencyMicros = 10'000;
    link.jitterMicros = 5'000;
    network.Link(a.Id(), b.Id(), link);

    // one datagram at a time so each arrival time is its own
    std::uint64_t smallest = ~0ull;
    std::uint64_t largest = 0;
    for (int i = 0; i < 2000; ++i)
    {
        const std::uint64_t sentAt = network.NowMicros();
        a.Send(b.Id(), MakeMessage(static_cast<std::uint32_t>(i), 4));
        std::uint64_t arrivedAt = 0;
        while (arrivedAt == 0)
        {
            network.Advance(100);
            for (const Delivery &delivery : Datagrams(Drain(network, b)))
            {
                arrivedAt = delivery.at;
            }
        }
        const std::uint64_t delay = arrivedAt - sentAt;
        smallest = std::min(smallest, delay);
        largest = std::max(largest, delay);
    }
    // 100 us steps round the arrival up to the next step
    EXPECT_GE(smallest, 10'000u);
    EXPECT_LE(largest, 15'100u);
    EXPECT_GT(largest - smallest, 3000u); // the jitter is really used
}

TEST(N2NetSimTransportTest, IndependentLossMatchesItsRate)
{
    SimNetwork network(21);
    SimTransport &a = network.AddEndpoint();
    SimTransport &b = network.AddEndpoint();
    SimLinkConfig link;
    link.latencyMicros = 1000;
    link.lossPpm = 200'000;
    network.Link(a.Id(), b.Id(), link);

    const std::vector<Delivery> received = SendStream(network, a, b, 20000, 100, 5000);
    const SimLinkStats *stats = network.GetLinkStats(a.Id(), b.Id());
    EXPECT_EQ(stats->submitted, 20000u);
    EXPECT_EQ(stats->droppedByLoss + received.size(), 20000u);
    EXPECT_EQ(stats->delivered, received.size());
    const double rate = static_cast<double>(stats->droppedByLoss) / 20000.0;
    EXPECT_GT(rate, 0.19);
    EXPECT_LT(rate, 0.21);
}

TEST(N2NetSimTransportTest, BurstLossComesInRuns)
{
    const auto lossRuns = [](const SimLinkConfig &link)
    {
        SimNetwork network(31);
        SimTransport &a = network.AddEndpoint();
        SimTransport &b = network.AddEndpoint();
        network.Link(a.Id(), b.Id(), link);
        const std::vector<Delivery> received = SendStream(network, a, b, 30000, 100, 5000);
        std::vector<bool> arrived(30000, false);
        for (const Delivery &delivery : received)
        {
            arrived[MessageIndex(delivery.data)] = true;
        }
        std::uint64_t lost = 0;
        std::uint64_t runs = 0;
        bool inRun = false;
        for (bool ok : arrived)
        {
            if (!ok)
            {
                ++lost;
                if (!inRun)
                {
                    ++runs;
                }
            }
            inRun = !ok;
        }
        return std::pair<double, double>(static_cast<double>(lost) / 30000.0,
                                         runs == 0 ? 0.0 : static_cast<double>(lost) / static_cast<double>(runs));
    };

    SimLinkConfig burst;
    burst.latencyMicros = 1000;
    burst.burstEnterPpm = 20'000;  // 2 % chance per datagram to start a burst
    burst.burstExitPpm = 200'000;  // a burst lasts 5 datagrams on average
    burst.burstLossPpm = 1'000'000;
    const auto [burstRate, burstRunLength] = lossRuns(burst);

    SimLinkConfig independent;
    independent.latencyMicros = 1000;
    independent.lossPpm = 90'000; // about the same overall rate
    const auto [independentRate, independentRunLength] = lossRuns(independent);

    EXPECT_GT(burstRate, 0.05);
    EXPECT_LT(burstRate, 0.14);
    EXPECT_NEAR(independentRate, 0.09, 0.015);
    EXPECT_GT(burstRunLength, 3.5);
    EXPECT_LT(independentRunLength, 1.3);
}

TEST(N2NetSimTransportTest, ReorderingOvertakesAndNothingIsLost)
{
    const auto inversions = [](const SimLinkConfig &link)
    {
        SimNetwork network(41);
        SimTransport &a = network.AddEndpoint();
        SimTransport &b = network.AddEndpoint();
        network.Link(a.Id(), b.Id(), link);
        const std::vector<Delivery> received = SendStream(network, a, b, 3000, 1000, 200'000);
        EXPECT_EQ(received.size(), 3000u);
        std::uint64_t count = 0;
        for (std::size_t i = 1; i < received.size(); ++i)
        {
            if (MessageIndex(received[i].data) < MessageIndex(received[i - 1].data))
            {
                ++count;
            }
        }
        return count;
    };

    SimLinkConfig ordered;
    ordered.latencyMicros = 10'000;
    EXPECT_EQ(inversions(ordered), 0u);

    SimLinkConfig reordering = ordered;
    reordering.reorderPpm = 100'000;
    reordering.reorderDelayMicros = 50'000;
    const std::uint64_t reordered = inversions(reordering);
    EXPECT_GT(reordered, 100u);
    EXPECT_LT(reordered, 1500u);

    // a reorder probability with no delay to apply does nothing
    SimLinkConfig noDelay = ordered;
    noDelay.reorderPpm = 500'000;
    EXPECT_EQ(inversions(noDelay), 0u);
}

TEST(N2NetSimTransportTest, DuplicationDeliversExtraCopies)
{
    SimNetwork network(51);
    SimTransport &a = network.AddEndpoint();
    SimTransport &b = network.AddEndpoint();
    SimLinkConfig link;
    link.latencyMicros = 2000;
    link.jitterMicros = 1000;
    link.duplicatePpm = 100'000;
    network.Link(a.Id(), b.Id(), link);

    const std::vector<Delivery> received = SendStream(network, a, b, 5000, 100, 10'000);
    const SimLinkStats *stats = network.GetLinkStats(a.Id(), b.Id());
    EXPECT_EQ(received.size(), 5000u + stats->duplicated);
    EXPECT_GT(stats->duplicated, 400u);
    EXPECT_LT(stats->duplicated, 600u);
    // every original arrives at least once
    std::vector<int> copies(5000, 0);
    for (const Delivery &delivery : received)
    {
        ++copies[MessageIndex(delivery.data)];
    }
    for (int count : copies)
    {
        EXPECT_GE(count, 1);
        EXPECT_LE(count, 2);
    }
}

TEST(N2NetSimTransportTest, BandwidthCapSerialisesDatagrams)
{
    SimNetwork network(61);
    SimTransport &a = network.AddEndpoint();
    SimTransport &b = network.AddEndpoint();
    SimLinkConfig link;
    link.bandwidthBytesPerSecond = 100'000; // 1000 bytes take 10 ms
    network.Link(a.Id(), b.Id(), link);

    const std::vector<std::uint8_t> payload(1000, 3);
    for (int i = 0; i < 5; ++i)
    {
        ASSERT_EQ(a.Send(b.Id(), payload), TransportSendResult::Sent);
    }
    std::vector<std::uint64_t> arrivals;
    for (int ms = 0; ms <= 60; ++ms)
    {
        for (const Delivery &delivery : Datagrams(Drain(network, b)))
        {
            arrivals.push_back(delivery.at);
        }
        network.Advance(1000);
    }
    EXPECT_EQ(arrivals, (std::vector<std::uint64_t>{10'000, 20'000, 30'000, 40'000, 50'000}));
}

TEST(N2NetSimTransportTest, QueueLimitTailDrops)
{
    SimNetwork network(71);
    SimTransport &a = network.AddEndpoint();
    SimTransport &b = network.AddEndpoint();
    SimLinkConfig link;
    link.bandwidthBytesPerSecond = 100'000;
    link.queueLimitBytes = 2500;
    network.Link(a.Id(), b.Id(), link);

    const std::vector<std::uint8_t> payload(1000, 3);
    for (int i = 0; i < 10; ++i)
    {
        a.Send(b.Id(), payload);
    }
    network.Advance(200'000);
    // The first finds 0 bytes waiting, the second 1000 (2000 with itself, within 2500), the third 2000 (3000 with itself:
    // over the limit), and the queue never drains while the rest are sent in the same instant
    EXPECT_EQ(Datagrams(Drain(network, b)).size(), 2u);
    const SimLinkStats *stats = network.GetLinkStats(a.Id(), b.Id());
    EXPECT_EQ(stats->droppedByQueueLimit, 8u);
    EXPECT_EQ(stats->delivered, 2u);
}

TEST(N2NetSimTransportTest, ReconfiguringADirectionTakesEffectForLaterDatagrams)
{
    SimNetwork network(81);
    SimTransport &a = network.AddEndpoint();
    SimTransport &b = network.AddEndpoint();
    network.Link(a.Id(), b.Id(), SimLinkConfig{});
    const std::vector<std::uint8_t> payload{1};
    a.Send(b.Id(), payload);
    SimLinkConfig dead;
    dead.lossPpm = 1'000'000;
    ASSERT_TRUE(network.SetLinkConfig(a.Id(), b.Id(), dead));
    a.Send(b.Id(), payload);
    EXPECT_EQ(Datagrams(Drain(network, b)).size(), 1u);
    EXPECT_FALSE(network.SetLinkConfig(a.Id(), 999, dead));
}

TEST(N2NetSimTransportTest, SameSeedSameBytesDifferentSeedDifferent)
{
    const std::uint64_t first = ScenarioHash(12345, false);
    EXPECT_EQ(first, ScenarioHash(12345, false));
    EXPECT_NE(first, ScenarioHash(12346, false));
}

TEST(N2NetSimTransportTest, AnotherLinkDoesNotDisturbThisOne)
{
    // Each link direction has its own random stream: traffic on a third endpoint changes nothing between a and b
    EXPECT_EQ(ScenarioHash(777, false), ScenarioHash(777, true));
}

TEST(N2NetSimTransportTest, AnImpairmentDoesNotShiftAnothersDecisions)
{
    // Every datagram draws the same random numbers whatever the config, so turning duplication on changes no
    // datagram's loss
    const auto lossPattern = [](std::uint32_t duplicatePpm)
    {
        SimNetwork network(91);
        SimTransport &a = network.AddEndpoint();
        SimTransport &b = network.AddEndpoint();
        SimLinkConfig link;
        link.latencyMicros = 1000;
        link.lossPpm = 300'000;
        link.duplicatePpm = duplicatePpm;
        network.Link(a.Id(), b.Id(), link);
        const std::vector<Delivery> received = SendStream(network, a, b, 4000, 100, 5000);
        std::vector<bool> arrived(4000, false);
        for (const Delivery &delivery : received)
        {
            arrived[MessageIndex(delivery.data)] = true;
        }
        return arrived;
    };
    EXPECT_EQ(lossPattern(0), lossPattern(500'000));
}

TEST(N2NetSimTransportTest, GoldenTraceIsTheSameOnEveryPlatform)
{
    // A noisy two-way link (loss, bursts, reorder, duplication, jitter, a bandwidth cap and queue limit) driven for
    // 3.4 s of virtual time, every delivery hashed. The constant must come out identically on every CI leg: Windows
    // MSVC, Linux GCC and Clang on x86_64 and arm64, and the web build under node.
    EXPECT_EQ(ScenarioHash(20261020, false), 0xF90BBB84793134BCULL);
}
