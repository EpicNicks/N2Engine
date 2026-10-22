// n2server_net_host: the test host of tests/smoke/n2server_net.py. It is N2Server (RunServer, the same code path, with
// the same arguments) with two hooks: it registers the test prefab before the prefab table is captured, and it reads
// commands from stdin, one per line, and runs them on the server's thread:
//
//   spawn <prefab> [<owner client id>]   -> "spawned id=<n>" or "spawn-failed <why>"
//   despawn <net id>                     -> "despawned id=<n> ok=<0|1>"
//   joined                               -> "joined count=<n> ids=<a,b,...>"
//   quit                                 -> stops the server (a clean stop)
//
// Stdin closing also stops it. Every answer is one line on stdout, flushed. It is a test tool, not a game server: the
// real N2Server has no such commands.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "NetTestCommon.hpp"
#include "n2server/Server.hpp"

using namespace N2Engine;

namespace
{
    // Static: the reader thread may still be blocked in getline when main returns
    NetTest::CommandQueue g_commands;

    void Answer(const std::string &line)
    {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }

    void RunCommand(Network::NetworkServer &server, const std::string &line, bool &stop)
    {
        std::istringstream words(line);
        std::string word;
        words >> word;
        if (word == "spawn")
        {
            std::string prefab;
            std::uint32_t owner = 0;
            words >> prefab;
            words >> owner;
            const auto id = server.Spawn(prefab, owner);
            if (id)
            {
                Answer("spawned id=" + std::to_string(id->value));
            }
            else
            {
                Answer("spawn-failed " + id.error());
            }
        }
        else if (word == "despawn")
        {
            std::uint32_t id = 0;
            words >> id;
            const bool ok = server.Despawn(Network::NetworkId{id});
            Answer("despawned id=" + std::to_string(id) + " ok=" + (ok ? "1" : "0"));
        }
        else if (word == "joined")
        {
            const std::vector<Network::ClientId> ids = server.JoinedClients();
            std::string list;
            for (const Network::ClientId id : ids)
            {
                list += (list.empty() ? "" : ",") + std::to_string(id);
            }
            Answer("joined count=" + std::to_string(ids.size()) + " ids=" + list);
        }
        else if (word == "quit")
        {
            stop = true;
        }
        else
        {
            Answer("unknown-command " + word);
        }
    }
}

int main(int argc, char *argv[])
{
    const auto parsed = Server::ParseServerArguments(argc > 1 ? std::vector<std::string>(argv + 1, argv + argc)
                                                              : std::vector<std::string>{});
    if (!parsed)
    {
        std::fprintf(stderr, "n2server_net_host: %s\n", parsed.error().c_str());
        return 1;
    }

    g_commands.StartReadingStdin();

    Server::ServerHooks hooks;
    hooks.beforeListening = []() -> std::optional<std::string>
    {
        if (!NetTest::RegisterCratePrefab())
        {
            return std::string("the test prefab could not be registered");
        }
        return std::nullopt;
    };
    hooks.onTick = [](Network::NetworkServer &server, const std::uint64_t) -> bool
    {
        // Read before the lines are taken: every line was pushed before the queue was closed, so when it was closed
        // already, the loop below takes all of them
        const bool closed = g_commands.Closed();
        bool stop = false;
        std::string line;
        while (!stop && g_commands.Pop(line))
        {
            RunCommand(server, line, stop);
        }
        // A "quit", or stdin ended and everything it said has been run
        return !stop && !closed;
    };
    const int exitCode = Server::RunServer(*parsed, hooks);
    // The thread that reads stdin is blocked in a read that holds stdin's lock, and the C library's exit takes every
    // stream's lock: returning from main would hang. RunServer has shut everything down, so end here.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(exitCode);
}
