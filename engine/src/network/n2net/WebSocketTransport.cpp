#include "engine/network/n2net/WebSocketTransport.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/websocket.h>
#else
#include "engine/network/n2net/UdpTransport.hpp"
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif

namespace N2Engine::N2Net
{
    namespace
    {
        constexpr std::size_t kQueueLimit = 256 * kMaxPacketSize;
        constexpr std::array<std::uint8_t, 4> kAccepted{'N', '2', 'O', 'K'};
        struct Event
        {
            TransportEventType type;
            PeerId peer;
            std::vector<std::uint8_t> data;
        };
        std::vector<std::uint8_t> Auth(std::uint32_t protocol, std::string_view token)
        {
            std::vector<std::uint8_t> b{'N', '2', 'W', 'S'};
            for (int i = 0; i < 4; ++i)
                b.push_back(static_cast<std::uint8_t>(protocol >> (8 * i)));
            b.insert(b.end(), token.begin(), token.end());
            return b;
        }
    } // namespace
#ifdef __EMSCRIPTEN__
    struct WebSocketTransport::Impl
    {
        EMSCRIPTEN_WEBSOCKET_T socket = 0;
        std::vector<std::uint8_t> auth;
        std::deque<Event> events;
        bool connected = false, failed = false, polling = false;
        double started = emscripten_get_now();
        void Fail()
        {
            if (failed)
                return;
            failed = true;
            connected = false;
            std::fill(auth.begin(), auth.end(), 0);
            auth.clear();
            events.push_back({TransportEventType::PeerDisconnected, 1, {}});
            if (socket > 0)
                emscripten_websocket_close(socket, 1000, nullptr);
        }
        ~Impl()
        {
            if (socket > 0)
            {
                emscripten_websocket_set_onopen_callback(socket, nullptr, nullptr);
                emscripten_websocket_set_onmessage_callback(socket, nullptr, nullptr);
                emscripten_websocket_set_onerror_callback(socket, nullptr, nullptr);
                emscripten_websocket_set_onclose_callback(socket, nullptr, nullptr);
                emscripten_websocket_close(socket, 1000, nullptr);
                emscripten_websocket_delete(socket);
            }
            std::fill(auth.begin(), auth.end(), 0);
        }
    };
    std::unique_ptr<WebSocketTransport> WebSocketTransport::Connect(std::string_view url, std::string_view token,
                                                                    std::uint32_t protocol, std::string &error)
    {
        bool safe = url.starts_with("wss://");
        if (url.starts_with("ws://"))
        {
            auto authority =
                url.substr(5, url.find('/', 5) == std::string_view::npos ? url.size() - 5 : url.find('/', 5) - 5);
            const auto colon = authority.find(':');
            const auto host = authority.substr(0, colon);
            safe = host == "localhost" || host == "127.0.0.1";
        }
        if (!safe || url.size() > 2048 || token.empty() || token.size() > 512 || !emscripten_websocket_is_supported())
        {
            error = "WebSocket requires wss:// (or localhost ws://), a bounded token and browser support";
            return {};
        }
        auto result = std::unique_ptr<WebSocketTransport>(new WebSocketTransport);
        auto &p = *result->_impl;
        p.auth = Auth(protocol, token);
        std::string address(url);
        EmscriptenWebSocketCreateAttributes attr{address.c_str(), "n2net", EM_TRUE};
        p.socket = emscripten_websocket_new(&attr);
        if (p.socket <= 0)
        {
            error = "WebSocket creation failed";
            return {};
        }
        emscripten_websocket_set_onopen_callback(
            p.socket, &p, [](int, const EmscriptenWebSocketOpenEvent *, void *v) -> EM_BOOL {
                auto &p = *static_cast<Impl *>(v);
                if (emscripten_websocket_send_binary(p.socket, p.auth.data(), p.auth.size()) !=
                    EMSCRIPTEN_RESULT_SUCCESS)
                    p.Fail();
                std::fill(p.auth.begin(), p.auth.end(), 0);
                p.auth.clear();
                return EM_TRUE;
            });
        emscripten_websocket_set_onmessage_callback(
            p.socket, &p, [](int, const EmscriptenWebSocketMessageEvent *e, void *v) -> EM_BOOL {
                auto &p = *static_cast<Impl *>(v);
                if (p.failed)
                    return EM_TRUE;
                if (e->isText || e->numBytes == 0 || e->numBytes > kMaxPacketSize || p.events.size() >= 256)
                {
                    p.Fail();
                    return EM_TRUE;
                }
                auto *b = static_cast<const std::uint8_t *>(e->data);
                if (!p.connected)
                {
                    if (e->numBytes != kAccepted.size() || !std::equal(kAccepted.begin(), kAccepted.end(), b))
                        p.Fail();
                    else
                    {
                        p.connected = true;
                        p.events.push_back({TransportEventType::PeerConnected, 1, {}});
                    }
                }
                else
                    p.events.push_back({TransportEventType::Datagram, 1, {b, b + e->numBytes}});
                return EM_TRUE;
            });
        emscripten_websocket_set_onerror_callback(p.socket, &p,
                                                  [](int, const EmscriptenWebSocketErrorEvent *, void *v) -> EM_BOOL {
                                                      static_cast<Impl *>(v)->Fail();
                                                      return EM_TRUE;
                                                  });
        emscripten_websocket_set_onclose_callback(p.socket, &p,
                                                  [](int, const EmscriptenWebSocketCloseEvent *, void *v) -> EM_BOOL {
                                                      static_cast<Impl *>(v)->Fail();
                                                      return EM_TRUE;
                                                  });
        return result;
    }
    std::unique_ptr<WebSocketTransport> WebSocketTransport::Listen(WebSocketListenOptions, std::string &error)
    {
        error = "A browser cannot listen";
        return {};
    }
    std::uint16_t WebSocketTransport::LocalPort() const noexcept
    {
        return 0;
    }
    std::optional<JoinTokenEntry> WebSocketTransport::Identity(PeerId) const
    {
        return {};
    }
    bool WebSocketTransport::GetPeerEndpoint(PeerId, PeerEndpoint &) const
    {
        return false;
    }
    TransportSendResult WebSocketTransport::Send(PeerId peer, std::span<const std::uint8_t> data)
    {
        auto &p = *_impl;
        if (peer != 1 || !p.connected || p.failed)
            return TransportSendResult::UnknownPeer;
        if (data.empty())
            return TransportSendResult::EmptyPacket;
        if (data.size() > kMaxPacketSize)
            return TransportSendResult::PacketTooLarge;
        std::size_t buffered = 0;
        if (emscripten_websocket_get_buffered_amount(p.socket, &buffered) != EMSCRIPTEN_RESULT_SUCCESS ||
            buffered + data.size() > kQueueLimit ||
            emscripten_websocket_send_binary(p.socket, const_cast<std::uint8_t *>(data.data()), data.size()) !=
                EMSCRIPTEN_RESULT_SUCCESS)
        {
            p.Fail();
            return TransportSendResult::UnknownPeer;
        }
        return TransportSendResult::Sent;
    }
    void WebSocketTransport::Poll(EventHandler handler)
    {
        if (_impl->polling)
            return;
        if (!_impl->connected && emscripten_get_now() - _impl->started > 5000)
            _impl->Fail();
        _impl->polling = true;
        auto events = std::move(_impl->events);
        _impl->events.clear();
        for (auto &e : events)
            handler(TransportEvent{e.type, e.peer, e.data});
        _impl->polling = false;
    }
    void WebSocketTransport::Disconnect(PeerId peer)
    {
        if (peer == 1 && !_impl->polling)
            _impl->Fail();
    }
#else
    namespace
    {
#ifdef _WIN32
        using Socket = SOCKET;
        constexpr Socket kInvalid = INVALID_SOCKET;
        void Close(Socket s)
        {
            closesocket(s);
        }
        bool Nonblock(Socket s)
        {
            u_long on = 1;
            return ioctlsocket(s, FIONBIO, &on) == 0;
        }
        bool Again()
        {
            return WSAGetLastError() == WSAEWOULDBLOCK;
        }
#else
        using Socket = int;
        constexpr Socket kInvalid = -1;
        void Close(Socket s)
        {
            close(s);
        }
        bool Nonblock(Socket s)
        {
            return fcntl(s, F_SETFL, O_NONBLOCK) == 0;
        }
        bool Again()
        {
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
#endif
        std::uint64_t Now()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
        std::string Lower(std::string s)
        {
            for (char &c : s)
                if (c >= 'A' && c <= 'Z')
                    c = char(c - 'A' + 'a');
            return s;
        }
        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                s.remove_suffix(1);
            return s;
        }
        bool HasToken(std::string_view list, std::string_view wanted, bool caseSensitive = false)
        {
            while (!list.empty())
            {
                auto comma = list.find(',');
                const auto token = Trim(list.substr(0, comma));
                if ((caseSensitive ? std::string(token) : Lower(std::string(token))) == wanted)
                    return true;
                if (comma == list.npos)
                    break;
                list.remove_prefix(comma + 1);
            }
            return false;
        }
        bool Upgrade(std::string_view http, std::string_view origin, std::string &response)
        {
            auto end = http.find("\r\n");
            if (end == http.npos || !http.substr(0, end).starts_with("GET /") ||
                !http.substr(0, end).ends_with(" HTTP/1.1"))
                return false;
            std::map<std::string, std::string> headers;
            http.remove_prefix(end + 2);
            while (!http.empty() && !http.starts_with("\r\n"))
            {
                end = http.find("\r\n");
                if (end == http.npos)
                    return false;
                auto line = http.substr(0, end);
                auto colon = line.find(':');
                if (colon == line.npos)
                    return false;
                auto name = Lower(std::string(line.substr(0, colon)));
                if (!headers.emplace(name, std::string(Trim(line.substr(colon + 1)))).second)
                    return false;
                http.remove_prefix(end + 2);
            }
            if (!headers.contains("host") || Lower(headers["upgrade"]) != "websocket" ||
                !HasToken(headers["connection"], "upgrade") || headers["sec-websocket-version"] != "13" ||
                !HasToken(headers["sec-websocket-protocol"], "n2net", true) ||
                (headers.contains("origin") ? headers["origin"] != origin : !origin.empty()))
                return false;
            const auto key = headers["sec-websocket-key"];
            std::array<unsigned char, 32> decoded{};
            std::size_t count = 0;
            if (key.size() != 24 ||
                mbedtls_base64_decode(decoded.data(), decoded.size(), &count,
                                      reinterpret_cast<const unsigned char *>(key.data()), key.size()) != 0 ||
                count != 16)
                return false;
            const auto input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            std::array<unsigned char, 20> digest{};
            std::array<unsigned char, 32> encoded{};
            if (mbedtls_sha1(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest.data()) != 0 ||
                mbedtls_base64_encode(encoded.data(), encoded.size(), &count, digest.data(), digest.size()) != 0)
                return false;
            response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                       "Upgrade\r\nSec-WebSocket-Protocol: n2net\r\nSec-WebSocket-Accept: " +
                       std::string(reinterpret_cast<char *>(encoded.data()), count) + "\r\n\r\n";
            return true;
        }
        std::vector<std::uint8_t> Frame(std::span<const std::uint8_t> bytes, std::uint8_t opcode = 2)
        {
            std::vector<std::uint8_t> out{static_cast<std::uint8_t>(0x80 | opcode)};
            if (bytes.size() < 126)
                out.push_back(static_cast<std::uint8_t>(bytes.size()));
            else
            {
                out.push_back(126);
                out.push_back(static_cast<std::uint8_t>(bytes.size() >> 8));
                out.push_back(static_cast<std::uint8_t>(bytes.size()));
            }
            out.insert(out.end(), bytes.begin(), bytes.end());
            return out;
        }
    } // namespace
    struct WebSocketTransport::Impl
    {
        struct Peer
        {
            Socket socket = kInvalid;
            PeerEndpoint endpoint;
            std::uint64_t accepted = 0, lastRead = 0;
            bool upgraded = false, connected = false, dead = false, fragmented = false, closing = false;
            std::optional<JoinTokenEntry> identity;
            std::vector<std::uint8_t> input, output, message;
            std::size_t written = 0;
        };
        WebSocketListenOptions options;
        Socket listener = kInvalid;
        std::uint16_t port = 0;
        PeerId next = 1;
        std::map<PeerId, Peer> peers;
        bool polling = false, socketsStarted = false;
        std::uint64_t acceptWindow = 0;
        unsigned accepts = 0;
        ~Impl()
        {
            for (auto &[id, p] : peers)
                Close(p.socket);
            if (listener != kInvalid)
                Close(listener);
#ifdef _WIN32
            if (socketsStarted)
                WSACleanup();
#endif
        }
        bool Queue(Peer &p, std::span<const std::uint8_t> bytes)
        {
            if (p.output.size() - p.written + bytes.size() > kQueueLimit)
            {
                p.dead = true;
                return false;
            }
            if (p.written)
            {
                p.output.erase(p.output.begin(), p.output.begin() + p.written);
                p.written = 0;
            }
            p.output.insert(p.output.end(), bytes.begin(), bytes.end());
            return true;
        }
        bool Parse(PeerId id, Peer &p, EventHandler handler, std::size_t &work)
        {
            while (p.input.size() >= 2 && work < 1024)
            {
                auto &b = p.input;
                const bool fin = (b[0] & 0x80) != 0;
                const auto op = b[0] & 15;
                if ((b[0] & 0x70) || !(b[1] & 0x80))
                    return false;
                std::size_t size = b[1] & 127, offset = 2;
                if (size == 127)
                    return false; // all legal n2net messages fit in a 16-bit frame
                if (size == 126)
                {
                    if (b.size() < 4)
                        return true;
                    size = std::size_t(b[2]) * 256 + b[3];
                    offset = 4;
                    if (size < 126)
                        return false;
                }
                const bool control = op >= 8;
                if (size > kMaxPacketSize || (control && (!fin || size > 125)) || (!control && op != 0 && op != 2) ||
                    (control && op != 8 && op != 9 && op != 10))
                    return false;
                if (b.size() < offset + 4 + size)
                    return true;
                std::vector<std::uint8_t> payload(size);
                for (std::size_t i = 0; i < size; ++i)
                    payload[i] = b[offset + 4 + i] ^ b[offset + i % 4];
                b.erase(b.begin(), b.begin() + offset + 4 + size);
                ++work;
                if (op == 8)
                {
                    if (payload.size() == 1)
                        return false;
                    if (!Queue(p, Frame(payload, 8)))
                        return false;
                    p.closing = true;
                    return true;
                }
                if (op == 9)
                {
                    if (!Queue(p, Frame(payload, 10)))
                        return false;
                    continue;
                }
                if (op == 10)
                    continue;
                if ((op == 0 && !p.fragmented) || (op == 2 && p.fragmented))
                    return false;
                if (p.message.size() + payload.size() > kMaxPacketSize)
                    return false;
                p.message.insert(p.message.end(), payload.begin(), payload.end());
                p.fragmented = !fin;
                if (!fin)
                    continue;
                auto message = std::move(p.message);
                p.message.clear();
                if (message.empty())
                    return false;
                if (!p.connected)
                {
                    const auto prefix = Auth(options.protocolId, {});
                    if (message.size() <= 8 || message.size() > 520 ||
                        !std::equal(prefix.begin(), prefix.end(), message.begin()))
                        return false;
                    const std::string_view token(reinterpret_cast<const char *>(message.data() + 8),
                                                 message.size() - 8);
                    try
                    {
                        if (options.authenticate)
                            p.identity = options.authenticate(token);
                    }
                    catch (...)
                    {
                        return false;
                    }
                    std::fill(message.begin(), message.end(), 0);
                    if (!p.identity)
                        return false;
                    // No token digest is exposed through this transport's identity API.
                    p.identity->hash.fill(0);
                    if (!Queue(p, Frame(kAccepted)))
                        return false;
                    p.connected = true;
                    handler(TransportEvent{TransportEventType::PeerConnected, id, {}});
                }
                else
                    handler(TransportEvent{TransportEventType::Datagram, id, message});
                if (p.dead)
                    return false;
            }
            return true;
        }
    };
    std::unique_ptr<WebSocketTransport> WebSocketTransport::Listen(WebSocketListenOptions options, std::string &error)
    {
        if (options.bindAddress != "127.0.0.1")
        {
            error = "WebSocket listener must bind 127.0.0.1; expose it through a TLS proxy";
            return {};
        }
        auto result = std::unique_ptr<WebSocketTransport>(new WebSocketTransport);
        auto &p = *result->_impl;
        p.options = std::move(options);
        p.options.maxPeers = std::clamp<std::uint32_t>(p.options.maxPeers, 1, 1024);
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            error = "Winsock startup failed";
            return {};
        }
        p.socketsStarted = true;
#endif
        p.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(p.options.port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (p.listener == kInvalid || !Nonblock(p.listener) ||
            bind(p.listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(p.listener, 64) != 0)
        {
            error = "WebSocket bind/listen failed";
            return {};
        }
#ifdef _WIN32
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        if (getsockname(p.listener, reinterpret_cast<sockaddr *>(&addr), &len) != 0)
        {
            error = "WebSocket local address failed";
            return {};
        }
        p.port = ntohs(addr.sin_port);
        return result;
    }
    std::unique_ptr<WebSocketTransport> WebSocketTransport::Connect(std::string_view, std::string_view, std::uint32_t,
                                                                    std::string &error)
    {
        error = "Connect is the Emscripten browser API";
        return {};
    }
    std::uint16_t WebSocketTransport::LocalPort() const noexcept
    {
        return _impl->port;
    }
    std::optional<JoinTokenEntry> WebSocketTransport::Identity(PeerId peer) const
    {
        auto f = _impl->peers.find(peer);
        return f == _impl->peers.end() ? std::nullopt : f->second.identity;
    }
    bool WebSocketTransport::GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const
    {
        auto f = _impl->peers.find(peer);
        if (f == _impl->peers.end())
            return false;
        endpoint = f->second.endpoint;
        return true;
    }
    TransportSendResult WebSocketTransport::Send(PeerId peer, std::span<const std::uint8_t> data)
    {
        auto f = _impl->peers.find(peer);
        if (f == _impl->peers.end() || !f->second.connected || f->second.dead || f->second.closing)
            return TransportSendResult::UnknownPeer;
        if (data.empty())
            return TransportSendResult::EmptyPacket;
        if (data.size() > kMaxPacketSize)
            return TransportSendResult::PacketTooLarge;
        return _impl->Queue(f->second, Frame(data)) ? TransportSendResult::Sent : TransportSendResult::UnknownPeer;
    }
    void WebSocketTransport::Disconnect(PeerId peer)
    {
        if (_impl->polling)
            return;
        auto f = _impl->peers.find(peer);
        if (f == _impl->peers.end())
            return;
        Close(f->second.socket);
        _impl->peers.erase(f);
    }
    void WebSocketTransport::Poll(EventHandler handler)
    {
        auto &s = *_impl;
        if (s.polling)
            return;
        s.polling = true;
        const auto now = Now();
        if (now - s.acceptWindow >= 1000)
        {
            s.acceptWindow = now;
            s.accepts = 0;
        }
        for (int i = 0; i < 16 && s.accepts < 64; ++i)
        {
            sockaddr_in address{};
#ifdef _WIN32
            int len = sizeof(address);
#else
            socklen_t len = sizeof(address);
#endif
            auto socket = accept(s.listener, reinterpret_cast<sockaddr *>(&address), &len);
            if (socket == kInvalid)
                break;
            ++s.accepts;
            if (s.next == 0 || s.peers.size() >= s.options.maxPeers || !Nonblock(socket))
            {
                Close(socket);
                continue;
            }
            const int noDelay = 1;
            if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay),
                           sizeof(noDelay)) != 0)
            {
                Close(socket);
                continue;
            }
            Impl::Peer peer;
            peer.socket = socket;
            peer.accepted = peer.lastRead = now;
            std::memcpy(peer.endpoint.bytes.data(), &address.sin_addr, 4);
            peer.endpoint.port = ntohs(address.sin_port);
            s.peers.emplace(s.next, std::move(peer));
            ++s.next;
        }
        std::size_t work = 0;
        const auto perPeerAllowance = std::max<std::size_t>(1, 1024 / std::max<std::size_t>(1, s.peers.size()));
        for (auto it = s.peers.begin(); it != s.peers.end();)
        {
            auto &[id, p] = *it;
            if ((!p.connected && now - p.accepted > 5000) || now - p.lastRead > 30'000)
                p.dead = true;
            std::array<char, 4096> bytes{};
            if (!p.dead && !p.closing && work < 1024)
            {
                const int n = static_cast<int>(recv(p.socket, bytes.data(), static_cast<int>(bytes.size()), 0));
                if (n > 0)
                {
                    p.lastRead = now;
                    if (p.input.size() + n > 8192)
                        p.dead = true;
                    else
                        p.input.insert(p.input.end(), bytes.begin(), bytes.begin() + n);
                }
                else if (n == 0 || !Again())
                    p.dead = true;
                if (!p.dead && !p.upgraded)
                {
                    std::string_view input(reinterpret_cast<char *>(p.input.data()), p.input.size());
                    auto end = input.find("\r\n\r\n");
                    if (end != input.npos)
                    {
                        std::string response;
                        if (end + 4 > 4096 || !Upgrade(input.substr(0, end + 4), s.options.allowedOrigin, response))
                            p.dead = true;
                        else
                        {
                            p.upgraded = true;
                            s.Queue(p, {reinterpret_cast<const std::uint8_t *>(response.data()), response.size()});
                            p.input.erase(p.input.begin(), p.input.begin() + end + 4);
                        }
                    }
                    else if (p.input.size() > 4096)
                        p.dead = true;
                }
                // Fair per-peer cap as well as the whole-poll cap, so a flood cannot consume every peer's work.
                const auto before = work;
                if (!p.dead && p.upgraded)
                {
                    auto allowance = std::min<std::size_t>(1024 - work, perPeerAllowance);
                    auto localWork = 1024 - allowance;
                    if (!s.Parse(id, p, handler, localWork))
                        p.dead = true;
                    work = before + (localWork - (1024 - allowance));
                }
            }
            if (!p.dead && p.written < p.output.size())
            {
#ifdef _WIN32
                constexpr int flags = 0;
#else
                constexpr int flags = MSG_NOSIGNAL;
#endif
                const int n = static_cast<int>(
                    send(p.socket, reinterpret_cast<const char *>(p.output.data() + p.written),
                         static_cast<int>(std::min<std::size_t>(p.output.size() - p.written, 16 * 1024)), flags));
                if (n > 0)
                {
                    p.written += n;
                    if (p.written == p.output.size())
                    {
                        p.output.clear();
                        p.written = 0;
                    }
                }
                else if (n == 0 || !Again())
                    p.dead = true;
            }
            if (p.closing && p.written == p.output.size())
                p.dead = true;
            if (p.dead)
            {
                const bool connected = p.connected;
                Close(p.socket);
                it = s.peers.erase(it);
                if (connected)
                    handler(TransportEvent{TransportEventType::PeerDisconnected, id, {}});
            }
            else
                ++it;
        }
        s.polling = false;
    }
#endif
    WebSocketTransport::WebSocketTransport() : _impl(std::make_unique<Impl>()) {}
    WebSocketTransport::~WebSocketTransport() = default;
} // namespace N2Engine::N2Net
