// A bounded native endpoint for the node/Emscripten transport smoke, not a game server.
#include "engine/network/n2net/WebSocketTransport.hpp"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace N2Engine::N2Net;
int main(int argc,char **argv)
{
    WebSocketListenOptions options;
    if (argc>1) options.allowedOrigin=argv[1];
    options.protocolId = 7;
    options.authenticate = [](std::string_view token) -> std::optional<JoinTokenEntry> {
        if (token != "smoke-only-token")
            return {};
        JoinTokenEntry identity;
        identity.user = "smoke";
        return identity;
    };
    std::string error;
    auto server = WebSocketTransport::Listen(std::move(options), error);
    if (!server)
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("WebSocket ready port=%u\n", static_cast<unsigned>(server->LocalPort()));
    std::fflush(stdout);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool done = false;
    auto handler = [&](const TransportEvent &e) {
        if (e.type == TransportEventType::Datagram)
        {
            if (e.data.size() == 4 && e.data[0] == 'q' && e.data[1] == 'u' && e.data[2] == 'i' && e.data[3] == 't')
                done = true;
            else if (server->Send(e.peer, e.data) != TransportSendResult::Sent)
                done = true;
        }
    };
    while (!done && std::chrono::steady_clock::now() < end)
    {
        server->Poll(handler);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done ? 0 : 1;
}
