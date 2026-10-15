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

namespace N2Engine::Scripting::Detail
{
    // Called by the panic function (LuaRuntime.cpp). An error raised outside every protected call makes luaD_throw
    // reset the thread (no call runs any more) and then call the panic function, which sol turns into a C++
    // exception. The reset leaves three fields as the error found them:
    // - allowhook: 0 when the error came from the count hook (luaD_hook clears it while the hook runs and only a
    //   protected call restores it). Left so, the hook never runs again and the instruction budget is gone for good:
    //   the next `while true do end` runs forever.
    // - status: the error code. The thread is usable again, so it is LUA_OK.
    // - the C-call count (the low 16 bits of nCcalls): the calls the error unwound are still counted, and about two
    //   hundred panics would leave every call failing with "C stack overflow". None runs now, so it is 0; the high
    //   bits (the non-yieldable count of the main thread) are kept.
    // The stack is not touched: the caller of the failed operation still owns it.
    void RepairThreadAfterPanic(lua_State *L)
    {
        L->allowhook = static_cast<lu_byte>(1);
        L->status = static_cast<lu_byte>(LUA_OK);
        L->nCcalls &= 0xffff0000u;
    }
}
