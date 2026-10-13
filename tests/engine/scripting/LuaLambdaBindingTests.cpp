#include <gtest/gtest.h>

#include <string>

#include "engine/scripting/LuaRuntime.hpp"

// Regression guard for the vendored sol2 type-name path (see the "N2Engine local patch" in external/sol2/include/sol/sol.hpp).
// Under GCC every lambda of one signature inside one function printed as the same name, so they shared a metatable and Lua
// called the wrong one. Each lambda must run its own body on every compiler.
TEST(LuaLambdaBindingTest, SameSignatureLambdasInOneFunctionKeepTheirOwnBodies)
{
    sol::state lua;
    lua.open_libraries(sol::lib::base);

    // Captureless lambdas given to set_function become plain function pointers in sol2 and never reach the metatable-name
    // path, so the lambdas are pushed as values (create_table_with / table.set), the path the engine bindings use.
    lua["m"] = lua.create_table_with(
        "first", [](int v) { return v + 1; },
        "second", [](int v) { return v * 10; },
        "third", [](int v) { return v - 7; });

    sol::table t = lua.create_table();
    t.set("a", [](int v) { return v + 100; });
    t.set("b", [](int v) { return v + 200; });
    lua["t"] = t;

    const int bias = 3;
    lua["cap1"] = [bias](int v) { return v + bias; };
    lua["cap2"] = [bias](int v) { return v * bias; };

    EXPECT_EQ(lua.safe_script("return m.first(5)").get<int>(), 6);
    EXPECT_EQ(lua.safe_script("return m.second(5)").get<int>(), 50);
    EXPECT_EQ(lua.safe_script("return m.third(5)").get<int>(), -2);
    EXPECT_EQ(lua.safe_script("return t.a(1)").get<int>(), 101);
    EXPECT_EQ(lua.safe_script("return t.b(1)").get<int>(), 201);
    // Interleaved calls after all are bound
    EXPECT_EQ(lua.safe_script("return m.first(1) + m.second(1) + m.third(1) + t.a(0) + t.b(0)").get<int>(), 2 + 10 - 6 + 100 + 200);
    EXPECT_EQ(lua.safe_script("return cap1(2)").get<int>(), 5);
    EXPECT_EQ(lua.safe_script("return cap2(2)").get<int>(), 6);
}
