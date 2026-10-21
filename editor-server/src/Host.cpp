#include "editor-server/Host.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/Version.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/scripting/ScriptDeclarationCache.hpp"

#include "editor-server/EditorServer.hpp"
#include "editor-server/EditorTls.hpp"
#include "editor-server/LogSink.hpp"
#include "editor-server/ServerLog.hpp"

namespace N2Engine::Editor
{
    namespace
    {
        std::atomic<bool> g_running{true};
        // SIGHUP (POSIX, with a tokens file): the main loop passes it on to the server, which reloads on its network thread
        std::atomic<bool> g_reloadTokens{false};

        void SignalHandler(int signal)
        {
            // Only the flag: the Logger is thread-safe (on Windows this runs on another thread), but a POSIX
            // handler can interrupt a thread that holds the Logger's lock, so logging here isn't signal-safe
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

        /// Installs SignalHandler for SIGINT and SIGTERM (and, where there is one and `reloadOnHangup` is set, SIGHUP:
        /// without a tokens file a hangup keeps its default, which ends the process), and puts back whatever was there
        /// before when it goes out of scope, so RunHost leaves a game's own host (or a test) with the handlers it had
        class SignalHandlersScope
        {
        public:
            explicit SignalHandlersScope(const bool reloadOnHangup = false)
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
            }

            ~SignalHandlersScope()
            {
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
        };

        Config::ApplicationOptions::RenderBackend ToRenderBackend(const HostRenderer renderer)
        {
            return renderer == HostRenderer::Software ? Config::ApplicationOptions::RenderBackend::SOFTWARE
                                                      : Config::ApplicationOptions::RenderBackend::OPENGL;
        }
    }

    int RunCreate(const HostOptions &options)
    {
        // CPU only: no Application::Init, so nothing (PhysX, GL, audio) is started and nothing but the lines below
        // reaches stdout or stderr
        IO::CreateProjectOptions create;
        create.name = options.projectName;
        create.projectId = options.projectIdFromPath
                               ? std::optional<Math::UUID>(IO::ResourceUUID::NamespaceForProjectDir(options.createPath))
                               : options.projectId;

        const auto created = IO::CreateProject(std::filesystem::path(options.createPath), create);
        if (!created)
        {
            std::println(stderr, "N2EditorHost --create: {}", created.error().message);
            return created.error().kind == IO::CreateProjectErrorKind::AlreadyAProject ? ExitCodeAlreadyAProject : 1;
        }
        std::println(stdout, "{}", FormatCreatedLine(created->projectId, created->startupScene));
        std::fflush(stdout);
        return 0;
    }

    int RunNewToken(const HostOptions &options)
    {
        // No engine: nothing but the token and the entry reaches stdout, and a failure one line on stderr
        const auto made = GenerateToken(options.newTokenUser, options.newTokenRole);
        if (!made)
        {
            std::println(stderr, "N2EditorHost --new-token: {}", made.error());
            return 1;
        }
        std::print(stdout, "{}", FormatNewTokenOutput(options.newTokenUser, options.newTokenRole, *made));
        std::fflush(stdout);
        return 0;
    }

    int RunHost(int argc, char *argv[])
    {
        const auto parsed = ParseHostArguments(argc > 1 ? std::vector<std::string>(argv + 1, argv + argc)
                                                        : std::vector<std::string>{});
        if (!parsed)
        {
            std::println(stderr, "{}", parsed.error());
            return 1;
        }
        return RunHost(*parsed);
    }

