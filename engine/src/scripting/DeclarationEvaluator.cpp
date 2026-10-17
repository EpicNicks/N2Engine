#include "engine/scripting/DeclarationEvaluator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>

#include <sol/sol.hpp>

#include "LuaGuard.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/scripting/LuaScript.hpp"

namespace N2Engine::Scripting
{
    namespace
    {
        // Longest field name and declared type kept (a longer name is not a field name a script would write)
        constexpr std::size_t MaxNameBytes = 256;
        // Longest error text kept
        constexpr std::size_t MaxErrorBytes = 200;
        // The smallest cap a state is given (the libraries and the environment take well under it)
        constexpr std::size_t MinCapBytes = std::size_t{1024} * 1024;
        constexpr const char *ReaderKey = "n2.declaration.reader";

        std::atomic<int> g_liveStates{0};

        // The environment of the throwaway state and the function that reads a script's declarations out of it, in Lua.
        // Run once, after the libraries and the budget sandbox (pcall, xpcall and setmetatable of the engine's own
        // sandbox, LuaRuntime.cpp), before the instruction hook is armed, with RefuseLongLoop and MaxTableLoop as its
        // arguments. It returns the reader.
        //
        // - Nothing of the engine exists in this state: no binding, no require, no package, io, os, debug, utf8 or
        //   coroutine library (never opened). Of the base library, the functions that load code, read files, print or
        //   steer the collector are removed, and so are the string functions that take patterns (their cost is C
        //   code the instruction budget cannot see), string.pack/unpack/packsize and math.random (a declaration must
        //   not depend on the clock). The table functions that loop in C are redone in Lua, with a bound on lengths
        //   and ranges (see below and docs/scripting-lua.html, the audit of the C loops).
        // - Anything a script reads that is not there answers ONE inert placeholder (STUB): a table whose metatable
        //   makes it callable, indexable (an integer index answers nil, so ipairs over it ends), usable in arithmetic
        //   and concatenation, and always returning itself; it compares less-than and equal to nothing. Its metatable
        //   is protected (getmetatable answers a string, setmetatable raises). It holds nothing, so a script that
        //   writes to it writes nowhere.
        // - Vector3 is made in Lua: a table tagged in a weak set, with x, y and z. Anything else about it is the stub.
        // - The reader mirrors LuaComponent::DescribeFields on an instance (a table whose metatable is the class the
        //   script returned): the same lookups, the same string-keyed entries, the same classification of a default.
        //   It reads each definition inside pcall, so a metamethod's error leaves that field to its default.
        //   It returns plain tables with no metatable, which the C++ side reads with the raw API.
        // The locals are captured here, so a script that replaces a global (type, next, rawget...) changes nothing.
        constexpr const char *SetupScript = R"LUA(
-- refuse: ends the evaluation as an exhausted budget (RefuseLongLoop); LOOPMAX: DeclarationEvaluator::MaxTableLoop
local refuse, LOOPMAX = ...
local type, rawequal, rawget, next, pcall, setmetatable = type, rawequal, rawget, next, pcall, setmetatable
local select, error = select, error
local mathtype, mtoint, maxint = math.type, math.tointeger, math.maxinteger
local ssub, sfind, srep, sformat = string.sub, string.find, string.rep, string.format
-- The only C table functions still called, and only on plain tables this script filled itself (see below)
local tconcat, tunpack, tsort = table.concat, table.unpack, table.sort
-- The strings' metatable is shared by every string: protected, so a script cannot add __len, __newindex or another
-- __index to it (getmetatable("") answers "protected")
getmetatable("").__metatable = "protected"
local MARK = "\1N2STUB\1"
-- What a failed allocation raises (Lua's preregistered message): a pcall of the reader cannot hide it from the result
local MEMERR = "not enough memory"
local touches = 0

dofile = nil
loadfile = nil
load = nil
collectgarbage = nil
print = nil
warn = nil

local STUB = {}
local function stub()
    touches = touches + 1
    return STUB
end
local function no()
    touches = touches + 1
    return false
end
setmetatable(STUB, {
    __metatable = "protected",
    __index = function(_, key)
        touches = touches + 1
        if type(key) == "number" then
            return nil
        end
        return STUB
    end,
    __newindex = function()
        touches = touches + 1
    end,
    __call = stub, __concat = stub, __unm = stub, __bnot = stub,
    __add = stub, __sub = stub, __mul = stub, __div = stub, __mod = stub, __pow = stub, __idiv = stub,
    __band = stub, __bor = stub, __bxor = stub, __shl = stub, __shr = stub,
    __len = function()
        touches = touches + 1
        return 0
    end,
    __lt = no, __le = no, __eq = no,
    -- tostring and string.format("%s") must answer a string: one that carries a marker the reader recognises, so a
    -- default built from it is unevaluable rather than the text "stub"
    __tostring = function() return MARK end,
})

math.random = STUB
math.randomseed = STUB
string.find = STUB
string.match = STUB
string.gmatch = STUB
string.gsub = STUB
string.dump = STUB
-- string.pack fills padding one byte at a time in C (up to the memory cap); no declaration packs binary data
string.pack = STUB
string.unpack = STUB
string.packsize = STUB

-- The table library loops in C over a length or a range, and one C call runs no Lua instruction: the hook (the
-- instruction budget and the deadline) cannot interrupt it, and a table's length is not bounded by its memory (a
-- border can be huge with a few elements, a __len metamethod can answer anything, and every access can walk a chain
-- of up to 2000 __index tables). So table.insert, remove, concat, unpack, move and sort are redone here in Lua: the
-- length is read once, every element is read and written by Lua code (each access is counted), and the C functions
-- are only handed plain tables this code filled itself (unpack, concat, sort with a Lua comparator). On top of that a
-- length or range above LOOPMAX (2^16, which no declaration needs) is refused before any loop: refuse()
-- ends the evaluation as an exhausted budget, which pcall cannot catch, so a refused call is never silently wrong data.
-- A first argument that is not a table never reaches a C table function: a string would pass its check through the
-- strings' metatable and loop over missing keys in C. insert, remove, concat, move and sort raise the stock type error
-- here; unpack (which takes any value) reads a string's elements in Lua like a table's.

-- An integer argument as luaL_checkinteger reads it (an integer, an integral float, a numeric string), or an error
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
-- The length the C function would read (#t, through __len), read once; refused above LOOPMAX
local function lengthOf(t, fname)
    local len = #t
    local tl = type(len)
    local n = nil
    if tl == "number" or tl == "string" then
        n = mtoint(len)
    end
    if n == nil then
        error("object length is not an integer", 3)
    end
    if n > LOOPMAX then
        refuse(fname)
    end
    return n
end
-- The stock type error of a table argument (no userdata exists here, so only a real table passes)
local function checkTable(v, argn, fname)
    if type(v) ~= "table" then
        error(sformat("bad argument #%d to '%s' (table expected, got %s)", argn, fname, type(v)), 3)
    end
end
-- A range first..last of more than LOOPMAX elements (or one whose size overflows) is refused
local function checkRange(first, last, fname)
    if last >= first and (last - first < 0 or last - first >= LOOPMAX) then
        refuse(fname)
    end
end

table.insert = function(t, ...)
    checkTable(t, 1, "insert")
    local nargs = select("#", ...)
    if nargs ~= 1 and nargs ~= 2 then
        error("wrong number of arguments to 'insert'", 2)
    end
    local e = lengthOf(t, "insert") + 1
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
    local size = lengthOf(t, "remove")
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

table.concat = function(t, sep, i, j)
    checkTable(t, 1, "concat")
    local last = lengthOf(t, "concat")
    local first = 1
    if i ~= nil then
        first = intArg(i, 3, "concat")
    end
    if j ~= nil then
        last = intArg(j, 4, "concat")
    end
    checkRange(first, last, "concat")
    local buf, n = {}, 0
    for k = first, last do
        local v = t[k]
        local tv = type(v)
        if tv ~= "string" and tv ~= "number" then
            error(sformat("invalid value (at index %d) in table for 'concat'", k), 2)
        end
        n = n + 1
        buf[n] = v
    end
    return tconcat(buf, sep, 1, n)
end

table.unpack = function(t, i, j)
    local first = 1
    if i ~= nil then
        first = intArg(i, 2, "unpack")
    end
    local last
    if j ~= nil then
        last = intArg(j, 3, "unpack")
    elseif type(t) == "table" then
        last = lengthOf(t, "unpack")
    else
        -- a string's byte length; any other value raises Lua's own length error here
        last = #t
    end
    if first > last then
        return
    end
    checkRange(first, last, "unpack")
    local buf, n = {}, 0
    for k = first, last do
        n = n + 1
        buf[n] = t[k]
    end
    return tunpack(buf, 1, n)
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
        checkRange(from, to, "move")
        local n = to - from
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

-- The default order as Lua code, so that every comparison the C sort makes runs counted instructions
local function lessThan(a, b)
    return a < b
end
table.sort = function(t, comp)
    checkTable(t, 1, "sort")
    local n = lengthOf(t, "sort")
    if n > 1 then
        if comp ~= nil and type(comp) ~= "function" then
            error(sformat("bad argument #2 to 'sort' (function expected, got %s)", type(comp)), 2)
        end
        -- A script's comparator is called through Lua as well: a C function given as the comparator (string.rep)
        -- would otherwise run once per comparison without a counted instruction
        local order = lessThan
        if comp ~= nil then
            order = function(a, b)
                return comp(a, b)
            end
        end
        -- A plain copy whose length is n even with holes (the C sort reads it through this __len, once)
        local buf = setmetatable({}, { __len = function() return n end })
        for k = 1, n do
            buf[k] = t[k]
        end
        tsort(buf, order)
        for k = 1, n do
            t[k] = buf[k]
        end
    end
end

string.rep = function(s, n, sep)
    if type(s) == "string" and #s == 0 and (sep == nil or (type(sep) == "string" and #sep == 0)) then
        return ""
    end
    return srep(s, n, sep)
end

local isVec = setmetatable({}, { __mode = "k" })
local vecMeta = {
    __metatable = "protected",
    __index = stub,
    __tostring = function() return "Vector3" end,
    __call = stub, __concat = stub, __unm = stub,
    __add = stub, __sub = stub, __mul = stub, __div = stub, __mod = stub, __pow = stub, __idiv = stub,
    __lt = no, __le = no,
}
local function makeVec(x, y, z)
    if x == nil then x = 0 end
    if y == nil then y = 0 end
    if z == nil then z = 0 end
    local v = setmetatable({ x = x, y = y, z = z }, vecMeta)
    isVec[v] = true
    return v
end
Vector3 = setmetatable({ new = function(x, y, z) return makeVec(x, y, z) end }, {
    __metatable = "protected",
    __index = stub,
    __call = function(_, x, y, z) return makeVec(x, y, z) end,
})

setmetatable(_G, { __metatable = "protected", __index = stub })

local function classify(v, maxString)
    local t = type(v)
    if t == "number" then
        if mathtype(v) == "integer" then
            return "int", v
        end
        return "float", v
    elseif t == "boolean" then
        return "bool", v
    elseif t == "string" then
        if #v > maxString then
            return "toolong"
        end
        if sfind(v, "", 1, true) then
            return "stub"
        end
        return "string", v
    elseif t == "table" then
        if rawequal(v, STUB) then
            return "stub"
        end
        if isVec[v] then
            local x, y, z = rawget(v, "x"), rawget(v, "y"), rawget(v, "z")
            if type(x) == "number" and type(y) == "number" and type(z) == "number" then
                return "vec3", x, y, z
            end
            if rawequal(x, STUB) or rawequal(y, STUB) or rawequal(z, STUB) then
                return "stub"
            end
        end
    end
    return "other"
end

local function readDeclarations(class, maxFields, maxString, maxName)
    local result = { fields = {} }
    if type(class) ~= "table" then
        result.notTable = true
        return result
    end
    if rawequal(class, STUB) then
        result.stubClass = true
        return result
    end
    -- A class with __gc is refused when an instance is made (the VM runs a finalizer with its hooks off)
    if rawget(class, "__gc") ~= nil then
        result.hasGc = true
        return result
    end
    local instance = setmetatable({}, class)
    local readable, declarations = pcall(function() return instance.SerializableFields end)
    if not readable then
        if declarations == MEMERR then
            result.memError = true
        end
        result.readError = type(declarations) == "string" and ssub(declarations, 1, 200) or "(an error value)"
        return result
    end
    if rawequal(declarations, STUB) then
        result.stubTable = true
        return result
    end
    if type(declarations) ~= "table" then
        return result
    end
    result.declared = true
    local count = 0
    local key, value = next(declarations)
    while key ~= nil do
        if type(key) == "string" then
            if #key > maxName then
                result.longName = true
            elseif count >= maxFields then
                result.truncated = true
            else
                count = count + 1
                local field = { name = key }
                if rawequal(value, STUB) then
                    field.kind = "stub"
                elseif type(value) == "table" then
                    -- As DescribeFields reads it: a table is a definition, and so is a Vector3 (a userdata there), whose
                    -- `type` and `default` are nothing
                    field.definition = true
                    if isVec[value] then
                        if rawequal(rawget(value, "x"), STUB) or rawequal(rawget(value, "y"), STUB)
                            or rawequal(rawget(value, "z"), STUB) then
                            field.kind = "stub"
                        else
                            field.kind = "other"
                        end
                    else
                        local declaredType, defaultValue
                        local read, readErr = pcall(function()
                            declaredType = value.type
                            defaultValue = value.default
                        end)
                        if not read and readErr == MEMERR then
                            result.memError = true
                        end
                        if type(declaredType) == "string" then
                            if #declaredType <= maxName and sfind(declaredType, "", 1, true) then
                                field.typeStub = true
                            elseif #declaredType <= maxName then
                                field.declaredType = declaredType
                            end
                        elseif rawequal(declaredType, STUB) then
                            field.typeStub = true
                        end
                        if read then
                            field.kind, field.a, field.b, field.c = classify(defaultValue, maxString)
                        else
                            field.kind = "other"
                        end
                    end
                else
                    field.kind, field.a, field.b, field.c = classify(value, maxString)
                end
                result.fields[count] = field
            end
        end
        key, value = next(declarations, key)
    end
    return result
end

return function(class, maxFields, maxString, maxName)
    local result = readDeclarations(class, maxFields, maxString, maxName)
    result.touches = touches
    return result
end
)LUA";

