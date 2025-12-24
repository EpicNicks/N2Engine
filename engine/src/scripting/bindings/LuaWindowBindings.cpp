#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/Window.hpp"
#include "engine/Application.hpp"

namespace N2Engine::Scripting::Bindings
{
    void BindWindow(LuaRuntime& runtime)
    {
        auto& lua = runtime.GetState();
        
        // ===== WindowMode Enum =====
        lua.new_enum("WindowMode",
            "Windowed", WindowMode::Windowed,
            "Fullscreen", WindowMode::Fullscreen,
            "BorderlessWindowed", WindowMode::BorderlessWindowed
        );
        
        // ===== Window =====
        lua.new_usertype<Window>("Window",
            sol::no_constructor,
            
            "GetTitle", &Window::GetTitle,
            "SetTitle", &Window::SetTitle,
            
            "GetDimensions", &Window::GetWindowDimensions,
            "SetWindowMode", &Window::SetWindowMode,
            
            "clearColor", &Window::clearColor
        );
        
        // ===== Window Global Access =====
        lua["Window"] = lua.create_table_with(
            "Get", []() -> Window& {
                return Application::GetInstance().GetWindow();
            },
            
            "GetWidth", []() -> int {
                return Application::GetInstance().GetWindow().GetWindowDimensions()[0];
            },
            
            "GetHeight", []() -> int {
                return Application::GetInstance().GetWindow().GetWindowDimensions()[1];
            },
            
            "SetTitle", [](const std::string& title) {
                Application::GetInstance().GetWindow().SetTitle(title);
            },
            
            "SetMode", [](WindowMode mode) {
                Application::GetInstance().GetWindow().SetWindowMode(mode);
            },
            
            "SetClearColor", [](const Common::Color& color) {
                Application::GetInstance().GetWindow().clearColor = color;
            }
        );
    }
}