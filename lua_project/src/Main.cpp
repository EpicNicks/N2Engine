#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <math/UUID.hpp>

#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

namespace
{
    using RenderBackend = N2Engine::Config::ApplicationOptions::RenderBackend;

    std::optional<RenderBackend> ParseRenderBackend(const std::string_view name)
    {
        if (name == "opengl")
        {
            return RenderBackend::OPENGL;
        }
        if (name == "software")
        {
            return RenderBackend::SOFTWARE;
        }
        return std::nullopt;
    }

    std::string_view RenderBackendTitle(const RenderBackend backend)
    {
        return backend == RenderBackend::OPENGL ? "OpenGL" : "software";
    }
}

// Usage: lua_project [project folder] [--renderer opengl|software]
// The project folder holds assets/scene.lua; it defaults to the lua_project source folder. The renderer defaults to
// software; the GPU smoke test (docs/testing.html) runs it with --renderer opengl.
int main(int argc, char *argv[])
{
    using namespace N2Engine;

    std::filesystem::path projectDir = N2_LUA_PROJECT_DIR;
    RenderBackend renderBackend = RenderBackend::SOFTWARE;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--renderer" || arg.starts_with("--renderer="))
        {
            std::string_view value;
            if (arg == "--renderer")
            {
                if (i + 1 >= argc)
                {
                    Logger::Error("--renderer needs a value: opengl or software");
                    return 1;
                }
                value = argv[++i];
            }
            else
            {
                value = arg.substr(std::string_view("--renderer=").size());
            }
            const auto parsed = ParseRenderBackend(value);
            if (!parsed)
            {
                Logger::Error(std::format("Unknown renderer '{}': expected opengl or software", value));
                return 1;
            }
            renderBackend = *parsed;
        }
        else
        {
            projectDir = arg;
        }
    }

    Application &application = Application::GetInstance();
    application.Init({
        .physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX,
        .renderBackend = renderBackend,
        .isHeadless = false,
    });
    if (!application.GetWindow().IsValid())
    {
        Logger::Error("No window or renderer; see the subsystem warnings above");
        return 1;
    }
    application.GetWindow().SetTitle(std::format("N2Engine lua_project ({})", RenderBackendTitle(renderBackend)));

    SceneManager::AddScene(Scene::Create("Lua Scene"), true);
    SceneManager::ProcessAnyPendingSceneChange();

    // Scripts are loaded through ResourceLoader, which resolves res:// paths under <project>/assets
    // The namespace the editor host uses for the same folder, so both give its assets the same UUIDs
    IO::ResourceUUID::Initialize(IO::ResourceUUID::NamespaceForProjectDir(projectDir));
    IO::ResourceLoader::Instance().Initialize(projectDir);

    if (!Scripting::LuaRuntime::Instance().RunFile(IO::ResourcePath("res://scene.lua")))
    {
        return 1;
    }

    application.Run();
    return 0;
}
