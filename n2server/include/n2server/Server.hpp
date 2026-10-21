#pragma once

#include "n2server/ServerOptions.hpp"

namespace N2Engine::Server
{
    /**
     * Runs a dedicated server, as N2Server does. With options.showHelp it prints ServerUsage() and returns 0; with
     * options.newTokenUser it is RunNewToken (TokenCli.hpp). With options.relay it prints one line to stderr
     * ("N2Server --relay: not implemented ...") and returns 1, before anything else starts. Otherwise:
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
     * At this stage the server authenticates and counts clients and logs when they connect and disconnect; it replicates
     * nothing and runs no gameplay networking (track B). Call it at most once at a time: it installs signal handlers and
     * uses the Application singleton.
     */
    int RunServer(const ServerOptions &options);

    /// The command line form: parses the arguments after the program name (ParseServerArguments), prints a parse error to
    /// stderr and returns 1, otherwise returns RunServer(options). N2Server's main is just this.
    int RunServer(int argc, char *argv[]);
}
