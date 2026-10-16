// What Lua's panic path leaves behind, repaired. The C API has no call for it, so this file reads the internal
// thread state (lstate.h of the Lua the engine builds from external/lua); nothing else in the engine does.

// The system headers Lua's headers include, first, so none of them is first seen inside the extern "C" block
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

extern "C"
{
#include "lstate.h"
}

#if LUA_VERSION_RELEASE_NUM != 50408
#error "LuaPanicRepair depends on Lua 5.4.8 internals (lstate.h): re-audit nCcalls/allowhook/status handling before upgrading"
#endif

namespace N2Engine::Scripting::Detail
{
    // What lua_newstate leaves in the main thread's nCcalls: preinit_thread sets 0, then incnny adds one
    // non-yieldable call ("main thread is always non yieldable", lstate.c). A thread from lua_newthread starts at 0.
    constexpr l_uint32 FreshMainThreadCalls = 0x10000;

    // Called by the panic function (LuaRuntime.cpp). An error raised outside every protected call makes luaD_throw
    // call luaE_resetthread, which unwinds the CallInfo list (no call runs any more) and cuts the stack down to the
    // error object (luaD_seterrorobj puts it in the first slot); luaD_throw then sets the status to the error code
    // and calls the panic function, which reads the message, pads the stack (see Panic in LuaRuntime.cpp) and throws
    // it as a C++ exception. The reset leaves three fields as the error found them:
    // - allowhook: 0 when the error came from the count hook (luaD_hook clears it while the hook runs and only a
    //   protected call restores it). Left so, the hook never runs again and the instruction budget is gone for good:
    //   the next `while true do end` runs forever.
    // - status: the error code. The thread is usable again, so it is LUA_OK.
    // - nCcalls: every call the error unwound is still counted. Its low 16 bits count C calls (about two hundred
    //   panics would leave every call failing with "C stack overflow"); its high 16 bits count non-yieldable calls,
    //   and luaD_callnoyield adds one of those per call too (nyci, lstate.h), so they would grow by one or more per
    //   panic as well. No call runs now, so it is set back to what a fresh thread has.
    // The stack itself is not repaired, its values are gone: a C++ object that held Lua stack values across the
    // unprotected operation that panicked pops whatever is there when destroyed (sol's message-handler guard does,
    // when a protected function call's arguments cannot be pushed); the panic function's padding keeps such pops
    // above the base, where a pop below it would have run Lua code and panicked again. The engine makes no unprotected Lua calls;
    // what reaches this is an allocation outside a protected call failing at the memory cap: the arguments sol pushes
    // for a component method or a script callback, and the collision/trigger argument. The callers of those, and a hot
    // reload, catch the exception and call LuaRuntime::RecoverAfterPanic, which resets the stack to its empty base; the
    // padding holds only while the unwound frames pop 16 values or fewer. A sol reference created or copied outside a
    // protected call (luaL_ref can grow the registry) never gets here: its construction is noexcept, so an exception
    // thrown from inside one would end the process (std::terminate). Every such site first reserves free registry
    // slots in a protected call (LuaRuntime::ReserveReferences, CountFreeRegistrySlots below), so luaL_ref allocates
    // nothing there; docs/scripting-lua.html (resource limits) lists them.
    void RepairThreadAfterPanic(lua_State *L)
    {
        L->allowhook = static_cast<lu_byte>(1);
        L->status = static_cast<lu_byte>(LUA_OK);
        L->nCcalls = L == G(L)->mainthread ? FreshMainThreadCalls : 0;
    }

    // How many free slots the registry hands out without allocating, counted up to `limit`: the length of luaL_ref's
    // free list (lauxlib.c of 5.4.8: t[freelist] is the first free index, t[index] the next, 0 ends the list; the list
    // lives at LUA_RIDX_LAST + 1 in this version, it was at 0 before 5.4.3). Reads only: no allocation, no error.
    int CountFreeRegistrySlots(lua_State *L, const int limit)
    {
        int count = 0;
        int isNumber = 0;
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_LAST + 1);
        lua_Integer index = lua_tointegerx(L, -1, &isNumber);
        lua_pop(L, 1);
        while (isNumber != 0 && index != 0 && count < limit)
        {
            ++count;
            lua_rawgeti(L, LUA_REGISTRYINDEX, index);
            index = lua_tointegerx(L, -1, &isNumber);
            lua_pop(L, 1);
        }
        return count;
    }

    // For the tests: the thread's nCcalls (C calls in the low 16 bits, non-yieldable calls in the high 16)
    uint32_t ThreadCallCounts(lua_State *L)
    {
        return static_cast<uint32_t>(L->nCcalls);
    }
}
