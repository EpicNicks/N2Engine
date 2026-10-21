#pragma once

#include <sol/sol.hpp>

namespace N2Engine::Scripting
{
    /**
     * Replaces math.sin, math.cos, math.tan, math.asin, math.acos, math.atan (both forms), math.exp and math.log (both
     * forms) of the state with the engine-owned functions of math/DeterministicMath.hpp, which give the same bits on
     * every platform where the C library's give platform-specific last bits (netcode C1, blocker B4 of the determinism
     * report). The replacements take and return numbers exactly as Lua's own do (numeric strings are accepted, a
     * non-number raises the same kind of error, math.atan(y) is math.atan(y, 1)); only the last bit of a result can
     * differ from the stock functions. math.log(x, 2) is Log2(x) and math.log(x, b) is Log(x) / Log(b).
     *
     * The state's math library must be open. LuaRuntime reaches this through InstallDeterministicMathIfSeeded.
     *
     * Not replaced: the ^ operator (luai_numpow in the Lua VM, which calls the C library's pow for every exponent but 2,
     * a source file outside the engine's own code), math.sqrt, math.floor, math.ceil and math.fmod (correctly rounded
     * or exact everywhere), and string.format / tostring (the C library's printf).
     */
    void InstallDeterministicMath(sol::state_view lua);

    /**
     * InstallDeterministicMath when determinism.luaSeed is set (LuaRuntime::GetLuaSeed has a value), the same gate as the
     * seeded string hash and math.random: a project that asks for reproducible Lua gets reproducible math too, and the
     * Deterministic network preset requires the seed. With it unset the stock functions stay, unchanged. True when
     * the functions were replaced. LuaRuntime calls it when it creates its state; a test calls it on a state of its own.
     */
    bool InstallDeterministicMathIfSeeded(sol::state_view lua);

    /**
     * Defines the global table Ordered with Ordered.pairs(t): an iterator over the raw keys of t in an order that is
     * the same on every run and every platform, which pairs() is not (its order depends on hash values, and for keys that
     * are tables, functions or userdata on addresses). The order: the array part first (1, 2, 3, ... up to the first
     * missing index, in that order), then every other key, grouped by type (numbers, then strings, then booleans) and
     * ascending within a group (numbers by value, strings by byte order, false before true). A key of any other type
     * (a table, function, userdata or thread) has no order and raises an error naming the type. The keys are
     * collected when Ordered.pairs is called; the values are read as the loop reaches them (a key removed meanwhile is
     * skipped, one added meanwhile is not visited). Values are read raw: __index is not consulted, and neither is __pairs.
     */
    void InstallOrderedPairs(sol::state_view lua);
}
