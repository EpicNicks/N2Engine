#include "n2server/Server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/Time.hpp"
#include "engine/Version.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkFingerprint.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/PrefabTable.hpp"
#include "engine/network/n2net/SecureTransport.hpp"
#include "engine/network/n2net/UdpTransport.hpp"
#include "engine/network/n2net/WebSocketTransport.hpp"
#include "engine/network/n2net/MultiplexTransport.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/security/Crypto.hpp"
#include "engine/serialization/JsonLimits.hpp"

#include "editor-server/EditorServer.hpp"
#include "editor-server/HostOptions.hpp"
#include "editor-server/LogSink.hpp"

#include "n2server/SceneLoading.hpp"
#include "n2server/TokenCli.hpp"

// Last: <windows.h> defines macros (near, far, ...) that must not reach the engine headers
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if !N2NET_HAS_SECURE || !N2NET_HAS_UDP
#error "N2Server needs the native UDP transport and the crypto library (it is not part of the web build)"
#endif

namespace N2Engine::Server
{
    namespace
    {
        std::atomic<bool> g_running{true};
        // SIGHUP (POSIX, with a tokens file): the main loop re-reads the tokens file
        std::atomic<bool> g_reloadTokens{false};
        // Set when RunServer has finished its clean shutdown, for the Windows console handler that waits for it
        std::atomic<bool> g_stopped{false};

        /// How long each pass of the main loop sleeps after polling the transport and running the ticks that are due
        constexpr std::chrono::milliseconds kLoopSleep{2};
        /// How often the loop reports handshakes that were dropped (only when some were, since the last report)
        constexpr std::chrono::seconds kDropReportInterval{60};

        void SignalHandler(int signal)
        {
            // Only the flags: logging from a POSIX signal handler is not safe (it can interrupt a thread that holds the
            // Logger's lock)
            if (signal == SIGINT || signal == SIGTERM)
            {
                g_running = false;
            }
#ifdef SIGHUP
            else if (signal == SIGHUP)
            {
                g_reloadTokens = true;
            }
#endif
        }

#if defined(_WIN32)
        BOOL WINAPI ConsoleHandler(const DWORD type)
        {
            switch (type)
            {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
                g_running = false;
                return TRUE;
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                g_running = false;
                // Windows ends the process when this returns, and allows a few seconds for it: wait for the main loop to
                // finish its clean shutdown (4.5 s at most)
                for (int i = 0; i < 45 && !g_stopped.load(); ++i)
                {
                    Sleep(100);
                }
                return TRUE;
            default:
                return FALSE;
            }
        }
#endif

        /// Installs SignalHandler for SIGINT and SIGTERM (and, where there is one and `reloadOnHangup` is set, SIGHUP:
        /// without a tokens file a hangup keeps its default, which ends the process), the console handler on Windows, and
        /// puts back what was there before when it goes out of scope
        class SignalHandlersScope
        {
        public:
            explicit SignalHandlersScope(const bool reloadOnHangup)
                : _previousInterrupt(std::signal(SIGINT, SignalHandler)),
                  _previousTerminate(std::signal(SIGTERM, SignalHandler))
            {
#ifdef SIGHUP
                if (reloadOnHangup)
                {
                    _previousHangup = std::signal(SIGHUP, SignalHandler);
                }
#else
                (void)reloadOnHangup;
#endif
#if defined(_WIN32)
                _consoleHandlerSet = SetConsoleCtrlHandler(ConsoleHandler, TRUE) != 0;
#endif
            }

            ~SignalHandlersScope()
            {
#if defined(_WIN32)
                if (_consoleHandlerSet)
                {
                    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
                }
#endif
                if (_previousInterrupt != SIG_ERR)
                {
                    std::signal(SIGINT, _previousInterrupt);
                }
                if (_previousTerminate != SIG_ERR)
                {
                    std::signal(SIGTERM, _previousTerminate);
                }
#ifdef SIGHUP
                if (_previousHangup.has_value() && *_previousHangup != SIG_ERR)
                {
                    std::signal(SIGHUP, *_previousHangup);
                }
#endif
            }

            SignalHandlersScope(const SignalHandlersScope &) = delete;
            SignalHandlersScope &operator=(const SignalHandlersScope &) = delete;

