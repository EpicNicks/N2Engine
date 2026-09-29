#pragma once

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

namespace N2Engine::Scripting
{
    /// Converts a Lua value to JSON. Tables with only keys 1..n become arrays; any other
    /// table (including an empty one) becomes an object with string keys.
    /// Functions and userdata can't be represented and become null.
    nlohmann::json LuaToJson(const sol::object &value);

    /// Builds the JSON that ActionMap::Deserialize expects from a Lua table of
    /// { ["Action Name"] = { binding, binding, ... } }, where each binding is a table such as
    /// { type = "Vector2Composite", up = "W", down = "S", left = "A", right = "D" }.
    nlohmann::json ActionMapJsonFromLua(const sol::table &actions);
}
