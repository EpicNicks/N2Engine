#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/network/n2net/FunctionRef.hpp"

namespace N2Engine::N2Net
{
    /// The largest datagram n2net ever sends or accepts, in bytes. Chosen to fit an IPv6 minimum MTU path (1280) with
    /// the IP and UDP headers, and WebRTC data channels. Every ITransport::MaxPacketSize() is at most this.
    inline constexpr std::size_t kMaxPacketSize = 1200;

    /// Identifies the other end of a link on one transport. Meaningful only to the transport that issued it; 0 is
    /// never a valid peer.
    using PeerId = std::uint32_t;
    inline constexpr PeerId kInvalidPeer = 0;

    enum class TransportEventType : std::uint8_t
    {
        /// A link to `peer` is now usable
        PeerConnected,
        /// The link to `peer` is gone (the other side left or the transport dropped it); Send to it now fails
        PeerDisconnected,
        /// One datagram from `peer`, exactly as sent
        Datagram,
    };

    struct TransportEvent
    {
        TransportEventType type = TransportEventType::Datagram;
        PeerId peer = kInvalidPeer;
        /// The datagram for Datagram, empty otherwise. Valid only during the callback: copy what you keep.
        std::span<const std::uint8_t> data;
    };

    enum class TransportSendResult : std::uint8_t
    {
        /// Accepted. A datagram transport may still lose it; that is what the reliable channel is for.
        Sent,
        /// No link to that peer (never connected, or disconnected)
        UnknownPeer,
        /// Larger than MaxPacketSize()
        PacketTooLarge,
        /// Zero bytes
        EmptyPacket,
    };

    /// The seam between the packet layer and a network: datagram-oriented, non-blocking and polled on one thread (the
    /// web build has no threads, so nothing here may block or need one). An implementation never throws across this
    /// interface. This library has no sockets: SimTransport is the in-memory implementation; the native UDP and the
    /// browser WebSocket transports are later work that implement this same interface.
    class ITransport
    {
    public:
        using EventHandler = FunctionRef<void(const TransportEvent &)>;

        virtual ~ITransport() = default;

        /// The largest datagram Send accepts and Poll delivers, at most kMaxPacketSize
        [[nodiscard]] virtual std::size_t MaxPacketSize() const noexcept = 0;

        /// True for a stream transport (WebSocket over TCP): it already delivers every datagram once and in order, so
        /// the packet layer skips its own resends (it keeps its sequence numbers and acks, for RTT and ordering checks)
        [[nodiscard]] virtual bool IsReliableStream() const noexcept = 0;

        /// Queues one datagram to `peer`. Never blocks.
        virtual TransportSendResult Send(PeerId peer, std::span<const std::uint8_t> datagram) = 0;

        /// Delivers every event that is due, in order, to `handler`, then returns. The handler may call Send, but not
        /// Poll or Disconnect on this transport.
        virtual void Poll(EventHandler handler) = 0;

        /// Drops the link to `peer` (no-op if there is none). The other side sees PeerDisconnected.
        virtual void Disconnect(PeerId peer) = 0;
    };
} // namespace N2Engine::N2Net
