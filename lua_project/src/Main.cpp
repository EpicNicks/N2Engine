#include <filesystem>

#include <math/UUID.hpp>

#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// Usage: lua_project [project folder]
// The project folder holds assets/scene.lua; it defaults to the lua_project source folder.
int main(int argc, char *argv[])
{
    using namespace N2Engine;

    const std::filesystem::path projectDir = argc > 1 ? argv[1] : N2_LUA_PROJECT_DIR;

    Application &application = Application::GetInstance();
    application.Init({
        .physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX,
        .renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE,
        .isHeadless = false,
    });
    if (!application.GetWindow().IsValid())
    {
        Logger::Error("No window or renderer; see the subsystem warnings above");
        return 1;
    }

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
