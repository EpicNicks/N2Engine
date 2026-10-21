#include "engine/network/n2net/UdpTransport.hpp"

#if N2NET_HAS_UDP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace N2Engine::N2Net
{
    namespace
    {
#ifdef _WIN32
        using SocketHandle = SOCKET;
        constexpr SocketHandle kNoSocket = INVALID_SOCKET;
        using AddrLen = int;

        // Defined in mswsock.h; spelled out so this file needs only winsock2.h
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef SIO_UDP_NETRESET
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR, 15)
#endif

        int LastSocketError() noexcept
        {
            return WSAGetLastError();
        }
        bool IsWouldBlock(int error) noexcept
        {
            return error == WSAEWOULDBLOCK;
        }
        bool IsInterrupted(int error) noexcept
        {
            return error == WSAEINTR;
        }
        /// An ICMP "port unreachable" for an earlier send, or the system's report of one: not a fault of this socket
        bool IsConnectionReset(int error) noexcept
        {
            return error == WSAECONNRESET || error == WSAENETRESET || error == WSAECONNREFUSED ||
                   error == WSAEHOSTUNREACH || error == WSAENETUNREACH;
        }
        bool IsMessageTooLong(int error) noexcept
        {
            return error == WSAEMSGSIZE;
        }
        bool IsAddressFamilyUnsupported(int error) noexcept
        {
            return error == WSAEAFNOSUPPORT || error == WSAEPROTONOSUPPORT || error == WSAEPFNOSUPPORT;
        }
        void CloseSocket(SocketHandle socket) noexcept
        {
            closesocket(socket);
        }
        bool SetNonBlocking(SocketHandle socket) noexcept
        {
            u_long enable = 1;
            return ioctlsocket(socket, FIONBIO, &enable) == 0;
        }
        constexpr int kSendFlags = 0;
#else
        using SocketHandle = int;
        constexpr SocketHandle kNoSocket = -1;
        using AddrLen = socklen_t;

        int LastSocketError() noexcept
        {
            return errno;
        }
        bool IsWouldBlock(int error) noexcept
        {
            return error == EWOULDBLOCK || error == EAGAIN;
        }
        bool IsInterrupted(int error) noexcept
        {
            return error == EINTR;
        }
        bool IsConnectionReset(int error) noexcept
        {
            return error == ECONNREFUSED || error == ECONNRESET || error == EHOSTUNREACH || error == ENETUNREACH;
        }
        bool IsMessageTooLong(int) noexcept
        {
            // Linux truncates silently (the length returned is the buffer's); there is no error to recognise
            return false;
        }
        bool IsAddressFamilyUnsupported(int error) noexcept
        {
            return error == EAFNOSUPPORT || error == EPROTONOSUPPORT;
        }
        void CloseSocket(SocketHandle socket) noexcept
        {
            close(socket);
        }
        bool SetNonBlocking(SocketHandle socket) noexcept
        {
            const int flags = fcntl(socket, F_GETFL, 0);
            if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0)
            {
                return false;
            }
            // Never leak the socket into a child process; best effort
            const int fdFlags = fcntl(socket, F_GETFD, 0);
            if (fdFlags >= 0)
            {
                fcntl(socket, F_SETFD, fdFlags | FD_CLOEXEC);
            }
            return true;
        }
#ifdef MSG_NOSIGNAL
        constexpr int kSendFlags = MSG_NOSIGNAL;
#else
        constexpr int kSendFlags = 0;
