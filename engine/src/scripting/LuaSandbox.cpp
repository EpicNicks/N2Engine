#include "engine/scripting/LuaSandbox.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <format>
#include <utility>

#include <sol/sol.hpp>

#include "LuaGuard.hpp"
#include "LuaSandboxText.hpp"

namespace N2Engine::Scripting
{
    namespace Detail
    {
        std::string CleanScriptText(std::string_view text)
        {
            bool truncated = false;
            if (text.size() > MaxErrorBytes)
            {
                std::size_t cut = MaxErrorBytes;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                text = text.substr(0, cut);
                truncated = true;
            }
            std::string clean;
            clean.reserve(text.size() + 3);
            for (const char c : text)
            {
                const auto byte = static_cast<unsigned char>(c);
                clean += byte < 0x20 || byte == 0x7F ? '?' : c;
            }
            if (truncated)
            {
                clean += "...";
            }
            return clean;
        }

        std::string ErrorObjectText(lua_State *L)
        {
            if (lua_type(L, -1) == LUA_TSTRING)
            {
                std::size_t length = 0;
                const char *text = lua_tolstring(L, -1, &length);
                return CleanScriptText(std::string_view(text, length));
            }
            return std::format("(a {} error value)", lua_typename(L, lua_type(L, -1)));
        }
    }

    namespace
    {
        std::atomic<int> g_liveSandboxes{0};

        // A deadline further away than this is not meaningful, and would overflow the clock's range
        constexpr std::uint64_t MaxMillis = std::uint64_t{24} * 60 * 60 * 1000;

        // Names removed from the base library of the EditorExtension profile: they load code or files, steer the
        // collector, or write to the process's output (print and warn: the host's Editor.Log replaces print later)
        constexpr const char *RemovedBaseFunctions[] = {"load",    "loadstring", "loadfile", "dofile",
                                                         "collectgarbage", "print", "warn"};
        // The string functions the EditorExtension profile does not have: the pattern functions (their C-level cost
        // is invisible to the instruction hook; design decision D6) and dump (it serializes a function's bytecode)
        constexpr const char *RemovedStringFunctions[] = {"find", "match", "gmatch", "gsub", "dump"};

        // A 0 field takes the sandbox's load budget value (never unlimited)
        SandboxBudget Resolve(const SandboxBudget &budget, const SandboxBudget &loadBudget)
        {
            SandboxBudget resolved = budget;
            if (resolved.instructions == 0)
            {
                resolved.instructions = loadBudget.instructions == 0 ? SandboxDefaults::LoadBudget.instructions
                                                                      : loadBudget.instructions;
            }
            if (resolved.maxMillis == 0)
            {
                resolved.maxMillis = loadBudget.maxMillis == 0 ? SandboxDefaults::LoadBudget.maxMillis
                                                               : loadBudget.maxMillis;
            }
            resolved.maxMillis = std::min(resolved.maxMillis, MaxMillis);
            return resolved;
        }

