#include "engine/network/n2net/WebSocketTransport.hpp"
#include <array>
#include <cstdio>
#include <emscripten.h>
#include <memory>
using namespace N2Engine::N2Net;
namespace
{
    std::unique_ptr<WebSocketTransport> transport;
    double started;
    int stage = 0;
    std::array<std::uint8_t, kMaxPacketSize> payload;
    void Tick()
    {
        if (emscripten_get_now() - started > 10000)
        {
            std::fprintf(stderr, "WebSocket timeout\n");
            emscripten_force_exit(1);
        }
        auto handler = [](const TransportEvent &e) {
            if (e.type == TransportEventType::PeerConnected)
            {
                stage = 1;
                if (transport->Send(e.peer, payload) != TransportSendResult::Sent)
                    emscripten_force_exit(2);
            }
            else if (e.type == TransportEventType::Datagram)
            {
                if (stage != 1 || e.data.size() != payload.size() ||
                    !std::equal(e.data.begin(), e.data.end(), payload.begin()))
                    emscripten_force_exit(3);
                const std::array<std::uint8_t, 4> quit{'q', 'u', 'i', 't'};
                transport->Send(e.peer, quit);
                stage = 2;
            }
            else if (stage != 2)
                emscripten_force_exit(4);
        };
        transport->Poll(handler);
        if (stage == 2)
        {
            transport.reset();
            emscripten_cancel_main_loop();
        }
    }
} // namespace
int main(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint8_t>(i);
    std::string error;
    transport = WebSocketTransport::Connect(argv[1], "smoke-only-token", 7, error);
    if (!transport)
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    started = emscripten_get_now();
    emscripten_set_main_loop(Tick, 0, 0);
    return 0;
}
