#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"
#include "engine/io/LatePacks.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/Vfs.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindApplication(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
        // ===== Assets: what a game asks before it loads a file that may come later (#425, LatePacks) =====
        lua["Assets"] = lua.create_table_with(
            // Whether the res:// file is there to load now: false while it is in a late pack that has not arrived (a web
            // package's build.web.lateAssets) or when it does not exist at all
            "IsAvailable", [](const std::string &path) -> bool
            {
                const std::filesystem::path resolved = IO::ResourceLoader::Instance().Resolve(IO::ResourcePath(path));
                return !IO::LatePacks::IsPending(resolved) && IO::Vfs::Instance().Exists(resolved);
            },
            // How many late packs have not arrived yet (0 outside the web player, and once all have)
            "PendingCount", []() -> int { return static_cast<int>(IO::LatePacks::Pending().size()); },
            // How many of them could not be fetched (the loader tried three times): their files will not come in this session
            "FailedCount", []() -> int { return static_cast<int>(IO::LatePacks::Failed().size()); });

        // ===== Application Global =====
        lua["Application"] = lua.create_table_with(
            // Refused on the editor host that edits (Application::SetScriptQuitAllowed), and whenever the process does not
            // run scripts (LuaExecutionPolicy::DeclarationsOnly). No script can reach this there, since nothing runs: it is
            // the guard for a path that is added later.
            "Quit", []()
            {
                if (!LuaRuntime::RunsScripts())
                {
                    Logger::Warn("Application.Quit is refused: this host does not run scripts");
                    return;
                }
                Application::QuitFromScript();
            },
            
            "GetPlatform", []() -> std::string {
#ifdef _WIN32
                return "Windows";
#elif __APPLE__
                return "macOS";
#elif __linux__
                return "Linux";
#else
                return "Unknown";
#endif
            },
            
            "GetCamera", []() -> Camera* {
                return Application::GetInstance().GetMainCamera();
            }
        );
    }
}