        // What a log line or an error entry may show of text that came from a script: control characters replaced by
        // '?', cut at MaxErrorBytes on a character boundary
        std::string Clean(std::string_view text)
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

        // The error object on top of the stack as text, read without converting it (a string as it is, anything else
        // by its type)
        std::string ErrorObjectText(lua_State *L)
        {
            if (lua_type(L, -1) == LUA_TSTRING)
            {
                std::size_t length = 0;
                const char *text = lua_tolstring(L, -1, &length);
                return Clean(std::string_view(text, length));
            }
            return std::format("(a {} error value)", lua_typename(L, lua_type(L, -1)));
        }

        // The table library's refusal (SetupScript: a length or range above MaxTableLoop): the evaluation
        // ends as an exhausted budget. The message is the budget error's, so the sandbox's pcall and xpcall hand it on,
        // and from here on the hook raises on every instruction, as when the instruction count runs out. Called only
        // by the evaluator's own wrappers, with the name of the table function.
        int RefuseLongLoop(lua_State *L)
        {
            Detail::LuaGuardState *guard = Detail::LuaGuardState::Of(L);
            guard->loopRefused = true;
            guard->exhausted = true;
            guard->step = 1;
            lua_sethook(L, &Detail::LuaGuardState::CountHook, LUA_MASKCOUNT, 1);
            const char *name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : "?";
            return luaL_error(L, "%s (table.%s over more than %d elements)", Detail::BudgetErrorText.data(), name,
                              static_cast<int>(DeclarationEvaluator::MaxTableLoop));
        }

