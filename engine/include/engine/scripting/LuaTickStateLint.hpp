#pragma once

#include <array>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace N2Engine::Scripting
{
    /// The script callbacks a deterministic tick runs (a fixed step and the physics events of its step)
    inline constexpr std::array<std::string_view, 13> kTickCallbacks = {
        "OnFixedUpdate",  "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit",
        "OnTriggerEnter", "OnTriggerStay",    "OnTriggerExit",
        "OnCollisionEnter2D", "OnCollisionStay2D", "OnCollisionExit2D",
        "OnTriggerEnter2D",   "OnTriggerStay2D",   "OnTriggerExit2D",
    };

    /**
     * The load-time lint of netcode design 9.2 option A (the N7 contract: state a rollback or a late joiner needs must be
     * a declared `replicated = true` field). It reads the bytecode of a script class's tick callbacks (kTickCallbacks)
     * and of what they reach, and names every place a tick writes state that a WorldSnapshot does not hold:
     *   - a module-level local assigned (`hidden = hidden + 1`: OP_SETUPVAL on a local of the chunk),
     *   - a field of a module-level local table, or of the class table itself, written (`state.n = 1`, `Script.hits = 1`:
     *     OP_SETTABUP), and a global written (the same opcode on _ENV),
     *   - a field of `self` written directly in a method (`self.timer = 0`: OP_SETFIELD on register 0 of a function whose
     *     first parameter is self) whose name is not in `replicatedFields`.
     * What it follows from a callback: the functions defined inside it (their writes to the chunk's locals), the local
     * functions it captures (`local function step() ... end`, by their upvalues), and the class's methods it calls on
     * self (`self:Advance()`, OP_SELF on register 0, looked up raw in the class table). What it does NOT see, so a clean
     * result is not a proof: a table reached through another value and then written (`state.player.hp = 1`,
     * `state[i] = 1`, `local s = self; s.x = 1`), self written inside a nested function, methods inherited through the
     * class's own metatable or called any other way (`Script.helper(self)`), coroutines, and state kept in C++ objects
     * the script calls. Writes in other callbacks (OnAttach, OnUpdate) are not reported: they do not run in a tick.
     *
     * `classIndex` is a stack index of the class table. The stack is left as it was. It allocates nothing in the Lua state
     * and runs no Lua code (raw reads only); it needs 6 free stack slots per level of nesting and stops following when
     * lua_checkstack refuses. Each finding is one sentence, "<callback> <what>", with no duplicates, in the order found.
     * Depends on Lua 5.4.8 internals (lobject.h, lopcodes.h).
     */
    [[nodiscard]] std::vector<std::string> LintTickState(lua_State *L, int classIndex,
                                                         const std::set<std::string, std::less<>> &replicatedFields);
}
