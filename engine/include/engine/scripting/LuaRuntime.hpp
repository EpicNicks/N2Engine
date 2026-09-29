#pragma once

#include <sol/sol.hpp>
#include <unordered_map>
#include <functional>
#include <optional>
#include "engine/io/ResourcePath.hpp"
#include "engine/scripting/LuaScript.hpp"

namespace N2Engine::Scripting
{
    class LuaRuntime
    {
    private:
        static LuaRuntime* _instance;

        sol::state _lua;
        std::unordered_map<std::string, sol::table> _loadedModules;
        std::unordered_map<std::string, std::vector<std::function<void()>>> _reloadCallbacks;
        std::optional<bool> _initializeResult;

        LuaRuntime();

    public:
        static LuaRuntime& Instance();
        static void Destroy();

        /// Registers all engine bindings. Safe to call more than once: later calls return the first result,
        /// because re-registering a usertype in the same state breaks the metatables of existing values.
        /// @returns false if a binding failed to register; the error is logged
        bool Initialize();
        sol::state& GetState() { return _lua; }

        // Module management
        sol::table LoadScriptAsModule(const IO::ResourcePath& path, LuaScript* script);
        sol::optional<sol::table> GetModule(const std::string& moduleName);
        void ReloadModule(const IO::ResourcePath& path, LuaScript* script);

        // Reload callbacks
        void RegisterReloadCallback(const std::string& moduleName, std::function<void()> callback);
        void ClearReloadCallbacks(const std::string& moduleName);

        // Path conversion (public for LuaComponent)
        std::string PathToModuleName(const IO::ResourcePath& path);

    private:
        void RegisterBindings();
        void SetupModuleSystem();
    };
}