        // The strings' metatable is shared by every string of the state: protected, so a script cannot add __len,
        // __newindex or another __index to it (getmetatable("") answers "protected")
        void ProtectStringMetatable(lua_State *L)
        {
            lua_pushliteral(L, "");
            if (lua_getmetatable(L, -1) != 0)
            {
                lua_pushliteral(L, "protected");
                lua_setfield(L, -2, "__metatable");
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }

        // string.rep of an empty string (and an empty separator) loops in C for as many times as it is asked, and
        // allocates nothing, so the allocator cap cannot refuse it and the hook cannot interrupt it: one call with
        // 2^60 would never return. The answer is "" whatever the count, so it is given without calling C
        // (as the declaration evaluator does).
        constexpr const char *EditorGuardScript = R"LUA(
local srep, type = string.rep, type
string.rep = function(s, n, sep)
    if type(s) == "string" and #s == 0 and (sep == nil or (type(sep) == "string" and #sep == 0)) then
        return ""
    end
    return srep(s, n, sep)
end

-- table.move, insert and remove loop in C over a range or a length, one C call runs no instruction, and a plain
-- table can have a huge border (2^40 with 41 elements) or be given a huge range with nothing in it: the hook could
-- not stop them. Here they are Lua with the stock semantics (metamethods included), so every element step is counted
-- and a huge range ends as a budget trip. concat, unpack and sort stay the stock C functions (see LuaSandbox.hpp).
local mtoint, maxint, sformat, select = math.tointeger, math.maxinteger, string.format, select
local function intArg(v, argn, fname)
    local tv = type(v)
    local n = nil
    if tv == "number" or tv == "string" then
        n = mtoint(v)
    end
    if n == nil then
        if tv == "number" then
            error(sformat("bad argument #%d to '%s' (number has no integer representation)", argn, fname), 3)
        end
        error(sformat("bad argument #%d to '%s' (number expected, got %s)", argn, fname,
            tv == "nil" and "no value" or tv), 3)
    end
    return n
end
local function checkTable(v, argn, fname)
    if type(v) ~= "table" then
        error(sformat("bad argument #%d to '%s' (table expected, got %s)", argn, fname, type(v)), 3)
    end
end
local function lengthOf(t)
    local n = mtoint(#t)
    if n == nil then
        error("object length is not an integer", 3)
    end
    return n
end
table.move = function(a1, f, e, t, a2)
    local explicit = a2 ~= nil
    if not explicit then
        a2 = a1
    end
    checkTable(a1, 1, "move")
    checkTable(a2, explicit and 5 or 1, "move")
    local from = intArg(f, 2, "move")
    local to = intArg(e, 3, "move")
    local dest = intArg(t, 4, "move")
    if to >= from then
        local n = to - from
        if n < 0 then
            error("bad argument #3 to 'move' (too many elements to move)", 2)
        end
        if dest > maxint - n then
            error("bad argument #4 to 'move' (destination wrap around)", 2)
        end
        if dest > to or dest <= from or (explicit and not (a1 == a2)) then
            for k = 0, n do
                a2[dest + k] = a1[from + k]
            end
        else
            for k = n, 0, -1 do
                a2[dest + k] = a1[from + k]
            end
        end
    end
    return a2
end
table.insert = function(t, ...)
    checkTable(t, 1, "insert")
    local nargs = select("#", ...)
    if nargs ~= 1 and nargs ~= 2 then
        error("wrong number of arguments to 'insert'", 2)
    end
    local e = lengthOf(t) + 1
    if nargs == 1 then
        t[e] = (...)
        return
    end
    local p, v = ...
    local pos = intArg(p, 2, "insert")
    if pos < 1 or pos > e then
        error("bad argument #2 to 'insert' (position out of bounds)", 2)
    end
    for i = e, pos + 1, -1 do
        t[i] = t[i - 1]
    end
    t[pos] = v
end
table.remove = function(t, ...)
    checkTable(t, 1, "remove")
    local size = lengthOf(t)
    local pos = size
    local p = ...
    if p ~= nil then
        pos = intArg(p, 2, "remove")
    end
    if pos ~= size and (pos < 1 or pos > size + 1) then
        error("bad argument #2 to 'remove' (position out of bounds)", 2)
    end
    local removed = t[pos]
    for i = pos, size - 1 do
        t[i] = t[i + 1]
    end
    if pos < size then
        pos = size
    end
    t[pos] = nil
    return removed
end
)LUA";

        void TrimForEditorExtension(lua_State *L)
        {
            for (const char *name : RemovedBaseFunctions)
            {
                lua_pushnil(L);
                lua_setglobal(L, name);
            }
            // What the library setup and sol leave in _G besides the libraries: "base" (the globals table again),
            // sol's default error handler (a function) and its exception handler (a light userdata: the address of
            // a function of the host's binary, which tostring would show). Nothing of the sandbox needs them: sol
            // falls back to its own handler when the global is not a light userdata, and an absent error handler
            // means no handler.
            for (const char *name : {"base", sol::detail::default_handler_name(),
                                     sol::detail::default_exception_handler_name()})
            {
                lua_pushnil(L);
                lua_setglobal(L, name);
            }
            lua_getglobal(L, "string");
            if (lua_istable(L, -1))
            {
                for (const char *name : RemovedStringFunctions)
                {
                    lua_pushnil(L);
                    lua_setfield(L, -2, name);
                }
            }
            lua_pop(L, 1);
            ProtectStringMetatable(L);
            // The profile's own code, before any budget is armed (the state has nothing else to run yet)
            if (luaL_loadbufferx(L, EditorGuardScript, std::strlen(EditorGuardScript), "=sandbox-setup", "t") != LUA_OK ||
                lua_pcall(L, 0, 0, 0) != LUA_OK)
            {
                throw std::runtime_error("The sandbox could not be set up");
            }
        }

        // An exception that left a call into the state: the panic function's (an allocation failing outside a
        // protected call) or a C++ error. Data like any other.
        SandboxOutcome ExceptionOutcome(const Detail::LuaGuardState &guard, const char *what)
        {
            SandboxOutcome outcome;
            outcome.ok = false;
            const std::string text = Detail::CleanScriptText(what);
            if (guard.timedOut)
            {
                outcome.budgetExceeded = true;
                outcome.timedOut = true;
                outcome.error = "The entry exceeded its time limit";
            }
            else if (guard.exhausted)
            {
                outcome.budgetExceeded = true;
                outcome.error = std::format("{} of {}", Detail::BudgetErrorText, guard.limit);
            }
            else
            {
                if (text.find(Detail::MemoryErrorText) != std::string::npos)
                {
                    outcome.memoryExhausted = true;
                }
                outcome.error = "The entry failed: " + text;
            }
            return outcome;
        }
    }

