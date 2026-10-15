#pragma once

#include <sol/sol.hpp>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include "engine/io/ResourcePath.hpp"
#include "engine/scripting/LuaScript.hpp"

namespace N2Engine::Scripting
{
    /// Resource limits of the Lua state. A value of 0 means unlimited (tools only; every engine build applies the defaults).
    struct LuaLimits
    {
        /// Lua VM instructions one outermost C++ -> Lua entry may run before it is aborted with a Lua error
        std::uint64_t instructionsPerEntry = 50'000'000;
        /// Bytes the whole state (every script shares it) may hold; an allocation past it fails with a memory error
        std::size_t maxBytes = std::size_t{256} * 1024 * 1024;
    };

    namespace Detail
    {
        /// What the state's allocator and count hook share (reached through lua_getallocf). Defined in LuaRuntime.cpp.
        struct LuaGuardState;
    }

    class LuaRuntime
    {
    private:
        static LuaRuntime* _instance;

        // Declared before _lua: the allocator and the hook use it until the state is closed
        std::unique_ptr<Detail::LuaGuardState> _guard;
        sol::state _lua;
        std::unordered_set<std::string> _limitLogged; // chunk names whose limit error was already logged
        std::unordered_map<std::string, sol::table> _loadedModules;
        struct ReloadCallback
        {
            const void *owner; // who registered it, so it can be removed (e.g. a destroyed LuaComponent)
            std::function<void()> callback;
        };
        std::unordered_map<std::string, std::vector<ReloadCallback>> _reloadCallbacks;
        std::optional<bool> _initializeResult;

        LuaRuntime();
        ~LuaRuntime(); // out of line: the guard state is defined in LuaRuntime.cpp

    public:
        static LuaRuntime& Instance();

        /// Registers all engine bindings. Safe to call more than once: later calls return the first result,
        /// because re-registering a usertype in the same state breaks the metatables of existing values.
        /// @returns false if a binding failed to register; the error is logged
        bool Initialize();
        sol::state& GetState() { return _lua; }

        /// Sets the instruction budget (per outermost entry from C++ into Lua) and the memory cap of the state; takes
        /// effect at the next entry (the cap at once). The defaults (LuaLimits) apply in every build.
        void SetLimits(const LuaLimits& limits);
        [[nodiscard]] LuaLimits GetLimits() const;
        /// Bytes the Lua state holds now (what the cap counts)
        [[nodiscard]] std::size_t GetMemoryUsed() const;
        /// True if the error text is one the budget or the cap raises ("script exceeded its instruction budget",
        /// "not enough memory"): what a caller of a protected call checks to fault a component instead of retrying it
        [[nodiscard]] static bool IsLimitError(std::string_view message);
        /// Runs C++ code that touches script tables (a table access can reach a script's metamethods) inside a Lua
        /// protected call (a budget scope too), so a Lua error, the budget or a failed allocation never goes down
        /// the panic path (which leaves the hook disabled) or jumps over the caller's C++ frames.
        /// Keep the body to the access itself: a Lua error abandons it without running its destructors.
        /// @returns false with the error text in `error` if the body raised a Lua error or threw
        bool ProtectedCall(const std::function<void()>& body, std::string& error);
        /// True the first time it is asked about a key: limit errors of chunks that run outside a component are
        /// logged once per chunk name
        bool ShouldLogLimitError(const std::string& key);

        /// Loads a script through ResourceLoader and runs it once (e.g. a scene setup script).
        /// @returns false if the file couldn't be loaded or the script raised an error; the error is logged
        bool RunFile(const IO::ResourcePath& path);
        /// Runs Lua source text in the global environment. The one way the engine runs script assets
        /// (RunFile, require, LuaComponent): binary (precompiled) chunks are rejected.
        sol::protected_function_result RunSource(std::string_view source, const std::string& chunkName);

        // Module management
        sol::table LoadScriptAsModule(const IO::ResourcePath& path, LuaScript* script);
        sol::optional<sol::table> GetModule(const std::string& moduleName);
        void ReloadModule(const IO::ResourcePath& path, LuaScript* script);

        // Reload callbacks
        /// @param owner identifies the registrant for UnregisterReloadCallbacks; may be null
        void RegisterReloadCallback(const std::string& moduleName, std::function<void()> callback,
                                    const void* owner = nullptr);
        /// Removes every reload callback the owner registered, for any module
        void UnregisterReloadCallbacks(const void* owner);
        void ClearReloadCallbacks(const std::string& moduleName);

        // Path conversion (public for LuaComponent)
        std::string PathToModuleName(const IO::ResourcePath& path);

    private:
        friend class LuaBudgetScope;
        void BeginEntry();
        void EndEntry(int outerDepth);
        // Not public: destroying the state while components still hold sol::table/sol::function
        // references into it left them dangling. Nothing calls it; the runtime lives for the process.
        static void Destroy();
        void RegisterBindings();
        void SetupModuleSystem();
    };

    /// Marks one entry from C++ into Lua (a protected call, a chunk run, a table access that can reach a metamethod).
    /// The OUTERMOST scope refills the instruction budget and ends by refilling it again; a scope inside one (a
    /// binding that calls back into Lua, require inside a chunk) shares the running budget and never refills it.
    /// Every C++ -> Lua entry of the engine opens one.
    class LuaBudgetScope
    {
        int _outerDepth;

    public:
        LuaBudgetScope();
        ~LuaBudgetScope();
        LuaBudgetScope(const LuaBudgetScope&) = delete;
        LuaBudgetScope& operator=(const LuaBudgetScope&) = delete;
    };
}