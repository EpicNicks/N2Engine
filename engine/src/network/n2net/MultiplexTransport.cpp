#include "engine/network/n2net/MultiplexTransport.hpp"
namespace N2Engine::N2Net
{
    std::size_t MultiplexTransport::MaxPeerPacketSize(PeerId peer) const noexcept
    {
        ITransport *transport;
        PeerId source;
        return Source(peer,transport,source) ? transport->MaxPeerPacketSize(source) : 0;
    }
    bool MultiplexTransport::Source(PeerId peer, ITransport *&transport, PeerId &source) const
    {
        auto f = _routes.find(peer);
        if (f == _routes.end())
            return false;
        transport = f->second.transport;
        source = f->second.peer;
        return true;
    }
    bool MultiplexTransport::IsReliablePeer(PeerId peer) const noexcept
    {
        ITransport *t;
        PeerId p;
        return Source(peer, t, p) && t->IsReliablePeer(p);
    }
    TransportSendResult MultiplexTransport::Send(PeerId peer, std::span<const std::uint8_t> data)
    {
        ITransport *t;
        PeerId p;
        return Source(peer, t, p) ? t->Send(p, data) : TransportSendResult::UnknownPeer;
    }
    bool MultiplexTransport::GetPeerEndpoint(PeerId peer, PeerEndpoint &endpoint) const
    {
        ITransport *t;
        PeerId p;
        return Source(peer, t, p) && t->GetPeerEndpoint(p, endpoint);
    }
    void MultiplexTransport::Disconnect(PeerId peer)
    {
        if (_polling)
            return;
        auto f = _routes.find(peer);
        if (f == _routes.end())
            return;
        f->second.transport->Disconnect(f->second.peer);
        _ids.erase({f->second.transport, f->second.peer});
        _routes.erase(f);
    }
    void MultiplexTransport::Poll(EventHandler handler)
    {
        if (_polling)
            return;
        _polling = true;
        for (auto *transport : _transports)
        {
            if (!transport)
                continue;
            std::vector<PeerId> refused;
            auto event = [&](const TransportEvent &e) {
                auto key = std::pair{transport, e.peer};
                if (e.type == TransportEventType::PeerConnected && !_ids.contains(key))
                {
                    if (_next == 0 || _routes.size() >= 4096)
                    {
                        refused.push_back(e.peer);
                        return;
                    }
                    _ids.emplace(key, _next);
                    _routes.emplace(_next, Route{transport, e.peer});
                    ++_next;
                }
                auto f = _ids.find(key);
                if (f == _ids.end())
                    return;
                const auto id = f->second;
                handler(TransportEvent{e.type, id, e.data});
                if (e.type == TransportEventType::PeerDisconnected)
                {
                    _routes.erase(id);
                    _ids.erase(key);
                }
            };
            transport->Poll(event);
            for (const auto peer : refused)
                transport->Disconnect(peer);
        }
        _polling = false;
    }
} // namespace N2Engine::N2Net
