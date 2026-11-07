// The load-time lint of netcode design 9.2 (LuaTickStateLint.hpp). The C API cannot read a function's bytecode, so this
// file reads the function prototypes of the Lua the engine builds from external/lua (lobject.h, lopcodes.h), as
// LuaPanicRepair.cpp reads its thread state. It only reads: no Lua code runs and nothing is allocated in the state.

#include "engine/scripting/LuaTickStateLint.hpp"

#include <cstddef>
#include <numeric>
#include <unordered_set>
#include <utility>

// The system headers Lua's headers include, first, so none of them is first seen inside the extern "C" block
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

extern "C"
{
#include "lua.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
}

#if LUA_VERSION_RELEASE_NUM != 50408
#error "LuaTickStateLint reads Lua 5.4.8 prototypes and opcodes (lobject.h, lopcodes.h): re-audit before upgrading"
#endif

namespace N2Engine::Scripting
{
    namespace
    {
        /// Functions and prototypes looked at, at most (a script is small; this bounds a pathological one)
        constexpr int kMaxVisits = 512;

        std::string Text(const TString *const text)
        {
            if (text == nullptr)
            {
                return "?";
            }
            return std::string(getstr(text), tsslen(text));
        }

        /// K[index] of `proto`, when it is a string
        bool ConstantString(const Proto *const proto, const int index, std::string &out)
        {
            if (index < 0 || index >= proto->sizek)
            {
                return false;
            }
            const TValue *const value = &proto->k[index];
            if (!ttisstring(value))
            {
                return false;
            }
            out = Text(tsvalue(value));
            return true;
        }

        /// A function whose first parameter is `self` (function Script:Name(...)): register 0 is self
        bool IsMethod(const Proto *const proto)
        {
            return proto->numparams >= 1 && proto->sizelocvars >= 1 && proto->locvars[0].startpc == 0 &&
                   proto->locvars[0].varname != nullptr && Text(proto->locvars[0].varname) == "self";
        }

        int Arg(const Instruction instruction, const int position, const int size)
        {
            return static_cast<int>((instruction >> position) & MASK1(size, 0));
        }

        class Linter
        {
        public:
            Linter(lua_State *state, const int classIndex, const std::set<std::string, std::less<>> &replicated)
                : L(state), classIndex_(classIndex), replicated_(replicated)
            {
            }

            void Callback(const std::string_view name)
            {
                if (PushClassFunction(name))
                {
                    Closure(lua_gettop(L), std::string(name));
                    lua_pop(L, 1);
                }
            }

            std::vector<std::string> TakeFindings() { return std::move(findings_); }

        private:
            void Add(std::string text)
            {
                if (seen_.insert(text).second)
                {
                    findings_.push_back(std::move(text));
                }
            }

            /// Pushes the class's own (raw) field `name` when it is a function; false (nothing pushed) otherwise
            bool PushClassFunction(const std::string_view name)
            {
                if (!lua_checkstack(L, 3))
                {
                    return false;
                }
                lua_pushnil(L);
                while (lua_next(L, classIndex_) != 0)
                {
                    if (lua_type(L, -2) == LUA_TSTRING)
                    {
                        std::size_t length = 0;
                        // A string key is not converted, so lua_next can go on with it
                        const char *const text = lua_tolstring(L, -2, &length);
                        if (std::string_view(text, length) == name && lua_type(L, -1) == LUA_TFUNCTION)
                        {
                            lua_remove(L, -2);
                            return true;
                        }
                    }
                    lua_pop(L, 1);
                }
                return false;
            }

            /// The Lua function at stack index `fn` (absolute): its own code and the inner functions, then the local
            /// functions it captures
            void Closure(const int fn, const std::string &where)
            {
                if (lua_type(L, fn) != LUA_TFUNCTION || lua_iscfunction(L, fn) != 0 || ++visits_ > kMaxVisits)
                {
                    return;
                }
                const auto *const closure = static_cast<const LClosure *>(lua_topointer(L, fn));
                if (closure == nullptr || !closures_.insert(closure).second)
                {
                    return;
                }
                const Proto *const proto = closure->p;
                std::vector<int> map(static_cast<std::size_t>(proto->sizeupvalues));
                std::iota(map.begin(), map.end(), 0);
                Function(proto, proto, map, where, IsMethod(proto));

                for (int index = 1; index <= proto->sizeupvalues; ++index)
                {
                    if (!lua_checkstack(L, 6))
                    {
                        return;
                    }
                    const char *const name = lua_getupvalue(L, fn, index);
                    if (name == nullptr)
                    {
                        return;
                    }
                    const std::string upvalue = name;
                    if (lua_type(L, -1) == LUA_TFUNCTION && lua_iscfunction(L, -1) == 0)
                    {
                        Closure(lua_gettop(L), where + " (through the local function '" + upvalue + "')");
                    }
                    lua_pop(L, 1);
                }
            }