        private:
            using Handler = decltype(SIG_DFL);
            Handler _previousInterrupt;
            Handler _previousTerminate;
            // Only when SIGHUP was taken over
            std::optional<Handler> _previousHangup;
#if defined(_WIN32)
            bool _consoleHandlerSet = false;
#endif
        };

        /// "ip:port", or "[ip]:port" for an IPv6 address
        std::string FormatAddress(const std::string &address, const std::uint16_t port)
        {
            if (address.find(':') != std::string::npos)
            {
                return std::format("[{}]:{}", address, port);
            }
            return std::format("{}:{}", address, port);
        }

        /// The handshakes the transport dropped, as "bad token 3, rate limited 1" (only what is not zero); empty for none
        std::string DropText(const N2Net::SecureTransportStats &stats)
        {
            std::string text;
            const auto add = [&text](const char *what, const std::uint64_t count)
            {
                if (count == 0)
                {
                    return;
                }
                if (!text.empty())
                {
                    text += ", ";
                }
                text += std::format("{} {}", what, count);
            };
            add("bad token", stats.droppedBadToken);
            add("session limit", stats.droppedSessionLimit);
            add("rate limited", stats.droppedRateLimited);
            add("backed off", stats.droppedBackedOff);
            add("no pre-auth slot", stats.droppedNoSlot);
            add("bad cookie", stats.droppedBadCookie);
            add("expired cookie", stats.droppedExpiredCookie);
            add("wrong protocol", stats.droppedWrongProtocol);
            add("duplicate", stats.droppedDuplicate);
            return text;
        }

        std::uint64_t DropTotal(const N2Net::SecureTransportStats &stats)
        {
            return stats.droppedBadToken + stats.droppedSessionLimit + stats.droppedRateLimited + stats.droppedBackedOff +
                   stats.droppedNoSlot + stats.droppedBadCookie + stats.droppedExpiredCookie + stats.droppedWrongProtocol +
                   stats.droppedDuplicate;
        }

        /// What the server knows of one authenticated client
        struct ClientInfo
        {
            std::string user;
            N2Net::JoinRole role = N2Net::JoinRole::Player;
            std::string address;
        };

        void LogAdmin(const std::string &text)
        {
            // Users and peer addresses are what the editor host calls Admin lines
            Logger::Info(text, Logger::LogSensitivity::Admin);
        }
    }

    int RunServer(int argc, char *argv[])
    {
        const auto parsed = ParseServerArguments(argc > 1 ? std::vector<std::string>(argv + 1, argv + argc)
                                                          : std::vector<std::string>{});
        if (!parsed)
        {
            std::println(stderr, "N2Server: {}", parsed.error());
            return 1;
        }
        return RunServer(*parsed);
    }

    int RunServer(const ServerOptions &options)
    {
        return RunServer(options, ServerHooks{});
    }

