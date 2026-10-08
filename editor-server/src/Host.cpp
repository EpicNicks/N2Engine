#include "editor-server/Host.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
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

#include "editor-server/EditorServer.hpp"

namespace N2Engine::Editor
{
    namespace
    {
        std::atomic<bool> g_running{true};

        void SignalHandler(int signal)
        {
            // Only the flag: the Logger is thread-safe (on Windows this runs on another thread), but a POSIX
            // handler can interrupt a thread that holds the Logger's lock, so logging here isn't signal-safe
            if (signal == SIGINT || signal == SIGTERM)
            {
                g_running = false;
            }
        }

        /// Installs SignalHandler for SIGINT and SIGTERM, and puts back whatever was there before when it goes out of
        /// scope, so RunHost leaves a game's own host (or a test) with the handlers it had
        class SignalHandlersScope
        {
        public:
            SignalHandlersScope()
                : _previousInterrupt(std::signal(SIGINT, SignalHandler)),
                  _previousTerminate(std::signal(SIGTERM, SignalHandler))
            {
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
            }

            SignalHandlersScope(const SignalHandlersScope &) = delete;
            SignalHandlersScope &operator=(const SignalHandlersScope &) = delete;

        private:
            using Handler = decltype(SIG_DFL);
            Handler _previousInterrupt;
            Handler _previousTerminate;
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
        const SignalHandlersScope signalHandlers;

        try
        {
            // Headless: a hidden window (OpenGL), or no window at all (the software renderer, Window::UsesNoWindow),
            // and audio on a loopback device streamed to the client
            // Constructed (not started) before Application::Init: it subscribes its event ring to the Logger, so every
            // startup line is kept for a client that connects later (PollEvents from 0). Not broadcastUnbroadcastLogs:
            // a Debug build's console subscribes during Init and would take that backlog.
            EditorServer server;

            auto &app = Application::GetInstance();
            app.Init({
                .projectPath = projectDir.string(),
                .physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX,
                .renderBackend = ToRenderBackend(options.renderer),
                .isHeadless = true,
            });

            Logger::Info("Engine initialized");

            // After Init, so their log lines reach the console (and the event ring). The project's id is the asset UUID
            // namespace, res:// resolves under <project>/assets, user:// is the project's own folder, and its settings
            // apply (after Init, which resets what they set). Without --project nothing is initialised, so assets and
            // asset references are unavailable, and no scene is loaded.
            if (project)
            {
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

                server.SetScenesDirectory(projectDir / "scenes");
                server.SetProject(projectDir, *project);

                // The startup scene, or an empty one with no file yet (so entity commands have a scene)
                if (!project->startupScene.empty())
                {
                    if (const auto opened = server.OpenSceneFile(project->startupScene); !opened)
                    {
                        Logger::Warn("Couldn't open the startup scene: " + opened.error());
                    }
                }
                if (SceneManager::GetCurScene() == nullptr)
                {
                    if (const auto created = server.NewSceneFile("", ""); !created)
                    {
                        Logger::Warn("Couldn't create an empty scene: " + created.error());
                    }
                }
            }
            else
            {
                Logger::Info("No --project given: assets and asset references are unavailable");
            }

            // Configure and start the editor server (constructed before Init, above)
            if (!accessToken.empty())
            {
                (void)server.SetAccessToken(accessToken);
                // The variable's name only, never the token
                Logger::Info("Clients must send Hello with the access token from " + options.tokenEnv);
            }
            if (options.exitOnDisconnect)
            {
                (void)server.SetStopOnDisconnect(true);
                Logger::Info("The host exits when its client's session ends (--exit-on-disconnect)");
            }
            if (options.bindAddress != EditorServer::DefaultBindAddress)
            {
                Logger::Warn("Editor server bound to " + options.bindAddress +
                             (accessToken.empty()
                                  ? ": it has no authentication, so anything that can reach it can control this host"
                                  : ": the access token isn't encrypted, so anything that can see the traffic can "
                                    "take it"));
            }
            const bool serverStarted = server.Start(options.port, options.bindAddress);
            if (!serverStarted)
            {
                Logger::Error("Editor server failed to start");
            }
            else
            {
                // For launchers (format in HostOptions.hpp): the bound port, which is the OS's pick with --port 0.
                // Straight to C stdout, as the Logger's console lines are (std::cout is redirected into the Logger,
                // which would prefix a level), and flushed, since a piped stdout is fully buffered. The network
                // thread posts its log lines to this thread, so no log line is written in the middle of this one.
                std::println(stdout, "{}", FormatReadyLine(server.GetPort()));
                std::fflush(stdout);
            }

            // Keep serving even without a renderer, so the editor can still query state such as GetEngineHealth
            auto &window = app.GetWindow();
            if (!window.IsValid())
            {
                Logger::Warn("No window or renderer; rendering commands will be unavailable");
            }
            else if (window.IsWindowless())
            {
                Logger::Info("Rendering with the software renderer, without a window");
            }

            // Main loop: handle OS events and run the editor's requests. Requests touch engine state, so they
            // run here on the main thread (see EditorServer); waiting for them doubles as the idle sleep. A
            // windowless window never closes, so the other conditions end it.
            while (g_running && server.IsRunning() && !app.IsQuitRequested() &&
                   (!window.IsValid() || !window.ShouldClose()))
            {
                // Poll window events to keep OS happy (even if window is hidden), and update input
                window.PollEvents();

                server.ProcessCommands(std::chrono::milliseconds(16));
                // Mixes the headless engine's audio for the time that passed, client or not (see UpdateAudio)
                server.UpdateAudio();
            }

            if (!g_running)
            {
                Logger::Info("Shutdown signal received");
            }
            server.ProcessCommands(); // flushes log lines the network thread has posted
            server.Stop();
            Logger::Info("Editor server stopped");

            // As Application::Run does, however the loop ended
            if (SceneManager::GetCurScene() != nullptr)
            {
                SceneManager::GetCurSceneRef().OnApplicationQuit();
            }

            app.Shutdown();
            Logger::Info("Engine shut down");

            if (!serverStarted)
            {
                return 1;
            }
        }
        catch (const std::exception &e)
        {
            Logger::Error(std::string("Fatal error: ") + e.what());
            return 1;
        }

        return 0;
    }
}
