#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "engine/network/n2net/ByteIO.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/Packet.hpp"
#include "engine/network/n2net/SimRandom.hpp"
#include "engine/network/n2net/SimTransport.hpp"

// Helpers shared by the n2net tests: a 64-bit FNV-1a hash for golden traces, deterministic message generators, a packet
// builder for hand-made (valid and hostile) datagrams, and TwoPeers, two Connections wired over a SimNetwork.

namespace N2Engine::N2Net::Testing
{
    inline constexpr std::uint32_t kTestProtocolId = 0x4E32'4E54; // "N2NT"
    inline constexpr std::uint32_t kTestConnectionId = 0x0000'0707;

    inline constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;

    inline std::uint64_t HashBytes(std::uint64_t hash, std::span<const std::uint8_t> bytes)
    {
        for (std::uint8_t byte : bytes)
        {
            hash = (hash ^ byte) * 1099511628211ull;
        }
        return hash;
    }

    inline std::uint64_t HashU64(std::uint64_t hash, std::uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8)
        {
            hash = (hash ^ ((value >> shift) & 0xFFu)) * 1099511628211ull;
        }
        return hash;
    }

    /// `size` bytes derived from `index` alone (the first four, when there are four, are the index)
    inline std::vector<std::uint8_t> MakeMessage(std::uint32_t index, std::size_t size)
    {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t i = 0; i < size; ++i)
        {
            bytes[i] = static_cast<std::uint8_t>((index * 31u + static_cast<std::uint32_t>(i) * 7u + (i >> 8)) & 0xFFu);
        }
        for (std::size_t i = 0; i < size && i < 4; ++i)
        {
            bytes[i] = static_cast<std::uint8_t>((index >> (8 * i)) & 0xFFu);
        }
        return bytes;
    }

    inline std::uint32_t MessageIndex(std::span<const std::uint8_t> message)
    {
        std::uint32_t index = 0;
        for (std::size_t i = 0; i < message.size() && i < 4; ++i)
        {
            index |= static_cast<std::uint32_t>(message[i]) << (8 * i);
        }
        return index;
    }

    inline ConnectionConfig TestConfig()
    {
        ConnectionConfig config;
        config.protocolId = kTestProtocolId;
        config.connectionId = kTestConnectionId;
        // The tests queue hundreds of messages at once; the default queue cap is its own test
        config.maxSendQueueBytes = 16u * 1024u * 1024u;
        return config;
    }

    /// Builds a datagram by hand, valid or not: header fields are public and blocks are appended as given
    class PacketBuilder
    {
    public:
        explicit PacketBuilder(std::uint16_t sequence)
        {
            header_.protocolId = kTestProtocolId;
            header_.connectionId = kTestConnectionId;
            header_.sequence = sequence;
        }

        PacketBuilder &Ack(std::uint16_t ack, std::uint32_t ackBits)
        {
            header_.hasAck = true;
            header_.ack = ack;
            header_.ackBits = ackBits;
            return *this;
        }

        PacketBuilder &Protocol(std::uint32_t id)
        {
            header_.protocolId = id;
            return *this;
        }

        PacketBuilder &ConnectionId(std::uint32_t id)
        {
            header_.connectionId = id;
            return *this;
        }

        PacketBuilder &Unit(std::uint16_t id, std::uint8_t flags, std::span<const std::uint8_t> data)
        {
            Raw(kBlockReliableUnit, id, flags, static_cast<std::uint16_t>(data.size()), data);
            return *this;
        }

        PacketBuilder &Unreliable(std::uint16_t sequence, std::span<const std::uint8_t> data)
        {
            body_.push_back(kBlockUnreliable);
            Push16(sequence);
            Push16(static_cast<std::uint16_t>(data.size()));
            body_.insert(body_.end(), data.begin(), data.end());
            return *this;
        }

        /// A unit block with an arbitrary claimed length
        PacketBuilder &Raw(std::uint8_t type, std::uint16_t id, std::uint8_t flags, std::uint16_t claimedLength,
                           std::span<const std::uint8_t> data)
        {
            body_.push_back(type);
            Push16(id);
            body_.push_back(flags);
            Push16(claimedLength);
            body_.insert(body_.end(), data.begin(), data.end());
            return *this;
        }

        PacketBuilder &Bytes(std::span<const std::uint8_t> bytes)
        {
            body_.insert(body_.end(), bytes.begin(), bytes.end());
            return *this;
        }

        [[nodiscard]] std::vector<std::uint8_t> Build() const
        {
            std::vector<std::uint8_t> packet(kHeaderSize);
            ByteWriter writer(packet.data(), kHeaderSize);
            WriteHeader(writer, header_);
            packet.insert(packet.end(), body_.begin(), body_.end());
            return packet;
        }

    private:
        void Push16(std::uint16_t value)
        {
            body_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            body_.push_back(static_cast<std::uint8_t>(value >> 8));
        }

        PacketHeader header_;
        std::vector<std::uint8_t> body_;
    };

    struct ReceivedMessage
    {
        MessageChannel channel = MessageChannel::ReliableOrdered;
        std::vector<std::uint8_t> data;
        std::uint64_t at = 0;
    };

    /// One Connection and what it delivered
    struct TestPeer
    {
        SimTransport *transport = nullptr;
        PeerId remote = kInvalidPeer;
        std::unique_ptr<Connection> connection;
        std::vector<ReceivedMessage> received;
        std::vector<PacketStatus> rejected;
    };

    /// Two peers over a SimNetwork. Step() advances the virtual clock, hands each peer what is due and lets each send.
    class TwoPeers
    {
    public:
        TwoPeers(std::uint64_t seed, const SimLinkConfig &link, const ConnectionConfig &configA = TestConfig(),
                 const ConnectionConfig &configB = TestConfig())
            : TwoPeers(seed, link, link, configA, configB)
        {
        }

        TwoPeers(std::uint64_t seed, const SimLinkConfig &aToB, const SimLinkConfig &bToA,
                 const ConnectionConfig &configA, const ConnectionConfig &configB)
            : network_(seed)
        {
            SimTransport &a = network_.AddEndpoint();
            SimTransport &b = network_.AddEndpoint();
            network_.Link(a.Id(), b.Id(), aToB, bToA);
            Setup(peerA, a, b, configA);
            Setup(peerB, b, a, configB);
        }

        void Step(std::uint64_t micros = 1000)
        {
            network_.Advance(micros);
            Pump(peerA);
            Pump(peerB);
            Send(peerA);
            Send(peerB);
        }

        void RunFor(std::uint64_t micros, std::uint64_t stepMicros = 1000)
        {
            for (std::uint64_t elapsed = 0; elapsed < micros; elapsed += stepMicros)
            {
                Step(stepMicros);
            }
        }

        /// Steps until `done()` or `limitMicros` of virtual time has passed; true if done
        bool RunUntil(const std::function<bool()> &done, std::uint64_t limitMicros, std::uint64_t stepMicros = 1000)
        {
            for (std::uint64_t elapsed = 0; elapsed < limitMicros; elapsed += stepMicros)
            {
                if (done())
                {
                    return true;
                }
                Step(stepMicros);
            }
            return done();
        }

        [[nodiscard]] SimNetwork &Network() { return network_; }
        [[nodiscard]] std::uint64_t Now() const { return network_.NowMicros(); }

        TestPeer peerA;
        TestPeer peerB;

    private:
        void Setup(TestPeer &peer, SimTransport &self, SimTransport &other, const ConnectionConfig &config)
        {
            peer.transport = &self;
            peer.remote = other.Id();
            ConnectionConfig adjusted = config;
            adjusted.reliableStream = config.reliableStream || self.IsReliableStream();
            peer.connection = std::make_unique<Connection>(adjusted, network_.NowMicros());
        }

        void Pump(TestPeer &peer)
        {
            const std::uint64_t now = network_.NowMicros();
            peer.transport->Poll(
                [&](const TransportEvent &event)
                {
                    if (event.type != TransportEventType::Datagram || event.peer != peer.remote)
                    {
                        return;
                    }
                    const PacketStatus status = peer.connection->OnPacket(
                        now, event.data,
                        [&](MessageChannel channel, std::span<const std::uint8_t> message)
                        {
                            ReceivedMessage received;
                            received.channel = channel;
                            received.data.assign(message.begin(), message.end());
                            received.at = now;
                            peer.received.push_back(std::move(received));
                        });
                    if (status != PacketStatus::Ok)
                    {
                        peer.rejected.push_back(status);
                    }
                });
        }

        void Send(TestPeer &peer)
        {
            const PeerId remote = peer.remote;
            SimTransport *transport = peer.transport;
            peer.connection->Update(network_.NowMicros(),
                                    [&](std::span<const std::uint8_t> packet) { transport->Send(remote, packet); });
        }

        SimNetwork network_;
    };
} // namespace N2Engine::N2Net::Testing
