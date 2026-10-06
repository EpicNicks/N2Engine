#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <print>
#include <string>
#include <system_error>
#include <vector>

#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

#include "editor-server/EditorServer.hpp"
#include "editor-server/HostOptions.hpp"

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
}

int main(int argc, char *argv[])
{
    const auto parsed = N2Engine::Editor::ParseHostArguments(std::vector<std::string>(argv + 1, argv + argc));
    if (!parsed)
    {
        std::println(stderr, "{}", parsed.error());
        return 1;
    }
    const N2Engine::Editor::HostOptions &options = *parsed;

    if (options.showHelp)
    {
        std::println(R"(
                        N2Engine Editor Host
                        Usage: N2EditorHost [options]
                            Options:
                            -p, --port <port>      Server port (default: 9999; 0 lets the OS pick a free one)
                            --bind <ipv4 address>  Address to listen on (default: 127.0.0.1)
                                                   WARNING: the protocol has no authentication; any other
                                                   address lets whoever can reach it control this host,
                                                   including deleting scene files
                            --project <path>       Project folder: res:// assets load from <path>/assets,
                                                   and DeleteScene works in <path>/scenes
                            -h, --help             Show this help
                        Once listening, prints the line "N2EditorHost ready port=<port>" to stdout.
                        )");
        return 0;
    }

    // The project folder must exist: ResourceLoader::Initialize would otherwise create it (for .import/).
    // (The UUID namespace normalises the path itself: ResourceUUID::NamespaceForProjectDir.)
    std::filesystem::path projectDir;
    if (!options.projectPath.empty())
    {
        std::error_code error;
        projectDir = std::filesystem::canonical(options.projectPath, error);
        if (error || !std::filesystem::is_directory(projectDir, error))
        {
            std::println(stderr, "Project folder not found: {}", options.projectPath);
            return 1;
        }
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    try
    {
        // Initialize engine in editor mode (no window shown initially, or hidden)
        N2Engine::Application::GetInstance().Init({
            .projectPath = projectDir.string(),
            .physicsBackend = N2Engine::Config::ApplicationOptions::PhysicsBackend::PHYSX,
            .renderBackend = N2Engine::Config::ApplicationOptions::RenderBackend::OPENGL,
            .isHeadless = true,
        });

        N2Engine::Logger::Info("Engine initialized");

        // After Init, so their log lines reach the console. As lua_project does: asset UUIDs are named from the
        // project folder's path (the same namespace, until projects get their own id), and res:// resolves under
        // <project>/assets. Without --project neither is initialised, so assets and asset references are
        // unavailable.
        if (!projectDir.empty())
        {
            N2Engine::IO::ResourceUUID::Initialize(N2Engine::IO::ResourceUUID::NamespaceForProjectDir(projectDir));
            N2Engine::IO::ResourceLoader::Instance().Initialize(projectDir);
        }
        else
        {
            N2Engine::Logger::Info("No --project given: assets and asset references are unavailable");
        }

        // Start editor server
        N2Engine::Editor::EditorServer server;
        if (!projectDir.empty())
        {
            server.SetScenesDirectory(projectDir / "scenes");
        }
        if (options.bindAddress != N2Engine::Editor::EditorServer::DefaultBindAddress)
        {
            N2Engine::Logger::Warn("Editor server bound to " + options.bindAddress +
                                   ": it has no authentication, so anything that can reach it can control this host");
        }
        const bool serverStarted = server.Start(options.port, options.bindAddress);
        if (!serverStarted)
        {
            N2Engine::Logger::Error("Editor server failed to start");
        }
        else
        {
            // For launchers (format in HostOptions.hpp): the bound port, which is the OS's pick with --port 0.
            // Straight to C stdout, as the Logger's console lines are (std::cout is redirected into the Logger,
            // which would prefix a level), and flushed, since a piped stdout is fully buffered. The network
            // thread posts its log lines to this thread, so no log line is written in the middle of this one.
            std::println(stdout, "{}", N2Engine::Editor::FormatReadyLine(server.GetPort()));
            std::fflush(stdout);
        }

        // Keep serving even without a window, so the editor can still query state such as GetEngineHealth
        auto &window = N2Engine::Application::GetInstance().GetWindow();
        if (!window.IsValid())
        {
            N2Engine::Logger::Warn("No window or renderer; rendering commands will be unavailable");
        }

        // Main loop: handle OS events and run the editor's requests. Requests touch engine state, so they
        // run here on the main thread (see EditorServer); waiting for them doubles as the idle sleep.
        auto &app = N2Engine::Application::GetInstance();
        while (g_running && server.IsRunning() && !app.IsQuitRequested() &&
               (!window.IsValid() || !window.ShouldClose()))
        {
            // Poll window events to keep OS happy (even if window is hidden)
            window.PollEvents();

            server.ProcessCommands(std::chrono::milliseconds(16));
            // Mixes the headless engine's audio for the time that passed, client or not (see UpdateAudio)
            server.UpdateAudio();
        }

        if (!g_running)
        {
            N2Engine::Logger::Info("Shutdown signal received");
        }
        server.ProcessCommands(); // flushes log lines the network thread has posted
        server.Stop();
        N2Engine::Logger::Info("Editor server stopped");

        // As Application::Run does, however the loop ended
        if (N2Engine::SceneManager::GetCurScene() != nullptr)
        {
            N2Engine::SceneManager::GetCurSceneRef().OnApplicationQuit();
        }

        N2Engine::Application::GetInstance().Shutdown();
        N2Engine::Logger::Info("Engine shut down");

        if (!serverStarted)
        {
            return 1;
        }
    }
    catch (const std::exception &e)
    {
        N2Engine::Logger::Error(std::string("Fatal error: ") + e.what());
        return 1;
    }

    return 0;
}
