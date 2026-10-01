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
        // No package library: the engine's require (SetupModuleSystem) loads modules through ResourceLoader,
        // and package only added ways around it (loadlib and the C searchers load native code; searchpath
        // and the file searcher open arbitrary paths)
        _lua.open_libraries(
            sol::lib::base,
            sol::lib::math,
            sol::lib::string,
            sol::lib::table
        );

        // Scripts are project content, so they get no way past the asset pipeline: no reading files
        // (dofile/loadfile) and no binary chunks, whose crafted bytecode can corrupt the VM. load keeps
        // working for source text. (RunSource applies the same rule to the engine's own loading.)
        _lua.script(R"(
            dofile = nil
            loadfile = nil

            local rawLoad = load
            local find = string.find
            load = function(chunk, chunkname, mode, ...)
                -- Never "b". A mode without "t" (e.g. "b") allows nothing, as natively it rejects text.
                local textMode = (mode == nil or find(mode, "t", 1, true)) and "t" or ""
                -- Pass env on only if given: load treats an explicit nil env as "no globals"
                if select('#', ...) > 0 then
                    return rawLoad(chunk, chunkname, textMode, ...)
                end
                return rawLoad(chunk, chunkname, textMode)
            end
        )");
    }

    sol::protected_function_result LuaRuntime::RunSource(const std::string_view source, const std::string &chunkName)
    {
        // Text only: a project .lua asset holding precompiled bytecode would otherwise run as-is
        return _lua.safe_script(source, sol::script_pass_on_error, chunkName, sol::load_mode::text);
    }

    bool LuaRuntime::Initialize()
    {
        if (_initializeResult.has_value())
        {
            return *_initializeResult;
        }

        try
        {
            RegisterBindings();
        }
        catch (const std::exception &e)
        {
            Logger::Error(std::format("LuaRuntime failed to initialize: {}", e.what()));
            _initializeResult = false;
            return false;
        }

        Logger::Info("LuaRuntime initialized with all bindings");
        _initializeResult = true;
        return true;
    }

    void LuaRuntime::RegisterBindings()
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
        Bindings::BindComponents(*this);

        SetupModuleSystem();

        // Types bound with sol::call_constructor are constructed as T(...); engine-api.lua also documents
        // T.new(...), which sol doesn't add on its own, so alias it to the call constructor
        _lua.script(R"(
            for _, name in ipairs({ "Vector2", "Vector3", "Vector4", "Quaternion", "Color", "PhysicsMaterial", "BoundingBox" }) do
                local T = _G[name]
                if T ~= nil and T.new == nil then
                    T.new = function(...) return T(...) end
                end
            end
        )");
    }

    bool LuaRuntime::RunFile(const IO::ResourcePath &path)
    {
        const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(path);
        if (!script)
        {
            Logger::Error(std::format("Lua script not found: {}", path.ToString()));
            return false;
        }

        const auto result = RunSource(script->GetSourceCode(), path.ToString());
        if (!result.valid())
        {
            const sol::error err = result;
            Logger::Error(std::format("Error running {}: {}", path.ToString(), err.what()));
            return false;
        }
        return true;
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

        auto result = RunSource(script->GetSourceCode(), path.ToString());

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

        // Notify callbacks. Iterate a copy: a callback can register or unregister callbacks
        // (LuaComponent::ReloadScript may call SetScript), which would invalidate the live vector
        if (auto it = _reloadCallbacks.find(moduleName); it != _reloadCallbacks.end())
        {
            const auto callbacks = it->second;
            for (const auto &entry : callbacks)
            {
                entry.callback();
            }
        }

        Logger::Info(std::format("Reloaded module: {}", moduleName));
    }

    void LuaRuntime::RegisterReloadCallback(const std::string &moduleName, std::function<void()> callback,
                                            const void *owner)
    {
        _reloadCallbacks[moduleName].push_back({owner, std::move(callback)});
    }

    void LuaRuntime::UnregisterReloadCallbacks(const void *owner)
    {
        if (!owner)
        {
            return;
        }
        for (auto &callbacks : _reloadCallbacks | std::views::values)
        {
            std::erase_if(callbacks, [owner](const ReloadCallback &entry) { return entry.owner == owner; });
        }
    }

    void LuaRuntime::ClearReloadCallbacks(const std::string &moduleName)
    {
        _reloadCallbacks.erase(moduleName);
    }
}