        std::string LoopRefusedText()
        {
            return std::format("A table library call over more than {} elements was refused (one C call, which the "
                               "instruction budget and the time limit cannot interrupt)",
                               DeclarationEvaluator::MaxTableLoop);
        }

        // A failed call of the evaluation, as data. The budget and the memory cap are read from the state's own
        // counters, not from the error object (see docs/scripting-lua.html, the limits: the object can be displaced)
        void ReportFailure(const Detail::LuaGuardState &guard, lua_State *L, const int status, const char *phase,
                           DeclarationResult &result)
        {
            if (guard.loopRefused)
            {
                result.budgetExceeded = true;
                result.errors.push_back(LoopRefusedText());
            }
            else if (guard.timedOut)
            {
                result.budgetExceeded = true;
                result.timedOut = true;
                result.errors.push_back("The evaluation exceeded its time limit");
            }
            else if (guard.exhausted)
            {
                result.budgetExceeded = true;
                result.errors.push_back(std::format("{} of {}", Detail::BudgetErrorText, guard.limit));
            }
            else if (status == LUA_ERRMEM)
            {
                result.memoryExhausted = true;
                result.errors.push_back(std::format("{} (the cap is {} bytes)", Detail::MemoryErrorText, guard.cap));
            }
            else
            {
                result.errors.push_back(std::format("{}: {}", phase, ErrorObjectText(L)));
            }
        }

