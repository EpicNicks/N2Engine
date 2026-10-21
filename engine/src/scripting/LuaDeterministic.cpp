#include "engine/scripting/LuaDeterministic.hpp"

#include <type_traits>

#include <math/DeterministicMath.hpp>

#include "engine/scripting/LuaRuntime.hpp"

namespace N2Engine::Scripting
{
    namespace
    {
        static_assert(std::is_same_v<lua_Number, double>, "the Lua shims are written for double-precision Lua numbers");

        int LuaSin(lua_State *L)
        {
            lua_pushnumber(L, Math::Sin(luaL_checknumber(L, 1)));
            return 1;
        }

        int LuaCos(lua_State *L)
        {
            lua_pushnumber(L, Math::Cos(luaL_checknumber(L, 1)));
            return 1;
        }

        int LuaTan(lua_State *L)
        {
            lua_pushnumber(L, Math::Tan(luaL_checknumber(L, 1)));
            return 1;
        }

        int LuaAsin(lua_State *L)
        {
            lua_pushnumber(L, Math::Asin(luaL_checknumber(L, 1)));
            return 1;
        }

        int LuaAcos(lua_State *L)
        {
            lua_pushnumber(L, Math::Acos(luaL_checknumber(L, 1)));
            return 1;
        }

        // math.atan(y [, x]): x defaults to 1, as in Lua 5.4
        int LuaAtan(lua_State *L)
        {
            const lua_Number y = luaL_checknumber(L, 1);
            const lua_Number x = luaL_optnumber(L, 2, 1.0);
            lua_pushnumber(L, Math::Atan2(y, x));
            return 1;
        }

        int LuaExp(lua_State *L)
        {
            lua_pushnumber(L, Math::Exp(luaL_checknumber(L, 1)));
            return 1;
        }

        // math.log(x [, base]): the natural logarithm, Log2 for base 2, else Log(x) / Log(base)
        int LuaLog(lua_State *L)
        {
            const lua_Number x = luaL_checknumber(L, 1);
            lua_Number result;
            if (lua_isnoneornil(L, 2))
            {
                result = Math::Log(x);
            }
            else
            {
                const lua_Number base = luaL_checknumber(L, 2);
                result = base == 2.0 ? Math::Log2(x) : Math::Log(x) / Math::Log(base);
            }
            lua_pushnumber(L, result);
            return 1;
        }

        struct Replacement
        {
            const char *name;
            lua_CFunction function;
        };

        constexpr Replacement kReplacements[] = {
            {"sin", LuaSin}, {"cos", LuaCos}, {"tan", LuaTan}, {"asin", LuaAsin},
            {"acos", LuaAcos}, {"atan", LuaAtan}, {"exp", LuaExp}, {"log", LuaLog},
        };

        // Ordered.pairs, in Lua: the array part, then the other keys by type and value. The locals are captured at load so a
        // script that reassigns type or next later cannot break it. (The string order is Lua's own "<", which compares
        // bytes in the "C" locale, the one the engine never leaves.)
        constexpr const char *kOrderedPairsSource = R"lua(
            local type, next, rawget, error = type, next, rawget, error
            local sort = table.sort
            local mathType = math.type
            local rank = { number = 1, string = 2, boolean = 3 }

            local function before(a, b)
                local rankA, rankB = rank[type(a)], rank[type(b)]
                if rankA ~= rankB then
                    return rankA < rankB
                end
                if rankA == 3 then
                    return b == true and a == false
                end
                return a < b
            end

            Ordered = Ordered or {}

            function Ordered.pairs(t)
                if type(t) ~= "table" then
                    error("Ordered.pairs: expected a table, got " .. type(t), 2)
                end
                local keys = {}
                local count = 0
                while rawget(t, count + 1) ~= nil do
                    count = count + 1
                    keys[count] = count
                end
                local rest = {}
                for key in next, t do
                    local keyType = type(key)
                    if rank[keyType] == nil then
                        error("Ordered.pairs: a key of type " .. keyType .. " has no order (keys must be numbers, strings or booleans)", 2)
                    end
                    if not (keyType == "number" and mathType(key) == "integer" and key >= 1 and key <= count) then
                        rest[#rest + 1] = key
                    end
                end
                sort(rest, before)
                for i = 1, #rest do
                    keys[count + i] = rest[i]
                end
                local index = 0
                return function()
                    while true do
                        index = index + 1
                        local key = keys[index]
                        if key == nil then
                            return nil
                        end
                        local value = rawget(t, key)
                        if value ~= nil then
                            return key, value
                        end
                    end
                end, t, nil
            end
        )lua";
    }

    void InstallDeterministicMath(sol::state_view lua)
    {
        lua_State *L = lua.lua_state();
        lua_getglobal(L, "math");
        if (lua_istable(L, -1))
        {
            for (const Replacement &replacement : kReplacements)
            {
                lua_pushcfunction(L, replacement.function);
                lua_setfield(L, -2, replacement.name);
            }
        }
        lua_pop(L, 1);
    }

    bool InstallDeterministicMathIfSeeded(sol::state_view lua)
    {
        if (!LuaRuntime::GetLuaSeed().has_value())
        {
            return false;
        }
        InstallDeterministicMath(lua);
        return true;
    }

    void InstallOrderedPairs(sol::state_view lua)
    {
        lua.script(kOrderedPairsSource);
    }
}