    int RunServer(const ServerOptions &options, const ServerHooks &hooks)
    {
        if (options.showHelp)
        {
            std::print("{}", ServerUsage());
            return 0;
        }
        if (!options.newTokenUser.empty())
        {
            return RunNewToken(options);
        }
        if (options.relay)
        {
            std::println(stderr, "N2Server --relay: not implemented (it is reserved for the deterministic relay mode, a later "
                                 "stage); start without it");
            return 1;
        }

        // The log sink: every Logger line at or above --log-level, one timestamped line each, on stderr (the journal,
        // `docker logs`) and in --log-file, in every build type. Before anything below logs. A --log-file that can't be
        // opened ends the server here, so an audit trail that was asked for is never silently missing.
        std::vector<Editor::LogOutput> logOutputs;
        logOutputs.push_back(Editor::StderrLogOutput());
        if (!options.logFile.empty())
        {
            auto fileOutput = Editor::OpenLogFileOutput(options.logFile);
            if (!fileOutput)
            {
                std::println(stderr, "N2Server {}", fileOutput.error());
                return 1;
            }
            logOutputs.push_back(std::move(*fileOutput));
        }
#ifdef SIGPIPE
        // A stderr (or a pipe behind it) whose reader has gone is a failed write the sink ignores, never a signal that
        // ends the server
        std::signal(SIGPIPE, SIG_IGN);
#endif
        const Editor::LogSink logSink(options.logLevel, std::move(logOutputs));

        if (!Security::CryptoAvailable() || !Security::InitializeCrypto())
        {
            std::println(stderr, "N2Server: this build has no working crypto library, so it cannot authenticate anyone");
            return 1;
        }

        // The tokens file: loaded (and checked: a regular file, small, private to its owner on POSIX, well formed) before
        // the engine starts. An error ends the server here with one line on stderr; it never says anything of the tokens.
        std::shared_ptr<const N2Net::JoinTokenTable> tokens;
        if (!options.tokensFile.empty())
        {
            auto loaded = N2Net::JoinTokenTable::Load(options.tokensFile);
            if (!loaded)
            {
                std::println(stderr, "N2Server --tokens-file: {}", loaded.error());
                return 1;
            }
            tokens = std::make_shared<const N2Net::JoinTokenTable>(std::move(*loaded));
        }

        // The project folder must exist and hold a valid project.n2proj of this engine's major version
        std::error_code error;
        const std::filesystem::path projectDir = std::filesystem::canonical(options.projectPath, error);
        if (error || !std::filesystem::is_directory(projectDir, error))
        {
            std::println(stderr, "Project folder not found or not a folder: {}", options.projectPath);
            return 1;
        }
        auto project = IO::ProjectFile::Load(projectDir);
        if (!project)
        {
            std::println(stderr, "{}", project.error());
            return 1;
        }
        const IO::EngineVersionMatch versionMatch = IO::CompareEngineVersions(project->engineVersion, EngineVersion());
        if (versionMatch == IO::EngineVersionMatch::MajorDiffers)
        {
            std::println(stderr, "{} was made with engine {}; this is engine {}, a different major version",
                         IO::ProjectFile::FileName, project->engineVersion, EngineVersion());
            return 1;
        }
        const std::string scenePath = options.scene.empty() ? project->startupScene : options.scene;
        if (scenePath.empty())
        {
            std::println(stderr, "N2Server: no scene to run: give --scene res://<folder>/<name>.scene, or set the project's "
                                 "startupScene");
            return 1;
        }

        g_running = true;
        g_reloadTokens = false;
        g_stopped = false;
        // The engine's orderly shutdown (the scene's OnApplicationQuit, then Application::Shutdown), run at most once on
        // every path that has started the engine, the exception path included. It never throws: whatever goes wrong in
        // it is logged and the rest still runs.
        bool engineRunning = false;
        const auto stopEngine = [&engineRunning]
        {
            if (!engineRunning)
            {
                return;
            }
            engineRunning = false;
            try
            {
                if (SceneManager::GetCurScene() != nullptr)
                {
                    SceneManager::GetCurSceneRef().OnApplicationQuit();
                }
            }
            catch (const std::exception &e)
            {
                Logger::Error(std::string("OnApplicationQuit failed while shutting down: ") + e.what());
            }
            catch (...)
            {
                Logger::Error("OnApplicationQuit failed while shutting down");
            }
            try
            {
                Application::GetInstance().Shutdown();
                Logger::Info("Engine shut down");
            }
            catch (const std::exception &e)
            {
                Logger::Error(std::string("Application::Shutdown failed: ") + e.what());
            }
            catch (...)
            {
                Logger::Error("Application::Shutdown failed");
            }
        };
        // Restored on every return below, the exception path included
        const SignalHandlersScope signalHandlers(!options.tokensFile.empty());

        try
        {
            // The project's own Lua runs here: this is the game, not an editor
            Scripting::LuaRuntime::SetExecutionPolicy(Scripting::LuaExecutionPolicy::Run);

            auto &app = Application::GetInstance();
            // Before Init, which may have started part of the engine before it throws
            engineRunning = true;
            const EngineHealth health = app.Init({
                .projectPath = projectDir.string(),
                .physicsBackend = Config::ApplicationOptions::PhysicsBackend::AUTO,
                .renderBackend = Config::ApplicationOptions::RenderBackend::NONE,
                .isHeadless = true,
                .audio = false,
                .physicsGpu = false,
            });

            // A refused project (network.preset deterministic with unmet requirements: a Failed Network row) never runs:
            // Init started nothing and logged every unmet requirement
            if (const SubsystemStatus *network = health.Find("Network");
                network != nullptr && network->state == SubsystemState::Failed)
            {
                std::println(stderr, "N2Server: the engine refused to start the project: {}", network->detail);
                stopEngine();
                return 1;
            }
            Logger::Info("Engine initialized (headless: no renderer, no window, no audio, physics GPU off)");

            // After Init, so their log lines reach the sink. The project's id is the asset UUID namespace, res:// resolves
            // under <project>/assets, user:// is the project's own folder, and its settings apply (after Init, which
            // resets what they set).
            Logger::Info(std::format("Opening project '{}' ({})", project->name, project->projectId.ToString()));
            if (versionMatch != IO::EngineVersionMatch::Compatible)
            {
                Logger::Warn(std::format("{} was made with engine {}; this is engine {}", IO::ProjectFile::FileName,
                                         project->engineVersion, EngineVersion()));
            }
            IO::ResourceUUID::Initialize(project->projectId);
            IO::ResourceLoader::Instance().Initialize(projectDir, project->UserDataPath());
            for (const std::string &problem : ApplyProjectSettings(project->settings))
            {
                Logger::Warn("Project settings: " + problem);
            }

            const auto loadedScene = LoadSceneForRun(projectDir, scenePath);
            if (!loadedScene)
            {
                std::println(stderr, "N2Server --scene: {}", loadedScene.error());
                Logger::Error("Couldn't load the scene: " + loadedScene.error());
                stopEngine();
                return 1;
            }
            Logger::Info("Loaded scene " + loadedScene->resourcePath);

            // The network world: the scene's NetworkObjects get their net ids (matched by GameObject UUID, so a client
            // that loads the same scene file computes the same ones). Declared before the transport and the server, so
            // it outlives both. A scene whose objects can't be matched (a UUID used twice) is a setup failure.
            Network::NetworkWorld world;
            const auto matched = world.BindScene(SceneManager::GetCurSceneRef());
            if (!matched)
            {
                std::println(stderr, "N2Server --scene: the network objects of {} can't be matched: {}",
                             loadedScene->resourcePath, matched.error());
                Logger::Error("Couldn't match the scene's network objects: " + matched.error());
                stopEngine();
                return 1;
            }
            Logger::Info(std::format("Network world: {} scene object(s) with a NetworkObject", *matched));
            if (hooks.beforeListening)
            {
                if (const std::optional<std::string> problem = hooks.beforeListening())
                {
                    std::println(stderr, "N2Server: {}", *problem);
                    Logger::Error("Startup hook failed: " + *problem);
                    stopEngine();
                    return 1;
                }
            }
            // Frozen now: a spawn names a prefab by its index in this table, which the fingerprint covers
            Network::PrefabTable prefabs = Network::PrefabTable::Capture();
            Logger::Info(std::format("Network world: {} prefab(s) in the prefab table", prefabs.Size()));

            // The network: a SecureTransport over a UdpTransport. It is the only way to open the UDP transport with
            // encryption counted as available, which is what allows a non-loopback bind at all (ParseServerArguments
            // has already refused one that has no tokens file and no --insecure-bind).
            N2Net::UdpTransportOptions udp;
            udp.bindAddress = options.bindAddress;
            udp.port = static_cast<std::uint16_t>(options.port);
            // Room for the clients and for the sources still in their handshake
            udp.maxPeers = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(options.maxClients) * 4u, 256u,
                                                     N2Net::kHardMaxUdpPeers);
            N2Net::SecureTransportOptions secureOptions;
            secureOptions.role = N2Net::SecureRole::Server;
            secureOptions.protocolId = options.protocolId;
            secureOptions.tokens = tokens;
            secureOptions.maxSessions = static_cast<std::uint32_t>(options.maxClients);
            N2Net::SecureUdpOpenResult opened = N2Net::SecureTransport::OpenUdp(udp, std::move(secureOptions));
            if (opened.transport == nullptr || !opened.transport->Healthy())
            {
                std::println(stderr, "N2Server: can't listen on {}: {}",
                             FormatAddress(options.bindAddress, static_cast<std::uint16_t>(options.port)),
                             opened.transport == nullptr ? opened.message : std::string("the secure layer could not start"));
                Logger::Error("Couldn't listen: " + opened.message);
                stopEngine();
                return 1;
            }
            N2Net::SecureTransport &secure = *opened.transport;
            N2Net::UdpTransport &udpTransport = *opened.udp;

            std::unique_ptr<N2Net::WebSocketTransport> webSocket;
            std::vector<N2Net::ITransport *> transports{&secure};
            if (options.webSocketPort >= 0)
            {
                N2Net::WebSocketListenOptions ws;
                ws.port = static_cast<std::uint16_t>(options.webSocketPort);
                ws.protocolId = options.protocolId;
                ws.maxPeers = static_cast<std::uint32_t>(options.maxClients);
                ws.allowedOrigin = options.webSocketOrigin;
                ws.authenticate = [&tokens](std::string_view token) -> std::optional<N2Net::JoinTokenEntry> {
                    if (!tokens) return {};
                    const auto hash = N2Net::HashJoinToken(token);
                    return hash ? tokens->Authenticate(*hash,std::nullopt) : std::nullopt;
                };
                std::string error;
                webSocket = N2Net::WebSocketTransport::Listen(std::move(ws),error);
                if (!webSocket) { Logger::Error(error); stopEngine(); return 1; }
                transports.push_back(webSocket.get());
            }
            N2Net::MultiplexTransport transport(std::move(transports));
            const std::string boundAddress = udpTransport.LocalAddress();
            const std::uint16_t boundPort = udpTransport.LocalPort();
            Logger::Info(std::format("Listening on {} (UDP), at most {} client(s), protocol id 0x{:08X}",
                                     FormatAddress(boundAddress, boundPort), options.maxClients, options.protocolId));
            if (!tokens)
            {
                Logger::Warn("No --tokens-file: nobody can join this server");
            }
            else if (tokens->Size() == 0)
            {
                Logger::Warn("The tokens file has no users: nobody can join this server");
            }
            else
            {
                // The path and the count only: never a token, and a hash is in the file, not here
                Logger::Info(std::format("Clients must join with the token of one of the {} user(s) in the tokens file {}",
                                         tokens->Size(), options.tokensFile));
            }
            if (!N2Net::IsLoopbackAddressText(options.bindAddress))
            {
                // Every session is encrypted and authenticated; what is exposed is the port, the handshake's CPU cost
                // (rate limited, with a bound on pending handshakes) and the number of sessions
                Logger::Warn("Bound to " + boundAddress + ", which is not a loopback address: anything that can reach the "
                             "port can start a handshake (limited by the pre-auth slots and rate limits); only a token "
                             "in the tokens file gets a session");
                if (options.insecureBind && options.tokensFile.empty())
                {
                    Logger::Warn("--insecure-bind was given and there is no tokens file: the server would otherwise have "
                                 "refused to start");
                }
            }

            // For launchers (format in ServerOptions.hpp), once listening: straight to C stdout, flushed, since a piped
            // stdout is fully buffered
            std::println(stdout, "{}{}", FormatReadyLine(boundPort, boundAddress),
                webSocket ? std::format(" websocketPort={}",webSocket->LocalPort()) : std::string{});
            std::fflush(stdout);

            // --exit-on-stdin-eof: the flag turns true when stdin ends or fails (a launcher that died)
            std::shared_ptr<std::atomic<bool>> stdinClosed;
            if (options.exitOnStdinEof)
            {
                stdinClosed = Editor::StartStdinEofWatcher();
                Logger::Info("The server exits when its stdin closes (--exit-on-stdin-eof)");
            }

            std::map<N2Net::PeerId, ClientInfo> clients;
            // Sessions to drop once the network update has returned (a transport's Disconnect does nothing inside its
            // Poll's handler)
            std::vector<N2Net::PeerId> dropAfterPoll;
            std::uint64_t clientsEverConnected = 0;
            // A client leaves the list when its transport peer goes: remotely (PeerDisconnected) or because the world
            // dropped it (a local Disconnect raises no event, so the network server's Left event calls this too)
            const auto forgetClient = [&](const N2Net::PeerId peer)
            {
                const auto found = clients.find(peer);
                if (found == clients.end())
                {
                    return;
                }
                const ClientInfo info = found->second;
                clients.erase(found);
                LogAdmin(std::format("Client disconnected: user={} role={} peer={} ({}/{})", info.user,
                                     N2Net::JoinRoleName(info.role), peer, clients.size(), options.maxClients));
            };
            const auto onEvent = [&](const N2Net::TransportEvent &event)
            {
                if (event.type == N2Net::TransportEventType::PeerConnected)
                {
                    N2Net::SecurePeerIdentity identity;
                    N2Net::ITransport *source = nullptr;
                    N2Net::PeerId sourcePeer = 0;
                    const bool routed = transport.Source(event.peer,source,sourcePeer);
                    bool identified = false;
                    if (routed && source == &secure) identified = secure.GetPeerIdentity(sourcePeer,identity);
                    else if (routed && webSocket && source == webSocket.get())
                    {
                        if (const auto who = webSocket->Identity(sourcePeer))
                        { identity.user = who->user; identity.role = who->role; identified = true; }
                    }
                    if (!identified)
                    {
                        // Cannot happen (a session is made from the token entry that matched, and carries its user and
                        // role); never keep a client that has none: the session is disconnected after this Poll
                        Logger::Warn("A session came up without an identity; it is disconnected after this poll");
                        dropAfterPoll.push_back(event.peer);
                        return;
                    }
                    ClientInfo info;
                    info.user = identity.user;
                    info.role = identity.role;
                    N2Net::UdpPeerInfo peerInfo;
                    if (source == &secure && udpTransport.GetPeerInfo(sourcePeer, peerInfo))
                    {
                        info.address = FormatAddress(peerInfo.address, peerInfo.port);
                    }
                    clients[event.peer] = info;
                    ++clientsEverConnected;
                    LogAdmin(std::format("Client connected: user={} role={} peer={} address={} ({}/{})", info.user,
                                         N2Net::JoinRoleName(info.role), event.peer,
                                         info.address.empty() ? std::string("unknown") : info.address, clients.size(),
                                         options.maxClients));
                }
                else if (event.type == N2Net::TransportEventType::PeerDisconnected)
                {
                    forgetClient(event.peer);
                }
                // Datagrams are the network server's: it runs a Connection per client over the same events
            };

            // The world protocol over the secure transport (NetworkServer polls the transport itself; onEvent above still
            // sees every event first, for the connect and disconnect log lines)
            Network::NetworkServerOptions netOptions;
            netOptions.protocolId = options.protocolId;
            netOptions.maxClients = static_cast<std::uint32_t>(options.maxClients);
            netOptions.scenePath = loadedScene->resourcePath;
            netOptions.sceneHash = loadedScene->contentHash;
            netOptions.prefabs = std::move(prefabs);
            // The networked input layout the game's scripts declared while the scene loaded (Network.DeclareInputLayout);
            // empty when none did, and then the server takes no input
            netOptions.inputLayout = Network::DeclaredInputLayout();
            // How often the replicated fields are sent: the project's network.authoritative.snapshotRate (30 by default)
            netOptions.stateSendRateHz = static_cast<std::uint32_t>(
                std::max(1, ReadNetworkSettings(project->settings).authoritative.snapshotRate));
            // The tick rate the clock pongs announce to the clients (netcode stage B3): the rate the engine's fixed ticks
            // really run at (under the None preset that is the fixed time step, not network.tickRate), or the clients'
            // clocks would run at another speed than the server's tick counter
            netOptions.tickRate = static_cast<std::uint32_t>(
                std::clamp(std::lround(1.0 / std::max(static_cast<double>(Time::GetFixedDeltaTime()), 1e-6)), 1l, 1000l));
            netOptions.lagCompMaxMicros =static_cast<std::uint64_t>(ReadNetworkSettings(project->settings).authoritative.lagCompMaxMs) * 1000;
            Network::NetworkServer netServer(transport, world, std::move(netOptions));
            netServer.BindPhysicsQueries();
            netServer.SetTransportObserver(onEvent);
            netServer.SetClientObserver(
                [&forgetClient](const Network::NetworkClientEvent &event)
                {
                    if (event.type == Network::NetworkClientEvent::Type::Joined)
                    {
                        LogAdmin(std::format("Network client joined the world: client={} peer={}", event.client,
                                             event.peer));
                    }
                    else
                    {
                        LogAdmin(std::format("Network client left the world: client={} peer={} joined={} ({})",
                                             event.client, event.peer, event.wasJoined ? "yes" : "no", event.reason));
                        forgetClient(event.peer);
                    }
                });
            const auto loopStart = std::chrono::steady_clock::now();
            const auto elapsedMicros = [&loopStart]
            {
                return static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - loopStart)
                        .count());
            };
            bool hookStop = false;

            TickOptions tick;
            tick.pollEvents = false; // no window to poll
            tick.render = false;     // nothing to draw with

            auto lastDropReport = std::chrono::steady_clock::now();
            std::uint64_t lastDropTotal = 0;

            // Main loop: network events and sends (NetworkServer::Update), then the fixed ticks that are due
            // (Application::Tick), then a short sleep
            while (g_running && !app.IsQuitRequested() && !hookStop && (!stdinClosed || !stdinClosed->load()))
            {
                // A SIGHUP: the new table applies to handshakes from now on; sessions that exist are kept. A file that
                // fails its checks leaves the old table in place.
                if (g_reloadTokens.exchange(false) && !options.tokensFile.empty())
                {
                    auto reloaded = N2Net::JoinTokenTable::Load(options.tokensFile);
                    if (reloaded)
                    {
                        const std::size_t users = reloaded->Size();
                        tokens = std::make_shared<const N2Net::JoinTokenTable>(std::move(*reloaded));
                        secure.SetTokens(tokens);
                        Logger::Info(std::format("Reloaded the tokens file: {} user(s)", users));
                    }
                    else
                    {
                        Logger::Warn("The tokens file was not reloaded (the previous one stays in force): " +
                                     reloaded.error());
                    }
                }

                // The tick input frames are bounded against (the most recent one begun)
                netServer.SetCurrentTick(static_cast<std::uint32_t>(Time::GetTick()));
                netServer.Update(elapsedMicros());
                for (const N2Net::PeerId peer : dropAfterPoll)
                {
                    netServer.DropPeer(peer, "it came up without an identity");
                }
                dropAfterPoll.clear();
                app.Tick(tick);
                if (hooks.onTick && !hooks.onTick(netServer, elapsedMicros()))
                {
                    hookStop = true;
                    Logger::Info("The server's host asked it to stop");
                }

                const auto now = std::chrono::steady_clock::now();
                if (now - lastDropReport >= kDropReportInterval)
                {
                    lastDropReport = now;
                    const std::uint64_t total = DropTotal(secure.Stats());
                    if (total != lastDropTotal)
                    {
                        lastDropTotal = total;
                        Logger::Info("Handshakes dropped since the server started: " + DropText(secure.Stats()));
                    }
                }
                std::this_thread::sleep_for(kLoopSleep);
            }

            if (!g_running)
            {
                Logger::Info("Shutdown signal received");
            }
            if (stdinClosed && stdinClosed->load())
            {
                Logger::Info("Stdin closed");
            }

            // Tell the clients (a sealed Disconnect each), then close the socket
            for (const auto &client : clients)
            {
                transport.Disconnect(client.first);
            }
            clients.clear();
            {
                const N2Net::SecureTransportStats &stats = secure.Stats();
                const std::string drops = DropText(stats);
                Logger::Info(std::format("Stopping: {} session(s) established, {} client(s) connected over the run, {} "
                                         "client(s) joined the world; handshakes dropped: {}",
                                         stats.sessionsEstablished, clientsEverConnected, netServer.Stats().clientsJoined,
                                         drops.empty() ? std::string("none") : drops));
            }
            opened.transport.reset();
            opened.udp = nullptr;
            Logger::Info("Server stopped");

            // As Application::Run does, however the loop ended
            stopEngine();
        }
        catch (const std::exception &e)
        {
            Logger::Error(std::string("Fatal error: ") + e.what());
            // The locals of the try block (the transport) are already gone; now the engine, in order, as on a normal exit
            stopEngine();
            g_stopped = true;
            return 1;
        }
        g_stopped = true;
        return 0;
    }
}
