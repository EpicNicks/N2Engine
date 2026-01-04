#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <print>

#include "engine/Application.hpp"
#include "engine/Logger.hpp"

#include "editor-server/EditorServer.hpp"

namespace
{
    std::atomic<bool> g_running{true};

    void SignalHandler(int signal)
    {
        if (signal == SIGINT || signal == SIGTERM)
        {
            N2Engine::Logger::Info("Shutdown signal received");
            g_running = false;
        }
    }
}

int main(int argc, char *argv[])
{
    int port = 9999;
    std::string projectPath;

    // Basic arg parsing
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "-p" || arg == "--port") && i + 1 < argc)
        {
            port = std::stoi(argv[++i]);
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
                            --project <path>       Project path
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
        server.Start(port);

        N2Engine::Logger::Info("Editor server listening on port " + std::to_string(port));

        // Main loop - just keep alive and handle OS events
        while (g_running && server.IsRunning() && !N2Engine::Application::GetInstance().GetWindow().ShouldClose())
        {
            // Poll window events to keep OS happy (even if window is hidden)
            N2Engine::Application::GetInstance().GetWindow().PollEvents();

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        server.Stop();
        N2Engine::Logger::Info("Editor server stopped");
    }
    catch (const std::exception &e)
    {
        N2Engine::Logger::Error(std::string("Fatal error: ") + e.what());
        return 1;
    }

    return 0;
}
