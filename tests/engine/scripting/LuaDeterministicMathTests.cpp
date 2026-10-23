#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <math/DeterministicMath.hpp>

#include "engine/scripting/LuaDeterministic.hpp"
#include "engine/scripting/LuaRuntime.hpp"

// Netcode C1, Lua side: the engine-owned math.sin/cos/tan/asin/acos/atan/exp/log (installed when determinism.luaSeed is set)
// and Ordered.pairs (always there).

using namespace N2Engine;
using namespace N2Engine::Scripting;

namespace
{
    class LuaSeedScope
    {
    public:
        explicit LuaSeedScope(const std::uint64_t seed) { LuaRuntime::SetLuaSeed(seed); }
        ~LuaSeedScope() { LuaRuntime::SetLuaSeed(std::nullopt); }
        LuaSeedScope(const LuaSeedScope &) = delete;
        LuaSeedScope &operator=(const LuaSeedScope &) = delete;
    };

    std::unique_ptr<sol::state> MakeState()
    {
        auto lua = std::make_unique<sol::state>();
        lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
        return lua;
    }

    std::uint64_t Bits(const double value)
    {
        return std::bit_cast<std::uint64_t>(value);
    }

    double Number(sol::state &lua, const std::string &code)
    {
        const sol::protected_function_result result = lua.safe_script("return " + code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            ADD_FAILURE() << code << ": " << err.what();
            return 0.0;
        }
        return result.get<double>();
    }

    std::string Text(sol::state &lua, const std::string &code)
    {
        const sol::protected_function_result result = lua.safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            return std::string("ERROR: ") + err.what();
        }
        return result.get<std::string>();
    }

    lua_CFunction MathFunction(sol::state &lua, const char *name)
    {
        lua_State *L = lua.lua_state();
        lua_getglobal(L, "math");
        lua_getfield(L, -1, name);
        const lua_CFunction function = lua_tocfunction(L, -1);
        lua_pop(L, 2);
        return function;
    }
}

TEST(LuaDeterministicMathTest, TheShimsAreTheEngineOwnedFunctionsBitForBit)
{
    const auto lua = MakeState();
    InstallDeterministicMath(*lua);
    for (const double x : {0.1, 0.5, 1.0, 2.0, 3.141592653589793, 10.0, 100.5, -0.7, -3.0, 12345.678, 1e-5})
    {
        const std::string arg = std::to_string(x);
        const double parsed = std::stod(arg);
        EXPECT_EQ(Bits(Number(*lua, "math.sin(" + arg + ")")), Bits(Math::Sin(parsed)));
        EXPECT_EQ(Bits(Number(*lua, "math.cos(" + arg + ")")), Bits(Math::Cos(parsed)));
        EXPECT_EQ(Bits(Number(*lua, "math.tan(" + arg + ")")), Bits(Math::Tan(parsed)));
        EXPECT_EQ(Bits(Number(*lua, "math.atan(" + arg + ")")), Bits(Math::Atan2(parsed, 1.0)));
        EXPECT_EQ(Bits(Number(*lua, "math.exp(" + arg + ")")), Bits(Math::Exp(parsed)));
        if (x > 0.0)
        {
            EXPECT_EQ(Bits(Number(*lua, "math.log(" + arg + ")")), Bits(Math::Log(parsed)));
            EXPECT_EQ(Bits(Number(*lua, "math.log(" + arg + ", 2)")), Bits(Math::Log2(parsed)));
            EXPECT_EQ(Bits(Number(*lua, "math.log(" + arg + ", 10)")), Bits(Math::Log(parsed) / Math::Log(10.0)));
            EXPECT_EQ(Bits(Number(*lua, "math.log(" + arg + ", 3)")), Bits(Math::Log(parsed) / Math::Log(3.0)));
        }
    }
    EXPECT_EQ(Bits(Number(*lua, "math.asin(0.3)")), Bits(Math::Asin(0.3)));
    EXPECT_EQ(Bits(Number(*lua, "math.acos(-0.6)")), Bits(Math::Acos(-0.6)));
    EXPECT_EQ(Bits(Number(*lua, "math.atan(2, -3)")), Bits(Math::Atan2(2.0, -3.0)));
    EXPECT_EQ(Number(*lua, "math.log(8, 2)"), 3.0);
}

