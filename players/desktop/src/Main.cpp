// The N2Engine desktop player (build targets stage 1; docs/build-targets.html#player): runs one game, the folder that
// holds its project.n2proj and assets/. A package made by tools/n2build puts the game in a "game" folder beside this
// executable, which is where the player looks when no folder is given. It takes what lua_project (a demo project, which
// keeps its own executable) solved: the folder argument, --renderer and its usage errors, the window title, --console-log.
//
// A game starts from the project's startupScene (a .scene file) when it names one, else from assets/scene.lua, the
// script that builds the scene (lua_project, the web player and the Triangle-Like port start that way).
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "PlayerOptions.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/ProjectSettings.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/io/Vfs.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneFile.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

namespace
{
    using namespace N2Engine;

    /// On stderr: the Logger has no console output until Application::Init (and none in Release)
    void Report(const std::string_view message)
    {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
    }

    /// The folder holding this executable (not the working directory: a package is started from anywhere)
    std::filesystem::path ExecutableDir(const char *argv0)
    {
#if defined(_WIN32)
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
            {
                break;
            }
            if (length < buffer.size())
            {
                buffer.resize(length);
                return std::filesystem::path(buffer).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
#else
        std::error_code error;
        if (const auto self = std::filesystem::read_symlink("/proc/self/exe", error); !error)
        {
            return self.parent_path();
        }
#endif
        std::error_code error2;
        const auto absolute = std::filesystem::absolute(argv0 != nullptr ? argv0 : "", error2);
        return error2 ? std::filesystem::current_path() : absolute.parent_path();
    }

    /// The project's first scene: its startupScene when it names one, else what assets/scene.lua builds
    bool StartGame(const IO::ProjectFile &project)
    {
        if (!project.startupScene.empty())
        {
            const auto file = IO::ResourceLoader::Instance().Load<SceneFile>(IO::ResourcePath(project.startupScene));
            if (file == nullptr)
            {
                Logger::Error(std::format("The startup scene {} could not be read (see the log)", project.startupScene));
                return false;
            }
            std::unique_ptr<Scene> scene = Scene::FromJSON(file->GetData(), true);
            if (scene == nullptr)
            {
                Logger::Error(std::format("The startup scene {} is not a valid scene (see the log)", project.startupScene));
                return false;
            }
            scene->SetUUID(file->GetUUID());
            SceneManager::AddScene(std::move(scene), true);
            SceneManager::ProcessAnyPendingSceneChange();
            return true;
        }

        SceneManager::AddScene(Scene::Create(project.name.empty() ? std::string("Game") : project.name), true);
        SceneManager::ProcessAnyPendingSceneChange();
        return Scripting::LuaRuntime::Instance().RunFile(IO::ResourcePath("res://scene.lua"));
    }
}

int main(int argc, char *argv[])
{
    using namespace N2Engine;

    const std::string programName = argc > 0 ? std::filesystem::path(argv[0]).filename().string() : "n2player";
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }
    const auto parsed = Player::ParsePlayerArguments(args);
    if (!parsed)
    {
        Report(parsed.error());
        Report(Player::PlayerUsage(programName));
        return 1;
    }
    Player::PlayerOptions options = *parsed;
    if (options.help)
    {
        std::printf("%s\n", Player::PlayerUsage(programName).c_str());
        return 0;
    }
    if (options.projectDir.empty())
    {
        options.projectDir = ExecutableDir(argc > 0 ? argv[0] : nullptr) / "game";
    }

    // A packaged game is one pack (game.n2pack, zip or 7z) mounted where its folder would be (docs/packs.html); the engine
    // then opens that folder as it opens any project, reading every file through the mount
    const auto isDirectory = [](const std::filesystem::path &p) {
        std::error_code error;
        return std::filesystem::is_directory(p, error);
    };
    const auto isFile = [](const std::filesystem::path &p) {
        std::error_code error;
        return std::filesystem::is_regular_file(p, error);
    };
    const Player::GameLocation game = Player::ResolveGame(options.projectDir, isDirectory, isFile);
    options.projectDir = game.projectDir;
    if (!game.pack.empty())
    {
        auto mount = IO::Pack::Open(game.pack);
        if (!mount)
        {
            Report(std::format("The game's pack can't be read: {}", mount.error()));
            return 1;
        }
        IO::Vfs::Instance().Mount(std::filesystem::absolute(game.projectDir), std::move(*mount));
    }

    // The project file first: the window's title is the game's name, and a folder without one is not a game
    const auto project = IO::ProjectFile::Load(options.projectDir);
    if (!project)
    {
        Report(std::format("{} is not a game folder: {}", options.projectDir.string(), project.error()));
        Report(Player::PlayerUsage(programName));
        return 1;
    }

    if (options.consoleLog)
    {
        Logger::InitializeDebugConsoleHelper();
    }

    Application &application = Application::GetInstance();
    application.Init({
        // Init reads the project's own settings (physics.backend among them); AUTO: no preference of the player's
        .projectPath = options.projectDir.string(),
        .physicsBackend = Config::ApplicationOptions::PhysicsBackend::AUTO,
        .renderBackend = options.renderBackend,
        .isHeadless = false,
    });
    if (!application.GetWindow().IsValid())
    {
        Logger::Error("No window or renderer; see the subsystem warnings above");
        return 1;
    }
    const std::string title = project->name.empty() ? std::string("N2Engine") : project->name;
#if defined(N2ENGINE_DEBUG)
    application.GetWindow().SetTitle(std::format("{} ({})", title, Player::RenderBackendTitle(options.renderBackend)));
#else
    application.GetWindow().SetTitle(title);
#endif

    // As the editor host opens a project: its projectId is the asset UUID namespace, user:// is the project's own
    // folder, and its settings apply
    IO::ResourceUUID::Initialize(project->projectId);
    IO::ResourceLoader::Instance().Initialize(options.projectDir, project->UserDataPath());
    for (const std::string &problem : ApplyProjectSettings(project->settings))
    {
        Logger::Warn("Project settings: " + problem);
    }

    if (!StartGame(*project))
    {
        return 1;
    }

    if (!options.frames)
    {
        application.Run();
        return 0;
    }
    if (!application.BeginRun())
    {
        return 1;
    }
    for (int frame = 0; frame < *options.frames && application.RunFrame(); ++frame)
    {
    }
    application.EndRun();
    return 0;
}
