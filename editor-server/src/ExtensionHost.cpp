#include "editor-server/ExtensionHost.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include <sol/sol.hpp>

#include "engine/Logger.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine::Editor
{
    using nlohmann::json;
    namespace fs = std::filesystem;

    std::string_view ExtensionStateName(const ExtensionState state)
    {
        switch (state)
        {
        case ExtensionState::Loaded:
            return "loaded";
        case ExtensionState::Failed:
            return "failed";
        case ExtensionState::Faulted:
            return "faulted";
        }
        return "failed";
    }

    // ==================== SHA-256 ====================

    namespace
    {
        constexpr std::array<std::uint32_t, 64> Sha256K = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        constexpr std::uint32_t RotR(const std::uint32_t x, const int n)
        {
            return (x >> n) | (x << (32 - n));
        }

        void Sha256Block(std::array<std::uint32_t, 8> &state, const unsigned char *block)
        {
            std::array<std::uint32_t, 64> w{};
            for (int i = 0; i < 16; ++i)
            {
                w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                       (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                       static_cast<std::uint32_t>(block[i * 4 + 3]);
            }
            for (int i = 16; i < 64; ++i)
            {
                const std::uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const std::uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
            for (int i = 0; i < 64; ++i)
            {
                const std::uint32_t s1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
                const std::uint32_t ch = (e & f) ^ (~e & g);
                const std::uint32_t t1 = h + s1 + ch + Sha256K[i] + w[i];
                const std::uint32_t s0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
                const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t t2 = s0 + maj;
                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
            state[5] += f;
            state[6] += g;
            state[7] += h;
        }
    }

    std::string ExtensionHost::Sha256Hex(const std::string_view data)
    {
        std::array<std::uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        const auto *bytes = reinterpret_cast<const unsigned char *>(data.data());
        std::size_t offset = 0;
        for (; offset + 64 <= data.size(); offset += 64)
        {
            Sha256Block(state, bytes + offset);
        }
        // The last block or two: the rest, 0x80, zeros, and the length in bits
        std::array<unsigned char, 128> tail{};
        const std::size_t rest = data.size() - offset;
        if (rest > 0)
        {
            std::memcpy(tail.data(), bytes + offset, rest);
        }
        tail[rest] = 0x80;
        const std::size_t tailSize = rest + 1 + 8 <= 64 ? 64 : 128;
        const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
        for (int i = 0; i < 8; ++i)
        {
            tail[tailSize - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
        }
        Sha256Block(state, tail.data());
        if (tailSize == 128)
        {
            Sha256Block(state, tail.data() + 64);
        }
        std::string hex;
        hex.reserve(64);
        for (const std::uint32_t word : state)
        {
            hex += std::format("{:08x}", word);
        }
        return hex;
    }

    // ==================== Text helpers ====================

    namespace
    {
        // Strict UTF-8 (no overlong forms, no surrogates, nothing past U+10FFFF)
        bool IsValidUtf8(const std::string_view text)
        {
            std::size_t i = 0;
            while (i < text.size())
            {
                const auto c = static_cast<unsigned char>(text[i]);
                std::size_t extra = 0;
                std::uint32_t code = 0;
                if (c < 0x80)
                {
                    ++i;
                    continue;
                }
                if (c >= 0xC2 && c <= 0xDF)
                {
                    extra = 1;
                    code = c & 0x1F;
                }
                else if (c >= 0xE0 && c <= 0xEF)
                {
                    extra = 2;
                    code = c & 0x0F;
                }
                else if (c >= 0xF0 && c <= 0xF4)
                {
                    extra = 3;
                    code = c & 0x07;
                }
                else
                {
                    return false;
                }
                if (i + extra >= text.size())
                {
                    return false;
                }
                for (std::size_t k = 1; k <= extra; ++k)
                {
                    const auto next = static_cast<unsigned char>(text[i + k]);
                    if ((next & 0xC0) != 0x80)
                    {
                        return false;
                    }
                    code = (code << 6) | (next & 0x3F);
                }
                if ((extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000) || code > 0x10FFFF ||
                    (code >= 0xD800 && code <= 0xDFFF))
                {
                    return false;
                }
                i += extra + 1;
            }
            return true;
        }

        bool HasControlCharacter(const std::string_view text)
        {
            return std::ranges::any_of(text, [](const char c)
            {
                const auto byte = static_cast<unsigned char>(c);
                return byte < 0x20 || byte == 0x7F;
            });
        }

        // What a log line or an error text may show: control characters replaced, cut to `maxBytes`
        std::string Printable(const std::string_view text, const std::size_t maxBytes = 300)
        {
            std::string result(text.substr(0, maxBytes));
            for (char &c : result)
            {
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 0x20 || byte == 0x7F)
                {
                    c = '?';
                }
            }
            return result;
        }

        // A path as UTF-8 text (path::string() converts through the system's code page on Windows, and can throw)
        std::string Utf8(const fs::path &path)
        {
            const std::u8string text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }

        void ReplaceAll(std::string &text, const std::string &from, const std::string &to)
        {
            if (from.empty())
            {
                return;
            }
            std::size_t position = 0;
            while ((position = text.find(from, position)) != std::string::npos)
            {
                text.replace(position, from.size(), to);
                position += to.size();
            }
        }

        // Audit F10: an error text never shows where the project is on the host. Lua is given only relative chunk
        // names, so this is a second line of defence (an error from the host's own code, a library, a message that
        // quotes a path).
        std::string StripHostPaths(std::string text, const fs::path &root)
        {
            if (root.empty())
            {
                return text;
            }
            std::vector<std::string> variants;
            std::error_code ec;
            const fs::path canonical = fs::weakly_canonical(root, ec);
            for (const fs::path &path : {root, canonical, root.parent_path()})
            {
                if (path.empty())
                {
                    continue;
                }
                const std::string slashes = Utf8(path); // generic_u8string: '/' separators
                variants.push_back(slashes);
                std::string backslashes = slashes;
                ReplaceAll(backslashes, "/", "\\");
                std::string doubled = slashes;
                ReplaceAll(doubled, "/", "\\\\");
                variants.push_back(std::move(backslashes));
                variants.push_back(std::move(doubled));
            }
            // Longest first, so a folder inside another is not left half-replaced
            std::ranges::sort(variants, [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
            for (const std::string &variant : variants)
            {
                if (variant.size() >= 3)
                {
                    ReplaceAll(text, variant, "<project>");
                }
            }
            return text;
        }

        std::string Lower(std::string text)
        {
            for (char &c : text)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return text;
        }
    }

    // ==================== The Editor namespace of a state ====================

    namespace
    {
        // Installed in every extension state before its file runs (E-X2: Editor.Extension only). It returns the function
        // the host calls on what the file returned: it checks the file returned the very table Editor.Extension gave
        // it, and copies that table into plain data (strings, numbers, booleans, tables; a function becomes a table
        // whose only key is the table FN), so the host reads it with raw stack calls that can never run a metamethod.
        // The copy runs under the load budget like the chunk, with its own limits: depth, entries, string length. `next`
        // is used, never `pairs`, so a __pairs metamethod is not consulted.
        constexpr const char *EditorPrelude = R"LUA(
local FN = {}
local MAX_DEPTH, MAX_NODES, MAX_STRING = 8, 4000, 4096
local declared = nil

local function copy(v, depth, count)
    local tv = type(v)
    if tv == "string" then
        if #v > MAX_STRING then
            error("a string longer than 4096 bytes in the declaration", 0)
        end
        return v
    elseif tv == "boolean" then
        return v
    elseif tv == "number" then
        if v ~= v or v == math.huge or v == -math.huge then
            error("a number that is NaN or infinite in the declaration", 0)
        end
        return v
    elseif tv == "function" then
        return { [FN] = true }
    elseif tv == "table" then
        if depth >= MAX_DEPTH then
            error("the declaration is nested deeper than 8 levels", 0)
        end
        local out = {}
        for k, x in next, v do
            count[1] = count[1] + 1
            if count[1] > MAX_NODES then
                error("the declaration has more than 4000 entries", 0)
            end
            if type(k) == "string" then
                if #k > 128 then
                    error("a key longer than 128 bytes in the declaration", 0)
                end
            elseif math.type(k) ~= "integer" then
                error("a key of the declaration is neither a string nor an integer", 0)
            end
            out[k] = copy(x, depth + 1, count)
        end
        return out
    end
    error("a " .. tv .. " value in the declaration", 0)
end

Editor = {
    Extension = function(declaration)
        if type(declaration) ~= "table" then
            error("Editor.Extension expects a table, got " .. type(declaration), 2)
        end
        declared = declaration
        return declaration
    end,
}

return function(returned)
    if declared == nil then
        error("the file did not call Editor.Extension{...}", 0)
    end
    if not rawequal(returned, declared) then
        error("the file must return the table that Editor.Extension{...} gave it", 0)
    end
    return copy(declared, 0, { 0 })
end
)LUA";

        constexpr int MaxSnapshotDepth = 10;
        constexpr std::size_t MaxSnapshotNodes = 10000;

        // The copy the prelude made, as JSON. A function (a table keyed by a table) is a JSON binary value, which no
        // data can be. Raw stack calls only: nothing here can run Lua code. An empty table is an empty object (the
        // validators accept it as an empty list).
        json ReadSnapshot(lua_State *L, const int index, const int depth, std::size_t &nodes)
        {
            if (++nodes > MaxSnapshotNodes)
            {
                throw std::runtime_error("the declaration is too large");
            }
            switch (lua_type(L, index))
            {
            case LUA_TNIL:
                return nullptr;
            case LUA_TBOOLEAN:
                return lua_toboolean(L, index) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(L, index) != 0)
                {
                    return static_cast<std::int64_t>(lua_tointeger(L, index));
                }
                return static_cast<double>(lua_tonumber(L, index));
            case LUA_TSTRING:
            {
                std::size_t length = 0;
                const char *text = lua_tolstring(L, index, &length);
                return std::string(text, length);
            }
            case LUA_TTABLE:
                break;
            default:
                throw std::runtime_error("the declaration holds a value that is not data");
            }

            if (depth > MaxSnapshotDepth)
            {
                throw std::runtime_error("the declaration is nested too deeply");
            }
            if (lua_checkstack(L, 8) == 0)
            {
                throw std::runtime_error("the declaration is nested too deeply");
            }
            const int table = lua_absindex(L, index);
            json object = json::object();
            std::map<std::int64_t, json> list;
            bool isFunction = false;
            lua_pushnil(L);
            while (lua_next(L, table) != 0)
            {
                const int keyType = lua_type(L, -2);
                if (keyType == LUA_TTABLE)
                {
                    isFunction = true;
                }
                else if (keyType == LUA_TSTRING)
                {
                    std::size_t length = 0;
                    const char *text = lua_tolstring(L, -2, &length);
                    std::string key(text, length);
                    object[std::move(key)] = ReadSnapshot(L, -1, depth + 1, nodes);
                }
                else if (keyType == LUA_TNUMBER && lua_isinteger(L, -2) != 0)
                {
                    list[static_cast<std::int64_t>(lua_tointeger(L, -2))] = ReadSnapshot(L, -1, depth + 1, nodes);
                }
                else
                {
                    throw std::runtime_error("a key of the declaration is neither a string nor an integer");
                }
                lua_pop(L, 1);
            }
            if (isFunction)
            {
                return json::binary(std::vector<std::uint8_t>{});
            }
            if (list.empty())
            {
                return object;
            }
            if (!object.empty())
            {
                throw std::runtime_error("a table of the declaration mixes a list and named entries");
            }
            json array = json::array();
            std::int64_t expected = 1;
            for (auto &[key, value] : list)
            {
                if (key != expected++)
                {
                    throw std::runtime_error("a list of the declaration has a gap (its keys must be 1, 2, 3 ...)");
                }
                array.push_back(std::move(value));
            }
            return array;
        }
    }

    // ==================== Shape validation ====================

    namespace
    {
        struct ShapeError : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        [[noreturn]] void Bad(const std::string &where, const std::string &what)
        {
            throw ShapeError(where.empty() ? what : std::format("{}: {}", where, what));
        }

        bool IsFunction(const json &value)
        {
            return value.is_binary();
        }

        bool IsList(const json &value)
        {
            return value.is_array() || (value.is_object() && value.empty());
        }

        std::string TypeOf(const json &value)
        {
            if (value.is_binary())
                return "a function";
            if (value.is_null())
                return "nil";
            if (value.is_boolean())
                return "a boolean";
            if (value.is_number())
                return "a number";
            if (value.is_string())
                return "a string";
            if (value.is_array() || (value.is_object() && value.empty()))
                return "a list";
            return "a table";
        }

        void CheckKeys(const json &object, const std::set<std::string> &allowed, const std::string &where)
        {
            for (const auto &[key, value] : object.items())
            {
                (void)value;
                if (!allowed.contains(key))
                {
                    Bad(where, std::format("unknown key '{}' (this host's API version {} knows: {})", Printable(key, 64),
                                           ExtensionHost::SupportedApiVersion,
                                           [&]
                                           {
                                               std::string names;
                                               for (const std::string &name : allowed)
                                               {
                                                   names += (names.empty() ? "" : ", ") + name;
                                               }
                                               return names;
                                           }()));
                }
            }
        }

        const json *Find(const json &object, const char *key)
        {
            const auto found = object.find(key);
            return found == object.end() ? nullptr : &*found;
        }

        std::string RequireString(const json &object, const char *key, const std::string &where, const std::size_t maxBytes)
        {
            const json *value = Find(object, key);
            if (value == nullptr)
            {
                Bad(where, std::format("'{}' is required", key));
            }
            if (!value->is_string())
            {
                Bad(where, std::format("'{}' must be a string, not {}", key, TypeOf(*value)));
            }
            std::string text = value->get<std::string>();
            if (text.empty() || text.size() > maxBytes)
            {
                Bad(where, std::format("'{}' must be 1 to {} bytes", key, maxBytes));
            }
            if (!IsValidUtf8(text) || HasControlCharacter(text))
            {
                Bad(where, std::format("'{}' must be text without control characters", key));
            }
            return text;
        }

        std::optional<std::string> OptionalString(const json &object, const char *key, const std::string &where,
                                                  const std::size_t maxBytes)
        {
            const json *value = Find(object, key);
            if (value == nullptr)
            {
                return std::nullopt;
            }
            return RequireString(object, key, where, maxBytes);
        }

        // An extension's display name: it ends up in audit lines, the log and a client's menu, so it holds no path
        // syntax. Path traversal ("../x", "a/b", "C:\x") is refused here.
        bool IsValidExtensionName(const std::string_view name)
        {
            if (name.empty() || name.size() > ExtensionHost::MaxNameBytes || !IsValidUtf8(name) ||
                HasControlCharacter(name))
            {
                return false;
            }
            if (name.front() == ' ' || name.back() == ' ' || name.front() == '.' || name.back() == '.')
            {
                return false;
            }
            if (name.find("..") != std::string_view::npos)
            {
                return false;
            }
            return name.find_first_of("/\\:<>\"|?*") == std::string_view::npos;
        }

        bool IsValidItemId(const std::string_view id)
        {
            if (id.empty() || id.size() > ExtensionHost::MaxItemIdBytes)
            {
                return false;
            }
            const auto alnum = [](const char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            };
            if (!alnum(id.front()))
            {
                return false;
            }
            return std::ranges::all_of(id, [&](const char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; });
        }

        bool IsValidFieldName(const std::string_view name)
        {
            if (name.empty() || name.size() > ExtensionHost::MaxFieldNameBytes)
            {
                return false;
            }
            const auto first = [](const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
            const auto rest = [&](const char c) { return first(c) || (c >= '0' && c <= '9'); };
            return first(name.front()) && std::ranges::all_of(name, rest);
        }

        // "Tools/Align/Snap selection to grid": non-empty segments of text, no "." or ".." segment, no backslash
        void CheckMenuPath(const std::string &path, const std::string &where)
        {
            if (path.size() > ExtensionHost::MaxPathBytes)
            {
                Bad(where, std::format("'path' is longer than {} bytes", ExtensionHost::MaxPathBytes));
            }
            std::size_t segments = 0;
            std::size_t start = 0;
            while (start <= path.size())
            {
                std::size_t end = path.find('/', start);
                if (end == std::string::npos)
                {
                    end = path.size();
                }
                const std::string segment = path.substr(start, end - start);
                if (segment.empty() || segment == "." || segment == ".." || segment.front() == ' ' ||
                    segment.back() == ' ' || segment.size() > 64 || segment.find('\\') != std::string::npos)
                {
                    Bad(where, "'path' must be segments separated by '/', each 1 to 64 bytes, not '.' or '..', with no "
                               "backslash and no space at either end");
                }
                ++segments;
                start = end + 1;
            }
            if (segments > ExtensionHost::MaxPathSegments)
            {
                Bad(where, std::format("'path' has more than {} segments", ExtensionHost::MaxPathSegments));
            }
        }

        double NumberOf(const json &value)
        {
            return value.get<double>();
        }

        // The wire form of a form's field (protocol.json's FieldSchema), built from a Lua param spec
        struct FormField
        {
            json schema;
            json defaultValue;
        };

        FormField BuildFormField(const std::string &name, const json &spec, const std::string &where)
        {
            if (!spec.is_object() || IsList(spec) || spec.empty())
            {
                Bad(where, "a param must be a table such as { type = \"number\", default = 1 }");
            }
            CheckKeys(spec, {"type", "default", "min", "max", "tooltip", "options", "displayName"}, where);
            const std::string type = RequireString(spec, "type", where, 32);

            FieldKind kind = FieldKind::Json;
            std::string typeName;
            if (type == "number")
            {
                kind = FieldKind::Float;
                typeName = "float";
            }
            else if (type == "int")
            {
                kind = FieldKind::Int;
                typeName = "int";
            }
            else if (type == "string")
            {
                kind = FieldKind::String;
                typeName = "string";
            }
            else if (type == "bool")
            {
                kind = FieldKind::Bool;
                typeName = "bool";
            }
            else if (type == "Vector3")
            {
                kind = FieldKind::Vector3;
                typeName = "Vector3";
            }
            else if (type == "GameObject")
            {
                kind = FieldKind::GameObjectRef;
                typeName = "GameObject";
            }
            else if (type == "enum")
            {
                kind = FieldKind::Enum;
                typeName = "enum";
            }
            else
            {
                Bad(where, std::format("'type' \"{}\" is not one of number, int, string, bool, Vector3, GameObject, enum",
                                       Printable(type, 32)));
            }
            const bool numeric = kind == FieldKind::Float || kind == FieldKind::Int;

            json schema = {
                {"name", name},
                {"displayName", DefaultDisplayName(name)},
                {"kind", std::string(FieldKindName(kind))},
                {"typeName", typeName},
                {"hidden", false},
                {"readOnly", false},
            };
            if (const auto displayName = OptionalString(spec, "displayName", where, 64))
            {
                schema["displayName"] = *displayName;
            }
            if (const auto tooltip = OptionalString(spec, "tooltip", where, ExtensionHost::MaxTooltipBytes))
            {
                schema["tooltip"] = *tooltip;
            }

            const json *options = Find(spec, "options");
            std::vector<std::string> enumList;
            if (kind == FieldKind::Enum)
            {
                if (options == nullptr || !options->is_array() || options->empty() ||
                    options->size() > ExtensionHost::MaxEnumOptions)
                {
                    Bad(where, std::format("an enum needs 'options', a list of 1 to {} strings",
                                           ExtensionHost::MaxEnumOptions));
                }
                std::set<std::string> seen;
                for (const json &option : *options)
                {
                    if (!option.is_string())
                    {
                        Bad(where, "'options' must be strings");
                    }
                    std::string text = option.get<std::string>();
                    if (text.empty() || text.size() > 64 || !IsValidUtf8(text) || HasControlCharacter(text))
                    {
                        Bad(where, "an option must be 1 to 64 bytes of text without control characters");
                    }
                    if (!seen.insert(text).second)
                    {
                        Bad(where, std::format("the option \"{}\" is listed twice", Printable(text, 64)));
                    }
                    enumList.push_back(std::move(text));
                }
                schema["enumOptions"] = enumList;
            }
            else if (options != nullptr)
            {
                Bad(where, "'options' only goes with type = \"enum\"");
            }

            const json *minimum = Find(spec, "min");
            const json *maximum = Find(spec, "max");
            if ((minimum != nullptr || maximum != nullptr) && !numeric)
            {
                Bad(where, "'min' and 'max' only go with type = \"number\" or \"int\"");
            }
            if (minimum != nullptr && !minimum->is_number())
            {
                Bad(where, "'min' must be a number");
            }
            if (maximum != nullptr && !maximum->is_number())
            {
                Bad(where, "'max' must be a number");
            }
            if (minimum != nullptr && maximum != nullptr && NumberOf(*minimum) > NumberOf(*maximum))
            {
                Bad(where, "'min' is greater than 'max'");
            }
            if (minimum != nullptr)
            {
                schema["min"] = *minimum;
            }
            if (maximum != nullptr)
            {
                schema["max"] = *maximum;
            }

            // The default, checked against the kind (and the range), or the kind's own default
            json defaultValue;
            const json *given = Find(spec, "default");
            switch (kind)
            {
            case FieldKind::Float:
            case FieldKind::Int:
                defaultValue = kind == FieldKind::Int ? json(0) : json(0.0);
                if (given != nullptr)
                {
                    if (!given->is_number())
                    {
                        Bad(where, std::format("'default' must be a number, not {}", TypeOf(*given)));
                    }
                    if (kind == FieldKind::Int)
                    {
                        const double d = NumberOf(*given);
                        if (std::floor(d) != d || std::fabs(d) > 9.0e15)
                        {
                            Bad(where, "'default' of an int must be a whole number");
                        }
                        defaultValue = static_cast<std::int64_t>(d);
                    }
                    else
                    {
                        defaultValue = *given;
                    }
                }
                else if (minimum != nullptr && NumberOf(*minimum) > 0)
                {
                    defaultValue = *minimum;
                }
                else if (maximum != nullptr && NumberOf(*maximum) < 0)
                {
                    defaultValue = *maximum;
                }
                if ((minimum != nullptr && NumberOf(defaultValue) < NumberOf(*minimum)) ||
                    (maximum != nullptr && NumberOf(defaultValue) > NumberOf(*maximum)))
                {
                    Bad(where, "'default' is outside 'min' and 'max'");
                }
                break;
            case FieldKind::String:
                defaultValue = "";
                if (given != nullptr)
                {
                    if (!given->is_string())
                    {
                        Bad(where, std::format("'default' must be a string, not {}", TypeOf(*given)));
                    }
                    if (given->get_ref<const std::string &>().size() > 1024)
                    {
                        Bad(where, "'default' is longer than 1024 bytes");
                    }
                    defaultValue = *given;
                }
                break;
            case FieldKind::Bool:
                defaultValue = false;
                if (given != nullptr)
                {
                    if (!given->is_boolean())
                    {
                        Bad(where, std::format("'default' must be true or false, not {}", TypeOf(*given)));
                    }
                    defaultValue = *given;
                }
                break;
            case FieldKind::Vector3:
                defaultValue = {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}};
                if (given != nullptr)
                {
                    const bool shaped = given->is_object() && given->size() == 3 && Find(*given, "x") != nullptr &&
                                        Find(*given, "y") != nullptr && Find(*given, "z") != nullptr &&
                                        Find(*given, "x")->is_number() && Find(*given, "y")->is_number() &&
                                        Find(*given, "z")->is_number();
                    if (!shaped)
                    {
                        Bad(where, "'default' of a Vector3 must be { x = .., y = .., z = .. }");
                    }
                    defaultValue = *given;
                }
                break;
            case FieldKind::GameObjectRef:
                defaultValue = nullptr;
                if (given != nullptr)
                {
                    Bad(where, "a GameObject param has no 'default'");
                }
                break;
            case FieldKind::Enum:
                defaultValue = enumList.front();
                if (given != nullptr)
                {
                    if (!given->is_string() ||
                        std::find(enumList.begin(), enumList.end(), given->get<std::string>()) == enumList.end())
                    {
                        Bad(where, "'default' of an enum must be one of its 'options'");
                    }
                    defaultValue = *given;
                }
                break;
            default:
                break;
            }
            return {std::move(schema), std::move(defaultValue)};
        }

        // A menu item's params (a table of name = spec) as a form: the fields sorted by name, and the defaults
        json BuildForm(const json &params, const std::string &where)
        {
            if (!params.is_object())
            {
                Bad(where, std::format("'params' must be a table of name = {{ type = .. }}, not {}", TypeOf(params)));
            }
            if (params.size() > ExtensionHost::MaxFormFields)
            {
                Bad(where, std::format("'params' has more than {} fields", ExtensionHost::MaxFormFields));
            }
            json fields = json::array();
            json defaults = json::object();
            for (const auto &[name, spec] : params.items())
            {
                if (!IsValidFieldName(name))
                {
                    Bad(where, std::format("the param name \"{}\" must be letters, digits and '_' (not starting with a "
                                           "digit), at most {} bytes",
                                           Printable(name, 64), ExtensionHost::MaxFieldNameBytes));
                }
                FormField field = BuildFormField(name, spec, std::format("{}.{}", where, name));
                fields.push_back(std::move(field.schema));
                defaults[name] = std::move(field.defaultValue);
            }
            return {{"fields", std::move(fields)}, {"defaults", std::move(defaults)}};
        }

        json BuildMenuItem(const json &item, const std::string &extensionId, const std::size_t number,
                           std::set<std::string> &ids)
        {
            const std::string where = std::format("menu[{}]", number);
            if (!item.is_object() || IsList(item))
            {
                Bad(where, std::format("an item must be a table, not {}", TypeOf(item)));
            }
            CheckKeys(item, {"id", "path", "title", "minRole", "enabledWhen", "confirm", "params", "run"}, where);

            const std::string id = RequireString(item, "id", where, ExtensionHost::MaxItemIdBytes);
            if (!IsValidItemId(id))
            {
                Bad(where, std::format("the id \"{}\" must be letters, digits, '.', '_' and '-', starting with a letter "
                                       "or digit, at most {} bytes",
                                       Printable(id, 64), ExtensionHost::MaxItemIdBytes));
            }
            if (!ids.insert(id).second)
            {
                Bad(where, std::format("the id \"{}\" is used by two items of this extension", id));
            }
            const std::string itemWhere = std::format("menu item '{}'", id);

            const std::string path = RequireString(item, "path", itemWhere, ExtensionHost::MaxPathBytes);
            CheckMenuPath(path, itemWhere);
            std::string title = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
            if (const auto given = OptionalString(item, "title", itemWhere, ExtensionHost::MaxTitleBytes))
            {
                title = *given;
            }

            std::string minRole = "editor";
            if (const auto given = OptionalString(item, "minRole", itemWhere, 16))
            {
                if (*given != "editor" && *given != "admin")
                {
                    Bad(itemWhere, "'minRole' must be \"editor\" or \"admin\" (a viewer never invokes)");
                }
                minRole = *given;
            }

            json enabledWhen = {{"selection", "any"}};
            if (const json *given = Find(item, "enabledWhen"))
            {
                if (!given->is_object() || IsList(*given))
                {
                    Bad(itemWhere, "'enabledWhen' must be a table such as { selection = \"nonEmpty\" }");
                }
                CheckKeys(*given, {"selection", "componentType"}, itemWhere + ".enabledWhen");
                if (const auto selection = OptionalString(*given, "selection", itemWhere + ".enabledWhen", 16))
                {
                    if (*selection != "any" && *selection != "nonEmpty" && *selection != "one" && *selection != "none")
                    {
                        Bad(itemWhere, "enabledWhen.selection must be \"any\", \"nonEmpty\", \"one\" or \"none\"");
                    }
                    enabledWhen["selection"] = *selection;
                }
                if (const auto componentType = OptionalString(*given, "componentType", itemWhere + ".enabledWhen", 64))
                {
                    enabledWhen["componentType"] = *componentType;
                }
            }

            const json *run = Find(item, "run");
            if (run == nullptr || !IsFunction(*run))
            {
                Bad(itemWhere, "'run' must be a function");
            }

            json result = {
                {"id", id},
                {"extension", extensionId},
                {"path", path},
                {"title", title},
                {"enabledWhen", std::move(enabledWhen)},
                {"minRole", minRole},
            };
            if (const auto confirm = OptionalString(item, "confirm", itemWhere, ExtensionHost::MaxConfirmBytes))
            {
                result["confirm"] = *confirm;
            }
            if (const json *params = Find(item, "params"))
            {
                result["form"] = BuildForm(*params, itemWhere + ".params");
            }
            return result;
        }

        struct ParsedDeclaration
        {
            std::string name;
            int apiVersion = 0;
            std::vector<std::string> capabilities;
            std::vector<json> items;
        };

        // The shape of what Editor.Extension{...} was given (the copy ReadSnapshot made). Throws ShapeError, whose
        // text says which part is wrong; the host records it as the file's error.
        ParsedDeclaration ValidateDeclaration(const json &declaration, const std::string &extensionId)
        {
            if (!declaration.is_object() || IsList(declaration))
            {
                Bad("", std::format("Editor.Extension must be given a table of name, apiVersion, capabilities and menu, "
                                    "not {}", TypeOf(declaration)));
            }
            CheckKeys(declaration, {"name", "apiVersion", "capabilities", "menu"}, "");

            ParsedDeclaration parsed;
            parsed.name = RequireString(declaration, "name", "", ExtensionHost::MaxNameBytes);
            if (!IsValidExtensionName(parsed.name))
            {
                Bad("", "'name' must be 1 to 64 bytes of text with no '/', '\\', ':', '..' or other path characters and "
                        "no space or '.' at either end");
            }

            const json *apiVersion = Find(declaration, "apiVersion");
            if (apiVersion == nullptr)
            {
                Bad("", "'apiVersion' is required");
            }
            if (!apiVersion->is_number_integer())
            {
                Bad("", std::format("'apiVersion' must be an integer, not {}", TypeOf(*apiVersion)));
            }
            const auto version = apiVersion->get<std::int64_t>();
            if (version != ExtensionHost::SupportedApiVersion)
            {
                Bad("", std::format("apiVersion {} is not supported (this host speaks {})", version,
                                    ExtensionHost::SupportedApiVersion));
            }
            parsed.apiVersion = static_cast<int>(version);

            if (const json *capabilities = Find(declaration, "capabilities"))
            {
                if (!IsList(*capabilities))
                {
                    Bad("", std::format("'capabilities' must be a list of strings, not {}", TypeOf(*capabilities)));
                }
                if (capabilities->size() > ExtensionHost::MaxCapabilities)
                {
                    Bad("", std::format("'capabilities' has more than {} entries", ExtensionHost::MaxCapabilities));
                }
                std::set<std::string> seen;
                for (const json &capability : *capabilities)
                {
                    if (!capability.is_string())
                    {
                        Bad("", "'capabilities' must be strings");
                    }
                    const std::string text = capability.get<std::string>();
                    bool known = false;
                    for (const std::string_view name : ExtensionHost::CapabilityNames)
                    {
                        known = known || name == text;
                    }
                    if (!known)
                    {
                        Bad("", std::format("unknown capability \"{}\" (this host grants: scene.read, scene.write, "
                                            "assets.read, assets.write)",
                                            Printable(text, 64)));
                    }
                    if (!seen.insert(text).second)
                    {
                        Bad("", std::format("the capability \"{}\" is listed twice", text));
                    }
                    parsed.capabilities.push_back(text);
                }
            }

            if (const json *menu = Find(declaration, "menu"))
            {
                if (!IsList(*menu))
                {
                    Bad("", std::format("'menu' must be a list of items, not {}", TypeOf(*menu)));
                }
                if (menu->size() > ExtensionHost::MaxMenuItems)
                {
                    Bad("", std::format("'menu' has more than {} items", ExtensionHost::MaxMenuItems));
                }
                std::set<std::string> ids;
                std::size_t number = 1;
                for (const json &item : *menu)
                {
                    parsed.items.push_back(BuildMenuItem(item, extensionId, number++, ids));
                }
            }
            return parsed;
        }
    }

    // ==================== Workers ====================

    namespace
    {
        // What every worker shares: the number of workers that exist, whichever host started them. Made on first use and
        // NEVER destroyed: a worker still running at process exit (abandoned in a long C call) must not find it gone.
        struct WorkerCount
        {
            std::mutex mutex;
            int live = 0;
        };

        WorkerCount &SharedWorkerCount()
        {
            static WorkerCount *const count = new WorkerCount();
            return *count;
        }

        std::atomic<std::uint64_t> g_abandonOverrideMillis{0};
        std::mutex g_hookMutex;
        std::function<void(const std::string &)> g_workerHook;

        std::function<void(const std::string &)> CurrentWorkerHook()
        {
            const std::lock_guard lock(g_hookMutex);
            return g_workerHook;
        }

        constexpr std::uint64_t DefaultAbandonMillis =
            2 * Scripting::SandboxDefaults::LoadBudget.maxMillis + ExtensionHost::GraceMillis;
    }

    struct ExtensionHost::Tracker
    {
        std::mutex mutex;
        std::condition_variable changed;
        int live = 0;
    };

    struct ExtensionLoadJob
    {
        // Inputs: copies, the worker may outlive the caller
        std::string id;
        std::string source;
        std::function<void(const std::string &)> hook;

        std::mutex mutex;
        std::condition_variable finished;
        bool done = false;

        // The result
        bool ok = false;
        std::string error;
        json snapshot;
        std::unique_ptr<Scripting::LuaSandbox> sandbox;
    };

    struct ExtensionHost::Entry
    {
        std::string id;
        std::string name;
        std::string sha256;
        int apiVersion = 0;
        ExtensionState state = ExtensionState::Failed;
        std::string error;
        std::vector<std::string> capabilities;
        std::vector<json> items;
        /// The state the file ran in, kept for E-X3 (idle until then)
        std::unique_ptr<Scripting::LuaSandbox> sandbox;
        std::deque<std::chrono::steady_clock::time_point> failures;
    };

    namespace
    {
        void Fail(ExtensionLoadJob &job, std::string text)
        {
            job.ok = false;
            job.error = std::move(text);
            job.sandbox.reset();
        }
    }

    namespace
    {
        // The worker's whole job: a state, the prelude, the file's chunk, the declaration copy, and its reading. Every
        // way it can go wrong is data in the job.
        void EvaluateFile(ExtensionLoadJob &job)
        {
            using namespace Scripting;
            try
            {
                if (job.hook)
                {
                    job.hook(job.id);
                }
                std::unique_ptr<LuaSandbox> sandbox = LuaSandbox::Create(SandboxProfile::EditorExtension);
                if (sandbox == nullptr)
                {
                    Fail(job, "The extension's state could not be created");
                    return;
                }
                lua_State *L = sandbox->State();

                // The Editor namespace: the prelude returns the function that reads the declaration back
                if (const SandboxOutcome prelude = sandbox->RunChunk(EditorPrelude, "=editor-prelude",
                                                                      SandboxDefaults::LoadBudget, 1);
                    !prelude.ok)
                {
                    Fail(job, "The editor API could not be set up: " + prelude.error);
                    return;
                }
                const int reader = luaL_ref(L, LUA_REGISTRYINDEX);

                // The file's chunk. Its name is the file's path inside editor/ (never a host path)
                if (const SandboxOutcome run = sandbox->RunChunk(job.source, "=" + job.id, SandboxDefaults::LoadBudget, 1);
                    !run.ok)
                {
                    Fail(job, run.error);
                    return;
                }

                // What it returned, through the prelude's function (under the load budget again)
                lua_rawgeti(L, LUA_REGISTRYINDEX, reader);
                lua_insert(L, -2);
                sandbox->Arm(SandboxDefaults::LoadBudget);
                const int status = lua_pcall(L, 1, 1, 0);
                sandbox->Disarm();
                if (status != LUA_OK)
                {
                    const SandboxOutcome failure = sandbox->FailureOutcome(status, "The declaration could not be read");
                    lua_settop(L, 0);
                    Fail(job, failure.error);
                    return;
                }
                std::size_t nodes = 0;
                job.snapshot = ReadSnapshot(L, -1, 0, nodes);
                lua_settop(L, 0);
                luaL_unref(L, LUA_REGISTRYINDEX, reader);

                job.sandbox = std::move(sandbox);
                job.ok = true;
            }
            catch (const std::exception &e)
            {
                Fail(job, std::string("The extension could not be evaluated: ") + e.what());
            }
            catch (...)
            {
                Fail(job, "The extension could not be evaluated");
            }
        }
    }

    // ==================== The host ====================

    ExtensionHost::ExtensionHost(fs::path projectRoot) : _root(std::move(projectRoot)), _tracker(std::make_shared<Tracker>())
    {
    }

    ExtensionHost::~ExtensionHost()
    {
        Shutdown();
        std::vector<Entry> entries;
        {
            const std::lock_guard lock(_mutex);
            entries.swap(_entries);
        }
        // The idle states close here, one after the other (a close is linear in what the state holds)
    }

    std::uint64_t ExtensionHost::AbandonMillis()
    {
        const std::uint64_t value = g_abandonOverrideMillis.load();
        return value == 0 ? DefaultAbandonMillis : value;
    }

    void ExtensionHost::SetAbandonMillisForTest(const std::uint64_t millis)
    {
        g_abandonOverrideMillis.store(millis);
    }

    void ExtensionHost::SetWorkerHookForTest(std::function<void(const std::string &)> hook)
    {
        const std::lock_guard lock(g_hookMutex);
        g_workerHook = std::move(hook);
    }

    int ExtensionHost::LiveWorkers()
    {
        WorkerCount &count = SharedWorkerCount();
        const std::lock_guard lock(count.mutex);
        return count.live;
    }

    int ExtensionHost::Shutdown(const std::chrono::milliseconds maxWait)
    {
        {
            const std::lock_guard lock(_mutex);
            _stopping = true;
        }
        std::unique_lock lock(_tracker->mutex);
        _tracker->changed.wait_for(lock, maxWait, [this] { return _tracker->live == 0; });
        return _tracker->live;
    }

    void ExtensionHost::SetChangedCallback(std::function<void(std::uint32_t)> callback)
    {
        const std::lock_guard lock(_mutex);
        _changed = std::move(callback);
    }

    std::vector<ExtensionHost::FoundFile> ExtensionHost::Discover() const
    {
        std::vector<FoundFile> found;
        if (_root.empty())
        {
            return found;
        }
        std::error_code ec;
        const fs::path folder = _root / std::string(FolderName);
        const fs::file_status folderStatus = fs::symlink_status(folder, ec);
        if (ec || !fs::exists(folderStatus))
        {
            return found; // no editor/ folder: no extensions
        }
        if (fs::is_symlink(folderStatus))
        {
            Logger::Warn("The project's editor folder is a link: no editor extensions are loaded from it");
            return found;
        }
        if (!fs::is_directory(folderStatus))
        {
            return found;
        }
        const fs::path canonicalFolder = fs::canonical(folder, ec);
        if (ec)
        {
            return found;
        }

        // Symbolic links (to files or folders) are skipped, directories are not followed through links, and a file whose
        // real path is not inside the folder (a junction, a link met on the way) is left out too
        fs::recursive_directory_iterator it(canonicalFolder, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        if (ec)
        {
            return found;
        }
        std::size_t visited = 0;
        for (; it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            if (++visited > MaxVisitedEntries)
            {
                Logger::Warn(std::format("The project's editor folder has more than {} entries: the rest are not looked at",
                                         MaxVisitedEntries));
                break;
            }
            std::error_code statusError;
            const fs::file_status status = it->symlink_status(statusError);
            if (statusError || fs::is_symlink(status))
            {
                continue;
            }
            if (fs::is_directory(status))
            {
                if (it.depth() >= MaxDirectoryDepth)
                {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!fs::is_regular_file(status) || Lower(Utf8(it->path().extension())) != ".lua")
            {
                continue;
            }

            const fs::path real = fs::canonical(it->path(), statusError);
            if (statusError)
            {
                continue;
            }
            const fs::path inside = real.lexically_relative(canonicalFolder);
            if (inside.empty() || inside.is_absolute() || *inside.begin() == fs::path(".."))
            {
                continue;
            }

            const std::u8string u8 = it->path().lexically_relative(canonicalFolder).generic_u8string();
            std::string id(u8.begin(), u8.end());
            if (id.empty() || !IsValidUtf8(id) || HasControlCharacter(id))
            {
                Logger::Warn("An editor extension file has a name that is not valid text and is not loaded");
                continue;
            }
            found.push_back({std::move(id), it->path()});
        }
        std::ranges::sort(found, [](const FoundFile &a, const FoundFile &b) { return a.id < b.id; });
        return found;
    }

    ExtensionHost::Entry ExtensionHost::LoadOne(const FoundFile &file)
    {
        Entry entry;
        entry.id = file.id;
        entry.name = file.id;
        entry.state = ExtensionState::Failed;
        const auto fail = [&](std::string text)
        {
            entry.state = ExtensionState::Failed;
            entry.error = Printable(StripHostPaths(std::move(text), _root));
            entry.sandbox.reset();
            entry.items.clear();
            entry.capabilities.clear();
        };

        std::error_code ec;
        const std::uintmax_t size = fs::file_size(file.path, ec);
        if (ec)
        {
            fail("The file could not be read");
            return entry;
        }
        if (size > MaxFileBytes)
        {
            fail(std::format("The file is larger than {} bytes and is not loaded", MaxFileBytes));
            return entry;
        }
        std::string source;
        {
            std::ifstream stream(file.path, std::ios::binary);
            if (!stream)
            {
                fail("The file could not be read");
                return entry;
            }
            source.resize(static_cast<std::size_t>(size));
            stream.read(source.data(), static_cast<std::streamsize>(size));
            source.resize(static_cast<std::size_t>(std::max<std::streamsize>(stream.gcount(), 0)));
        }
        entry.sha256 = Sha256Hex(source);
        if (source.starts_with("\xEF\xBB\xBF"))
        {
            source.erase(0, 3); // a byte order mark is no Lua
        }

        bool stopping = false;
        {
            const std::lock_guard lock(_mutex);
            stopping = _stopping;
        }
        if (stopping)
        {
            fail("The host is shutting down: the file is not loaded");
            return entry;
        }

        // An abandoned worker of this same text still running: it is not started a second time
        {
            const std::lock_guard lock(_mutex);
            if (const auto found = _abandoned.find(file.id); found != _abandoned.end())
            {
                if (found->second.first == entry.sha256 && !found->second.second.expired())
                {
                    const std::shared_ptr<ExtensionLoadJob> running = found->second.second.lock();
                    bool stillRunning = false;
                    if (running != nullptr)
                    {
                        const std::lock_guard jobLock(running->mutex);
                        stillRunning = !running->done;
                    }
                    if (stillRunning)
                    {
                        // fail() below takes no lock of _mutex
                        entry.state = ExtensionState::Failed;
                        entry.error = "An earlier load of this same file is still running (it was abandoned); the file is "
                                      "tried again at the next reload";
                        return entry;
                    }
                }
                _abandoned.erase(found);
            }
        }

        auto job = std::make_shared<ExtensionLoadJob>();
        job->id = file.id;
        job->source = std::move(source);
        job->hook = CurrentWorkerHook();

        WorkerCount &workers = SharedWorkerCount();
        {
            const std::lock_guard lock(workers.mutex);
            if (workers.live >= MaxWorkers)
            {
                const int live = workers.live;
                fail(std::format("{} extension evaluations are still running (abandoned ones count): the file is tried "
                                 "again at the next reload",
                                 live));
                return entry;
            }
            ++workers.live;
        }
        {
            const std::lock_guard lock(_tracker->mutex);
            ++_tracker->live;
        }

        const std::shared_ptr<Tracker> tracker = _tracker;
        try
        {
            std::thread([job, tracker]
            {
                EvaluateFile(*job);
                // The evaluation is over: the worker stops counting before anyone is told it has an answer, so a caller
                // that has its result finds this worker's slot free. What follows touches only the job and the tracker,
                // which this thread owns a share of.
                {
                    WorkerCount &count = SharedWorkerCount();
                    const std::lock_guard lock(count.mutex);
                    --count.live;
                }
                {
                    const std::lock_guard lock(tracker->mutex);
                    --tracker->live;
                }
                tracker->changed.notify_all();
                {
                    const std::lock_guard lock(job->mutex);
                    job->done = true;
                }
                job->finished.notify_all();
            }).detach();
        }
        catch (...)
        {
            {
                const std::lock_guard lock(workers.mutex);
                --workers.live;
            }
            {
                const std::lock_guard lock(_tracker->mutex);
                --_tracker->live;
            }
            _tracker->changed.notify_all();
            fail("A thread could not be started to load the file");
            return entry;
        }

        const std::uint64_t deadline = AbandonMillis();
        {
            std::unique_lock lock(job->mutex);
            if (!job->finished.wait_for(lock, std::chrono::milliseconds(deadline), [&] { return job->done; }))
            {
                lock.unlock();
                // Abandoned: the worker still holds the job and frees it (and its state) when it ends; its result is
                // never read
                {
                    const std::lock_guard registry(_mutex);
                    _abandoned[file.id] = {entry.sha256, job};
                }
                fail(std::format("Loading the file took longer than {} ms and was abandoned", deadline));
                return entry;
            }
        }

        if (!job->ok)
        {
            fail(job->error);
            return entry;
        }
        try
        {
            ParsedDeclaration parsed = ValidateDeclaration(job->snapshot, file.id);
            entry.name = std::move(parsed.name);
            entry.apiVersion = parsed.apiVersion;
            entry.capabilities = std::move(parsed.capabilities);
            entry.items = std::move(parsed.items);
            entry.sandbox = std::move(job->sandbox);
            entry.state = ExtensionState::Loaded;
        }
        catch (const ShapeError &e)
        {
            fail(std::string("The declaration is not valid: ") + e.what());
        }
        catch (const std::exception &e)
        {
            fail(std::string("The declaration could not be read: ") + e.what());
        }
        return entry;
    }

    void ExtensionHost::LogEntry(const Entry &entry) const
    {
        if (entry.state == ExtensionState::Loaded)
        {
            std::string capabilities;
            for (const std::string &capability : entry.capabilities)
            {
                capabilities += (capabilities.empty() ? "" : ",") + capability;
            }
            Logger::Info(std::format("Editor extension: {}@{} capabilities=[{}] menu items={} ({})", entry.name,
                                     entry.sha256.substr(0, 8), capabilities, entry.items.size(), entry.id));
        }
        else
        {
            Logger::Warn(std::format("Editor extension {} did not load: {}", Printable(entry.id, 120),
                                     Printable(entry.error)));
        }
    }

    void ExtensionHost::Load()
    {
        std::vector<FoundFile> files = Discover();
        if (files.size() > MaxFiles)
        {
            Logger::Warn(std::format("The project's editor folder has {} extension files: only the first {} (in name "
                                     "order) are loaded",
                                     files.size(), MaxFiles));
            files.resize(MaxFiles);
        }

        std::vector<Entry> entries;
        entries.reserve(files.size());
        std::map<std::string, std::string> itemOwners; // item id -> the extension that has it
        for (const FoundFile &file : files)
        {
            Entry entry = LoadOne(file);
            if (entry.state == ExtensionState::Loaded)
            {
                // Item ids are global: InvokeMenuItem names an item and nothing else. The first extension (in file order)
                // keeps an id; a later one that uses it fails, as one whose own items clash does.
                for (const json &item : entry.items)
                {
                    const std::string id = item.at("id").get<std::string>();
                    const auto owner = itemOwners.find(id);
                    if (owner != itemOwners.end())
                    {
                        entry.state = ExtensionState::Failed;
                        entry.error = Printable(std::format("The menu item id \"{}\" is already used by the extension {}",
                                                            id, owner->second));
                        entry.sandbox.reset();
                        entry.items.clear();
                        entry.capabilities.clear();
                        break;
                    }
                }
                if (entry.state == ExtensionState::Loaded)
                {
                    for (const json &item : entry.items)
                    {
                        itemOwners[item.at("id").get<std::string>()] = entry.id;
                    }
                }
            }
            entries.push_back(std::move(entry));
        }

        std::vector<Entry> previous;
        {
            const std::lock_guard lock(_mutex);
            previous.swap(_entries);
            _entries = std::move(entries);
            ++_uiRevision;
        }
        // The old states close here, outside the registry's mutex

        std::size_t loaded = 0;
        for (const Entry &entry : _entries)
        {
            LogEntry(entry);
            loaded += entry.state == ExtensionState::Loaded ? 1 : 0;
        }
        Logger::Info(std::format("Editor extensions: {} of {} loaded", loaded, _entries.size()));
    }

    void ExtensionHost::Reload()
    {
        Load();
        std::function<void(std::uint32_t)> changed;
        std::uint32_t revision = 0;
        {
            const std::lock_guard lock(_mutex);
            changed = _changed;
            revision = _uiRevision;
        }
        if (changed)
        {
            changed(revision);
        }
    }

    json ExtensionHost::DescribeJson() const
    {
        const std::lock_guard lock(_mutex);
        json extensions = json::array();
        for (const Entry &entry : _entries)
        {
            json item = {
                {"id", entry.id},
                {"name", entry.name},
                {"sha256", entry.sha256},
                {"apiVersion", entry.apiVersion},
                {"state", std::string(ExtensionStateName(entry.state))},
                {"capabilities", entry.capabilities},
                {"itemCount", entry.items.size()},
            };
            if (!entry.error.empty())
            {
                item["error"] = entry.error;
            }
            extensions.push_back(std::move(item));
        }
        return {{"enabled", true}, {"uiRevision", _uiRevision}, {"extensions", std::move(extensions)}};
    }

    json ExtensionHost::UiJson() const
    {
        const std::lock_guard lock(_mutex);
        json items = json::array();
        for (const Entry &entry : _entries)
        {
            if (entry.state == ExtensionState::Failed)
            {
                continue;
            }
            for (const json &item : entry.items)
            {
                items.push_back(item);
            }
        }
        return {{"uiRevision", _uiRevision}, {"menuItems", std::move(items)}, {"inspectorSections", json::array()}};
    }

    json ExtensionHost::DisabledDescribeJson()
    {
        return {{"enabled", false}, {"uiRevision", 0}, {"extensions", json::array()}};
    }

    json ExtensionHost::DisabledUiJson()
    {
        return {{"uiRevision", 0}, {"menuItems", json::array()}, {"inspectorSections", json::array()}};
    }

    std::uint32_t ExtensionHost::UiRevision() const
    {
        const std::lock_guard lock(_mutex);
        return _uiRevision;
    }

    std::size_t ExtensionHost::ExtensionCount() const
    {
        const std::lock_guard lock(_mutex);
        return _entries.size();
    }

    std::size_t ExtensionHost::LoadedCount() const
    {
        const std::lock_guard lock(_mutex);
        return static_cast<std::size_t>(std::ranges::count_if(
            _entries, [](const Entry &entry) { return entry.state == ExtensionState::Loaded; }));
    }

    ExtensionState ExtensionHost::StateOf(const std::string_view id) const
    {
        const std::lock_guard lock(_mutex);
        for (const Entry &entry : _entries)
        {
            if (entry.id == id)
            {
                return entry.state;
            }
        }
        return ExtensionState::Failed;
    }

    ExtensionState ExtensionHost::NoteFailure(const std::string_view id, const std::chrono::steady_clock::time_point now)
    {
        std::function<void(std::uint32_t)> changed;
        std::uint32_t revision = 0;
        ExtensionState state = ExtensionState::Failed;
        bool becameFaulted = false;
        {
            const std::lock_guard lock(_mutex);
            for (Entry &entry : _entries)
            {
                if (entry.id != id)
                {
                    continue;
                }
                state = entry.state;
                if (entry.state != ExtensionState::Loaded)
                {
                    break;
                }
                entry.failures.push_back(now);
                while (!entry.failures.empty() && now - entry.failures.front() > FaultWindow)
                {
                    entry.failures.pop_front();
                }
                if (entry.failures.size() >= static_cast<std::size_t>(FaultThreshold))
                {
                    entry.state = ExtensionState::Faulted;
                    entry.error = std::format("Disabled until reload: {} failures within {} minutes", FaultThreshold,
                                              FaultWindow.count());
                    ++_uiRevision;
                    becameFaulted = true;
                    state = entry.state;
                    changed = _changed;
                    revision = _uiRevision;
                }
                break;
            }
        }
        if (becameFaulted)
        {
            Logger::Warn(std::format("Editor extension {} is faulted until reload ({} failures within {} minutes)",
                                     Printable(std::string(id), 120), FaultThreshold, FaultWindow.count()));
            if (changed)
            {
                changed(revision);
            }
        }
        return state;
    }

    void ExtensionHost::ForEachStateForTest(const std::function<void(const std::string &, lua_State *)> &visit) const
    {
        const std::lock_guard lock(_mutex);
        for (const Entry &entry : _entries)
        {
            if (entry.sandbox != nullptr && entry.sandbox->State() != nullptr)
            {
                visit(entry.id, entry.sandbox->State());
            }
        }
    }
}
