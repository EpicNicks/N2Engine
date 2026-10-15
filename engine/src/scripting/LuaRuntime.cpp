#include "engine/scripting/LuaRuntime.hpp"

#include <algorithm>
#include <cstdlib>
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
    namespace
    {
        constexpr std::string_view BudgetErrorText = "script exceeded its instruction budget";
        constexpr std::string_view MemoryErrorText = "not enough memory";
        // Instructions between two calls of the hook: small enough to stop a loop promptly, large enough that the
        // hook's cost is negligible
        constexpr std::uint64_t HookGranularity = 1000;
    }

    namespace Detail
    {
        // LuaPanicRepair.cpp (it reads Lua's internal thread state, so it is kept to a file of its own)
        void RepairThreadAfterPanic(lua_State *L);

        struct LuaGuardState
        {
            // Memory: what the allocator counts and refuses
            std::size_t used = 0;
            std::size_t cap = LuaLimits{}.maxBytes;

            // Instructions
            std::uint64_t limit = LuaLimits{}.instructionsPerEntry;
            std::int64_t remaining = 0;
            std::uint64_t step = HookGranularity; // instructions the installed hook counts per call
            bool exhausted = false;               // the budget ran out in this entry: the hook raises on every instruction
            int depth = 0;                        // LuaBudgetScopes open
            lua_State *state = nullptr;

            void Arm()
            {
                exhausted = false;
                if (limit == 0)
                {
                    remaining = 0;
                    lua_sethook(state, nullptr, 0, 0);
                    return;
                }
                step = std::min(limit, HookGranularity);
                remaining = static_cast<std::int64_t>(std::min<std::uint64_t>(limit, INT64_MAX));
                lua_sethook(state, &LuaGuardState::CountHook, LUA_MASKCOUNT, static_cast<int>(step));
            }

            static LuaGuardState *Of(lua_State *L)
            {
                void *userData = nullptr;
                lua_getallocf(L, &userData);
                return static_cast<LuaGuardState *>(userData);
            }

            // Lua calls this for an error raised outside every protected call (an unprotected table access that hit
            // a script's metamethod, or a failed allocation): sol turns it into a C++ exception. Lua has already
            // reset the thread (no call is running any more, the stack is cut down to the error object, which
            // default_at_panic then removes too), but not three of its fields; they are repaired here, before the
            // exception leaves (LuaPanicRepair.cpp says which, and what the emptied stack means for a caller)
            static int Panic(lua_State *L)
            {
                RepairThreadAfterPanic(L);
                return sol::default_at_panic(L);
            }

            static void CountHook(lua_State *L, lua_Debug *)
            {
                LuaGuardState *guard = Of(L);
                if (!guard->exhausted)
                {
                    guard->remaining -= static_cast<std::int64_t>(guard->step);
                    if (guard->remaining > 0)
                    {
                        return;
                    }
                    // Out of budget. A script can catch the error (pcall), so from here on the hook fires on every
                    // instruction and raises again, until the outermost entry ends and Arm refills the budget
                    guard->exhausted = true;
                    guard->step = 1;
                    lua_sethook(L, &LuaGuardState::CountHook, LUA_MASKCOUNT, 1);
                }
                luaL_error(L, "%s of %I", BudgetErrorText.data(), static_cast<lua_Integer>(guard->limit));
            }
        };

        // lua_Alloc with a cap. When ptr is NULL, osize is a type tag (LUA_TSTRING...), not a size. A free or a shrink
        // always succeeds; a growth past the cap returns NULL, which Lua turns into a memory error (after one
        // emergency garbage collection and a retry).
        void *LuaCappedAlloc(void *userData, void *ptr, const std::size_t osize, const std::size_t nsize)
        {
            auto *guard = static_cast<LuaGuardState *>(userData);
            const std::size_t oldSize = ptr != nullptr ? osize : 0;
            // What the others hold. Never underflows: used includes oldSize of every live block
            const std::size_t others = guard->used >= oldSize ? guard->used - oldSize : 0;

            if (nsize == 0)
            {
                std::free(ptr);
                guard->used = others;
                return nullptr;
            }
            if (nsize > oldSize && guard->cap != 0 && (others >= guard->cap || nsize > guard->cap - others))
            {
                return nullptr;
            }
            void *block = std::realloc(ptr, nsize);
            if (block == nullptr)
            {
                return nullptr; // the process is out of memory (the old block stays valid)
            }
            guard->used = others + nsize;
            return block;
        }
    }

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
        : _guard(std::make_unique<Detail::LuaGuardState>()),
          _lua(&Detail::LuaGuardState::Panic, &Detail::LuaCappedAlloc, _guard.get())
    {
        _guard->state = _lua.lua_state();
        _guard->Arm();

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

        // A script must not catch the budget error: pcall and xpcall hand it on. (After the budget runs out the hook
        // also raises on every instruction, so the code after a pcall dies anyway; this is the second barrier.)
        _lua["__n2_is_budget_error"] = [](const sol::object &message) -> bool
        {
            return message.is<std::string>() && message.as<std::string>().find(BudgetErrorText) != std::string::npos;
        };
        _lua.script(R"(
            local rawPcall, rawXpcall = pcall, xpcall
            local isBudgetError = __n2_is_budget_error
            __n2_is_budget_error = nil
            local function check(ok, ...)
                if not ok and isBudgetError((...)) then
                    error((...), 0)
                end
                return ok, ...
            end
            pcall = function(...) return check(rawPcall(...)) end
            xpcall = function(...) return check(rawXpcall(...)) end

            -- The VM runs a __gc finalizer with its debug hooks off, so the instruction budget cannot see into it:
            -- scripts get no finalizers. (Lua registers one only if __gc is in the metatable at setmetatable.)
            local rawSetmetatable, rawGet, typeOf = setmetatable, rawget, type
            setmetatable = function(t, mt)
                if typeOf(mt) == "table" and rawGet(mt, "__gc") ~= nil then
                    error("__gc metamethods are not available to scripts", 2)
                end
                return rawSetmetatable(t, mt)
            end
        )");
    }

    LuaRuntime::~LuaRuntime() = default;

    void LuaRuntime::SetLimits(const LuaLimits &limits)
    {
        _guard->cap = limits.maxBytes;
        _guard->limit = limits.instructionsPerEntry;
        if (_guard->depth == 0)
        {
            _guard->Arm();
        }
        // Inside an entry the new budget applies from the next one: this entry keeps counting down its own
    }

    LuaLimits LuaRuntime::GetLimits() const
    {
        return {_guard->limit, _guard->cap};
    }

    std::size_t LuaRuntime::GetMemoryUsed() const
    {
        return _guard->used;
    }

    bool LuaRuntime::IsLimitError(const std::string_view message)
    {
        return message.find(BudgetErrorText) != std::string_view::npos ||
               message.find(MemoryErrorText) != std::string_view::npos;
    }

    bool LuaRuntime::ShouldLogLimitError(const std::string &key)
    {
        return _limitLogged.insert(key).second;
    }

    void LuaRuntime::BeginEntry()
    {
        if (_guard->depth++ == 0)
        {
            _guard->Arm();
        }
    }

    void LuaRuntime::EndEntry(const int outerDepth)
    {
        // Restore the depth the scope found, not "one less": an error raised under a Lua protected call jumps over the
        // C++ frames in between (Lua is C), so scopes opened inside it never ran their destructors. Counting them down
        // one by one would leave the depth raised for good and the budget never refilled.
        _guard->depth = outerDepth;
        if (outerDepth == 0)
        {
            // (No lua_closethread here for a panic: Panic repairs the thread. Resetting the thread here wiped the stack
            // under a result the caller still held, such as the one RunSource returns, or under a Lua call still running
            // when the outermost scope was opened by a binding a script called.)
            // Back to a full budget, so C++ that touches Lua outside any scope never meets the exhausted one
            _guard->Arm();
        }
    }

    LuaBudgetScope::LuaBudgetScope()
        : _outerDepth(LuaRuntime::Instance()._guard->depth)
    {
        LuaRuntime::Instance().BeginEntry();
    }

    LuaBudgetScope::~LuaBudgetScope()
    {
        LuaRuntime::Instance().EndEntry(_outerDepth);
    }

    namespace
    {
        struct ProtectedBody
        {
            const std::function<void()> *body;
            std::string error;
        };

        int ProtectedTrampoline(lua_State *L)
        {
            auto *call = static_cast<ProtectedBody *>(lua_touserdata(L, 1));
            try
            {
                (*call->body)();
            }
            catch (const std::exception &e)
            {
                call->error = e.what();
            }
            catch (...)
            {
                call->error = "unknown error";
            }
            return 0;
        }
    }

    bool LuaRuntime::ProtectedCall(const std::function<void()> &body, std::string &error)
    {
        const LuaBudgetScope budget;
        lua_State *L = _lua.lua_state();
        ProtectedBody call{&body, {}};
        lua_pushcfunction(L, &ProtectedTrampoline);
        lua_pushlightuserdata(L, &call);
        const int status = lua_pcall(L, 1, 0, 0);
        if (status != LUA_OK)
        {
            const char *message = lua_tostring(L, -1);
            error = message != nullptr ? message : (status == LUA_ERRMEM ? std::string(MemoryErrorText) : "script error");
            lua_pop(L, 1);
            return false;
        }
        if (!call.error.empty())
        {
            error = std::move(call.error);
            return false;
        }
        return true;
    }

    sol::protected_function_result LuaRuntime::RunSource(const std::string_view source, const std::string &chunkName)
    {
        // Text only: a project .lua asset holding precompiled bytecode would otherwise run as-is
        const LuaBudgetScope budget;
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
        Bindings::BindUI(*this);

        SetupModuleSystem();

        // Types bound with sol::call_constructor are constructed as T(...); engine-api.lua also documents
        // T.new(...), which sol doesn't add on its own, so alias it to the call constructor
        _lua.script(R"(
            for _, name in ipairs({ "Vector2", "Vector3", "Vector4", "Quaternion", "Color", "PhysicsMaterial", "BoundingBox", "Ray" }) do
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
            if (!IsLimitError(err.what()) || ShouldLogLimitError(path.ToString()))
            {
                Logger::Error(std::format("Error running {}: {}", path.ToString(), err.what()));
            }
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
            if (!IsLimitError(err.what()) || ShouldLogLimitError(path.ToString()))
            {
                Logger::Error(std::format("Failed to load module {}: {}", moduleName, err.what()));
            }
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
        // The module run and every component's reload callback share one budget
        const LuaBudgetScope budget;
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
