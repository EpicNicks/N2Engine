#include "engine/scripting/LuaRuntime.hpp"

#include <algorithm>
#include <cstdio>
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
#include <n2seed.h>
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
        // Stack slots the panic function leaves filled (the error object, then nils) for C++ frames that pop while the
        // exception unwinds. Below LUA_MINSTACK (20), which the thread reset reserves above the base, so no allocation.
        constexpr int PanicPadding = 16;

        // The error object at `index` as text, without converting it: lua_tolstring on a number makes a new string,
        // an allocation that, at the memory cap and outside a protected call, goes down the panic path. A number is
        // read as a number (lua_tointeger/lua_tonumber leave the object as it is) and formatted into a C++ buffer.
        std::string ErrorValueText(lua_State *L, const int index)
        {
            const int type = lua_type(L, index);
            if (type == LUA_TSTRING)
            {
                std::size_t length = 0;
                const char *text = lua_tolstring(L, index, &length);
                return std::string(text, length);
            }
            if (type == LUA_TNUMBER)
            {
                char number[64];
                if (lua_isinteger(L, index))
                {
                    std::snprintf(number, sizeof(number), "%lld", static_cast<long long>(lua_tointeger(L, index)));
                }
                else
                {
                    std::snprintf(number, sizeof(number), "%.14g", static_cast<double>(lua_tonumber(L, index)));
                }
                return std::format("(a number error value: {})", number);
            }
            return std::format("(a {} error value)", lua_typename(L, type));
        }

        // What a log line may show of text that came from a script or a path: control characters replaced by '?',
        // cut at 100 bytes on a character boundary (the same rule as the editor host's SanitizeForLog)
        std::string SanitizeForLog(std::string_view text)
        {
            constexpr std::size_t MaxBytes = 100;
            bool truncated = false;
            if (text.size() > MaxBytes)
            {
                std::size_t cut = MaxBytes;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                text = text.substr(0, cut);
                truncated = true;
            }
            std::string result;
            result.reserve(text.size() + 3);
            for (const char c : text)
            {
                const auto byte = static_cast<unsigned char>(c);
                result += byte < 0x20 || byte == 0x7F ? '?' : c;
            }
            if (truncated)
            {
                result += "...";
            }
            return result;
        }
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
            std::uint64_t panics = 0;             // errors that reached the panic function
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
            // a script's metamethod, or a failed allocation), and it throws the error as a sol::error, as sol's own
            // default_at_panic does. Lua has already reset the thread (no call is running any more, the stack is cut
            // down to its base and the error object), but not three of its fields; they are repaired here, before the
            // exception leaves (LuaPanicRepair.cpp says which).
            // Unlike default_at_panic, this does not empty the stack: it leaves the error object and pads the stack with
            // nils up to PanicPadding slots (within the slots the reset already reserved, so nothing is allocated). C++
            // frames the exception unwinds can pop the stack: sol's protected_function call removes its message
            // handler (lua_remove) when its arguments could not be pushed. On an empty stack that pop goes below the
            // base, and lua_settop then "closes" the base slot as a to-be-closed variable: calling nil raises a new
            // error, whose message cannot be allocated at the cap, which panics again inside a destructor while the
            // first exception unwinds: std::terminate. The caller that catches the exception resets the stack to its
            // base (LuaRuntime::RecoverAfterPanic).
            static int Panic(lua_State *L)
            {
                RepairThreadAfterPanic(L);
                ++Of(L)->panics;
                // Only a string is read: converting another value with lua_tolstring could allocate
                std::string message = "An unexpected error occurred and panicked";
                if (lua_type(L, -1) == LUA_TSTRING)
                {
                    std::size_t length = 0;
                    const char *text = lua_tolstring(L, -1, &length);
                    message.assign(text, length);
                }
                lua_settop(L, PanicPadding);
                throw sol::error(message);
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

    void LuaRuntime::SetLuaSeed(const std::optional<std::uint64_t> seed)
    {
        n2lua_setseed(seed.has_value() ? 1 : 0, seed.value_or(0));
        if (_instance != nullptr && _instance->_stateLuaSeed != seed)
        {
            Logger::Warn("determinism.luaSeed changed after the Lua state was created; it applies after a restart");
        }
    }

    std::optional<std::uint64_t> LuaRuntime::GetLuaSeed()
    {
        unsigned long long seed = 0;
        if (n2lua_getseed(&seed) != 0)
        {
            return static_cast<std::uint64_t>(seed);
        }
        return std::nullopt;
    }

    LuaRuntime::LuaRuntime()
        : _stateLuaSeed(GetLuaSeed()),
          _guard(std::make_unique<Detail::LuaGuardState>()),
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
            -- Lua calls a message handler where the error is raised, before unwinding. The budget error is raised in
            -- the count hook, and the VM runs hooks with hooks off (luaD_hook clears allowhook until the hook
            -- returns), so a script's handler called there would run uncounted: `xpcall(f, function() while true do
            -- end end)` would never end. The script's handler is therefore never handed a budget error; this small
            -- wrapper (which runs a few instructions and calls no script code for it) hands it on unchanged.
            local isFunction = type
            xpcall = function(f, handler, ...)
                if isFunction(handler) ~= "function" then
                    return check(rawXpcall(f, handler, ...)) -- xpcall's own argument error
                end
                return check(rawXpcall(f, function(message)
                    if isBudgetError(message) then
                        return message
                    end
                    return handler(message)
                end, ...))
            end

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

    std::string LuaRuntime::LimitAwareError(const std::string_view message, const bool memoryError) const
    {
        if (_guard->exhausted)
        {
            // The text the count hook raises
            return std::format("{} of {}", BudgetErrorText, _guard->limit);
        }
        if (memoryError)
        {
            return std::string(MemoryErrorText); // what Lua's preregistered memory error message says
        }
        return std::string(message);
    }

    std::uint64_t LuaRuntime::PanicCount() const
    {
        return _guard->panics;
    }

    void LuaRuntime::RecoverAfterPanic(const std::uint64_t panicsBefore)
    {
        if (_guard->panics != panicsBefore)
        {
            // The panic function left the error object and padding on the stack (see Panic), and the frames the
            // exception unwound may have popped some of it (the message handler of a protected call). Nothing on the
            // stack is anyone's any more: back to the empty base.
            lua_settop(_lua.lua_state(), 0);
        }
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
            // Not read from the stack when the budget or the memory cap was hit: the error unwound the body's C++
            // frames, and where those run destructors on the way that pop the stack (MSVC runs C++ destructors on a
            // longjmp), Lua copies the wrong slot as the error object. Seen in the MSVC Debug build: the error text
            // was the key the body was reading, not the budget error.
            if (_guard->exhausted)
            {
                error = LimitAwareError({});
            }
            else if (status == LUA_ERRMEM)
            {
                error = std::string(MemoryErrorText);
            }
            else
            {
                error = ErrorValueText(L, -1);
            }
            lua_pop(L, 1);
            return false;
        }
        if (!call.error.empty())
        {
            error = LimitAwareError(call.error);
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

    sol::protected_function_result LuaRuntime::RunChunk(const std::string_view source, const std::string &chunkName)
    {
        sol::protected_function_result result = RunSource(source, chunkName);
        if (!result.valid() || result.return_count() <= 1)
        {
            return result;
        }
        // Keep the first value only: the result no longer pops the others, they are dropped here (no allocation)
        lua_State *L = result.lua_state();
        const int first = result.stack_index();
        result.abandon();
        lua_settop(L, first);
        return sol::protected_function_result(L, first, 1, 1, sol::call_status::ok);
    }

    std::string LuaRuntime::ErrorText(const sol::protected_function_result &result)
    {
        return ErrorValueText(result.lua_state(), result.stack_index());
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

        const auto result = RunChunk(script->GetSourceCode(), path.ToString());
        if (!result.valid())
        {
            const std::string error = LimitAwareError(ErrorText(result), result.status() == sol::call_status::memory);
            if (!IsLimitError(error) || ShouldLogLimitError(path.ToString()))
            {
                Logger::Error(std::format("Error running {}: {}", path.ToString(), error));
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
        // A new empty table, made in a protected call: at the memory cap an allocation outside one would go down the
        // panic path (from ReloadModule, which no Lua call protects). On failure it is an invalid table (nil to Lua).
        const auto emptyTable = [this]
        {
            sol::table table;
            std::string error;
            ProtectedCall([&] { table = _lua.create_table(); }, error);
            return table;
        };

        auto result = RunChunk(script->GetSourceCode(), path.ToString());

        if (!result.valid())
        {
            const std::string error = LimitAwareError(ErrorText(result), result.status() == sol::call_status::memory);
            if (!IsLimitError(error) || ShouldLogLimitError(path.ToString()))
            {
                Logger::Error(std::format("Failed to load module {}: {}", moduleName, error));
            }
            return emptyTable();
        }

        sol::table moduleTable;

        if (result.return_count() > 0 && result[0].is<sol::table>())
        {
            moduleTable = result[0];
        }
        else
        {
            moduleTable = emptyTable();
        }

        if (!moduleTable.valid())
        {
            // The empty table could not be made (the memory cap): nothing is stored, so a later require runs the
            // module again instead of being handed nil as the module
            _loadedModules.erase(moduleName);
            Logger::Error(std::format("Failed to load module {}: {}", moduleName, MemoryErrorText));
            return moduleTable;
        }
        // (A reference copy outside a protected call, as is result[0] above: see the known limitation in
        // docs/scripting-lua.html, follow-up sec-lua-registry-reserve)
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
        // A hot reload is an outermost entry no Lua call protects (a client's WriteTextAsset on the editor host): a C++
        // exception from it (an allocation outside a protected call failing at the memory cap, which comes down the
        // panic path) is reported here instead of leaving into the host's command handling, and one failing reload
        // callback does not stop the others. Not covered: a sol reference created or copied outside a protected call
        // is noexcept, so its failure ends the process before any catch (known limitation, docs/scripting-lua.html)
        const auto guarded = [this, &moduleName](const auto &step)
        {
            const std::uint64_t panicsBefore = PanicCount();
            try
            {
                step();
            }
            catch (const std::exception &e)
            {
                RecoverAfterPanic(panicsBefore);
                Logger::Error(std::format("Reloading module {} failed: {}", SanitizeForLog(moduleName),
                                          SanitizeForLog(e.what())));
            }
            catch (...)
            {
                RecoverAfterPanic(panicsBefore);
                Logger::Error(std::format("Reloading module {} failed", SanitizeForLog(moduleName)));
            }
        };
        guarded([&] { LoadScriptAsModule(path, script); });

        // Notify callbacks. Iterate a copy: a callback can register or unregister callbacks
        // (LuaComponent::ReloadScript may call SetScript), which would invalidate the live vector
        if (auto it = _reloadCallbacks.find(moduleName); it != _reloadCallbacks.end())
        {
            const auto callbacks = it->second;
            for (const auto &entry : callbacks)
            {
                guarded([&] { entry.callback(); });
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
