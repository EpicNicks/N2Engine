#include "engine/scripting/LuaJson.hpp"

#include <cmath>
#include <cstdint>

namespace N2Engine::Scripting
{
    namespace
    {
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
}
