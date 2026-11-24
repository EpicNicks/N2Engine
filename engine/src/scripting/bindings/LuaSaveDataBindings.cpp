#include "engine/scripting/bindings/LuaBindings.hpp"
#include "engine/scripting/LuaJson.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/Logger.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace N2Engine::Scripting::Bindings
{
    namespace
    {
        namespace fs = std::filesystem;

        // A save name is a plain file name: letters, digits, '_', '-' and '.', 1 to 64 characters, not starting
        // with '.' (so never "..", a hidden file or a path)
        bool IsValidSaveName(const std::string &name)
        {
            if (name.empty() || name.size() > SaveDataMaxNameLength || name.front() == '.')
            {
                return false;
            }
            for (const char c : name)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                c == '_' || c == '-' || c == '.';
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }

        // file_clock has no portable conversion to system_clock before C++20 clock_cast, which not every standard
        // library here has: shift by the offset between the two clocks now (exact to well under a millisecond)
        double FileTimeToUnixSeconds(const fs::file_time_type written)
        {
            const auto system = std::chrono::system_clock::now() +
                                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                    written - fs::file_time_type::clock::now());
            return std::chrono::duration<double>(system.time_since_epoch()).count();
        }

        // user://saves/<name>, or an empty path when the name is not allowed or there is no user data folder
        fs::path SavePath(const std::string &name, const char *function)
        {
            if (!IsValidSaveName(name))
            {
                throw std::runtime_error(std::format(
                    "{}: '{}' is not a save name (letters, digits, _ - . only, at most {} characters, not starting "
                    "with '.')", function, name, SaveDataMaxNameLength));
            }
            return IO::ResourceLoader::Instance().Resolve(IO::ResourcePath("user://saves/" + name));
        }
    }

    void BindSaveData(LuaRuntime &runtime)
    {
        auto &lua = runtime.GetState();

        // ===== SaveData (global): small text files in the project's user://saves folder =====
        lua["SaveData"] = lua.create_table_with(
            // Writes the whole file (to a temporary file first, then renamed over the old one, so a crash never
            // leaves half a save). Returns true, or false and a message.
            "Write", [](const std::string &name, const std::string &text) -> std::tuple<bool, sol::optional<std::string>>
            {
                const fs::path path = SavePath(name, "SaveData.Write");
                if (path.empty())
                {
                    return {false, std::string("no user data folder")};
                }
                if (text.size() > SaveDataMaxBytes)
                {
                    return {false, std::format("{} bytes is over the {} byte limit", text.size(), SaveDataMaxBytes)};
                }
                std::error_code ec;
                fs::create_directories(path.parent_path(), ec);
                fs::path temporary = path;
                temporary += ".tmp";
                {
                    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                    if (!out)
                    {
                        return {false, std::format("cannot write {}", name)};
                    }
                    out.write(text.data(), static_cast<std::streamsize>(text.size()));
                    if (!out)
                    {
                        return {false, std::format("cannot write {}", name)};
                    }
                }
                fs::rename(temporary, path, ec);
                if (ec)
                {
                    fs::remove(temporary, ec);
                    return {false, std::format("cannot replace {}", name)};
                }
                return {true, sol::nullopt};
            },
            // The file's text, or nil when it does not exist (or cannot be read)
            "Read", [](const std::string &name) -> sol::optional<std::string>
            {
                const fs::path path = SavePath(name, "SaveData.Read");
                std::error_code ec;
                if (path.empty() || !fs::is_regular_file(path, ec))
                {
                    return sol::nullopt;
                }
                if (fs::file_size(path, ec) > SaveDataMaxBytes)
                {
                    Logger::Warn(std::format("SaveData.Read: {} is over the {} byte limit", name, SaveDataMaxBytes));
                    return sol::nullopt;
                }
                std::ifstream in(path, std::ios::binary);
                if (!in)
                {
                    return sol::nullopt;
                }
                std::ostringstream text;
                text << in.rdbuf();
                return text.str();
            },
            "Exists", [](const std::string &name)
            {
                const fs::path path = SavePath(name, "SaveData.Exists");
                std::error_code ec;
                return !path.empty() && fs::is_regular_file(path, ec);
            },
            // When the file was last written, as Unix time (UTC seconds, a float), or nil when it does not exist.
            // Compare it with Time.unixTime for offline progress.
            "GetModifiedTime", [](const std::string &name) -> sol::optional<double>
            {
                const fs::path path = SavePath(name, "SaveData.GetModifiedTime");
                std::error_code ec;
                if (path.empty() || !fs::is_regular_file(path, ec))
                {
                    return sol::nullopt;
                }
                const fs::file_time_type written = fs::last_write_time(path, ec);
                if (ec)
                {
                    return sol::nullopt;
                }
                return FileTimeToUnixSeconds(written);
            },
            // Removes the file; true if it existed
            "Delete", [](const std::string &name)
            {
                const fs::path path = SavePath(name, "SaveData.Delete");
                std::error_code ec;
                return !path.empty() && fs::remove(path, ec);
            }
        );

        // ===== Json (global) =====
        lua["Json"] = lua.create_table_with(
            // A Lua value as JSON text. A table with keys exactly 1..n is an array, any other table an object (keys
            // sorted); functions, userdata and cycles become null. pretty: indent with two spaces.
            "Encode", [](const sol::object &value, const sol::optional<bool> pretty)
            {
                return LuaToJson(value).dump(pretty.value_or(false) ? 2 : -1);
            },
            // JSON text as a Lua value (arrays as 1-based tables, null as nil), or nil and a message when the text
            // is not JSON. options.numbers: "auto" (default; integers as Lua integers) or "float" (every number a
            // Lua float, so arithmetic on large values never wraps)
            "Decode", [](const std::string &text, const sol::optional<sol::table> options,
                         sol::this_state state) -> std::tuple<sol::object, sol::object>
            {
                sol::state_view lua(state);
                JsonNumbers numbers = JsonNumbers::Auto;
                if (options)
                {
                    const sol::object mode = (*options)["numbers"];
                    if (mode.valid() && mode.get_type() != sol::type::lua_nil)
                    {
                        const std::string name = mode.is<std::string>() ? mode.as<std::string>() : std::string();
                        if (name == "float")
                        {
                            numbers = JsonNumbers::Float;
                        }
                        else if (name != "auto")
                        {
                            throw std::runtime_error("Json.Decode: options.numbers is \"auto\" or \"float\"");
                        }
                    }
                }
                const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
                if (parsed.is_discarded())
                {
                    return {sol::lua_nil, sol::make_object(lua, std::string("Json.Decode: not valid JSON"))};
                }
                return {JsonToLua(parsed, lua, JsonMaxDepth, "Json.Decode", numbers), sol::lua_nil};
            }
        );
    }
}