    int RunHost(const HostOptions &options)
    {
        if (options.showHelp)
        {
            std::print("{}", HostUsage());
            return 0;
        }
        if (!options.createPath.empty())
        {
            return RunCreate(options);
        }
        if (!options.newTokenUser.empty())
        {
            return RunNewToken(options);
        }

        // The log sink: every Logger line at or above --log-level, one timestamped line each, on stderr (the journal, `docker
        // logs`) and in --log-file, in every build type (a Release build has no console subscriber of its own). Before
        // anything below logs, so a startup line is kept. A --log-file that can't be opened ends the host here, with one
        // line on stderr and exit code 1, so an audit trail that was asked for is never silently missing. Destroyed when
        // RunHost returns, after its last line.
        std::vector<LogOutput> logOutputs;
        logOutputs.push_back(StderrLogOutput());
        if (!options.logFile.empty())
        {
            auto fileOutput = OpenLogFileOutput(options.logFile);
            if (!fileOutput)
            {
                std::println(stderr, "N2EditorHost {}", fileOutput.error());
                return 1;
            }
            logOutputs.push_back(std::move(*fileOutput));
        }
#ifdef SIGPIPE
        // A stderr (or a pipe behind it) whose reader has gone is a failed write the sink ignores, never a signal that ends
        // the host (the editor server does the same when it starts, which is after the first lines are written)
        std::signal(SIGPIPE, SIG_IGN);
#endif
        const LogSink logSink(options.logLevel, std::move(logOutputs));

        // Before the engine starts (and Lua with it): the variable is removed from the environment once read. On
        // stderr, as the Logger has no console output until Application::Init (and none in Release).
        std::string accessToken;
        if (!options.tokenEnv.empty())
        {
            auto token = ReadAccessToken(options.tokenEnv);
            if (!token)
            {
                std::println(stderr, "{}", token.error());
                return 1;
            }
            accessToken = std::move(*token);
        }

        // A tokens file needs the TLS library for its SHA-256: a host asked for per-user tokens that it can't check must
        // not start (and so must not fall back to an open one). The file itself is loaded below, with the server.
        if (!options.tokensFile.empty() && !EditorTlsServer::IsAvailable())
        {
            std::println(stderr, "N2EditorHost --tokens-file: this build has no TLS library, which supplies the SHA-256 "
                                 "of the token hashes (configure it with -DN2ENGINE_EDITOR_TLS=ON)");
            return 1;
        }

        // Before the engine starts, like the token: a host asked for TLS that can't serve it must not start. This is the
        // refusal of a build without N2ENGINE_EDITOR_TLS too, so a launcher never gets a plaintext host it asked to be
        // encrypted.
        if (options.tls && !EditorTlsServer::IsAvailable())
        {
            std::println(stderr, "N2EditorHost --tls: this build has no TLS support (configure it with "
                                 "-DN2ENGINE_EDITOR_TLS=ON)");
            return 1;
        }

        // The project folder must exist (ResourceLoader::Initialize would otherwise create it, for .import/) and hold
        // a valid project.n2proj of this engine's major version. Checked before the engine starts, so a launcher
        // pointed at the wrong folder gets one line on stderr and exit code 1.
        std::filesystem::path projectDir;
        std::optional<IO::ProjectFile> project;
        IO::EngineVersionMatch versionMatch = IO::EngineVersionMatch::Compatible;
        if (!options.projectPath.empty())
        {
            std::error_code error;
            projectDir = std::filesystem::canonical(options.projectPath, error);
            if (error || !std::filesystem::is_directory(projectDir, error))
            {
                std::println(stderr, "Project folder not found or not a folder: {}", options.projectPath);
                return 1;
            }
            auto loaded = IO::ProjectFile::Load(projectDir);
            if (!loaded)
            {
                std::println(stderr, "{}", loaded.error());
                // A folder an older host or lua_project opened has .meta files whose UUIDs came from its path: a plain
                // --create would give it a new id and change them all
                if (!std::filesystem::exists(IO::ProjectFile::PathIn(projectDir), error) &&
                    std::filesystem::is_directory(projectDir / ".import", error))
                {
                    std::println(stderr, "It has asset metadata (.import/): to keep its asset UUIDs, make it a project "
                                         "with N2EditorHost --create \"{}\" --project-id {}",
                                 options.projectPath, ProjectIdFromPath);
                }
                return 1;
            }
            versionMatch = IO::CompareEngineVersions(loaded->engineVersion, EngineVersion());
            if (versionMatch == IO::EngineVersionMatch::MajorDiffers)
            {
                std::println(stderr, "{} was made with engine {}; this is engine {}, a different major version",
                             IO::ProjectFile::FileName, loaded->engineVersion, EngineVersion());
                return 1;
            }
            project = std::move(*loaded);
        }

        g_running = true;
        // Restored on every return below, the exception path included
        const SignalHandlersScope signalHandlers(!options.tokensFile.empty());

        try
        {
            // Headless: a hidden window (OpenGL), or no window at all (the software renderer, Window::UsesNoWindow),
            // and audio on a loopback device streamed to the client
            // Constructed (not started) before Application::Init: it subscribes its event ring to the Logger, so every
            // startup line is kept for a client that connects later (PollEvents from 0). Not broadcastUnbroadcastLogs:
            // a Debug build's console subscribes during Init and would take that backlog.
            EditorServer server;

            // --tls: the certificate and key come from (or are made in) the user's data folder, never the project's.
            // An error ends the host here, before the engine starts, with one line on stderr and exit code 1.
            if (options.tls)
            {
                const std::filesystem::path tlsDirectory = EditorTlsServer::DefaultDirectory();
                std::error_code tlsError;
                if (!tlsDirectory.empty() && !projectDir.empty())
                {
                    // Belt and braces: the user's data folder is not meant to be inside a project, but a HOME or APPDATA
                    // that points into one must not put the private key where a commit could pick it up
                    const std::filesystem::path resolved = std::filesystem::weakly_canonical(tlsDirectory, tlsError);
                    const std::filesystem::path relative = resolved.lexically_relative(projectDir);
                    if (!tlsError && !relative.empty() && *relative.begin() != "..")
                    {
                        std::println(stderr, "N2EditorHost --tls: the certificate folder {} is inside the project",
                                     tlsDirectory.string());
                        return 1;
                    }
                }
                if (const auto enabled = server.EnableTls(tlsDirectory); !enabled)
                {
                    std::println(stderr, "N2EditorHost --tls: {}", enabled.error());
                    return 1;
                }
            }

            // --tokens-file: loaded (and checked: private on POSIX, well formed) before the engine starts, like the token. An
            // error ends the host here with one line on stderr and exit code 1; it never says anything of the tokens.
            if (!options.tokensFile.empty())
            {
                if (const auto loaded = server.SetTokensFile(options.tokensFile); !loaded)
                {
                    std::println(stderr, "N2EditorHost --tokens-file: {}", loaded.error());
                    return 1;
                }
            }

            // Scripts in the scenes a client loads must not end an editor host: only the --play child, which is a running
            // game, lets Application.Quit through. Set before any scene (and so any script) is loaded.
            Application::SetScriptQuitAllowed(!options.playScene.empty());

            // The same split for project Lua as a whole: a host that edits never runs it. A LuaComponent there reads the
            // fields its script declares (a budgeted evaluation in a throwaway state with no engine bindings, off this
            // thread) and the engine's Lua state runs nothing of the project: not a scene's script, not a callback, not a
            // reloaded module. Only the --play child, a running game, runs scripts. Process-wide and set before any scene
            // loads, because a scene is built (and its scripts would run) before it is put in edit mode.
            Scripting::LuaRuntime::SetExecutionPolicy(options.playScene.empty()
                                                          ? Scripting::LuaExecutionPolicy::DeclarationsOnly
                                                          : Scripting::LuaExecutionPolicy::Run);

            auto &app = Application::GetInstance();
            const EngineHealth health = app.Init({
                .projectPath = projectDir.string(),
                .physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX,
                .renderBackend = ToRenderBackend(options.renderer),
                .isHeadless = true,
            });

            // A refused project (network.preset deterministic with unmet requirements: a Failed Network row) is never
            // edited or played: Init started nothing and logged every unmet requirement, so the host ends here, before
            // play mode, the edit loop and the ready line, with exit code 1 and a line on stderr
            if (const SubsystemStatus *network = health.Find("Network");
                network != nullptr && network->state == SubsystemState::Failed)
            {
                std::println(stderr, "N2EditorHost: the engine refused to start the project: {}", network->detail);
                return 1;
            }

            ServerLog::Info("Engine initialized");

            // After Init, so their log lines reach the console (and the event ring). The project's id is the asset UUID
            // namespace, res:// resolves under <project>/assets, user:// is the project's own folder, and its settings
            // apply (after Init, which resets what they set). Without --project nothing is initialised, so assets and
            // asset references are unavailable, and no scene is loaded.
            if (project)
            {
                ServerLog::Info(std::format("Opening project '{}' ({})", project->name, project->projectId.ToString()));
                if (versionMatch != IO::EngineVersionMatch::Compatible)
                {
                    ServerLog::Warn(std::format("{} was made with engine {}; this is engine {}", IO::ProjectFile::FileName,
                                             project->engineVersion, EngineVersion()));
                }
                IO::ResourceUUID::Initialize(project->projectId);
                // A play host shares the folder with the host that edits, which owns its .meta files, .n2/asset-state.json
                // and .import folder: it reads them and writes none (ResourceLoader::SetReadOnly)
                IO::ResourceLoader::Instance().SetReadOnly(!options.playScene.empty());
                IO::ResourceLoader::Instance().Initialize(projectDir, project->UserDataPath());
                for (const std::string &problem : ApplyProjectSettings(project->settings))
                {
                    ServerLog::Warn("Project settings: " + problem);
                }

                server.SetScenesDirectory(projectDir / "scenes");
                server.SetProject(projectDir, *project);

                // The startup scene, or an empty one with no file yet (so entity commands have a scene). An exception
                // (a scene whose loading throws) is a warning like any other failure, never the end of the host.
                // A play host (--play) opens its snapshot below instead.
                if (options.playScene.empty() && !project->startupScene.empty())
                {
                    try
                    {
                        // One allowance for the declarations of every script of the scene, as a command has
                        const Scripting::ScriptDeclarationCache::CommandBudget evaluationBudget;
                        if (const auto opened = server.OpenSceneFile(project->startupScene); !opened)
                        {
                            ServerLog::Warn("Couldn't open the startup scene: " + opened.error());
                        }
                    }
                    catch (const std::exception &e)
                    {
                        ServerLog::Warn(std::string("Couldn't open the startup scene: ") + e.what());
                    }
                }
                if (options.playScene.empty() && SceneManager::GetCurScene() == nullptr)
                {
                    try
                    {
                        if (const auto created = server.NewSceneFile("", ""); !created)
                        {
                            ServerLog::Warn("Couldn't create an empty scene: " + created.error());
                        }
                    }
                    catch (const std::exception &e)
                    {
                        ServerLog::Warn(std::string("Couldn't create an empty scene: ") + e.what());
                    }
                }
            }
            else
            {
                ServerLog::Info("No --project given: assets and asset references are unavailable");
            }

            // --editor-extensions: the project's Lua editor extensions, read once here (never as a side effect of loading a
            // scene) and again only on ReloadEditorExtensions. The flag combinations that make this unsafe were refused when
            // the arguments were parsed (ParseHostArguments).
            if (options.editorExtensions)
            {
                if (!project)
                {
                    ServerLog::Warn("--editor-extensions without --project: there is no project folder to find extensions in");
                }
                ServerLog::Info("Editor extensions are on (--editor-extensions): the project's editor/ folder runs Lua on this host");
                if (!server.EnableEditorExtensions(project ? projectDir : std::filesystem::path{}))
                {
                    ServerLog::Warn("The editor extensions could not be enabled");
                }
                else if (options.editorExtensionsWatch)
                {
                    server.SetEditorExtensionsWatching(true);
                    ServerLog::Info("The editor extensions watcher is on (--editor-extensions-watch): a changed file in "
                                    "editor/ is reloaded");
                }
            }

            // --play: the game is the snapshot (a file a launcher got from WritePlaySnapshot, or a res:// scene of the
            // project); the host serves it as a play host. A snapshot that can't be played ends the host before it
            // listens, with exit code 1 and a line on stderr, so a launcher never waits for a ready line that won't come.
            if (!options.playScene.empty())
            {
                std::filesystem::path snapshotFile;
                std::string playError;
                if (options.playScene.starts_with("res://"))
                {
                    if (!project)
                    {
                        playError = "a res:// scene needs --project";
                    }
                    else if (const auto resolved = EditorServer::ResolveScenePath(projectDir / "assets", options.playScene))
                    {
                        snapshotFile = resolved->file;
                    }
                    else
                    {
                        playError = resolved.error();
                    }
                }
                else
                {
                    snapshotFile = std::filesystem::absolute(std::filesystem::path(options.playScene));
                }
                if (playError.empty())
                {
                    if (const auto entered = server.EnterPlayMode(snapshotFile); !entered)
                    {
                        playError = entered.error();
                    }
                }
                if (!playError.empty())
                {
                    std::println(stderr, "N2EditorHost --play: {}", playError);
                    ServerLog::Error("Couldn't start playing: " + playError);
                    app.Shutdown();
                    return 1;
                }
            }

            // Configure and start the editor server (constructed before Init, above)
            if (!accessToken.empty())
            {
                (void)server.SetAccessToken(accessToken);
                // The variable's name only, never the token
                ServerLog::Info("Clients must send Hello with the access token from " + options.tokenEnv);
            }
            if (server.HasTokensFile())
            {
                // The path only: never a token, and a hash is in the file, not here
                ServerLog::Info("Clients must send Hello with the token of a user in the tokens file " + options.tokensFile);
            }
            if (server.IsTlsEnabled())
            {
                // The fingerprint is public (it is in the ready line); the key and the token never reach a log
                ServerLog::Info("Serving TLS; certificate SHA-256 " + server.TlsFingerprint());
            }
            if (options.noRateLimit)
            {
                (void)server.SetRateLimiting(false);
                if (server.RequiresAccessToken())
                {
                    ServerLog::Warn("Failed-Hello rate limiting is off (--no-rate-limit): wrong tokens can be tried as fast "
                                 "as the host can answer, and each costs it up to the Hello timeout");
                }
            }
            else if (server.RequiresAccessToken())
            {
                ServerLog::Info("Failed-Hello rate limiting is on: an address that fails 3 Hellos, or connects and never "
                             "finishes one, is backed off (1 s, doubling to 5 min) and closed at accept; loopback has no "
                             "backoff (the slots and the handshake rate bound it); users in a tokens file have one of "
                             "up to 30 s, which the right token passes");
            }
            // The deadlines that bound how long a connection that has not said Hello holds one pre-auth slot (the
            // pre-Hello budget is EditorServer::DefaultHelloTimeout, 2 s, from the end of the TLS handshake, which has
            // its own DefaultTlsHandshakeTimeout, 5 s; neither is a flag)
            // The pre-auth side: this many connections in their TLS handshake and Hello at once, each non-loopback
            // address limited to its share, and handshakes (the TLS CPU) rationed. The host sees the peer address the
            // proxy or container presents: loopback only when the proxy connects over loopback, and loopback has no
            // per-address limits (see the docs).
            (void)server.SetPreAuthSlots(static_cast<size_t>(options.preAuthSlots));
            TokenBucketOptions handshakeRate;
            handshakeRate.globalPerSecond = 20;
            handshakeRate.perAddressPerSecond = 5;
            (void)server.SetHandshakeRateLimit(handshakeRate);
            (void)server.SetIdleTimeout(std::chrono::seconds(options.idleTimeoutSeconds));
            (void)server.SetWriteTimeout(std::chrono::seconds(options.writeTimeoutSeconds));
            (void)server.SetMaxSessionAge(std::chrono::minutes(options.maxSessionMinutes));
            ServerLog::Info(std::format("Connection limits: {}{} to say Hello, idle timeout {}, write timeout {} s, "
                                     "maximum session {}",
                                     options.tls ? std::format("{} ms for the TLS handshake, then ",
                                                               server.GetTlsHandshakeTimeout().count())
                                                 : std::string(),
                                     std::format("{} ms", server.GetHelloTimeout().count()),
                                     options.idleTimeoutSeconds > 0 ? std::format("{} s", options.idleTimeoutSeconds)
                                                                    : std::string("off"),
                                     options.writeTimeoutSeconds,
                                     options.maxSessionMinutes > 0 ? std::format("{} min", options.maxSessionMinutes)
                                                                   : std::string("off")));
            if (options.idleTimeoutSeconds == 0)
            {
                ServerLog::Warn("The idle timeout is off (--idle-timeout 0): a client that connects and goes quiet keeps "
                             "every other client out until it disconnects");
            }
            if (options.readOnly)
            {
                (void)server.SetReadOnly(true);
                ServerLog::Info("Read-only host (--read-only): commands that change anything are refused");
            }
            if (options.exitOnDisconnect)
            {
                (void)server.SetStopOnDisconnect(true);
                ServerLog::Info("The host exits when its client's session ends (--exit-on-disconnect)");
            }
            if (options.bindAddress != EditorServer::DefaultBindAddress)
            {
                // Only when the flag actually overrode the rule (non-loopback and not token + TLS)
                if (options.insecureBind && !IsLoopbackBindAddress(options.bindAddress) &&
                    !(server.RequiresAccessToken() && server.IsTlsEnabled()))
                {
                    ServerLog::Warn("--insecure-bind was given: " + options.bindAddress +
                                 " is not a loopback address and the host would otherwise have refused to start "
                                 "without a token and --tls");
                }
                if (server.IsTlsEnabled())
                {
                    // Encrypted, but a client that doesn't check the certificate's fingerprint can be talked into
                    // sending the token to someone else, and nothing here limits who may connect
                    ServerLog::Warn("Editor server bound to " + options.bindAddress +
                                 (!server.RequiresAccessToken()
                                      ? ": the traffic is encrypted (TLS) but it has no authentication, so anything that "
                                        "can reach it can control this host"
                                      : ": the traffic is encrypted (TLS), so a client that pins the certificate "
                                        "fingerprint from the ready line keeps the access token private; one that "
                                        "doesn't can be impersonated and give it away"));
                }
                else
                {
                    ServerLog::Warn("Editor server bound to " + options.bindAddress +
                                 (!server.RequiresAccessToken()
                                      ? ": it has no authentication, so anything that can reach it can control this host"
                                      : ": the access token isn't encrypted, so anything that can see the traffic can "
                                        "take it"));
                }
            }
            const bool serverStarted = server.Start(options.port, options.bindAddress);
            if (!serverStarted)
            {
                ServerLog::Error("Editor server failed to start");
            }
            else
            {
                // For launchers (format in HostOptions.hpp): the bound port, which is the OS's pick with --port 0.
                // Straight to C stdout, as the Logger's console lines are (std::cout is redirected into the Logger,
                // which would prefix a level), and flushed, since a piped stdout is fully buffered. The network
                // thread posts its log lines to this thread, so no log line is written in the middle of this one.
                std::println(stdout, "{}", FormatReadyLine(server.GetPort(), server.TlsFingerprint()));
                std::fflush(stdout);
            }

            // Keep serving even without a renderer, so the editor can still query state such as GetEngineHealth
            auto &window = app.GetWindow();
            if (!window.IsValid())
            {
                ServerLog::Warn("No window or renderer; rendering commands will be unavailable");
            }
            else if (window.IsWindowless())
            {
                ServerLog::Info("Rendering with the software renderer, without a window");
            }

            // --exit-on-stdin-eof: the flag turns true when stdin ends or fails (a launcher that died)
            std::shared_ptr<std::atomic<bool>> stdinClosed;
            if (options.exitOnStdinEof)
            {
                stdinClosed = StartStdinEofWatcher();
                ServerLog::Info("The host exits when its stdin closes (--exit-on-stdin-eof)");
            }

            // Main loop: handle OS events and run the editor's requests. Requests touch engine state, so they
            // run here on the main thread (see EditorServer); waiting for them doubles as the idle sleep. A
            // windowless window never closes, so the other conditions end it.
            while (g_running && server.IsRunning() && !app.IsQuitRequested() &&
                   (!window.IsValid() || !window.ShouldClose()) && (!stdinClosed || !stdinClosed->load()))
            {
                // A SIGHUP: the server's network thread re-reads the tokens file (this only sets its flag)
                if (g_reloadTokens.exchange(false))
                {
                    server.RequestTokensReload();
                }

                if (server.IsPlayMode())
                {
                    // A play host: a game frame (Application::Tick polls the window's events and updates the input itself),
                    // then the client's commands for the rest of the frame, so neither starves the other
                    server.RunPlayFrame();
                    continue;
                }

                // Poll window events to keep OS happy (even if window is hidden), and update input
                window.PollEvents();

                server.ProcessCommands(std::chrono::milliseconds(16));
                // Mixes the headless engine's audio for the time that passed, client or not (see UpdateAudio)
                server.UpdateAudio();
            }

            if (!g_running)
            {
                ServerLog::Info("Shutdown signal received");
            }
            if (stdinClosed && stdinClosed->load())
            {
                ServerLog::Info("Stdin closed");
            }
            server.ProcessCommands(); // flushes log lines the network thread has posted
            server.Stop();
            ServerLog::Info("Editor server stopped");

            // The same for an editor extension still being evaluated (abandoned at its deadline, stuck in C)
            if (const int leftRunning = server.ShutdownEditorExtensions(std::chrono::milliseconds(1000)); leftRunning > 0)
            {
                ServerLog::Warn(std::format("{} editor extension evaluation(s) still running at exit; they are left to end with "
                                         "the process",
                                         leftRunning));
            }

            // A script evaluation that is still running (abandoned at its deadline, stuck in a long garbage collection)
            // cannot be stopped: wait a moment for it, then leave it. It owns everything it touches and holds no host
            // data, so the host ends either way (ScriptDeclarationCache).
            if (const int leftRunning = Scripting::ScriptDeclarationCache::Instance().Shutdown(); leftRunning > 0)
            {
                ServerLog::Warn(std::format("{} script evaluation(s) still running at exit; they are left to end with the process",
                                         leftRunning));
            }

            // As Application::Run does, however the loop ended
            if (SceneManager::GetCurScene() != nullptr)
            {
                SceneManager::GetCurSceneRef().OnApplicationQuit();
            }

            app.Shutdown();
            ServerLog::Info("Engine shut down");

            if (!serverStarted)
            {
                return 1;
            }
        }
        catch (const std::exception &e)
        {
            ServerLog::Error(std::string("Fatal error: ") + e.what());
            return 1;
        }

        return 0;
    }
}
