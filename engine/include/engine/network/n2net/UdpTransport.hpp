#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "engine/network/n2net/Transport.hpp"

// UdpTransport is the native ITransport: non-blocking UDP over BSD sockets (Linux) or Winsock (Windows). The web build
// has no sockets, so there the class does not exist (N2NET_HAS_UDP is 0) and the rest of n2net is unaffected.
#if defined(__EMSCRIPTEN__)
#define N2NET_HAS_UDP 0
#else
#define N2NET_HAS_UDP 1
#endif

#if N2NET_HAS_UDP

namespace N2Engine::N2Net
{
    /// Why a UdpTransport call failed. Never thrown: Open and Connect report it as data.
    enum class UdpError : std::uint8_t
    {
        None,
        /// The address is not a numeric IPv4 or IPv6 address (a host name, an empty string and a scoped IPv6 address
        /// are all refused: a name could resolve to anything, so the loopback check would mean nothing)
        InvalidAddress,
        /// A non-loopback address with neither encryption available nor insecureBind (the fail-closed rule)
        NonLoopbackRefused,
        /// The system has no usable sockets (Winsock failed to start) or no support for the address family
        SocketUnavailable,
        /// socket(), setting non-blocking mode or the required socket options failed
        SocketSetupFailed,
        /// bind() failed (address in use, not an address of this machine, no permission)
        BindFailed,
        /// Connect: the cap on peers (UdpTransportOptions::maxPeers) is reached
        TooManyPeers,
        /// Connect: called from inside the Poll handler
        Busy,
        /// Connect: the address is already a peer
        AlreadyConnected,
        /// Connect: the port is 0
        InvalidPort,
        /// Connect: the address is of the other family than the socket (an IPv6 address on an IPv4 socket, or an IPv4
        /// address on an IPv6 socket that is not dual-stack: see DualStack())
        FamilyMismatch,
    };

    struct UdpTransportOptions
    {
        /// The local address to bind: a numeric IPv4 or IPv6 address. "127.0.0.1" and "::1" are loopback; "0.0.0.0" and
        /// "::" (all interfaces) are not. The default is IPv4 loopback.
        std::string bindAddress = "127.0.0.1";
        /// 0 picks a free port (read it back with LocalPort())
        std::uint16_t port = 0;
        /// An IPv6 socket bound to the wildcard "::" accepts IPv4 peers too (as IPv4-mapped addresses) when the system
        /// allows it; DualStack() tells whether it did. A socket bound to one IPv6 address (such as ::1) is never dual
        /// stack. Ignored for an IPv4 bind address. False sets IPV6_V6ONLY.
        bool dualStack = true;

        /// Permits a non-loopback bind (and a non-loopback Connect) while encryption is not available: the explicit
        /// override the editor host spells --insecure-bind. A one-time warning goes to `warningSink` (stderr when
        /// unset) when it actually overrides the rule.
        bool insecureBind = false;
        std::function<void(std::string_view)> warningSink;

        /// A datagram from a source that is not a peer creates one (a server). False: only Connect creates peers (a
        /// client), and everything else is dropped.
        bool acceptUnknownPeers = true;
        /// The most peers tracked at once, from Connect and from unknown sources together. A source beyond it is dropped.
        /// Clamped to [1, kHardMaxUdpPeers].
        std::uint32_t maxPeers = 256;
        /// A peer that sends nothing for this long is dropped (PeerDisconnected), so spoofed sources cannot hold the
        /// peer table forever. 0 never drops (the peer cap still bounds the table).
        std::uint32_t peerIdleTimeoutMs = 30'000;
        /// The most datagrams one Poll reads, so a flood cannot keep the caller in Poll. The rest wait in the socket.
        std::uint32_t maxDatagramsPerPoll = 1024;

        /// Requested socket buffer sizes in bytes (the system may grant less)
        std::uint32_t receiveBufferBytes = 1u << 20;
        std::uint32_t sendBufferBytes = 256u * 1024u;
    };

    /// The hard ceiling for UdpTransportOptions::maxPeers
    inline constexpr std::uint32_t kHardMaxUdpPeers = 4096;

    /// Counters since Open. All are data: nothing here throws or logs.
    struct UdpTransportStats
    {
        std::uint64_t datagramsReceived = 0;
        std::uint64_t bytesReceived = 0;
        std::uint64_t datagramsSent = 0;
        std::uint64_t bytesSent = 0;
        /// Zero-length datagrams
        std::uint64_t droppedEmpty = 0;
        /// More than MaxPacketSize() bytes (or truncated by the system for being larger than the buffer)
        std::uint64_t droppedOversize = 0;
        /// From port 0, an unspecified, broadcast or multicast address
        std::uint64_t droppedBadSource = 0;
        /// From an unknown source while acceptUnknownPeers is false
        std::uint64_t droppedUnknownSource = 0;
        /// From an unknown source while the peer table is full
        std::uint64_t droppedPeerLimit = 0;
        /// Peers created from a first datagram
        std::uint64_t peersAccepted = 0;
        /// Peers removed for being idle
        std::uint64_t peersTimedOut = 0;
        /// Windows WSAECONNRESET (a previous send hit a closed port) and Linux ECONNREFUSED: ignored
        std::uint64_t connectionResetsIgnored = 0;
        /// Reads that failed for any other reason (the Poll stops reading until the next call)
        std::uint64_t receiveErrors = 0;
        /// The send buffer was full (WOULDBLOCK): that datagram was dropped, as a congested network would
        std::uint64_t sendWouldBlock = 0;
        /// Any other send failure
        std::uint64_t sendErrors = 0;
        /// The system error code (errno or WSAGetLastError) of the last failure not counted as ignored; 0 if none
        int lastErrorCode = 0;
    };