#endif
#endif

        /// sendto and recvfrom with the platform's length types; the result is the byte count or a negative number
        long long SendRaw(SocketHandle socket, const std::uint8_t *data, std::size_t size, const sockaddr_storage &to,
                          AddrLen toLength) noexcept
        {
#ifdef _WIN32
            return sendto(socket, reinterpret_cast<const char *>(data), static_cast<int>(size), kSendFlags,
                          reinterpret_cast<const sockaddr *>(&to), toLength);
#else
            return sendto(socket, data, size, kSendFlags, reinterpret_cast<const sockaddr *>(&to), toLength);
#endif
        }

        long long RecvRaw(SocketHandle socket, std::uint8_t *data, std::size_t size, sockaddr_storage &from,
                          AddrLen &fromLength) noexcept
        {
            fromLength = static_cast<AddrLen>(sizeof from);
#ifdef _WIN32
            return recvfrom(socket, reinterpret_cast<char *>(data), static_cast<int>(size), 0,
                            reinterpret_cast<sockaddr *>(&from), &fromLength);
#else
            return recvfrom(socket, data, size, 0, reinterpret_cast<sockaddr *>(&from), &fromLength);
#endif
        }

        /// An interrupted call is retried at most this many times in a row, so nothing here can spin
        constexpr int kMaxInterruptedRetries = 16;

        /// A peer's address in a form that is the same wherever it came from: the family, the 4 or 16 address bytes, the
        /// port and (IPv6) the scope. One socket is one family, so an address is always spelled one way.
        struct Address
        {
            bool v6 = false;
            std::array<std::uint8_t, 16> bytes{};
            std::uint16_t port = 0;
            std::uint32_t scope = 0;

            bool operator==(const Address &other) const noexcept
            {
                return v6 == other.v6 && port == other.port && scope == other.scope && bytes == other.bytes;
            }
        };

        struct AddressHash
        {
            std::size_t operator()(const Address &address) const noexcept
            {
                std::uint64_t hash = 1469598103934665603ull;
                for (const std::uint8_t byte : address.bytes)
                {
                    hash = (hash ^ byte) * 1099511628211ull;
                }
                hash = (hash ^ address.port) * 1099511628211ull;
                hash = (hash ^ address.scope) * 1099511628211ull;
                return static_cast<std::size_t>(hash);
            }
        };

        bool IsV4Mapped(const std::array<std::uint8_t, 16> &bytes) noexcept
        {
            for (std::size_t i = 0; i < 10; ++i)
            {
                if (bytes[i] != 0)
                {
                    return false;
                }
            }
            return bytes[10] == 0xFF && bytes[11] == 0xFF;
        }

        /// Parses a numeric IPv4 or IPv6 address. A name, an empty string and anything with a zone ('%') are refused.
        bool ParseNumeric(std::string_view text, Address &out) noexcept
        {
            if (text.empty() || text.size() > 45)
            {
                return false;
            }
            char buffer[48];
            std::memcpy(buffer, text.data(), text.size());
            buffer[text.size()] = '\0';
            if (std::strlen(buffer) != text.size() || std::strchr(buffer, '%') != nullptr)
            {
                return false; // an embedded NUL or a zone
            }
            Address address;
            in_addr v4{};
            if (inet_pton(AF_INET, buffer, &v4) == 1)
            {
                address.v6 = false;
                std::memcpy(address.bytes.data(), &v4, 4);
                out = address;
                return true;
            }
            in6_addr v6{};
            if (inet_pton(AF_INET6, buffer, &v6) == 1)
            {
                address.v6 = true;
                std::memcpy(address.bytes.data(), &v6, 16);
                out = address;
                return true;
            }
            return false;
        }

        bool IsLoopback(const Address &address) noexcept
        {
            if (!address.v6)
            {
                return address.bytes[0] == 127;
            }
            if (IsV4Mapped(address.bytes))
            {
                return address.bytes[12] == 127;
            }
            for (std::size_t i = 0; i < 15; ++i)
            {
                if (address.bytes[i] != 0)
                {
                    return false;
                }
            }
            return address.bytes[15] == 1;
        }

        /// A usable remote endpoint: a real port and a unicast address (not unspecified, broadcast or multicast)
        bool IsUsablePeer(const Address &address) noexcept
        {
            if (address.port == 0)
            {
                return false;
            }
            const std::uint8_t *v4 = nullptr;
            if (!address.v6)
            {
                v4 = address.bytes.data();
            }
            else if (IsV4Mapped(address.bytes))
            {
                v4 = address.bytes.data() + 12;
            }
            if (v4 != nullptr)
            {
                // 0.0.0.0/8 (unspecified and "this network"), then 224.0.0.0 up: multicast, reserved and broadcast
                return v4[0] != 0 && v4[0] < 224;
            }
            if (address.bytes[0] == 0xFF)
            {
                return false; // multicast
            }
            return std::any_of(address.bytes.begin(), address.bytes.end(), [](std::uint8_t b) { return b != 0; });
        }

        std::string AddressText(const Address &address)
        {
            char buffer[INET6_ADDRSTRLEN + 1] = {};
            if (!address.v6)
            {
                in_addr v4{};
                std::memcpy(&v4, address.bytes.data(), 4);
                inet_ntop(AF_INET, &v4, buffer, sizeof buffer);
            }
            else if (IsV4Mapped(address.bytes))
            {
                in_addr v4{};
                std::memcpy(&v4, address.bytes.data() + 12, 4);
                inet_ntop(AF_INET, &v4, buffer, sizeof buffer);
            }
            else
            {
                in6_addr v6{};
                std::memcpy(&v6, address.bytes.data(), 16);
                inet_ntop(AF_INET6, &v6, buffer, sizeof buffer);
            }
            return buffer;
        }

        /// A sockaddr for `address` on a socket of the given family; false if the family cannot reach it
        bool ToSockaddr(const Address &address, bool socketIsV6, sockaddr_storage &storage, AddrLen &length) noexcept
        {
            std::memset(&storage, 0, sizeof storage);
            if (!socketIsV6)
            {
                if (address.v6)
                {
                    return false;
                }
                auto &in = reinterpret_cast<sockaddr_in &>(storage);
                in.sin_family = AF_INET;
                in.sin_port = htons(address.port);
                std::memcpy(&in.sin_addr, address.bytes.data(), 4);
                length = static_cast<AddrLen>(sizeof(sockaddr_in));
                return true;
            }
            auto &in6 = reinterpret_cast<sockaddr_in6 &>(storage);
            in6.sin6_family = AF_INET6;
            in6.sin6_port = htons(address.port);
            if (address.v6)
            {
                std::memcpy(&in6.sin6_addr, address.bytes.data(), 16);
                in6.sin6_scope_id = address.scope;
            }
            else
            {
                // An IPv4 peer of an IPv6 socket is its IPv4-mapped address
                std::uint8_t mapped[16] = {};
                mapped[10] = 0xFF;
                mapped[11] = 0xFF;
                std::memcpy(mapped + 12, address.bytes.data(), 4);
                std::memcpy(&in6.sin6_addr, mapped, 16);
            }
            length = static_cast<AddrLen>(sizeof(sockaddr_in6));
            return true;
        }

        bool FromSockaddr(const sockaddr_storage &storage, Address &out) noexcept
        {
            Address address;
            if (storage.ss_family == AF_INET)
            {
                const auto &in = reinterpret_cast<const sockaddr_in &>(storage);
                address.v6 = false;
                std::memcpy(address.bytes.data(), &in.sin_addr, 4);
                address.port = ntohs(in.sin_port);
            }
            else if (storage.ss_family == AF_INET6)
            {
                const auto &in6 = reinterpret_cast<const sockaddr_in6 &>(storage);
                address.v6 = true;
                std::memcpy(address.bytes.data(), &in6.sin6_addr, 16);
                address.port = ntohs(in6.sin6_port);
                address.scope = in6.sin6_scope_id;
            }
            else
            {
                return false;
            }
            out = address;
            return true;
        }

        std::uint64_t NowMillis() noexcept
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch())
                                                  .count());
        }

