#include "engine/scripting/LuaRuntime.hpp"

#include <fstream>

#include "engine/io/ResourceLoader.hpp"
#include "engine/GameObject.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"

#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/input/InputTypes.hpp"
#include "engine/input/InputBindingFactory.hpp"
#include "engine/input/InputMapping.hpp"
#include "engine/input/InputValue.hpp"
#include "engine/input/Input.hpp"
#include "engine/scripting/bindings/LuaBindings.hpp"

namespace N2Engine::Scripting
{
    LuaRuntime *LuaRuntime::_instance = nullptr;

    LuaRuntime& LuaRuntime::Instance()
    {
        if (!_instance)
        {
            _instance = new LuaRuntime();
        }
        return *_instance;
    }

    void LuaRuntime::Destroy()
    {
        delete _instance;
        _instance = nullptr;
    }

    LuaRuntime::LuaRuntime()
    {
        _lua.open_libraries(
            sol::lib::base,
            sol::lib::package,
            sol::lib::math,
            sol::lib::string,
            sol::lib::table
        );
    }

    void LuaRuntime::Initialize()
    {
        Bindings::BindMath(*this);
        Bindings::BindCore(*this);
        Bindings::BindUtility(*this);

        // Gameplay systems
        Bindings::BindPhysics(*this);
        Bindings::BindAudio(*this);

        // Input & Events
        Bindings::BindInput(*this);
        Bindings::BindEvents(*this);

        // Engine services
        Bindings::BindTime(*this);
        Bindings::BindDebug(*this);
        Bindings::BindApplication(*this);
        Bindings::BindWindow(*this);
        Bindings::BindCamera(*this);

        SetupModuleSystem();

        Logger::Info("LuaRuntime initialized with all bindings");
    }

    void LuaRuntime::SetupModuleSystem()
    {
        _lua["require"] = [this](const std::string &moduleName) -> sol::object
        {
            if (auto it = _loadedModules.find(moduleName); it != _loadedModules.end())
            {
                return it->second;
            }

            std::string pathStr = moduleName;
            std::ranges::replace(pathStr, '.', '/');
            IO::ResourcePath path(IO::PathType::Resource, pathStr + ".lua");

            auto script = IO::ResourceLoader::Instance().Load<LuaScript>(path);
            if (!script)
            {
                Logger::Error(std::format("Failed to require module: {}", moduleName));
                return sol::nil;
            }

            return LoadScriptAsModule(path, script.get());
        };

        _lua["_loaded_modules"] = [this]() -> std::vector<std::string>
        {
            std::vector<std::string> names;
            for (const auto &name : _loadedModules | std::views::keys)
            {
                names.push_back(name);
            }
            return names;
        };
    }

    std::string LuaRuntime::PathToModuleName(const IO::ResourcePath &path)
    {
        std::string moduleName = path.GetPath();

        if (moduleName.ends_with(".lua"))
        {
            moduleName = moduleName.substr(0, moduleName.length() - 4);
        }

        std::ranges::replace(moduleName, '/', '.');

        return moduleName;
    }

    sol::table LuaRuntime::LoadScriptAsModule(const IO::ResourcePath &path, LuaScript *script)
    {
        std::string moduleName = PathToModuleName(path);

        auto result = _lua.safe_script(script->GetSourceCode(), sol::script_pass_on_error);

        if (!result.valid())
        {
            sol::error err = result;
            Logger::Error(std::format("Failed to load module {}: {}", moduleName, err.what()));
            return _lua.create_table();
        }

        sol::table moduleTable;

        if (result.return_count() > 0 && result[0].is<sol::table>())
        {
            moduleTable = result[0];
        }
        else
        {
            moduleTable = _lua.create_table();
        }

        _loadedModules[moduleName] = moduleTable;
        Logger::Info(std::format("Loaded Lua module: {}", moduleName));

        return moduleTable;
    }

    sol::optional<sol::table> LuaRuntime::GetModule(const std::string &moduleName)
    {
        auto it = _loadedModules.find(moduleName);
        if (it != _loadedModules.end())
        {
            return it->second;
        }
        return sol::nullopt;
    }

    void LuaRuntime::ReloadModule(const IO::ResourcePath &path, LuaScript *script)
    {
        std::string moduleName = PathToModuleName(path);

        _loadedModules.erase(moduleName);
        LoadScriptAsModule(path, script);

        // Notify callbacks
        if (auto it = _reloadCallbacks.find(moduleName); it != _reloadCallbacks.end())
        {
            for (auto &callback : it->second)
            {
                callback();
            }
        }

        Logger::Info(std::format("Reloaded module: {}", moduleName));
    }

    void LuaRuntime::RegisterReloadCallback(const std::string &moduleName, std::function<void()> callback)
    {
        _reloadCallbacks[moduleName].push_back(std::move(callback));
    }

    void LuaRuntime::ClearReloadCallbacks(const std::string &moduleName)
    {
        _reloadCallbacks.erase(moduleName);
    }
}