    struct UdpPeerInfo
    {
        /// The peer's address as text ("127.0.0.1", "::1"; IPv4-mapped addresses are shown as IPv4), without the port
        std::string address;
        std::uint16_t port = 0;
        std::uint64_t bytesReceived = 0;
        std::uint64_t bytesSent = 0;
        std::uint64_t datagramsReceived = 0;
        std::uint64_t datagramsSent = 0;
    };

    class UdpTransport;

    struct UdpOpenResult
    {
        /// Null when `error` is not None
        std::unique_ptr<UdpTransport> transport;
        UdpError error = UdpError::None;
        /// The system error code behind SocketSetupFailed, BindFailed or SocketUnavailable (0 otherwise)
        int systemError = 0;
        /// One line saying what went wrong, empty on success
        std::string message;
    };

    struct UdpConnectResult
    {
        PeerId peer = kInvalidPeer;
        UdpError error = UdpError::None;
        std::string message;
    };

    /// True for a numeric IPv4 address in 127.0.0.0/8, ::1 and an IPv4-mapped 127/8 address (::ffff:127.x.x.x). False
    /// for every other text, including host names, so a name or an odd spelling is never trusted as loopback.
    [[nodiscard]] bool IsLoopbackAddressText(std::string_view address);

    /// The native ITransport: one non-blocking UDP socket, polled on the calling thread (no threads, no blocking call).
    /// Peers are identified by source address and port. Single-threaded: call everything from one thread.
    ///
    /// Datagrams are passed through exactly as sent. This layer does no framing and does not reply to anything on its
    /// own: it sends only what Send is given, and only to known peers, so an unknown or spoofed source can never make
    /// it transmit (no amplification here; the cookie handshake that vets a source is the next layer's). A peer made
    /// by an unknown source's first datagram is unvetted: UdpPeerInfo's byte counters let that layer cap what it
    /// sends back to what it received.
    ///
    /// Fail closed: Open refuses a non-loopback bind unless `encryptionAvailable` is true or options.insecureBind is
    /// set. Only SecureTransport::OpenUdp passes true: it puts the handshake and the sealed packets (SecureTransport.hpp)
    /// over the transport it opens, so every datagram it carries for the layers above is encrypted and authenticated.
    /// Any other caller passes false. Connect to a non-loopback address follows the same rule.
    ///
    /// Disconnect only forgets the peer locally: no packet is sent, so the other side finds out by its own idle
    /// timeout. A later datagram from that address makes a new peer with a new PeerId (when unknown sources are
    /// accepted).
    class UdpTransport final : public ITransport
    {
    public:
        [[nodiscard]] static UdpOpenResult Open(const UdpTransportOptions &options, bool encryptionAvailable);

        ~UdpTransport() override;
        UdpTransport(const UdpTransport &) = delete;
        UdpTransport &operator=(const UdpTransport &) = delete;

        [[nodiscard]] std::size_t MaxPacketSize() const noexcept override { return kMaxPacketSize; }
        [[nodiscard]] bool IsReliableStream() const noexcept override { return false; }
        TransportSendResult Send(PeerId peer, std::span<const std::uint8_t> datagram) override;
        void Poll(EventHandler handler) override;
        /// A no-op when called from inside the Poll handler (which the interface forbids)
        void Disconnect(PeerId peer) override;

        /// Adds `address:port` as a peer without waiting for it to send: PeerConnected is delivered by the next Poll.
        /// Nothing is sent. Numeric addresses only.
        UdpConnectResult Connect(std::string_view address, std::uint16_t port);

        /// The bound port (the one the system picked when options.port was 0)
        [[nodiscard]] std::uint16_t LocalPort() const noexcept;
        /// The bound address as text
        [[nodiscard]] const std::string &LocalAddress() const noexcept;
        [[nodiscard]] bool IsIpv6() const noexcept;
        /// True when an IPv6 socket also reaches IPv4 peers: only a "::" bind with options.dualStack that the system
        /// accepted. When false, Connect to an IPv4 address on an IPv6 socket returns FamilyMismatch.
        [[nodiscard]] bool DualStack() const noexcept;
        /// True when the fail-closed rule was overridden by insecureBind for this transport
        [[nodiscard]] bool InsecureOverrideUsed() const noexcept;
        [[nodiscard]] std::size_t PeerCount() const noexcept;
        [[nodiscard]] const UdpTransportStats &Stats() const noexcept;
        /// False for an unknown peer
        [[nodiscard]] bool GetPeerInfo(PeerId peer, UdpPeerInfo &info) const;
        /// The peer's address and port (an IPv4-mapped address as IPv4); false for an unknown peer
        [[nodiscard]] bool GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const override;

    private:
        struct Impl;
        explicit UdpTransport(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace N2Engine::N2Net

#endif // N2NET_HAS_UDP
