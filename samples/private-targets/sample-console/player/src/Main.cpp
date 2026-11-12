// The sample private target's player (build targets stage 11; docs/private-targets.html): the desktop player's start-up
// (the same arguments and the game.n2pack beside the executable, through n2_player_options), with the target's own platform
// backend handed to the window before the engine starts. Everything target-specific is in SampleConsoleHost.
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "PlayerOptions.hpp"
#include "SampleConsoleHost.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/io/Vfs.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;

int main(int argc, char *argv[])
{
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }
    const auto parsed = Player::ParsePlayerArguments(args);
    if (!parsed)
    {
        std::fprintf(stderr, "%s\n%s\n", parsed.error().c_str(), Player::PlayerUsage("sample_console_player").c_str());
        return 1;
    }
    Player::PlayerOptions options = *parsed;
    if (options.projectDir.empty())
    {
        const std::filesystem::path self = std::filesystem::absolute(argc > 0 ? argv[0] : "sample_console_player");
        options.projectDir = self.parent_path() / "game";
    }
    const auto isDirectory = [](const std::filesystem::path &p) { std::error_code e; return std::filesystem::is_directory(p, e); };
    const auto isFile = [](const std::filesystem::path &p) { std::error_code e; return std::filesystem::is_regular_file(p, e); };
    const Player::GameLocation game = Player::ResolveGame(options.projectDir, isDirectory, isFile);
    if (!game.pack.empty())
    {
        auto mount = IO::Pack::Open(game.pack);
        if (!mount)
        {
            std::fprintf(stderr, "The game's pack can't be read: %s\n", mount.error().c_str());
            return 1;
        }
        IO::Vfs::Instance().Mount(std::filesystem::absolute(game.projectDir), std::move(*mount));
    }
    const auto project = IO::ProjectFile::Load(game.projectDir);
    if (!project)
    {
        std::fprintf(stderr, "%s\n", project.error().c_str());
        return 1;
    }
    if (options.consoleLog)
    {
        Logger::InitializeDebugConsoleHelper();
    }

    Application &application = Application::GetInstance();
    // The target's backend, before Init: the window takes it instead of the desktop one, and its renderer with it
    application.GetWindow().SetPlatformHost(std::make_unique<SampleConsole::SampleConsoleHost>(
        Platform::HostPaths{game.projectDir, project->UserDataPath(), std::filesystem::temp_directory_path()}));
    application.Init({
        .projectPath = game.projectDir.string(),
        .physicsBackend = Config::ApplicationOptions::PhysicsBackend::AUTO,
        .renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE,
        .isHeadless = false,
    });
    if (!application.GetWindow().IsValid())
    {
        std::fprintf(stderr, "No renderer: %s\n", application.GetWindow().GetInitError().c_str());
        return 1;
    }
    IO::ResourceUUID::Initialize(project->projectId);
    IO::ResourceLoader::Instance().Initialize(game.projectDir, project->UserDataPath());
    for (const std::string &problem : ApplyProjectSettings(project->settings))
    {
        Logger::Warn("Project settings: " + problem);
    }
    SceneManager::AddScene(Scene::Create(project->name.empty() ? std::string("Game") : project->name), true);
    SceneManager::ProcessAnyPendingSceneChange();
    if (!Scripting::LuaRuntime::Instance().RunFile(IO::ResourcePath("res://scene.lua")))
    {
        return 1;
    }
    if (!application.BeginRun())
    {
        return 1;
    }
    const int frames = options.frames.value_or(300);
    for (int frame = 0; frame < frames && application.RunFrame(); ++frame)
    {
    }
    application.EndRun();
    std::printf("sample-console: ran %d frames\n", frames);
    return 0;
}