    struct LuaSandbox::Impl
    {
        SandboxProfile profile = SandboxProfile::Declarations;
        // Before the state and outliving it (Close destroys the state only): the allocator and the hook use it until
        // the state is closed, and the counters are read from it afterwards
        std::unique_ptr<Detail::LuaGuardState> guard;
        std::unique_ptr<sol::state> lua;
        SandboxOutcome prelude;
        SandboxBudget loadBudget = SandboxDefaults::LoadBudget;
        bool counted = false;

        void CloseState()
        {
            lua.reset();
            if (counted)
            {
                counted = false;
                g_liveSandboxes.fetch_sub(1);
            }
        }

        ~Impl()
        {
            CloseState();
        }
    };

    LuaSandbox::LuaSandbox(std::unique_ptr<Impl> impl) : _impl(std::move(impl))
    {
    }

    LuaSandbox::~LuaSandbox() = default;

    std::unique_ptr<LuaSandbox> LuaSandbox::Create(const SandboxProfile profile, const SandboxOptions &options)
    {
        auto impl = std::make_unique<Impl>();
        impl->profile = profile;
        impl->guard = std::make_unique<Detail::LuaGuardState>();
        Detail::LuaGuardState &guard = *impl->guard;
        guard.cap = std::max(options.maxBytes == 0 ? SandboxDefaults::MaxBytes : options.maxBytes,
                             SandboxDefaults::MinBytes);
        impl->loadBudget = Resolve(options.loadBudget, SandboxDefaults::LoadBudget);
        guard.limit = impl->loadBudget.instructions;

        g_liveSandboxes.fetch_add(1);
        impl->counted = true;
        impl->lua = std::make_unique<sol::state>(&Detail::LuaGuardState::Panic, &Detail::LuaCappedAlloc, &guard);
        lua_State *L = impl->lua->lua_state();
        guard.state = L;

        switch (profile)
        {
        case SandboxProfile::Declarations:
            impl->lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
            Detail::InstallBudgetSandbox(*impl->lua);
            break;
        case SandboxProfile::EditorExtension:
            impl->lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table,
                                      sol::lib::utf8);
            Detail::InstallBudgetSandbox(*impl->lua);
            TrimForEditorExtension(L);
            break;
        }

        std::string_view prelude = profile == SandboxProfile::EditorExtension ? SandboxDefaults::EditorExtensionPrelude
                                                                               : std::string_view{};
        if (options.prelude)
        {
            prelude = *options.prelude;
        }
        const SandboxBudget loadBudget = options.loadBudget;
        auto sandbox = std::unique_ptr<LuaSandbox>(new LuaSandbox(std::move(impl)));
        if (!prelude.empty())
        {
            sandbox->_impl->prelude = sandbox->RunChunk(prelude, "=prelude", loadBudget);
        }
        return sandbox;
    }

