#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Application.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindApplication(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
        // ===== Application Global =====
        lua["Application"] = lua.create_table_with(
            "Quit", &Application::Quit,
            
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