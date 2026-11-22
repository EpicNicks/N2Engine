#include "engine/scripting/LuaJson.hpp"

#include <cmath>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace N2Engine::Scripting
{
    namespace
    {
        // Tables currently being converted: a table that (directly or indirectly) contains itself
        // becomes null at the point it recurs, instead of recursing until the stack overflows
        thread_local std::unordered_set<const void *> t_tablesInProgress;

        nlohmann::json NumberToJson(const double number)
        {
            // Lua scripts write whole numbers for counts and ids; keep them integral in JSON
            constexpr double MaxExactInteger = 9007199254740992.0; // 2^53
            if (std::trunc(number) == number && std::abs(number) < MaxExactInteger)
            {
                return static_cast<std::int64_t>(number);
            }
            return number;
        }

        nlohmann::json TableToJson(const sol::table &table)
        {
            const void *identity = table.pointer();
            if (!t_tablesInProgress.insert(identity).second)
            {
                return nullptr; // cycle
            }
            struct Leave
            {
                const void *id;
                ~Leave() { t_tablesInProgress.erase(id); }
            } leave{identity};

            std::size_t keyCount = 0;
            bool sequentialKeys = true;
            for (const auto &[key, value] : table)
            {
                ++keyCount;
                if (key.get_type() != sol::type::number)
                {
                    sequentialKeys = false;
                }
            }

            // A table is a sequence when its keys are exactly 1..n
            if (keyCount > 0 && sequentialKeys && table.size() == keyCount)
            {
                nlohmann::json array = nlohmann::json::array();
                for (std::size_t i = 1; i <= keyCount; ++i)
                {
                    array.push_back(LuaToJson(table.get<sol::object>(i)));
                }
                return array;
            }

            nlohmann::json object = nlohmann::json::object();
            for (const auto &[key, value] : table)
            {
                const std::string name = key.get_type() == sol::type::string
                                             ? key.as<std::string>()
                                             : LuaToJson(key).dump();
                object[name] = LuaToJson(value);
            }
            return object;
        }
    }

    nlohmann::json LuaToJson(const sol::object &value)
    {
        switch (value.get_type())
        {
        case sol::type::boolean:
            return value.as<bool>();
        case sol::type::number:
            return NumberToJson(value.as<double>());
        case sol::type::string:
            return value.as<std::string>();
        case sol::type::table:
            return TableToJson(value.as<sol::table>());
        default:
            return nullptr;
        }
    }

    nlohmann::json ActionMapJsonFromLua(const sol::table &actions)
    {
        nlohmann::json actionsJson = nlohmann::json::object();
        for (const auto &[key, value] : actions)
        {
            if (key.get_type() != sol::type::string || value.get_type() != sol::type::table)
            {
                continue; // Not an { ["Action Name"] = { bindings } } entry
            }

            // By index rather than pairs(), so bindings keep the order they were written in
            const sol::table bindingList = value.as<sol::table>();
            nlohmann::json bindings = nlohmann::json::array();
            for (std::size_t i = 1; i <= bindingList.size(); ++i)
            {
                bindings.push_back(LuaToJson(bindingList.get<sol::object>(i)));
            }
            actionsJson[key.as<std::string>()] = {{"bindings", bindings}};
        }
        return {{"actions", actionsJson}};
    }

    namespace
    {
        sol::object JsonToLuaAt(const nlohmann::json &value, sol::state_view lua, const int depth, const int maxDepth,
                                const std::string_view what)
        {
            if (depth > maxDepth)
            {
                throw std::runtime_error(std::format("{}: nesting deeper than {}", what, maxDepth));
            }
            switch (value.type())
            {
            case nlohmann::json::value_t::boolean:
                return sol::make_object(lua, value.get<bool>());
            case nlohmann::json::value_t::number_integer:
                return sol::make_object(lua, value.get<std::int64_t>());
            case nlohmann::json::value_t::number_unsigned:
            {
                const auto u = value.get<std::uint64_t>();
                if (u <= static_cast<std::uint64_t>(INT64_MAX))
                {
                    return sol::make_object(lua, static_cast<std::int64_t>(u));
                }
                return sol::make_object(lua, static_cast<double>(u));
            }
            case nlohmann::json::value_t::number_float:
                return sol::make_object(lua, value.get<double>());
            case nlohmann::json::value_t::string:
                return sol::make_object(lua, value.get<std::string>());
            case nlohmann::json::value_t::array:
            {
                sol::table table = lua.create_table(static_cast<int>(value.size()), 0);
                int index = 1;
                for (const auto &element : value)
                {
                    table[index++] = JsonToLuaAt(element, lua, depth + 1, maxDepth, what);
                }
                return table;
            }
            case nlohmann::json::value_t::object:
            {
                sol::table table = lua.create_table(0, static_cast<int>(value.size()));
                for (const auto &[key, element] : value.items())
                {
                    table[key] = JsonToLuaAt(element, lua, depth + 1, maxDepth, what);
                }
                return table;
            }
            default:
                return sol::lua_nil; // null and discarded values
            }
        }
    }

    sol::object JsonToLua(const nlohmann::json &value, sol::state_view lua, const int maxDepth, const std::string_view what)
    {
        return JsonToLuaAt(value, lua, 0, maxDepth, what);
    }
}
