#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "engine/network/NetworkServer.hpp"
#include "n2server/ServerOptions.hpp"

namespace N2Engine::Server
{
    /// How a host built over RunServer takes part in the server's start and its loop (a test host, a game's own server
    /// executable). N2Server itself passes none.
    struct ServerHooks
    {
        /// Called once, with the engine running, the scene loaded and its NetworkObjects given their ids, before the
        /// prefab table is captured and before the server listens: register the game's prefabs here
        /// (PrefabManager::RegisterPrefab). Returns nullopt to go on, or a message that ends the server with exit code
        /// 1 (printed to stderr, before any ready line).
        std::function<std::optional<std::string>()> beforeListening;
        /// Called once per pass of the main loop, after the network update, with the running network server and a
        /// monotonic microsecond clock: spawn and despawn here. Returns false to stop the loop (a clean stop).
        std::function<bool(Network::NetworkServer &server, std::uint64_t nowMicros)> onTick;
    };

    /**
     * Runs a dedicated server, as N2Server does. With options.showHelp it prints ServerUsage() and returns 0; with
     * options.newTokenUser it is RunNewToken (TokenCli.hpp). With options.relay the project must use the Deterministic
     * preset (else one line on stderr and 1, before the engine starts) and step 3 serves a Network::LockstepRelay instead of a
     * NetworkServer (no world is replicated). Otherwise:
     *  1. the log sink starts (stderr, and --log-file), then the tokens file loads (--tokens-file; a file the server
     *     would not accept, such as one other users can read on POSIX, ends the server here), then the project's
     *     project.n2proj is read, all before the engine starts;
     *  2. the engine starts headless with no renderer, no window and no audio, and physics.gpu forced off
     *     (Application::Init with RenderBackend::NONE, audio false, physicsGpu false), the project's resources and
     *     settings load, and the scene (--scene, else the project's startup scene) is loaded as a running game;
     *  3. a SecureTransport over a UdpTransport listens on --bind and --port (n2net: the handshake, sealed packets, the
     *     pre-auth slots and rate limits, at most --max-clients sessions), and the ready line (FormatReadyLine) is printed
     *     to stdout, flushed;
     *  4. this thread serves until SIGINT or SIGTERM (a console's Ctrl+C, Ctrl+Break, close, logoff or shutdown on
     *     Windows), the project quitting through Application::Quit, or (with --exit-on-stdin-eof) stdin closing: each pass
     *     polls the transport, runs Application::Tick (the fixed ticks that are due, without drawing or polling windows)
     *     and sleeps a moment. On POSIX SIGHUP re-reads the tokens file.
     *     Then the sessions are told (a sealed Disconnect), the transport closes and the engine shuts down.
     * Returns 0 after a clean stop, 1 when the tokens file, the project, the scene, the bind or the engine could not be
     * set up (one line on stderr, before the ready line), or a std::exception escaped. On that last path the engine is
     * still shut down in order (the scene's OnApplicationQuit, then Application::Shutdown, each guarded so neither throws),
     * and the exit code stays 1.
     *
     * The server hosts the world of the scene (netcode stage B1, docs/netcode.html "The network world"): after the scene
     * is loaded its NetworkObjects are given net ids (Network::NetworkWorld::BindScene; a scene that can't be matched ends
     * the server with exit code 1), the prefab table is captured, and a Network::NetworkServer runs over the secure
     * transport: each client that joins is sent the fingerprint and the join state, and every spawn and despawn is
     * replicated. N2Server spawns nothing by itself; a host spawns through ServerHooks::onTick. Call it at most once at a
     * time: it installs signal handlers and uses the Application singleton.
     */
    int RunServer(const ServerOptions &options);

    /// RunServer with hooks (see ServerHooks)
    int RunServer(const ServerOptions &options, const ServerHooks &hooks);

    /// The command line form: parses the arguments after the program name (ParseServerArguments), prints a parse error to
    /// stderr and returns 1, otherwise returns RunServer(options). N2Server's main is just this.
    int RunServer(int argc, char *argv[]);
}