        bool GetString(lua_State *L, const int table, const char *key, std::string &out)
        {
            lua_getfield(L, table, key);
            const bool isString = lua_type(L, -1) == LUA_TSTRING;
            if (isString)
            {
                std::size_t length = 0;
                const char *text = lua_tolstring(L, -1, &length);
                out.assign(text, length);
            }
            lua_pop(L, 1);
            return isString;
        }

        bool GetFlag(lua_State *L, const int table, const char *key)
        {
            lua_getfield(L, table, key);
            const bool flag = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return flag;
        }

        double GetNumber(lua_State *L, const int table, const char *key)
        {
            lua_getfield(L, table, key);
            const double value = lua_tonumberx(L, -1, nullptr);
            lua_pop(L, 1);
            return value;
        }

        // The reader's plain-table answer, with the raw API (the tables have no metatable, so nothing here runs script
        // code; the state's cap is lifted for it, so the few strings it makes cannot fail)
        void ReadAnswer(lua_State *L, const int answer, const DeclarationLimits &limits, DeclarationResult &result)
        {
            result.stubUses = static_cast<std::uint64_t>(GetNumber(L, answer, "touches"));
            if (GetFlag(L, answer, "memError"))
            {
                // The reader's own pcalls caught a refused allocation: what they read is not trustworthy
                result.memoryExhausted = true;
                result.errors.push_back(std::format("{} while reading the declarations", Detail::MemoryErrorText));
            }
            if (GetFlag(L, answer, "notTable"))
            {
                result.errors.push_back("Script must return a table");
                return;
            }
            if (GetFlag(L, answer, "stubClass"))
            {
                result.unevaluable = true;
                result.unevaluableReason = "the script returns a value that comes from an engine call";
                return;
            }
            if (GetFlag(L, answer, "hasGc"))
            {
                result.errors.push_back("The script defines __gc, which scripts may not use");
                return;
            }
            std::string readError;
            if (GetString(L, answer, "readError", readError))
            {
                result.errors.push_back("Error reading SerializableFields: " + Clean(readError));
                return;
            }
            if (GetFlag(L, answer, "stubTable"))
            {
                result.unevaluable = true;
                result.unevaluableReason = "SerializableFields comes from an engine call";
                return;
            }
            result.declared = GetFlag(L, answer, "declared");
            if (GetFlag(L, answer, "truncated"))
            {
                result.errors.push_back(
                    std::format("SerializableFields has more than {} fields: the rest are ignored", limits.maxFields));
            }
            if (GetFlag(L, answer, "longName"))
            {
                result.errors.push_back(
                    std::format("A field name longer than {} bytes was ignored", MaxNameBytes));
            }

            lua_getfield(L, answer, "fields");
            const int list = lua_gettop(L);
            if (lua_type(L, list) == LUA_TTABLE)
            {
                const auto count = static_cast<lua_Integer>(lua_rawlen(L, list));
                for (lua_Integer i = 1; i <= count; ++i)
                {
                    lua_rawgeti(L, list, i);
                    const int entry = lua_gettop(L);
                    DeclaredField field;
                    if (GetString(L, entry, "name", field.name))
                    {
                        field.isDefinition = GetFlag(L, entry, "definition");
                        GetString(L, entry, "declaredType", field.declaredType);
                        if (GetFlag(L, entry, "typeStub"))
                        {
                            field.unevaluable = true;
                            field.unevaluableReason = "its type comes from an engine call";
                        }
                        std::string kind;
                        GetString(L, entry, "kind", kind);
                        lua_getfield(L, entry, "a");
                        const int first = lua_gettop(L);
                        if (kind == "int" && lua_isinteger(L, first) != 0)
                        {
                            field.defaultKind = LuaDefaultKind::Integer;
                            field.defaultValue = static_cast<std::int64_t>(lua_tointeger(L, first));
                        }
                        else if (kind == "float")
                        {
                            const double number = lua_tonumber(L, first);
                            field.defaultKind = LuaDefaultKind::Float;
                            if (std::isfinite(number))
                            {
                                field.defaultValue = number;
                            }
                            else
                            {
                                field.unevaluable = true;
                                field.unevaluableReason = "its default is not a finite number";
                            }
                        }
                        else if (kind == "bool")
                        {
                            field.defaultKind = LuaDefaultKind::Bool;
                            field.defaultValue = lua_toboolean(L, first) != 0;
                        }
                        else if (kind == "string" && lua_type(L, first) == LUA_TSTRING)
                        {
                            std::size_t length = 0;
                            const char *text = lua_tolstring(L, first, &length);
                            field.defaultKind = LuaDefaultKind::String;
                            field.defaultValue = std::string(text, length);
                        }
                        else if (kind == "vec3")
                        {
                            // As ExtractSerializableFields stores a Vector3: its float components
                            const double x = lua_tonumber(L, first);
                            const double y = GetNumber(L, entry, "b");
                            const double z = GetNumber(L, entry, "c");
                            field.defaultKind = LuaDefaultKind::Vector3;
                            const auto fx = static_cast<float>(x);
                            const auto fy = static_cast<float>(y);
                            const auto fz = static_cast<float>(z);
                            if (std::isfinite(fx) && std::isfinite(fy) && std::isfinite(fz))
                            {
                                field.defaultValue = {{"x", fx}, {"y", fy}, {"z", fz}};
                            }
                            else
                            {
                                field.unevaluable = true;
                                field.unevaluableReason = "its default has a component that is not a finite number";
                            }
                        }
                        else if (kind == "stub")
                        {
                            field.unevaluable = true;
                            field.unevaluableReason = "its default comes from an engine call";
                        }
                        else if (kind == "toolong")
                        {
                            field.unevaluable = true;
                            field.unevaluableReason =
                                std::format("its default is a string longer than {} bytes", limits.maxStringBytes);
                        }
                        lua_pop(L, 1); // "a"
                        result.fields.push_back(std::move(field));
                    }
                    lua_settop(L, list);
                }
            }
            lua_settop(L, answer);
            std::ranges::sort(result.fields, {}, &DeclaredField::name);
        }

