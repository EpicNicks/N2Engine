// n2server_smoke_client: the test client of tests/smoke/n2server_smoke.py. It connects to a running N2Server through the
// A5 handshake (SecureTransport over UdpTransport) and reports what happened on stdout. It is a test tool, not a game
// client: it plays nothing and sends nothing but the handshake, a keep-alive and a disconnect.
//
//   n2server_smoke_client --port <n> [--host <ip>] [--protocol-id <n>] [--hold-ms <n>] [--expect-role <role>]
//                         [--expect-failure]
//
// The join token is read from the first line of stdin, never from the command line (a command line is visible to every
// user of the machine). Output, one line each: "connected role=<role>" once the handshake completed, then
// "disconnected" after the client left; or "failed reason=<reason>" when it did not connect within the time allowed.
// Exit code 0 when the outcome is what was expected (a connection, or with --expect-failure no connection), else 1.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "engine/network/n2net/SecureTransport.hpp"
#include "engine/security/Crypto.hpp"
#include "n2server/ServerOptions.hpp"

using namespace N2Engine;

namespace
{
    constexpr std::chrono::seconds kConnectTimeout{15};

    const char *FailureName(const N2Net::SecureClientFailure failure)
    {
        switch (failure)
        {
        case N2Net::SecureClientFailure::None:
            return "none";
        case N2Net::SecureClientFailure::HandshakeTimeout:
            return "handshake-timeout";
        case N2Net::SecureClientFailure::IdleTimeout:
            return "idle-timeout";
        case N2Net::SecureClientFailure::ClosedByServer:
            return "closed-by-server";
        case N2Net::SecureClientFailure::TransportClosed:
            return "transport-closed";
        case N2Net::SecureClientFailure::CryptoFailure:
            return "crypto-failure";
        case N2Net::SecureClientFailure::SequenceExhausted:
            return "sequence-exhausted";
        }
        return "unknown";
    }

    int Usage()
    {
        std::fprintf(stderr, "usage: n2server_smoke_client --port <n> [--host <ip>] [--protocol-id <n>] [--hold-ms <n>] "
                             "[--expect-role <role>] [--expect-failure]   (the token is the first line of stdin)\n");
        return 2;
    }
}

int main(int argc, char *argv[])
{
    std::string host = "127.0.0.1";
    int port = 0;
    std::uint32_t protocolId = Server::ServerOptions::DefaultProtocolId;
    int holdMs = 300;
    std::optional<N2Net::JoinRole> expectedRole;
    bool expectFailure = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--expect-failure")
        {
            expectFailure = true;
            continue;
        }
        if (i + 1 >= argc)
        {
            return Usage();
        }
        const std::string value = argv[++i];
        if (arg == "--host")
        {
            host = value;
        }
        else if (arg == "--port")
        {
            port = std::atoi(value.c_str());
        }
        else if (arg == "--protocol-id")
        {
            const auto id = Server::ParseProtocolId(value);
            if (!id)
            {
                return Usage();
            }
            protocolId = *id;
        }
        else if (arg == "--hold-ms")
        {
            holdMs = std::atoi(value.c_str());
        }
        else if (arg == "--expect-role")
        {
            expectedRole = N2Net::ParseJoinRole(value);
            if (!expectedRole)
            {
                return Usage();
            }
        }
        else
        {
            return Usage();
        }
    }
    if (port <= 0 || port > 65535)
    {
        return Usage();
    }

    std::string token;
    std::getline(std::cin, token);
    while (!token.empty() && (token.back() == '\r' || token.back() == '\n'))
    {
        token.pop_back();
    }

    N2Net::UdpTransportOptions udp;
    udp.acceptUnknownPeers = false;
    N2Net::SecureTransportOptions options;
    options.role = N2Net::SecureRole::Client;
    options.protocolId = protocolId;
    options.joinToken = token;
    N2Net::SecureUdpOpenResult opened = N2Net::SecureTransport::OpenUdp(udp, std::move(options));
    Security::SecureZero(token.data(), token.size());
    if (opened.transport == nullptr)
    {
        std::fprintf(stderr, "can't open the client transport: %s\n", opened.message.c_str());
        return 1;
    }

    const N2Net::UdpConnectResult peer = opened.udp->Connect(host, static_cast<std::uint16_t>(port));
    if (peer.error != N2Net::UdpError::None || !opened.transport->Connect(peer.peer))
    {
        std::fprintf(stderr, "can't start the handshake with %s:%d: %s\n", host.c_str(), port, peer.message.c_str());
        return 1;
    }

    const auto noEvent = [](const N2Net::TransportEvent &) {};
    const auto deadline = std::chrono::steady_clock::now() + kConnectTimeout;
    while (opened.transport->ClientState() != N2Net::SecureClientState::Connected &&
           opened.transport->ClientState() != N2Net::SecureClientState::Failed &&
           std::chrono::steady_clock::now() < deadline)
    {
        opened.transport->Poll(noEvent);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (opened.transport->ClientState() != N2Net::SecureClientState::Connected)
    {
        std::printf("failed reason=%s\n", FailureName(opened.transport->ClientFailure()));
        std::fflush(stdout);
        return expectFailure ? 0 : 1;
    }

    const N2Net::JoinRole granted = opened.transport->GrantedRole().value_or(N2Net::JoinRole::Player);
    std::printf("connected role=%s\n", std::string(N2Net::JoinRoleName(granted)).c_str());
    std::fflush(stdout);
    if (expectFailure)
    {
        return 1;
    }
    if (expectedRole && *expectedRole != granted)
    {
        std::fprintf(stderr, "expected role %s\n", std::string(N2Net::JoinRoleName(*expectedRole)).c_str());
        return 1;
    }

    // Stay a moment (the server's first sealed packet from us is the keep-alive sent at once), then leave: a sealed
    // Disconnect, which the server reports as the client leaving
    const auto holdUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
    while (std::chrono::steady_clock::now() < holdUntil)
    {
        opened.transport->Poll(noEvent);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    opened.transport->Disconnect(peer.peer);
    for (int i = 0; i < 20; ++i)
    {
        opened.transport->Poll(noEvent);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("disconnected\n");
    std::fflush(stdout);
    return 0;
}
