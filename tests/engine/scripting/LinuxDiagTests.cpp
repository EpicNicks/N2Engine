#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "engine/Layers.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;

// TEMPORARY diagnostics for the Linux leg
TEST(LinuxDiag, Probe)
{
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    sol::state &lua = LuaRuntime::Instance().GetState();
    const char *exprs[] = {
        "type(Layers.MaskOf)",
        "tostring(Layers.MaskOf(8))",
        "type(Layers.MaskOf(8))",
        "select('#', Layers.MaskOf(8))",
        "tostring(Layers.NameToLayer('Water'))",
        "tostring(Layers.GetMask('Water'))",
        "type(Input.GetMousePosition)",
        "type(Input.GetMousePosition())",
        "tostring(Input.GetMouseButton(0))",
        "type(Physics.RaycastAll(Vector3(0,0,0), Vector3(0,0,0)))",
        "select('#', Physics.RaycastAll(Vector3(0,0,0), Vector3(0,0,0)))",
        "type(Physics.Raycast)",
        "type(Vector3(1,2,3))",
    };
    for (const char *e : exprs)
    {
        const std::string code = std::string("return ") + e;
        const sol::protected_function_result r = lua.safe_script(code, sol::script_pass_on_error);
        if (!r.valid())
        {
            const sol::error err = r;
            std::printf("DIAG %s -> ERROR %s\n", e, err.what());
        }
        else
        {
            std::printf("DIAG %s -> [%s] %s\n", e, sol::type_name(lua.lua_state(), r.get_type()).c_str(),
                        r.get_type() == sol::type::string ? r.get<std::string>().c_str() : "");
        }
    }
}