#ifdef _WIN32
        /// Winsock is reference counted: every transport starts it and ends it once
        bool StartSockets(int &error) noexcept
        {
            WSADATA data;
            const int result = WSAStartup(MAKEWORD(2, 2), &data);
            if (result != 0)
            {
                error = result;
                return false;
            }
            return true;
        }
        void StopSockets() noexcept
        {
            WSACleanup();
        }
#else
        bool StartSockets(int &) noexcept
        {
            return true;
        }
        void StopSockets() noexcept {}
#endif
    } // namespace

    bool IsLoopbackAddressText(std::string_view address)
    {
        Address parsed;
        return ParseNumeric(address, parsed) && IsLoopback(parsed);
    }

    struct UdpTransport::Impl
    {
        struct Peer
        {
            Address address;
            sockaddr_storage sockaddress{};
            AddrLen sockaddressLength = 0;
            std::uint64_t lastReceiveMs = 0;
            UdpPeerInfo counters;
        };

        UdpTransportOptions options;
        SocketHandle socket = kNoSocket;
        bool socketsStarted = false;
        bool ipv6 = false;
        bool dualStack = false;
        bool encryptionAvailable = false;
        bool insecureOverride = false;
        bool warned = false;
        bool polling = false;
        std::uint16_t localPort = 0;
        std::string localAddress;
        UdpTransportStats stats;

        PeerId nextPeerId = 1;
        std::unordered_map<PeerId, Peer> peers;
        std::unordered_map<Address, PeerId, AddressHash> byAddress;
        /// Peers made by Connect whose PeerConnected is not delivered yet
        std::vector<PeerId> pendingConnected;
        std::uint32_t maxPeers = 1;

        ~Impl()
        {
            if (socket != kNoSocket)
            {
                CloseSocket(socket);
            }
            if (socketsStarted)
            {
                StopSockets();
            }
        }

        void Warn(const std::string &text)
        {
            if (warned)
            {
                return;
            }
            warned = true;
            if (options.warningSink)
            {
                options.warningSink(text);
            }
            else
            {
                std::fprintf(stderr, "%s\n", text.c_str());
                std::fflush(stderr);
            }
        }

        PeerId AllocatePeerId() noexcept
        {
            // The counter wraps after 2^32 peers: skip 0 and any id still in use
            for (;;)
            {
                const PeerId candidate = nextPeerId++;
                if (candidate != kInvalidPeer && peers.find(candidate) == peers.end())
                {
                    return candidate;
                }
            }
        }

        PeerId AddPeer(const Address &address, const sockaddr_storage &storage, AddrLen length, std::uint64_t now)
        {
            const PeerId id = AllocatePeerId();
            Peer peer;
            peer.address = address;
            peer.sockaddress = storage;
            peer.sockaddressLength = length;
            peer.lastReceiveMs = now;
            peer.counters.address = AddressText(address);
            peer.counters.port = address.port;
            peers.emplace(id, std::move(peer));
            byAddress.emplace(address, id);
            return id;
        }

        void RemovePeer(PeerId id)
        {
            const auto found = peers.find(id);
            if (found == peers.end())
            {
                return;
            }
            byAddress.erase(found->second.address);
            peers.erase(found);
        }

        void NoteFailure(int error) noexcept
        {
            stats.lastErrorCode = error;
        }
    };

    UdpTransport::UdpTransport(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    UdpTransport::~UdpTransport() = default;

    namespace
    {
        UdpOpenResult Fail(UdpError error, int systemError, std::string message)
        {
            UdpOpenResult result;
            result.error = error;
            result.systemError = systemError;
            result.message = std::move(message);
            return result;
        }
    } // namespace

    UdpOpenResult UdpTransport::Open(const UdpTransportOptions &options, bool encryptionAvailable)
    {
        Address bindAddr;
        if (!ParseNumeric(options.bindAddress, bindAddr))
        {
            return Fail(UdpError::InvalidAddress, 0,
                        "bind address '" + options.bindAddress +
                            "' is not a numeric IPv4 or IPv6 address (host names are refused)");
        }
        const bool loopback = IsLoopback(bindAddr);
        if (!loopback && !encryptionAvailable && !options.insecureBind)
        {
            return Fail(UdpError::NonLoopbackRefused, 0,
                        "refusing to bind " + options.bindAddress +
                            ": it is not a loopback address and there is no encryption yet. Bind 127.0.0.1 or ::1, or "
                            "pass insecureBind to accept that anyone who can reach the port can read and forge its traffic");
        }

        auto impl = std::make_unique<Impl>();
        impl->options = options;
        impl->maxPeers = std::clamp<std::uint32_t>(options.maxPeers, 1, kHardMaxUdpPeers);
        impl->options.maxDatagramsPerPoll = std::max<std::uint32_t>(options.maxDatagramsPerPoll, 1);
        impl->encryptionAvailable = encryptionAvailable;
        impl->ipv6 = bindAddr.v6;

        int systemError = 0;
        if (!StartSockets(systemError))
        {
            return Fail(UdpError::SocketUnavailable, systemError, "the system's socket library could not start");
        }
        impl->socketsStarted = true;

        impl->socket = ::socket(bindAddr.v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (impl->socket == kNoSocket)
        {
            systemError = LastSocketError();
            return Fail(IsAddressFamilyUnsupported(systemError) ? UdpError::SocketUnavailable : UdpError::SocketSetupFailed,
                        systemError, "socket() failed");
        }
        if (!SetNonBlocking(impl->socket))
        {
            return Fail(UdpError::SocketSetupFailed, LastSocketError(), "could not make the socket non-blocking");
        }

#ifdef _WIN32
        // The Windows UDP pitfall: an ICMP "port unreachable" for an earlier send makes a later recvfrom fail with
        // WSAECONNRESET (and WSAENETRESET), which would end a read loop for a datagram that is not even ours. Both
        // reports are turned off; Poll still ignores them if some driver delivers one anyway.
        {
            BOOL newBehavior = FALSE;
            DWORD returned = 0;
            WSAIoctl(impl->socket, SIO_UDP_CONNRESET, &newBehavior, sizeof newBehavior, nullptr, 0, &returned, nullptr,
                     nullptr);
            WSAIoctl(impl->socket, SIO_UDP_NETRESET, &newBehavior, sizeof newBehavior, nullptr, 0, &returned, nullptr,
                     nullptr);
            // Without this another process can bind the same port with SO_REUSEADDR and share (take) the traffic
            const BOOL exclusive = TRUE;
            setsockopt(impl->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
                       sizeof exclusive);
        }
#endif
        // SO_REUSEADDR is deliberately not set: on Linux it lets a second socket share a UDP port and on Windows it
        // lets one take it over

        if (bindAddr.v6)
        {
            const int v6Only = options.dualStack ? 0 : 1;
            const bool applied = setsockopt(impl->socket, IPPROTO_IPV6, IPV6_V6ONLY,
                                            reinterpret_cast<const char *>(&v6Only), sizeof v6Only) == 0;
            // Dual stack is on only if it was asked for and the system took the option (some refuse it: then the
            // socket keeps the system default, which DualStack() reports as unknown-off)
            impl->dualStack = options.dualStack && applied;
        }

        const int receiveBuffer = static_cast<int>(std::min<std::uint32_t>(options.receiveBufferBytes, 64u << 20));
        const int sendBuffer = static_cast<int>(std::min<std::uint32_t>(options.sendBufferBytes, 64u << 20));
        if (receiveBuffer > 0)
        {
            setsockopt(impl->socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&receiveBuffer),
                       sizeof receiveBuffer);
        }
        if (sendBuffer > 0)
        {
            setsockopt(impl->socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&sendBuffer),
                       sizeof sendBuffer);
        }

        Address wanted = bindAddr;
        wanted.port = options.port;
        sockaddr_storage storage;
        AddrLen length = 0;
        ToSockaddr(wanted, bindAddr.v6, storage, length);
        if (::bind(impl->socket, reinterpret_cast<const sockaddr *>(&storage), length) != 0)
        {
            systemError = LastSocketError();
            return Fail(UdpError::BindFailed, systemError,
                        "could not bind " + options.bindAddress + ":" + std::to_string(options.port));
        }
        sockaddr_storage actual{};
        AddrLen actualLength = static_cast<AddrLen>(sizeof actual);
        Address local;
        if (getsockname(impl->socket, reinterpret_cast<sockaddr *>(&actual), &actualLength) != 0 ||
            !FromSockaddr(actual, local))
        {
            return Fail(UdpError::SocketSetupFailed, LastSocketError(), "getsockname() failed");
        }
        impl->localPort = local.port;
        impl->localAddress = options.bindAddress;

        if (!loopback && !encryptionAvailable)
        {
            // Here only because insecureBind is set
            impl->insecureOverride = true;
            impl->Warn("WARNING: UdpTransport is bound to " + options.bindAddress + ":" + std::to_string(impl->localPort) +
                       ", a non-loopback address, with NO encryption or authentication (insecureBind). Anyone who can "
                       "reach this port can read, forge and replay its traffic.");
        }

        UdpOpenResult result;
        result.transport.reset(new UdpTransport(std::move(impl)));
        return result;
    }

    UdpConnectResult UdpTransport::Connect(std::string_view address, std::uint16_t port)
    {
        Impl &state = *impl_;
        UdpConnectResult result;
        const auto fail = [&result](UdpError error, std::string message)
        {
            result.error = error;
            result.message = std::move(message);
            return result;
        };

        if (state.polling)
        {
            return fail(UdpError::Busy, "Connect was called from inside the Poll handler");
        }
        Address target;
        if (!ParseNumeric(address, target))
        {
            return fail(UdpError::InvalidAddress, "'" + std::string(address) + "' is not a numeric IPv4 or IPv6 address");
        }
        if (port == 0)
        {
            return fail(UdpError::InvalidPort, "port 0 is not a destination");
        }
        target.port = port;
        if (!IsUsablePeer(target))
        {
            return fail(UdpError::InvalidAddress, "'" + std::string(address) + "' is not a unicast address");
        }
        if (!state.ipv6 && target.v6)
        {
            return fail(UdpError::FamilyMismatch, "this socket is IPv4: it cannot reach an IPv6 address");
        }
        if (state.ipv6 && !target.v6 && !state.dualStack)
        {
            return fail(UdpError::FamilyMismatch, "this IPv6 socket is not dual-stack: it cannot reach an IPv4 address");
        }
        if (!IsLoopback(target) && !state.encryptionAvailable && !state.options.insecureBind)
        {
            return fail(UdpError::NonLoopbackRefused,
                        "refusing to connect to " + std::string(address) +
                            ": it is not a loopback address and there is no encryption yet (see insecureBind)");
        }
        sockaddr_storage storage;
        AddrLen length = 0;
        if (!ToSockaddr(target, state.ipv6, storage, length))
        {
            return fail(UdpError::FamilyMismatch, "this socket cannot reach that address");
        }
        // The key is how the system will report the peer's own datagrams: on an IPv6 socket an IPv4 peer comes back
        // as its IPv4-mapped IPv6 address
        Address key;
        FromSockaddr(storage, key);
        if (state.byAddress.find(key) != state.byAddress.end())
        {
            return fail(UdpError::AlreadyConnected, "that address is already a peer");
        }
        if (state.peers.size() >= state.maxPeers)
        {
            return fail(UdpError::TooManyPeers, "the peer limit is reached");
        }
        if (!IsLoopback(target) && !state.encryptionAvailable)
        {
            state.insecureOverride = true;
            state.Warn("WARNING: UdpTransport is connecting to " + std::string(address) +
                       ", a non-loopback address, with NO encryption or authentication (insecureBind). Anyone on the "
                       "path can read, forge and replay its traffic.");
        }
        result.peer = state.AddPeer(key, storage, length, NowMillis());
        state.pendingConnected.push_back(result.peer);
        return result;
    }

    TransportSendResult UdpTransport::Send(PeerId peer, std::span<const std::uint8_t> datagram)
    {
        Impl &state = *impl_;
        const auto found = state.peers.find(peer);
        if (found == state.peers.end())
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
        Impl::Peer &target = found->second;
        for (int attempt = 0; attempt <= kMaxInterruptedRetries; ++attempt)
        {
            const long long sent =
                SendRaw(state.socket, datagram.data(), datagram.size(), target.sockaddress, target.sockaddressLength);
            if (sent >= 0)
            {
                ++state.stats.datagramsSent;
                state.stats.bytesSent += datagram.size();
                ++target.counters.datagramsSent;
                target.counters.bytesSent += datagram.size();
                return TransportSendResult::Sent;
            }
            const int error = LastSocketError();
            if (IsInterrupted(error))
            {
                continue;
            }
            if (IsWouldBlock(error))
            {
                ++state.stats.sendWouldBlock;
            }
            else if (IsConnectionReset(error))
            {
                ++state.stats.connectionResetsIgnored;
            }
            else
            {
                ++state.stats.sendErrors;
                state.NoteFailure(error);
            }
            break;
        }
        // Accepted as far as the caller can tell: a datagram that does not get out is a lost datagram, and the reliable
        // channel exists for that. The counters above are where it shows.
        return TransportSendResult::Sent;
    }

    void UdpTransport::Disconnect(PeerId peer)
    {
        if (impl_->polling)
        {
            return;
        }
        impl_->RemovePeer(peer);
    }

    void UdpTransport::Poll(EventHandler handler)
    {
        Impl &state = *impl_;
        if (state.polling)
        {
            return;
        }
        struct PollingScope
        {
            bool &flag;
            explicit PollingScope(bool &f) : flag(f) { flag = true; }
            ~PollingScope() { flag = false; }
        } scope(state.polling);

        const std::uint64_t now = NowMillis();

        // Peers made by Connect
        std::vector<PeerId> connected;
        connected.swap(state.pendingConnected);
        for (const PeerId id : connected)
        {
            if (state.peers.find(id) != state.peers.end())
            {
                TransportEvent event;
                event.type = TransportEventType::PeerConnected;
                event.peer = id;
                handler(event);
            }
        }

        // Peers that went quiet. Removed before the event, so a Send from the handler to one fails cleanly.
        if (state.options.peerIdleTimeoutMs != 0)
        {
            std::vector<PeerId> expired;
            for (const auto &entry : state.peers)
            {
                if (now - entry.second.lastReceiveMs >= state.options.peerIdleTimeoutMs)
                {
                    expired.push_back(entry.first);
                }
            }
            // Ascending ids: a deterministic order whatever the hash map's
            std::sort(expired.begin(), expired.end());
            for (const PeerId id : expired)
            {
                state.RemovePeer(id);
                ++state.stats.peersTimedOut;
                TransportEvent event;
                event.type = TransportEventType::PeerDisconnected;
                event.peer = id;
                handler(event);
            }
        }

        // One byte more than the largest datagram, to tell a datagram of exactly that size from a longer one
        std::array<std::uint8_t, kMaxPacketSize + 1> buffer;
        int interrupted = 0;
        for (std::uint32_t read = 0; read < state.options.maxDatagramsPerPoll;)
        {
            sockaddr_storage storage{};
            AddrLen storageLength = 0;
            const long long received = RecvRaw(state.socket, buffer.data(), buffer.size(), storage, storageLength);
            if (received < 0)
            {
                const int error = LastSocketError();
                if (IsWouldBlock(error))
                {
                    break;
                }
                if (IsInterrupted(error))
                {
                    if (++interrupted > kMaxInterruptedRetries)
                    {
                        break;
                    }
                    continue;
                }
                ++read;
                if (IsMessageTooLong(error))
                {
                    // Windows: a datagram longer than the buffer
                    ++state.stats.droppedOversize;
                    continue;
                }
                if (IsConnectionReset(error))
                {
                    ++state.stats.connectionResetsIgnored;
                    continue;
                }
                ++state.stats.receiveErrors;
                state.NoteFailure(error);
                break;
            }
            ++read;
            interrupted = 0;
            const std::size_t size = static_cast<std::size_t>(received);
            if (size == 0)
            {
                ++state.stats.droppedEmpty;
                continue;
            }
            if (size > kMaxPacketSize)
            {
                ++state.stats.droppedOversize;
                continue;
            }
            Address source;
            if (!FromSockaddr(storage, source) || !IsUsablePeer(source))
            {
                ++state.stats.droppedBadSource;
                continue;
            }

            PeerId id = kInvalidPeer;
            const auto known = state.byAddress.find(source);
            if (known != state.byAddress.end())
            {
                id = known->second;
            }
            else
            {
                if (!state.options.acceptUnknownPeers)
                {
                    ++state.stats.droppedUnknownSource;
                    continue;
                }
                if (state.peers.size() >= state.maxPeers)
                {
                    ++state.stats.droppedPeerLimit;
                    continue;
                }
                id = state.AddPeer(source, storage, storageLength, now);
                ++state.stats.peersAccepted;
                TransportEvent connectedEvent;
                connectedEvent.type = TransportEventType::PeerConnected;
                connectedEvent.peer = id;
                handler(connectedEvent);
            }

            Impl::Peer &peer = state.peers.find(id)->second;
            peer.lastReceiveMs = NowMillis();
            ++peer.counters.datagramsReceived;
            peer.counters.bytesReceived += size;
            ++state.stats.datagramsReceived;
            state.stats.bytesReceived += size;

            TransportEvent event;
            event.type = TransportEventType::Datagram;
            event.peer = id;
            event.data = std::span<const std::uint8_t>(buffer.data(), size);
            handler(event);
        }
    }

    std::uint16_t UdpTransport::LocalPort() const noexcept
    {
        return impl_->localPort;
    }

    const std::string &UdpTransport::LocalAddress() const noexcept
    {
        return impl_->localAddress;
    }

    bool UdpTransport::IsIpv6() const noexcept
    {
        return impl_->ipv6;
    }

    bool UdpTransport::DualStack() const noexcept
    {
        return impl_->dualStack;
    }

    bool UdpTransport::InsecureOverrideUsed() const noexcept
    {
        return impl_->insecureOverride;
    }

    std::size_t UdpTransport::PeerCount() const noexcept
    {
        return impl_->peers.size();
    }

    const UdpTransportStats &UdpTransport::Stats() const noexcept
    {
        return impl_->stats;
    }

    bool UdpTransport::GetPeerInfo(PeerId peer, UdpPeerInfo &info) const
    {
        const auto found = impl_->peers.find(peer);
        if (found == impl_->peers.end())
        {
            return false;
        }
        info = found->second.counters;
        return true;
    }

    bool UdpTransport::GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const
    {
        const auto found = impl_->peers.find(peer);
        if (found == impl_->peers.end())
        {
            return false;
        }
        const Address &address = found->second.address;
        endpoint = PeerEndpoint{};
        endpoint.port = address.port;
        if (!address.v6)
        {
            std::copy(address.bytes.begin(), address.bytes.begin() + 4, endpoint.bytes.begin());
        }
        else if (IsV4Mapped(address.bytes))
        {
            std::copy(address.bytes.begin() + 12, address.bytes.end(), endpoint.bytes.begin());
        }
        else
        {
            endpoint.ipv6 = true;
            endpoint.bytes = address.bytes;
        }
        return true;
    }
} // namespace N2Engine::N2Net

#endif // N2NET_HAS_UDP
