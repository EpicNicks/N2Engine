#pragma once
#include "engine/network/n2net/JoinTokens.hpp"
#include "engine/network/n2net/Transport.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace N2Engine::N2Net
{
    struct WebSocketListenOptions
    {
        // Plain HTTP is confined to loopback. Use a TLS-terminating reverse proxy for public wss://.
        std::string bindAddress = "127.0.0.1";
        std::uint16_t port = 0;
        std::uint32_t protocolId = 0;
        std::uint32_t maxPeers = 64;
        // Empty refuses everybody. Called once after upgrade, with a bounded token; never log the argument.
        std::function<std::optional<JoinTokenEntry>(std::string_view)> authenticate;
        // Exact allowed Origin, empty accepts non-browser tools only (requests without Origin).
        std::string allowedOrigin;
    };
    class WebSocketTransport final : public ITransport
    {
      public:
        ~WebSocketTransport() override;
        WebSocketTransport(const WebSocketTransport &) = delete;
        WebSocketTransport &operator=(const WebSocketTransport &) = delete;
        // Native server. Loopback only, bounded HTTP headers, frames, queues and authentication deadlines.
        static std::unique_ptr<WebSocketTransport> Listen(WebSocketListenOptions options, std::string &error);
        // Emscripten client. wss://, or ws:// on localhost for development. Token is sent as the first binary
        // message, never in a URL or HTTP headers. Peer 1 is reported connected only after token acceptance.
        static std::unique_ptr<WebSocketTransport> Connect(std::string_view url, std::string_view token,
                                                           std::uint32_t protocolId, std::string &error);
        [[nodiscard]] std::uint16_t LocalPort() const noexcept;
        [[nodiscard]] std::optional<JoinTokenEntry> Identity(PeerId peer) const;
        [[nodiscard]] std::size_t MaxPacketSize() const noexcept override
        {
            return kMaxPacketSize;
        }
        [[nodiscard]] bool IsReliableStream() const noexcept override
        {
            return true;
        }
        TransportSendResult Send(PeerId peer, std::span<const std::uint8_t> data) override;
        void Poll(EventHandler handler) override;
        void Disconnect(PeerId peer) override;
        bool GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const override;

      private:
        WebSocketTransport();
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
} // namespace N2Engine::N2Net