TEST(LuaDeterministicMathTest, TheShimsKeepLuasArgumentRules)
{
    const auto lua = MakeState();
    InstallDeterministicMath(*lua);
    // A numeric string is accepted, as luaL_checknumber does; a table is an error naming the function
    EXPECT_EQ(Bits(Number(*lua, "math.sin('0.5')")), Bits(Math::Sin(0.5)));
    EXPECT_NE(Text(*lua, "return math.sin({})").find("ERROR"), std::string::npos);
    EXPECT_NE(Text(*lua, "return math.atan()").find("ERROR"), std::string::npos);
    // Integers are converted to floats; NaN comes out as the canonical NaN
    EXPECT_EQ(Bits(Number(*lua, "math.sin(1)")), Bits(Math::Sin(1.0)));
    EXPECT_EQ(Text(*lua, "return tostring(math.sin(math.huge) ~= math.sin(math.huge))"), "true");
    EXPECT_EQ(Text(*lua, "return math.type(math.exp(0))"), "float");
}

TEST(LuaDeterministicMathTest, TheShimsReplaceOnlyWhatTheyName)
{
    const auto stock = MakeState();
    const auto shimmed = MakeState();
    InstallDeterministicMath(*shimmed);
    for (const char *name : {"sin", "cos", "tan", "asin", "acos", "atan", "exp", "log"})
    {
        EXPECT_NE(MathFunction(*stock, name), MathFunction(*shimmed, name)) << name;
    }
    for (const char *name : {"sqrt", "floor", "ceil", "fmod", "abs", "max", "min", "tointeger", "random"})
    {
        EXPECT_EQ(MathFunction(*stock, name), MathFunction(*shimmed, name)) << name;
    }
    EXPECT_EQ(Number(*shimmed, "math.pi"), Number(*stock, "math.pi"));
}

// The gate: the same switch as the seeded hash and math.random
TEST(LuaDeterministicMathTest, TheShimsAreInstalledExactlyWhenTheLuaSeedIsSet)
{
    const auto stock = MakeState();
    {
        const auto unset = MakeState();
        EXPECT_FALSE(InstallDeterministicMathIfSeeded(*unset));
        EXPECT_EQ(MathFunction(*unset, "sin"), MathFunction(*stock, "sin"));
        EXPECT_EQ(MathFunction(*unset, "log"), MathFunction(*stock, "log"));
    }
    {
        const LuaSeedScope scope(99);
        const auto seeded = MakeState();
        EXPECT_TRUE(InstallDeterministicMathIfSeeded(*seeded));
        EXPECT_NE(MathFunction(*seeded, "sin"), MathFunction(*stock, "sin"));
        EXPECT_EQ(Bits(Number(*seeded, "math.cos(1.25)")), Bits(Math::Cos(1.25)));
    }
    const auto after = MakeState();
    EXPECT_FALSE(InstallDeterministicMathIfSeeded(*after));
}

TEST(LuaDeterministicMathTest, TheEngineRuntimeFollowsTheSeedItWasCreatedWith)
{
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    const LuaRuntime &runtime = LuaRuntime::Instance();
    EXPECT_EQ(runtime.HasDeterministicMath(), runtime.StateLuaSeed().has_value());
    const auto stock = MakeState();
    sol::state &state = LuaRuntime::Instance().GetState();
    if (!runtime.HasDeterministicMath())
    {
        EXPECT_EQ(MathFunction(state, "sin"), MathFunction(*stock, "sin"));
    }
    else
    {
        EXPECT_NE(MathFunction(state, "sin"), MathFunction(*stock, "sin"));
    }
    // Ordered.pairs is in the engine's state either way
    EXPECT_EQ(Text(state, "return type(Ordered.pairs)"), "function");
}

// -- Ordered.pairs -----------------------------------------------------------------------------------------------------

