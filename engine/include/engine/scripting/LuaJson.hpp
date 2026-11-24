#pragma once

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <string_view>

namespace N2Engine::Scripting
{
    /// Converts a Lua value to JSON. Tables with only keys 1..n become arrays; any other
    /// table (including an empty one) becomes an object with string keys.
    /// Functions and userdata can't be represented and become null. A Lua integer becomes a JSON integer and a Lua
    /// float a JSON float (a whole float is written as "3.0"), so the number subtype survives a round trip; NaN and
    /// infinities become null.
    nlohmann::json LuaToJson(const sol::object &value);

    /// How JsonToLua turns JSON numbers into Lua numbers
    enum class JsonNumbers
    {
        Auto,  ///< integers (no fraction, no exponent) as Lua integers, other numbers as Lua floats
        Float, ///< every number as a Lua float, so arithmetic on it never wraps
    };

    /// Converts JSON to a Lua value: arrays as 1-based tables, objects as tables with string keys, integers as Lua
    /// integers (or floats with JsonNumbers::Float), null as nil. Nesting deeper than maxDepth throws
    /// std::runtime_error("<what>: nesting deeper than N").
    sol::object JsonToLua(const nlohmann::json &value, sol::state_view lua, int maxDepth, std::string_view what,
                          JsonNumbers numbers = JsonNumbers::Auto);

    /// Builds the JSON that ActionMap::Deserialize expects from a Lua table of
    /// { ["Action Name"] = { binding, binding, ... } }, where each binding is a table such as
    /// { type = "Vector2Composite", up = "W", down = "S", left = "A", right = "D" }.
    nlohmann::json ActionMapJsonFromLua(const sol::table &actions);
}
