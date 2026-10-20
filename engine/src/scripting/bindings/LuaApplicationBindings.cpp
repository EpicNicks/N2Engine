#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Application.hpp"
#include "engine/Logger.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindApplication(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
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