        // The whole evaluation in a state of its own. Everything of it lives in this function: the state is closed when
        // it returns or throws, before the caller reads the guard's counters.
        void Run(Detail::LuaGuardState &guard, const std::string_view source, const std::string &chunkName,
                 const DeclarationLimits &limits, DeclarationResult &result)
        {
            sol::state lua(&Detail::LuaGuardState::Panic, &Detail::LuaCappedAlloc, &guard);
            lua_State *L = lua.lua_state();
            guard.state = L;
            lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
            Detail::InstallBudgetSandbox(lua);

            // The environment and the reader, before the budget is armed (they are the evaluator's own code)
            int status = luaL_loadbufferx(L, SetupScript, std::strlen(SetupScript), "=declaration-setup", "t");
            if (status == LUA_OK)
            {
                lua_pushcfunction(L, &RefuseLongLoop);
                lua_pushinteger(L, static_cast<lua_Integer>(DeclarationEvaluator::MaxTableLoop));
                status = lua_pcall(L, 2, 1, 0);
            }
            if (status != LUA_OK)
            {
                ReportFailure(guard, L, status, "The declaration environment could not be set up", result);
                return;
            }
            lua_setfield(L, LUA_REGISTRYINDEX, ReaderKey);

            // From here on every instruction counts, for the chunk and for the reading alike (one entry), and so does
            // the wall clock
            const std::uint64_t millis = limits.maxMillis == 0 ? DeclarationLimits{}.maxMillis : limits.maxMillis;
            guard.hasDeadline = true;
            guard.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(millis);
            guard.Arm();

            const std::string name = chunkName.size() > 100 ? chunkName.substr(0, 100) : chunkName;
            // Text only: a precompiled chunk is refused, as everywhere in the engine
            status = luaL_loadbufferx(L, source.data(), source.size(), name.c_str(), "t");
            if (status == LUA_OK)
            {
                status = lua_pcall(L, 0, 1, 0);
            }
            if (status != LUA_OK)
            {
                ReportFailure(guard, L, status, "Error running the script", result);
                return;
            }

            // [the chunk's first return value] -> [reader, value, limits...] -> [answer]
            lua_getfield(L, LUA_REGISTRYINDEX, ReaderKey);
            lua_insert(L, -2);
            lua_pushinteger(L, static_cast<lua_Integer>(limits.maxFields));
            lua_pushinteger(L, static_cast<lua_Integer>(limits.maxStringBytes));
            lua_pushinteger(L, static_cast<lua_Integer>(MaxNameBytes));
            status = lua_pcall(L, 4, 1, 0);
            if (status != LUA_OK)
            {
                ReportFailure(guard, L, status, "Error reading the declarations", result);
                return;
            }

            // The answer is plain data; reading it makes a few small strings. The cap is for the script's own work.
            guard.cap = 0;
            ReadAnswer(L, lua_gettop(L), limits, result);
        }
    }

