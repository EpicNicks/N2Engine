#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>
#include <charconv>
#include <filesystem>
#include <print>

#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

#include "editor-server/EditorServer.hpp"

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

    bool ParsePort(const std::string &text, int &port)
    {
        int value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value < 1 || value > 65535)
        {
            return false;
        }
        port = value;
        return true;
    }
}

int main(int argc, char *argv[])
{
    int port = 9999;
    std::string bindAddress = N2Engine::Editor::EditorServer::DefaultBindAddress;
    std::string projectPath;

    // Basic arg parsing
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-p" || arg == "--port") && i + 1 < argc)
        {
            if (!ParsePort(argv[++i], port))
            {
                std::println(stderr, "Invalid port: {}", argv[i]);
                return 1;
            }
        }
        else if (arg == "--bind" && i + 1 < argc)
        {
            bindAddress = argv[++i];
        }
        else if ((arg == "--project") && i + 1 < argc)
        {
            projectPath = argv[++i];
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::println(R"(
                        N2Engine Editor Host
                        Usage: N2EditorHost [options]
                            Options:
                            -p, --port <port>      Server port (default: 9999)
                            --bind <ipv4 address>  Address to listen on (default: 127.0.0.1)
                                                   WARNING: the protocol has no authentication; any other
                                                   address lets whoever can reach it control this host,
                                                   including deleting scene files
                            --project <path>       Project path (DeleteScene works in <path>/scenes)
                            -h, --help             Show this help
                        )");
            return 0;
        }
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    try
    {
        // Initialize engine in editor mode (no window shown initially, or hidden)
        N2Engine::Application::GetInstance().Init({
            .projectPath = projectPath,
            .physicsBackend = N2Engine::Config::ApplicationOptions::PhysicsBackend::PHYSX,
            .renderBackend = N2Engine::Config::ApplicationOptions::RenderBackend::OPENGL,
            .isHeadless = true,
        });

        N2Engine::Logger::Info("Engine initialized");

        // Start editor server
        N2Engine::Editor::EditorServer server;
        if (!projectPath.empty())
        {
            server.SetScenesDirectory(std::filesystem::path(projectPath) / "scenes");
        }
        if (bindAddress != N2Engine::Editor::EditorServer::DefaultBindAddress)
        {
            N2Engine::Logger::Warn("Editor server bound to " + bindAddress +
                                   ": it has no authentication, so anything that can reach it can control this host");
        }
        const bool serverStarted = server.Start(port, bindAddress);
        if (!serverStarted)
        {
            N2Engine::Logger::Error("Editor server failed to start");
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