namespace
{
    std::string Order(sol::state &lua, const std::string &tableConstructor)
    {
        return Text(lua, "local t = " + tableConstructor + R"(
            local out = {}
            for k, v in Ordered.pairs(t) do out[#out + 1] = tostring(k) .. "=" .. tostring(v) end
            return table.concat(out, ","))");
    }
}

TEST(OrderedPairsTest, TheArrayPartComesFirstInOrder)
{
    const auto lua = MakeState();
    InstallOrderedPairs(*lua);
    EXPECT_EQ(Order(*lua, "{ 'a', 'b', 'c' }"), "1=a,2=b,3=c");
    // The array part stops at the first hole; the rest follow, numbers ascending
    EXPECT_EQ(Order(*lua, "{ [1] = 'a', [2] = 'b', [4] = 'd', [3.5] = 'x', [-2] = 'n', [0] = 'z' }"),
              "1=a,2=b,-2=n,0=z,3.5=x,4=d");
}

TEST(OrderedPairsTest, OtherKeysAreGroupedByTypeThenSortedWithinTheGroup)
{
    const auto lua = MakeState();
    InstallOrderedPairs(*lua);
    EXPECT_EQ(Order(*lua, "{ 'x', 'y', zebra = 1, apple = 2, mango = 3, [true] = 'T', [false] = 'F', [10] = 'ten', [5] = 'five' }"),
              "1=x,2=y,5=five,10=ten,apple=2,mango=3,zebra=1,false=F,true=T");
    // Strings compare by bytes: uppercase before lowercase, a prefix before the longer string
    EXPECT_EQ(Order(*lua, "{ b = 1, B = 2, a = 3, ab = 4, [''] = 5 }"), "=5,B=2,a=3,ab=4,b=1");
}

TEST(OrderedPairsTest, TheOrderDoesNotDependOnHowTheTableWasBuilt)
{
    const auto lua = MakeState();
    InstallOrderedPairs(*lua);
    const std::string script = R"(
        local function order(keys)
            local t = {}
            for _, k in ipairs(keys) do t[k] = true end
            local out = {}
            for k in Ordered.pairs(t) do out[#out + 1] = tostring(k) end
            return table.concat(out, ",")
        end
        local forward, backward = {}, {}
        for i = 1, 200 do forward[i] = "key_" .. ((i * 7919) % 1009) end
        for i = 200, 1, -1 do backward[#backward + 1] = forward[i] end
        return order(forward) == order(backward) and "same" or "different"
    )";
    EXPECT_EQ(Text(*lua, script), "same");
}

TEST(OrderedPairsTest, AKeyWithNoOrderIsAnErrorNamingItsType)
{
    const auto lua = MakeState();
    InstallOrderedPairs(*lua);
    const std::string tableKey = Text(*lua, "for k in Ordered.pairs({ [{}] = 1 }) do end return 'no error'");
    EXPECT_NE(tableKey.find("ERROR"), std::string::npos);
    EXPECT_NE(tableKey.find("table"), std::string::npos);
    EXPECT_NE(tableKey.find("Ordered.pairs"), std::string::npos);
    const std::string functionKey = Text(*lua, "for k in Ordered.pairs({ [print] = 1 }) do end return 'no error'");
    EXPECT_NE(functionKey.find("function"), std::string::npos);
    EXPECT_NE(Text(*lua, "Ordered.pairs(42)").find("expected a table, got number"), std::string::npos);
}

TEST(OrderedPairsTest, ValuesAreReadAsTheLoopReachesThemAndRemovedKeysAreSkipped)
{
    const auto lua = MakeState();
    InstallOrderedPairs(*lua);
    const std::string script = R"(
        local t = { a = 1, b = 2, c = 3 }
        local seen = {}
        for k, v in Ordered.pairs(t) do
            seen[#seen + 1] = k .. v
            if k == "a" then t.b = nil; t.c = 30; t.d = 4 end
        end
        return table.concat(seen, ",")
    )";
    EXPECT_EQ(Text(*lua, script), "a1,c30");
    // A false value is a value; the metatable's __index is not consulted
    EXPECT_EQ(Order(*lua, "setmetatable({ x = false }, { __index = function() return 1 end })"), "x=false");
}

TEST(OrderedPairsTest, AnEmptyTableLoopsZeroTimes)
{
    const auto lua = MakeState();
    InstallOrderedPairs(*lua);
    EXPECT_EQ(Order(*lua, "{}"), "");
}