    SandboxProfile LuaSandbox::Profile() const
    {
        return _impl->profile;
    }

    lua_State *LuaSandbox::State() const
    {
        return _impl->lua ? _impl->lua->lua_state() : nullptr;
    }

    const SandboxOutcome &LuaSandbox::PreludeOutcome() const
    {
        return _impl->prelude;
    }

    void LuaSandbox::Arm(const SandboxBudget &budget)
    {
        if (!_impl->lua)
        {
            return;
        }
        const SandboxBudget resolved = Resolve(budget, _impl->loadBudget);
        Detail::LuaGuardState &guard = *_impl->guard;
        guard.limit = resolved.instructions;
        guard.hasDeadline = true;
        guard.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(resolved.maxMillis);
        guard.Arm();
    }

    void LuaSandbox::Disarm()
    {
        if (!_impl->lua)
        {
            return;
        }
        _impl->guard->hasDeadline = false;
        lua_sethook(_impl->lua->lua_state(), nullptr, 0, 0);
    }

    SandboxOutcome LuaSandbox::FailureOutcome(const int status, const char *phase)
    {
        SandboxOutcome outcome;
        outcome.ok = false;
        const Detail::LuaGuardState &guard = *_impl->guard;
        if (!_impl->lua && !guard.timedOut && !guard.exhausted && status != LUA_ERRMEM)
        {
            outcome.error = "The sandbox is closed";
            return outcome;
        }
        if (guard.timedOut)
        {
            outcome.budgetExceeded = true;
            outcome.timedOut = true;
            outcome.error = "The entry exceeded its time limit";
        }
        else if (guard.exhausted)
        {
            outcome.budgetExceeded = true;
            outcome.error = std::format("{} of {}", Detail::BudgetErrorText, guard.limit);
        }
        else if (status == LUA_ERRMEM)
        {
            outcome.memoryExhausted = true;
            outcome.error = std::format("{} (the cap is {} bytes)", Detail::MemoryErrorText, guard.cap);
        }
        else
        {
            outcome.error = std::format("{}: {}", phase, Detail::ErrorObjectText(_impl->lua->lua_state()));
        }
        return outcome;
    }

    SandboxOutcome LuaSandbox::RunChunk(const std::string_view source, const std::string &chunkName,
                                        const SandboxBudget &budget, const int resultCount)
    {
        SandboxOutcome outcome;
        if (!_impl->lua)
        {
            outcome.ok = false;
            outcome.error = "The sandbox is closed";
            return outcome;
        }
        lua_State *L = _impl->lua->lua_state();
        const int results = std::max(resultCount, 0);
        const std::string name = chunkName.size() > 100 ? chunkName.substr(0, 100) : chunkName;

        Arm(budget);
        try
        {
            // Text only: a precompiled chunk is refused, as everywhere in the engine
            int status = luaL_loadbufferx(L, source.data(), source.size(), name.c_str(), "t");
            if (status == LUA_OK)
            {
                status = lua_pcall(L, 0, results, 0);
            }
            if (status != LUA_OK)
            {
                outcome = FailureOutcome(status, "Error running the chunk");
                lua_settop(L, 0);
            }
        }
        catch (const std::exception &e)
        {
            outcome = ExceptionOutcome(*_impl->guard, e.what());
            lua_settop(L, 0);
        }
        catch (...)
        {
            outcome.ok = false;
            outcome.error = "The entry failed";
            lua_settop(L, 0);
        }
        Disarm();
        return outcome;
    }

    SandboxCounters LuaSandbox::Counters() const
    {
        const Detail::LuaGuardState &guard = *_impl->guard;
        SandboxCounters counters;
        counters.usedBytes = guard.used;
        counters.peakBytes = guard.peak;
        counters.capBytes = guard.cap;
        counters.instructionLimit = guard.limit;
        counters.budgetExhausted = guard.exhausted;
        counters.timedOut = guard.timedOut;
        return counters;
    }

    void LuaSandbox::Close()
    {
        _impl->CloseState();
    }

    int LuaSandbox::LiveStates()
    {
        return g_liveSandboxes.load();
    }
}