            /// The upvalue of `root` (the closure's prototype) that `map` sends upvalue `index` of the current function
            /// to; -1 for a local of an enclosing function inside the closure (not state the closure keeps)
            static int Map(const std::vector<int> &map, const int index)
            {
                return index >= 0 && static_cast<std::size_t>(index) < map.size() ? map[static_cast<std::size_t>(index)] : -1;
            }

            static std::string UpvalueName(const Proto *const root, const int index)
            {
                return index >= 0 && index < root->sizeupvalues ? Text(root->upvalues[index].name) : std::string("?");
            }

            void Function(const Proto *const proto, const Proto *const root, const std::vector<int> &map,
                          const std::string &where, const bool method)
            {
                if (!protos_.insert(proto).second || ++visits_ > kMaxVisits)
                {
                    return;
                }
                for (int pc = 0; pc < proto->sizecode; ++pc)
                {
                    const Instruction instruction = proto->code[pc];
                    switch (GET_OPCODE(instruction))
                    {
                    case OP_SETUPVAL:
                    {
                        // UpValue[B] := R[A]
                        const int upvalue = Map(map, Arg(instruction, POS_B, SIZE_B));
                        if (upvalue >= 0)
                        {
                            Add(where + " assigns '" + UpvalueName(root, upvalue) +
                                "', a local of the script outside the function (an upvalue)");
                        }
                        break;
                    }
                    case OP_SETTABUP:
                    {
                        // UpValue[A][K[B]:shortstring] := RK(C)
                        const int upvalue = Map(map, Arg(instruction, POS_A, SIZE_A));
                        if (upvalue < 0)
                        {
                            break;
                        }
                        std::string key = "?";
                        (void)ConstantString(proto, Arg(instruction, POS_B, SIZE_B), key);
                        const std::string table = UpvalueName(root, upvalue);
                        if (table == "_ENV")
                        {
                            Add(where + " writes the global '" + key + "'");
                        }
                        else
                        {
                            Add(where + " writes the field '" + key + "' of '" + table +
                                "', a table the script keeps outside the function (an upvalue)");
                        }
                        break;
                    }
                    case OP_SETFIELD:
                    {
                        // R[A][K[B]:shortstring] := RK(C), with R[0] self
                        if (!method || Arg(instruction, POS_A, SIZE_A) != 0)
                        {
                            break;
                        }
                        std::string key;
                        if (ConstantString(proto, Arg(instruction, POS_B, SIZE_B), key) && !replicated_.contains(key) &&
                            key != "gameObject" && key != "component")
                        {
                            Add(where + " writes self." + key + ", which is not a replicated field");
                        }
                        break;
                    }
                    case OP_SELF:
                    {
                        // R[A+1] := R[B]; R[A] := R[B][RK(C):string]: self:Name(...) when B is self and C a constant
                        if (!method || Arg(instruction, POS_B, SIZE_B) != 0 || Arg(instruction, POS_k, 1) == 0)
                        {
                            break;
                        }
                        std::string key;
                        if (ConstantString(proto, Arg(instruction, POS_C, SIZE_C), key) && PushClassFunction(key))
                        {
                            Closure(lua_gettop(L), where + " (through self:" + key + ")");
                            lua_pop(L, 1);
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }
                // The functions defined inside it: an upvalue of one is a local of this function (instack) or one of
                // this function's own upvalues
                for (int index = 0; index < proto->sizep; ++index)
                {
                    const Proto *const inner = proto->p[index];
                    if (inner == nullptr)
                    {
                        continue;
                    }
                    std::vector<int> innerMap(static_cast<std::size_t>(inner->sizeupvalues), -1);
                    for (int upvalue = 0; upvalue < inner->sizeupvalues; ++upvalue)
                    {
                        if (inner->upvalues[upvalue].instack == 0)
                        {
                            innerMap[static_cast<std::size_t>(upvalue)] = Map(map, inner->upvalues[upvalue].idx);
                        }
                    }
                    Function(inner, root, innerMap, where, false);
                }
            }

            lua_State *L;
            int classIndex_;
            const std::set<std::string, std::less<>> &replicated_;
            int visits_ = 0;
            std::unordered_set<const void *> closures_;
            std::unordered_set<const void *> protos_;
            std::unordered_set<std::string> seen_;
            std::vector<std::string> findings_;
        };
    }

    std::vector<std::string> LintTickState(lua_State *L, int classIndex,
                                           const std::set<std::string, std::less<>> &replicatedFields)
    {
        if (L == nullptr || !lua_checkstack(L, 6))
        {
            return {};
        }
        classIndex = lua_absindex(L, classIndex);
        if (lua_type(L, classIndex) != LUA_TTABLE)
        {
            return {};
        }
        const int top = lua_gettop(L);
        Linter linter(L, classIndex, replicatedFields);
        for (const std::string_view callback : kTickCallbacks)
        {
            linter.Callback(callback);
        }
        lua_settop(L, top);
        return linter.TakeFindings();
    }
}
