#pragma once

// What the Lua state's allocator, instruction hook and panic function share (the runtime's state in LuaRuntime.cpp and
// the declaration evaluator's throwaway state both use it; neither duplicates it). Private to engine/src/scripting.

#include <sol/sol.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/scripting/LuaRuntime.hpp"

namespace N2Engine::Scripting::Detail
{
    inline constexpr std::string_view BudgetErrorText = "script exceeded its instruction budget";
    inline constexpr std::string_view MemoryErrorText = "not enough memory";
    // Instructions between two calls of the hook: small enough to stop a loop promptly, large enough that the
    // hook's cost is negligible
    inline constexpr std::uint64_t HookGranularity = 1000;
    // Stack slots the panic function leaves filled (the error object, then nils) for C++ frames that pop while the
    // exception unwinds. Below LUA_MINSTACK (20), which the thread reset reserves above the base, so no allocation.
    inline constexpr int PanicPadding = 16;

    // LuaPanicRepair.cpp (it reads Lua's internal thread state, so it is kept to a file of its own)
    void RepairThreadAfterPanic(lua_State *L);
    // Also there: the length of the registry's free list (what luaL_ref reuses before it allocates), up to limit
    int CountFreeRegistrySlots(lua_State *L, int limit);

    struct LuaGuardState
    {
        // Memory: what the allocator counts and refuses
        std::size_t used = 0;
        std::size_t peak = 0; // the most bytes held at once (the declaration evaluator reports it)
        std::size_t cap = LuaLimits{}.maxBytes;

        // Instructions
        std::uint64_t limit = LuaLimits{}.instructionsPerEntry;
        std::int64_t remaining = 0;
        std::uint64_t step = HookGranularity; // instructions the installed hook counts per call
        bool exhausted = false;               // the budget ran out in this entry: the hook raises on every instruction
        int depth = 0;                        // LuaBudgetScopes open
        std::uint64_t panics = 0;             // errors that reached the panic function
        lua_State *state = nullptr;
#if defined(N2ENGINE_LUA_ALLOC_FAULT_HOOK)
        // Tests only (the engine target defines the macro when N2ENGINE_BUILD_TESTS is on; a shipped build has
        // none of this): growth allocations that may still succeed before every later one is refused, as at the
        // memory cap, and how many growths were asked for. Switched on only through the functions at the end of
        // LuaRuntime.cpp, which nothing in the engine calls.
        static constexpr std::uint64_t NoFault = UINT64_MAX;
        std::uint64_t growthsAllowed = NoFault;
        std::uint64_t growthRequests = 0;
#endif

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

    /// The allocator of a guarded state: counts the bytes held and refuses a growth past the cap (LuaRuntime.cpp)
    void *LuaCappedAlloc(void *userData, void *ptr, std::size_t osize, std::size_t nsize);

    /// Makes pcall and xpcall hand the budget error on, and setmetatable refuse a metatable with __gc (LuaRuntime.cpp):
    /// the sandbox every state that runs script code under a LuaGuardState gets. The state needs the base library.
    void InstallBudgetSandbox(sol::state_view lua);
}