    bool DeclarationResult::Complete() const
    {
        return errors.empty() && !budgetExceeded && !memoryExhausted && !unevaluable && !AnyFieldUnevaluable();
    }

    bool DeclarationResult::AnyFieldUnevaluable() const
    {
        return std::ranges::any_of(fields, &DeclaredField::unevaluable);
    }

    std::vector<FieldInfo> DeclarationResult::ToFieldInfos() const
    {
        std::vector<FieldInfo> infos;
        infos.reserve(fields.size());
        for (const DeclaredField &field : fields)
        {
            infos.push_back(MakeLuaFieldInfo(field.name, field.declaredType, field.defaultKind));
        }
        return infos;
    }

    nlohmann::json DeclarationResult::DefaultValues() const
    {
        nlohmann::json values = nlohmann::json::object();
        for (const DeclaredField &field : fields)
        {
            if (!field.defaultValue.is_null())
            {
                values[field.name] = field.defaultValue;
            }
        }
        return values;
    }

    int DeclarationEvaluator::LiveStates()
    {
        return g_liveStates.load();
    }

    DeclarationResult DeclarationEvaluator::Evaluate(const std::string_view source, const std::string &chunkName,
                                                      const DeclarationLimits &limits)
    {
        DeclarationResult result;
        if (source.size() > limits.maxSourceBytes)
        {
            result.errors.push_back(
                std::format("The script is {} bytes; the limit is {}", source.size(), limits.maxSourceBytes));
            return result;
        }

        // Before the state and outliving it: its allocator and its hook use it until the state is closed
        auto guard = std::make_unique<Detail::LuaGuardState>();
        // 0 is "unlimited" for the runtime's tools, never here; a state needs about 100 KiB to exist at all
        guard->cap = limits.maxBytes == 0 ? DeclarationLimits{}.maxBytes : std::max(limits.maxBytes, MinCapBytes);
        guard->limit = limits.instructions == 0 ? DeclarationLimits{}.instructions : limits.instructions;
        g_liveStates.fetch_add(1);
        try
        {
            Run(*guard, source, chunkName, limits, result);
        }
        catch (const std::exception &e)
        {
            // An error that reached the panic function (an allocation failing outside a protected call) or a C++ error:
            // data like any other. The state was closed as the exception left Run.
            const std::string text = Clean(e.what());
            if (guard->loopRefused)
            {
                result.budgetExceeded = true;
                result.errors.push_back(LoopRefusedText());
            }
            else if (guard->timedOut)
            {
                result.budgetExceeded = true;
                result.timedOut = true;
                result.errors.push_back("The evaluation exceeded its time limit");
            }
            else if (guard->exhausted)
            {
                result.budgetExceeded = true;
                result.errors.push_back(std::format("{} of {}", Detail::BudgetErrorText, guard->limit));
            }
            else
            {
                if (text.find(Detail::MemoryErrorText) != std::string::npos)
                {
                    result.memoryExhausted = true;
                }
                result.errors.push_back("The evaluation failed: " + text);
            }
        }
        catch (...)
        {
            result.errors.push_back("The evaluation failed");
        }
        result.peakBytes = guard->peak;
        result.leakedBytes = guard->used;
        g_liveStates.fetch_sub(1);
        return result;
    }

    DeclarationResult DeclarationEvaluator::EvaluateScript(const IO::ResourcePath &path, const DeclarationLimits &limits)
    {
        const auto script = IO::ResourceLoader::Instance().Load<LuaScript>(path);
        if (!script)
        {
            DeclarationResult result;
            result.errors.push_back("Lua script not found: " + Clean(path.ToString()));
            return result;
        }
        return Evaluate(script->GetSourceCode(), path.ToString(), limits);
    }
}
