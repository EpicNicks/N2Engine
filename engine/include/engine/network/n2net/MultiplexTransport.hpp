#pragma once
#include "engine/network/n2net/Transport.hpp"
#include <map>
#include <utility>
#include <vector>

namespace N2Engine::N2Net
{
    // One polled world over multiple authenticated transports. Underlying transports must outlive this object.
    class MultiplexTransport final : public ITransport
    {
      public:
        explicit MultiplexTransport(std::vector<ITransport *> transports) : _transports(std::move(transports)) {}
        std::size_t MaxPacketSize() const noexcept override
        {
            return kMaxPacketSize;
        }
        std::size_t MaxPeerPacketSize(PeerId peer) const noexcept override;
        bool IsReliableStream() const noexcept override
        {
            return false;
        }
        bool IsReliablePeer(PeerId peer) const noexcept override;
        TransportSendResult Send(PeerId peer, std::span<const std::uint8_t> data) override;
        void Poll(EventHandler handler) override;
        void Disconnect(PeerId peer) override;
        bool GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const override;
        bool Source(PeerId peer, ITransport *&transport, PeerId &sourcePeer) const;

      private:
        struct Route
        {
            ITransport *transport;
            PeerId peer;
        };
        std::vector<ITransport *> _transports;
        std::map<PeerId, Route> _routes;
        std::map<std::pair<ITransport *, PeerId>, PeerId> _ids;
        PeerId _next = 1;
        bool _polling = false;
    };
} // namespace N2Engine::N2Net
