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

    lua.set_function("first", [](int v) { return v + 1; });
    lua.set_function("second", [](int v) { return v * 10; });
    lua.set_function("third", [](int v) { return v - 7; });

    sol::table t = lua.create_table();
    t.set_function("a", [](int v) { return v + 100; });
    t.set_function("b", [](int v) { return v + 200; });
    lua["t"] = t;

    EXPECT_EQ(lua.safe_script("return first(5)").get<int>(), 6);
    EXPECT_EQ(lua.safe_script("return second(5)").get<int>(), 50);
    EXPECT_EQ(lua.safe_script("return third(5)").get<int>(), -2);
    EXPECT_EQ(lua.safe_script("return t.a(1)").get<int>(), 101);
    EXPECT_EQ(lua.safe_script("return t.b(1)").get<int>(), 201);
    // Interleaved calls after all are bound
    EXPECT_EQ(lua.safe_script("return first(1) + second(1) + third(1) + t.a(0) + t.b(0)").get<int>(), 2 + 10 - 6 + 100 + 200);
}
