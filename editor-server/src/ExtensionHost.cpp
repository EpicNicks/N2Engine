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
#include "editor-server/ServerLog.hpp"
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
local runs = {}
-- What the host gave this state to reach it with, taken before any file code runs and then removed from the globals:
-- the only way out of the state is these wrappers
local host = __n2_host
__n2_host = nil
-- The library functions the wrappers use, taken now, so a file that later replaces `type` or `table.concat` in its own
-- state cannot change what the wrappers do
local type, next, rawget, rawequal, error, select, tostring = type, next, rawget, rawequal, error, select, tostring
local concat, ssub = table.concat, string.sub

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

-- Editor.Log: the lines go to the host as requests; outside a menu item (while the file loads) they are dropped
local function join(...)
    local n = select("#", ...)
    local parts = {}
    for i = 1, n do
        parts[i] = tostring((select(i, ...)))
    end
    local text = concat(parts, " ")
    if #text > 4096 then
        text = ssub(text, 1, 4096)
    end
    return text
end

local function logger(level)
    return function(...)
        host("log", { level = level, text = join(...) })
    end
end

local Log = { Info = logger("info"), Warn = logger("warn"), Error = logger("error") }

local function expect(value, kind, fname, what)
    if type(value) ~= kind then
        error(fname .. " expects " .. what .. ", got " .. type(value), 3)
    end
end

-- Editor.Scene: installed by Editor.Extension for the capabilities the file declared. Every function is a request the
-- host answers through its command layer; values are plain tables (the JSON of the protocol) and ids are strings.
local function sceneApi(read, write)
    local Scene = {}
    if read then
        function Scene.GetHierarchy()
            return host("scene.getHierarchy")
        end
        function Scene.GetEntity(id)
            expect(id, "string", "Editor.Scene.GetEntity", "an entity id (a string)")
            return host("scene.getEntity", { id = id })
        end
        function Scene.GetComponent(entityId, componentId)
            expect(entityId, "string", "Editor.Scene.GetComponent", "an entity id (a string)")
            expect(componentId, "string", "Editor.Scene.GetComponent", "a component id (a string)")
            return host("scene.getComponent", { entityId = entityId, componentId = componentId })
        end
        function Scene.GetLocalTransform(id)
            expect(id, "string", "Editor.Scene.GetLocalTransform", "an entity id (a string)")
            return host("scene.getLocalTransform", { id = id })
        end
        function Scene.FindByName(name)
            expect(name, "string", "Editor.Scene.FindByName", "a name (a string)")
            return host("scene.findByName", { name = name })
        end
        function Scene.GetComponentTypes()
            return host("scene.getComponentTypes")
        end
    end
    if write then
        function Scene.CreateEntity(spec)
            if spec == nil then
                spec = {}
            end
            expect(spec, "table", "Editor.Scene.CreateEntity", "a table such as { name = \"Cube\", parent = id }")
            return host("scene.createEntity", spec)
        end
        function Scene.Destroy(id)
            expect(id, "string", "Editor.Scene.Destroy", "an entity id (a string)")
            return host("scene.destroy", { id = id })
        end
        function Scene.Duplicate(id)
            expect(id, "string", "Editor.Scene.Duplicate", "an entity id (a string)")
            return host("scene.duplicate", { id = id })
        end
        function Scene.SetParent(id, parent, index)
            expect(id, "string", "Editor.Scene.SetParent", "an entity id (a string)")
            return host("scene.setParent", { id = id, parent = parent, index = index })
        end
        function Scene.SetProperties(id, properties)
            expect(id, "string", "Editor.Scene.SetProperties", "an entity id (a string)")
            expect(properties, "table", "Editor.Scene.SetProperties", "a table of name, tag, layer and active")
            return host("scene.setProperties", { id = id, properties = properties })
        end
        function Scene.SetLocalTransform(id, transform)
            expect(id, "string", "Editor.Scene.SetLocalTransform", "an entity id (a string)")
            expect(transform, "table", "Editor.Scene.SetLocalTransform", "a table of position, rotation and scale")
            return host("scene.setLocalTransform", { id = id, transform = transform })
        end
        function Scene.AddComponent(id, typeName, values)
            expect(id, "string", "Editor.Scene.AddComponent", "an entity id (a string)")
            expect(typeName, "string", "Editor.Scene.AddComponent", "a component type name (a string)")
            return host("scene.addComponent", { id = id, type = typeName, values = values })
        end
        function Scene.RemoveComponent(entityId, componentId)
            expect(entityId, "string", "Editor.Scene.RemoveComponent", "an entity id (a string)")
            expect(componentId, "string", "Editor.Scene.RemoveComponent", "a component id (a string)")
            return host("scene.removeComponent", { entityId = entityId, componentId = componentId })
        end
        function Scene.SetFields(entityId, componentId, values)
            expect(entityId, "string", "Editor.Scene.SetFields", "an entity id (a string)")
            expect(componentId, "string", "Editor.Scene.SetFields", "a component id (a string)")
            expect(values, "table", "Editor.Scene.SetFields", "a table of field values")
            return host("scene.setFields", { entityId = entityId, componentId = componentId, values = values })
        end
    end
    return Scene
end

Editor = {
    Extension = function(declaration)
        if type(declaration) ~= "table" then
            error("Editor.Extension expects a table, got " .. type(declaration), 2)
        end
        declared = declaration
        local read, write = false, false
        local capabilities = rawget(declaration, "capabilities")
        if type(capabilities) == "table" then
            for _, capability in next, capabilities do
                if capability == "scene.read" then
                    read = true
                elseif capability == "scene.write" then
                    write = true
                end
            end
        end
        if read or write then
            Editor.Scene = sceneApi(read, write)
        else
            Editor.Scene = nil
        end
        return declaration
    end,
    Log = Log,
}

-- The host's print is gone from the state (the profile removes it): print writes to the log, as Editor.Log.Info
print = function(...)
    Log.Info(...)
end

-- The first function reads the declaration back and keeps the functions of the menu; the second runs one of them
return function(returned)
    if declared == nil then
        error("the file did not call Editor.Extension{...}", 0)
    end
    if not rawequal(returned, declared) then
        error("the file must return the table that Editor.Extension{...} gave it", 0)
    end
    local snapshot = copy(declared, 0, { 0 })
    -- The functions are taken now: what the file does to its own tables afterwards cannot change what runs
    runs = {}
    local menu = rawget(declared, "menu")
    if type(menu) == "table" then
        for _, item in next, menu do
            if type(item) == "table" then
                local id, run = rawget(item, "id"), rawget(item, "run")
                if type(id) == "string" and type(run) == "function" then
                    runs[id] = run
                end
            end
        end
    end
    return snapshot
end, function(itemId, ctx, params)
    local run = runs[itemId]
    if run == nil then
        error("the extension has no menu item " .. tostring(itemId), 0)
    end
    return run(ctx, params)
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

        // Whether an int param may hold it as a bound or a default: finite, whole, and small enough that a double (and
        // the int64 a default is turned into) represents it exactly
        bool IsWholeNumber(const double value)
        {
            return std::isfinite(value) && std::floor(value) == value && std::fabs(value) <= 9.0e15;
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
            if (kind == FieldKind::Int)
            {
                // A whole number a double holds exactly: no fraction, no NaN or infinity, nothing past 9e15
                if (minimum != nullptr && !IsWholeNumber(NumberOf(*minimum)))
                {
                    Bad(where, "'min' of an int must be a whole number");
                }
                if (maximum != nullptr && !IsWholeNumber(NumberOf(*maximum)))
                {
                    Bad(where, "'max' of an int must be a whole number");
                }
            }
            // An int's bounds are whole (checked above) and go out as integers, 3.0 included
            const auto asBound = [kind](const json &bound)
            { return kind == FieldKind::Int ? json(static_cast<std::int64_t>(NumberOf(bound))) : bound; };
            if (minimum != nullptr)
            {
                schema["min"] = asBound(*minimum);
            }
            if (maximum != nullptr)
            {
                schema["max"] = asBound(*maximum);
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
                        if (!IsWholeNumber(NumberOf(*given)))
                        {
                            Bad(where, "'default' of an int must be a whole number");
                        }
                        defaultValue = static_cast<std::int64_t>(NumberOf(*given));
                    }
                    else
                    {
                        defaultValue = *given;
                    }
                }
                else if (minimum != nullptr && NumberOf(*minimum) > 0)
                {
                    defaultValue = asBound(*minimum);
                }
                else if (maximum != nullptr && NumberOf(*maximum) < 0)
                {
                    defaultValue = asBound(*maximum);
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
        std::atomic<std::uint64_t> g_reloadBudgetOverrideMillis{0};
        std::atomic<std::size_t> g_totalBytesOverride{0};
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
        /// The cap of the file's state (the smaller of the per-state cap and what is left of the total budget)
        std::size_t capBytes = 0;

        std::mutex mutex;
        std::condition_variable finished;
        bool done = false;

        // The result
        bool ok = false;
        std::string error;
        /// The failure was the state's memory cap
        bool memoryExhausted = false;
        json snapshot;
        std::unique_ptr<Scripting::LuaSandbox> sandbox;
        /// The registry reference of the prelude's invoker (runs one menu item), kept for the state's life
        int invokeRef = LUA_NOREF;
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
        /// The state the file ran in (idle between invocations; null while an invocation owns it, and for good once one
        /// was abandoned)
        std::unique_ptr<Scripting::LuaSandbox> sandbox;
        /// The registry reference, in that state, of the function that runs one menu item
        int invokeRef = LUA_NOREF;
        /// What the state held at its most while it loaded: its share of the total memory budget
        std::size_t chargedBytes = 0;
        std::deque<std::chrono::steady_clock::time_point> failures;
    };

    // ==================== Invocation: the script's side ====================

    // One invocation of a menu item on a worker thread. It owns copies of everything the worker touches and the state
    // itself (moved in from the registry for the call), so an abandoned worker finds nothing it must not use: it never
    // reads or writes the registry, and the state is closed by whichever side lets go last.
    struct ExtensionInvokeJob
    {
        // Inputs: copies, the worker may outlive the caller
        std::string id;
        std::string itemId;
        /// What the script gets as ctx: {itemId, selection}
        json context;
        json params;
        Scripting::SandboxBudget budget;
        int invokeRef = LUA_NOREF;
        std::function<void(const std::string &)> hook;
        std::unique_ptr<Scripting::LuaSandbox> sandbox;

        // The conversation with the command thread, all under `mutex`
        struct Request
        {
            std::string op;
            json args;
        };
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
        /// The command thread gave up waiting: a request now fails at once and a call that was waiting stops waiting
        bool abandoned = false;
        std::optional<Request> request;
        std::optional<ExtensionHost::NestedAnswer> answer;

        // Worker thread only
        std::uint32_t nested = 0;
        std::uint32_t logLines = 0;
        std::uint64_t bytes = 0;

        // The result (read by the command thread once `done`)
        ExtensionHost::InvokeFailure failure = ExtensionHost::InvokeFailure::None;
        std::string error;
        json rawResult;
    };

    namespace
    {
        constexpr int MaxValueDepth = 64;
        constexpr std::size_t MaxValueNodes = 200000;
        // The address names the registry slot that holds the running invocation's job (a light userdata) for the host
        // function; empty while no menu item runs
        const char InvokeJobKey = 0;

        // A value of the script as JSON, with the bytes it will move. Raw stack calls only (lua_next, lua_type and the
        // conversions of values that need none), so nothing here can run a metamethod or allocate in the state. Throws
        // std::runtime_error for a value that is not data, or too big; nothing is left to clean up but the stack, which
        // the caller resets.
        json ReadLuaValue(lua_State *L, const int index, const int depth, std::size_t &nodes, std::uint64_t &bytes)
        {
            if (++nodes > MaxValueNodes)
            {
                throw std::runtime_error("the value has more than 200000 entries");
            }
            bytes += 8;
            switch (lua_type(L, index))
            {
            case LUA_TNIL:
                return nullptr;
            case LUA_TBOOLEAN:
                return lua_toboolean(L, index) != 0;
            case LUA_TNUMBER:
            {
                if (lua_isinteger(L, index) != 0)
                {
                    return static_cast<std::int64_t>(lua_tointeger(L, index));
                }
                const double number = static_cast<double>(lua_tonumber(L, index));
                if (!std::isfinite(number))
                {
                    throw std::runtime_error("a number that is NaN or infinite cannot be sent to the host");
                }
                return number;
            }
            case LUA_TSTRING:
            {
                std::size_t length = 0;
                const char *text = lua_tolstring(L, index, &length);
                bytes += length;
                return std::string(text, length);
            }
            case LUA_TTABLE:
                break;
            default:
                throw std::runtime_error(std::format("a {} cannot be sent to the host (only nil, booleans, numbers, strings "
                                                     "and tables of them can)",
                                                     lua_typename(L, lua_type(L, index))));
            }

            if (depth >= MaxValueDepth)
            {
                throw std::runtime_error("the value is nested deeper than 64 levels");
            }
            if (lua_checkstack(L, 8) == 0)
            {
                throw std::runtime_error("the value is nested too deeply");
            }
            const int table = lua_absindex(L, index);
            json object = json::object();
            std::map<std::int64_t, json> list;
            lua_pushnil(L);
            while (lua_next(L, table) != 0)
            {
                const int keyType = lua_type(L, -2);
                if (keyType == LUA_TSTRING)
                {
                    std::size_t length = 0;
                    const char *text = lua_tolstring(L, -2, &length);
                    bytes += length;
                    std::string key(text, length);
                    object[std::move(key)] = ReadLuaValue(L, -1, depth + 1, nodes, bytes);
                }
                else if (keyType == LUA_TNUMBER && lua_isinteger(L, -2) != 0)
                {
                    list[static_cast<std::int64_t>(lua_tointeger(L, -2))] = ReadLuaValue(L, -1, depth + 1, nodes, bytes);
                }
                else
                {
                    throw std::runtime_error("a table key is neither a string nor an integer");
                }
                lua_pop(L, 1);
            }
            if (list.empty())
            {
                return object; // an empty table is an empty object; a reader that wants a list takes it as an empty one
            }
            if (!object.empty())
            {
                throw std::runtime_error("a table mixes a list and named entries");
            }
            json array = json::array();
            std::int64_t expected = 1;
            for (auto &[key, value] : list)
            {
                if (key != expected++)
                {
                    throw std::runtime_error("a list has a gap (its keys must be 1, 2, 3 ...)");
                }
                array.push_back(std::move(value));
            }
            return array;
        }

        // An estimate of what a JSON value costs to move, for the invocation's byte cap (strings and keys by length, 8
        // bytes a node), without recursion
        std::uint64_t JsonBytes(const json &value)
        {
            std::uint64_t bytes = 0;
            std::vector<const json *> pending{&value};
            while (!pending.empty())
            {
                const json *item = pending.back();
                pending.pop_back();
                bytes += 8;
                if (item->is_string())
                {
                    bytes += item->get_ref<const std::string &>().size();
                }
                else if (item->is_object())
                {
                    for (auto it = item->begin(); it != item->end(); ++it)
                    {
                        bytes += it.key().size();
                        pending.push_back(&it.value());
                    }
                }
                else if (item->is_array())
                {
                    for (const json &child : *item)
                    {
                        pending.push_back(&child);
                    }
                }
            }
            return bytes;
        }

        // JSON as a Lua value on the stack: plain tables and strings, an object's null entries left out. The value is
        // first flattened, in C++ and outside any protected call, into a list of steps that point into the JSON; then a
        // C function run under lua_pcall (PushJson) carries the steps out with raw stack calls. A Lua error there (a
        // table the allocator refuses at the cap) leaves through that pcall past one frame that holds nothing with a
        // destructor: no nlohmann iterator, no string, no vector is alive across it.
        struct PushStep
        {
            enum class Kind : std::uint8_t
            {
                Nil,
                Bool,
                Integer,
                Number,
                String,
                /// lua_createtable with `count` list slots
                Array,
                /// lua_createtable with `count` record slots
                Object,
                /// The value on top goes into the table below it at index `integer`
                SetIndex,
                /// The key and the value on top go into the table below them
                SetField,
            };
            Kind kind = Kind::Nil;
            bool boolean = false;
            std::int64_t integer = 0;
            double number = 0.0;
            const char *text = nullptr;
            std::size_t count = 0;
        };

        void FlattenJson(const json &value, const int depth, std::vector<PushStep> &steps)
        {
            if (depth > MaxValueDepth + 8)
            {
                throw std::runtime_error("the answer is nested too deeply");
            }
            PushStep step;
            switch (value.type())
            {
            case json::value_t::boolean:
                step.kind = PushStep::Kind::Bool;
                step.boolean = value.get<bool>();
                steps.push_back(step);
                return;
            case json::value_t::number_integer:
                step.kind = PushStep::Kind::Integer;
                step.integer = value.get<std::int64_t>();
                steps.push_back(step);
                return;
            case json::value_t::number_unsigned:
                step.kind = PushStep::Kind::Number;
                step.number = static_cast<double>(value.get<std::uint64_t>());
                steps.push_back(step);
                return;
            case json::value_t::number_float:
                step.kind = PushStep::Kind::Number;
                step.number = value.get<double>();
                steps.push_back(step);
                return;
            case json::value_t::string:
            {
                const std::string &text = value.get_ref<const std::string &>();
                step.kind = PushStep::Kind::String;
                step.text = text.data();
                step.count = text.size();
                steps.push_back(step);
                return;
            }
            case json::value_t::array:
            {
                step.kind = PushStep::Kind::Array;
                step.count = std::min<std::size_t>(value.size(), 1u << 20);
                steps.push_back(step);
                std::int64_t slot = 1;
                for (const json &item : value)
                {
                    FlattenJson(item, depth + 1, steps);
                    PushStep set;
                    set.kind = PushStep::Kind::SetIndex;
                    set.integer = slot++;
                    steps.push_back(set);
                }
                return;
            }
            case json::value_t::object:
            {
                step.kind = PushStep::Kind::Object;
                step.count = std::min<std::size_t>(value.size(), 1u << 20);
                steps.push_back(step);
                for (auto it = value.begin(); it != value.end(); ++it)
                {
                    if (it.value().is_null())
                    {
                        continue;
                    }
                    const std::string &key = it.key();
                    PushStep keyStep;
                    keyStep.kind = PushStep::Kind::String;
                    keyStep.text = key.data();
                    keyStep.count = key.size();
                    steps.push_back(keyStep);
                    FlattenJson(it.value(), depth + 1, steps);
                    PushStep set;
                    set.kind = PushStep::Kind::SetField;
                    steps.push_back(set);
                }
                return;
            }
            default:
                steps.push_back(step); // nil
                return;
            }
        }

        // Runs under lua_pcall: only raw pointers and integers are alive here
        int PushStepsThunk(lua_State *L)
        {
            const auto *steps = static_cast<const std::vector<PushStep> *>(lua_touserdata(L, 1));
            const PushStep *step = steps->data();
            const PushStep *const end = step + steps->size();
            for (; step != end; ++step)
            {
                luaL_checkstack(L, 3, "the answer is nested too deeply");
                switch (step->kind)
                {
                case PushStep::Kind::Nil:
                    lua_pushnil(L);
                    break;
                case PushStep::Kind::Bool:
                    lua_pushboolean(L, step->boolean ? 1 : 0);
                    break;
                case PushStep::Kind::Integer:
                    lua_pushinteger(L, static_cast<lua_Integer>(step->integer));
                    break;
                case PushStep::Kind::Number:
                    lua_pushnumber(L, static_cast<lua_Number>(step->number));
                    break;
                case PushStep::Kind::String:
                    lua_pushlstring(L, step->text, step->count);
                    break;
                case PushStep::Kind::Array:
                    lua_createtable(L, static_cast<int>(step->count), 0);
                    break;
                case PushStep::Kind::Object:
                    lua_createtable(L, 0, static_cast<int>(step->count));
                    break;
                case PushStep::Kind::SetIndex:
                    lua_rawseti(L, -2, static_cast<lua_Integer>(step->integer));
                    break;
                case PushStep::Kind::SetField:
                    lua_rawset(L, -3);
                    break;
                }
            }
            return 1;
        }

        // Pushes `value` on the stack. False, with nothing pushed, when the state could not hold it. Throws
        // std::runtime_error for a value nested too deeply (before anything is pushed).
        bool PushJson(lua_State *L, const json &value)
        {
            std::vector<PushStep> steps;
            FlattenJson(value, 0, steps);
            lua_pushcfunction(L, &PushStepsThunk);
            lua_pushlightuserdata(L, &steps);
            if (lua_pcall(L, 1, 1, 0) != LUA_OK)
            {
                lua_pop(L, 1); // the error object
                return false;
            }
            return true;
        }

        ExtensionHost::NestedAnswer Refused(std::string text)
        {
            ExtensionHost::NestedAnswer answer;
            answer.ok = false;
            answer.error = std::move(text);
            return answer;
        }

        // The worker hands one request to the command thread and waits for its answer (or for the call to be abandoned)
        ExtensionHost::NestedAnswer RequestFromHost(ExtensionInvokeJob &job, std::string op, json args)
        {
            std::unique_lock lock(job.mutex);
            if (job.abandoned)
            {
                return Refused("the host stopped waiting for this call");
            }
            job.request = ExtensionInvokeJob::Request{std::move(op), std::move(args)};
            job.changed.notify_all();
            job.changed.wait(lock, [&job] { return job.answer.has_value() || job.abandoned; });
            if (!job.answer.has_value())
            {
                return Refused("the host stopped waiting for this call");
            }
            ExtensionHost::NestedAnswer answer = std::move(*job.answer);
            job.answer.reset();
            return answer;
        }

        // What one host call came to: how many results it left on the stack, or -1 with the text of the Lua error
        struct HostStep
        {
            int results = 0;
            std::string error;
        };

        HostStep PerformHostCall(lua_State *L)
        {
            HostStep step;
            const auto refuse = [&step](std::string text)
            {
                step.results = -1;
                step.error = std::move(text);
            };
            try
            {
                if (lua_type(L, 1) != LUA_TSTRING)
                {
                    refuse("the host call needs the name of an operation");
                    return step;
                }
                std::size_t length = 0;
                const char *text = lua_tolstring(L, 1, &length);
                const std::string op(text, length);

                ExtensionInvokeJob *job = nullptr;
                lua_rawgetp(L, LUA_REGISTRYINDEX, &InvokeJobKey);
                if (lua_type(L, -1) == LUA_TLIGHTUSERDATA)
                {
                    job = static_cast<ExtensionInvokeJob *>(lua_touserdata(L, -1));
                }
                lua_pop(L, 1);
                if (job == nullptr)
                {
                    if (op == "log")
                    {
                        return step; // while the file loads no menu item runs: there is nobody to write to
                    }
                    refuse("Editor.Scene can be used only while a menu item runs, not while its file loads");
                    return step;
                }

                json args = nullptr;
                if (lua_gettop(L) >= 2 && lua_type(L, 2) != LUA_TNIL)
                {
                    std::size_t nodes = 0;
                    std::uint64_t bytes = 0;
                    args = ReadLuaValue(L, 2, 0, nodes, bytes);
                    job->bytes += bytes;
                }

                if (op == "log")
                {
                    ++job->logLines;
                    if (job->logLines > ExtensionHost::MaxLogLines)
                    {
                        if (job->logLines > ExtensionHost::MaxLogLines + 1)
                        {
                            return step; // dropped
                        }
                        args = json{{"level", "warn"},
                                    {"text", std::format("(more than {} lines in one call: the rest are dropped)",
                                                         ExtensionHost::MaxLogLines)}};
                    }
                }
                else if (++job->nested > ExtensionHost::MaxNested())
                {
                    refuse(std::format("the call sent more than {} commands to the host", ExtensionHost::MaxNested()));
                    return step;
                }
                if (job->bytes > ExtensionHost::MaxInvocationBytes)
                {
                    refuse("the call moved more than 64 MiB between the script and the host");
                    return step;
                }

                const ExtensionHost::NestedAnswer answer = RequestFromHost(*job, op, std::move(args));
                if (!answer.ok)
                {
                    refuse(answer.error);
                    return step;
                }
                if (answer.value.is_null())
                {
                    return step;
                }
                job->bytes += JsonBytes(answer.value);
                if (job->bytes > ExtensionHost::MaxInvocationBytes)
                {
                    refuse("the call moved more than 64 MiB between the script and the host");
                    return step;
                }
                if (!PushJson(L, answer.value))
                {
                    refuse("the answer is too large for the script's memory");
                    return step;
                }
                step.results = 1;
            }
            catch (const std::exception &e)
            {
                refuse(e.what());
            }
            catch (...)
            {
                refuse("the host call failed");
            }
            return step;
        }

        // The one function the script reaches the host with (through the prelude's wrappers). It raises a Lua error,
        // positioned at the script's call, for anything the host refuses.
        int HostCall(lua_State *L)
        {
            // The message is pushed under a protected call (PushJson) while the C++ objects live; everything that can
            // raise a Lua error (a refused allocation included) runs after they are gone, so no error leaves past one
            bool pushed = false;
            {
                HostStep step = PerformHostCall(L);
                if (step.results >= 0)
                {
                    return step.results;
                }
                try
                {
                    pushed = PushJson(L, json(std::move(step.error)));
                }
                catch (...)
                {
                    pushed = false; // the host's own memory ran out: the fixed text below
                }
            }
            if (pushed)
            {
                luaL_where(L, 2);
                lua_insert(L, -2);
                lua_concat(L, 2);
            }
            else
            {
                lua_pushliteral(L, "the host refused the call");
            }
            return lua_error(L);
        }

        // The worker's whole job: the context, the call under the invocation budget, the result read back as JSON. Every
        // way it can go wrong is data in the job.
        void RunInvocation(ExtensionInvokeJob &job)
        {
            using namespace Scripting;
            using Failure = ExtensionHost::InvokeFailure;
            const auto fail = [&job](const Failure kind, std::string text)
            {
                job.failure = kind;
                job.error = std::move(text);
            };
            try
            {
                if (job.hook)
                {
                    job.hook(job.id);
                }
                {
                    const std::lock_guard lock(job.mutex);
                    if (job.abandoned)
                    {
                        fail(Failure::Abandoned, "The call was abandoned before it started");
                        return;
                    }
                }
                lua_State *L = job.sandbox != nullptr ? job.sandbox->State() : nullptr;
                if (L == nullptr)
                {
                    fail(Failure::HostError, "The extension's state is closed");
                    return;
                }
                // The job's slot exists since the file loaded (false), so setting it never allocates
                lua_settop(L, 0);
                lua_pushlightuserdata(L, &job);
                lua_rawsetp(L, LUA_REGISTRYINDEX, &InvokeJobKey);

                // The arguments are pushed under protected calls (PushJson): the state's cap may refuse them
                lua_rawgeti(L, LUA_REGISTRYINDEX, job.invokeRef);
                if (!PushJson(L, json(job.itemId)) || !PushJson(L, job.context) || !PushJson(L, job.params))
                {
                    lua_settop(L, 0);
                    lua_pushboolean(L, 0);
                    lua_rawsetp(L, LUA_REGISTRYINDEX, &InvokeJobKey);
                    fail(Failure::MemoryExhausted, "The extension's state has no room for the call's arguments");
                    return;
                }

                job.sandbox->Arm(job.budget);
                const int status = lua_pcall(L, 3, 1, 0);
                job.sandbox->Disarm();
                lua_pushboolean(L, 0);
                lua_rawsetp(L, LUA_REGISTRYINDEX, &InvokeJobKey);
                if (status != LUA_OK)
                {
                    const SandboxOutcome outcome = job.sandbox->FailureOutcome(status, "The menu item failed");
                    lua_settop(L, 0);
                    if (outcome.budgetExceeded)
                    {
                        fail(Failure::BudgetExceeded, outcome.error);
                    }
                    else if (outcome.memoryExhausted)
                    {
                        fail(Failure::MemoryExhausted, outcome.error);
                    }
                    else
                    {
                        fail(Failure::LuaError, outcome.error);
                    }
                    return;
                }
                try
                {
                    std::size_t nodes = 0;
                    std::uint64_t bytes = 0;
                    job.rawResult = ReadLuaValue(L, -1, 0, nodes, bytes);
                }
                catch (const std::exception &e)
                {
                    lua_settop(L, 0);
                    fail(Failure::BadResult, std::string("The menu item returned something that is not a result: ") + e.what());
                    return;
                }
                lua_settop(L, 0);
            }
            catch (const std::exception &e)
            {
                fail(Failure::HostError, std::string("The menu item could not be run: ") + e.what());
            }
            catch (...)
            {
                fail(Failure::HostError, "The menu item could not be run");
            }
        }

        std::atomic<std::uint64_t> g_invokeInstructionsOverride{0};
        std::atomic<std::uint64_t> g_invokeMillisOverride{0};
        std::atomic<std::int64_t> g_invokeGraceOverride{-1};
        std::atomic<std::uint32_t> g_maxNestedOverride{0};
        std::function<void(const std::string &)> g_invokeHook;

        std::function<void(const std::string &)> CurrentInvokeHook()
        {
            const std::lock_guard lock(g_hookMutex);
            return g_invokeHook;
        }

        Scripting::SandboxBudget CurrentInvocationBudget()
        {
            Scripting::SandboxBudget budget = Scripting::SandboxDefaults::InvocationBudget;
            if (const std::uint64_t instructions = g_invokeInstructionsOverride.load(); instructions != 0)
            {
                budget.instructions = instructions;
            }
            if (const std::uint64_t millis = g_invokeMillisOverride.load(); millis != 0)
            {
                budget.maxMillis = millis;
            }
            return budget;
        }

        std::uint64_t CurrentInvokeGraceMillis()
        {
            const std::int64_t value = g_invokeGraceOverride.load();
            return value < 0 ? ExtensionHost::InvokeGraceMillis : static_cast<std::uint64_t>(value);
        }
    }

    namespace
    {
        void Fail(ExtensionLoadJob &job, std::string text, const bool memoryExhausted = false)
        {
            job.ok = false;
            job.memoryExhausted = memoryExhausted;
            job.error = std::move(text);
            job.sandbox.reset();
        }
    }

    namespace
    {
        // Runs under lua_pcall with the prelude's two functions (1: the declaration reader, 2: the invoker): makes the
        // registry slot of the running invocation's job (false: no menu item runs), so an invocation later sets it
        // without an allocation, and references both functions. Returns the invoker's reference, then the reader's.
        int InstallHostCall(lua_State *L)
        {
            // Under lua_pcall: the global's key may need an allocation the state's cap refuses
            lua_pushcfunction(L, &HostCall);
            lua_setglobal(L, "__n2_host");
            return 0;
        }

        int RegisterPrelude(lua_State *L)
        {
            lua_pushboolean(L, 0);
            lua_rawsetp(L, LUA_REGISTRYINDEX, &InvokeJobKey);
            lua_pushvalue(L, 1);
            const int reader = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_pushvalue(L, 2);
            const int invoker = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_pushinteger(L, invoker);
            lua_pushinteger(L, reader);
            return 2;
        }

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
                SandboxOptions options;
                options.maxBytes = job.capBytes;
                std::unique_ptr<LuaSandbox> sandbox = LuaSandbox::Create(SandboxProfile::EditorExtension, options);
                if (sandbox == nullptr)
                {
                    Fail(job, "The extension's state could not be created");
                    return;
                }
                lua_State *L = sandbox->State();

                // The Editor namespace. The host's one entry point is a global the prelude takes into a local and removes
                // before any file code runs; the prelude returns the function that reads the declaration back and the
                // one that runs a menu item (on top)
                lua_pushcfunction(L, &InstallHostCall);
                if (const int status = lua_pcall(L, 0, 0, 0); status != LUA_OK)
                {
                    const SandboxOutcome failure = sandbox->FailureOutcome(status, "The editor API could not be set up");
                    lua_settop(L, 0);
                    Fail(job, failure.error, failure.memoryExhausted);
                    return;
                }
                if (const SandboxOutcome prelude = sandbox->RunChunk(EditorPrelude, "=editor-prelude",
                                                                      SandboxDefaults::LoadBudget, 2);
                    !prelude.ok)
                {
                    Fail(job, "The editor API could not be set up: " + prelude.error, prelude.memoryExhausted);
                    return;
                }
                // The two functions go into the registry, and the slot of the running invocation's job is made, all
                // under a protected call: each can allocate, and the state's cap may refuse it
                lua_pushcfunction(L, &RegisterPrelude);
                lua_insert(L, -3);
                if (const int status = lua_pcall(L, 2, 2, 0); status != LUA_OK)
                {
                    const SandboxOutcome failure = sandbox->FailureOutcome(status, "The editor API could not be set up");
                    lua_settop(L, 0);
                    Fail(job, failure.error, failure.memoryExhausted);
                    return;
                }
                const int reader = static_cast<int>(lua_tointeger(L, -1));
                const int invoker = static_cast<int>(lua_tointeger(L, -2));
                lua_settop(L, 0);

                // The file's chunk. Its name is the file's path inside editor/ (never a host path)
                if (const SandboxOutcome run = sandbox->RunChunk(job.source, "=" + job.id, SandboxDefaults::LoadBudget, 1);
                    !run.ok)
                {
                    Fail(job, run.error, run.memoryExhausted);
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
                    Fail(job, failure.error, failure.memoryExhausted);
                    return;
                }
                std::size_t nodes = 0;
                job.snapshot = ReadSnapshot(L, -1, 0, nodes);
                lua_settop(L, 0);
                luaL_unref(L, LUA_REGISTRYINDEX, reader);

                job.invokeRef = invoker;
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

    void ExtensionHost::SetReloadBudgetMillisForTest(const std::uint64_t millis)
    {
        g_reloadBudgetOverrideMillis.store(millis);
    }

    void ExtensionHost::SetMaxTotalBytesForTest(const std::size_t bytes)
    {
        g_totalBytesOverride.store(bytes);
    }

    std::uint64_t ExtensionHost::ReloadBudget()
    {
        const std::uint64_t value = g_reloadBudgetOverrideMillis.load();
        return value == 0 ? ReloadBudgetMillis : value;
    }

    std::size_t ExtensionHost::TotalBytesBudget()
    {
        const std::size_t value = g_totalBytesOverride.load();
        return value == 0 ? MaxTotalBytes : value;
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
            ServerLog::Warn("The project's editor folder is a link: no editor extensions are loaded from it");
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
                ServerLog::Warn(std::format("The project's editor folder has more than {} entries: the rest are not looked at",
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
                ServerLog::Warn("An editor extension file has a name that is not valid text and is not loaded");
                continue;
            }
            found.push_back({std::move(id), it->path()});
        }
        std::ranges::sort(found, [](const FoundFile &a, const FoundFile &b) { return a.id < b.id; });
        return found;
    }

    ExtensionHost::Entry ExtensionHost::LoadOne(const FoundFile &file, const std::size_t memoryLeft,
                                                const std::chrono::steady_clock::time_point deadline)
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

        // The Load's time is spent: nothing more starts, and the file is tried again at the next reload (nothing is
        // remembered about it)
        if (std::chrono::steady_clock::now() >= deadline)
        {
            fail(std::format("reload time budget exhausted: the reload's {} ms ran out before this file was reached; the "
                             "file is tried again at the next reload",
                             ReloadBudget()));
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

        // The states loaded before this file hold what they hold; this one gets the smaller of its own cap and the rest
        if (memoryLeft < Scripting::SandboxDefaults::MinBytes)
        {
            fail(std::format("extension memory budget exhausted: the extension states already loaded use the {} bytes "
                             "all of them may hold; the file is tried again at the next reload",
                             TotalBytesBudget()));
            return entry;
        }
        const std::size_t capBytes = std::min(Scripting::SandboxDefaults::MaxBytes, memoryLeft);

        auto job = std::make_shared<ExtensionLoadJob>();
        job->id = file.id;
        job->capBytes = capBytes;
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

        // The wait is the abandon deadline, cut short where the Load's own time ends
        const std::uint64_t abandonMillis = AbandonMillis();
        // Rounded up: the wait must not end a hair before the budget does, or the next file would still start
        const std::int64_t leftMillis = std::max<std::int64_t>(
            1, std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        const bool cutByBudget = static_cast<std::uint64_t>(leftMillis) < abandonMillis;
        const std::uint64_t waitMillis = cutByBudget ? static_cast<std::uint64_t>(leftMillis) : abandonMillis;
        {
            std::unique_lock lock(job->mutex);
            if (!job->finished.wait_for(lock, std::chrono::milliseconds(waitMillis), [&] { return job->done; }))
            {
                lock.unlock();
                // Abandoned: the worker still holds the job and frees it (and its state) when it ends; its result is
                // never read
                {
                    const std::lock_guard registry(_mutex);
                    _abandoned[file.id] = {entry.sha256, job};
                }
                if (cutByBudget)
                {
                    fail(std::format("reload time budget exhausted: the file was still loading when the reload's {} ms "
                                     "ran out and was abandoned; it is tried again at the next reload",
                                     ReloadBudget()));
                }
                else
                {
                    fail(std::format("Loading the file took longer than {} ms and was abandoned", abandonMillis));
                }
                return entry;
            }
        }

        if (!job->ok)
        {
            if (job->memoryExhausted && capBytes < Scripting::SandboxDefaults::MaxBytes)
            {
                // The cap that stopped it was the shrunk one: the total budget, not the file's own state
                fail(std::format("extension memory budget exhausted: the file needs more than the {} bytes left of the "
                                 "{} bytes all extension states may hold",
                                 capBytes, TotalBytesBudget()));
                return entry;
            }
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
            entry.invokeRef = job->invokeRef;
            entry.chargedBytes = entry.sandbox != nullptr ? entry.sandbox->Counters().peakBytes : 0;
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

    std::pair<bool, std::string> ExtensionHost::FormatLogLine(const Entry &entry)
    {
        if (entry.state == ExtensionState::Loaded)
        {
            std::string capabilities;
            for (const std::string &capability : entry.capabilities)
            {
                capabilities += (capabilities.empty() ? "" : ",") + capability;
            }
            return {true, std::format("Editor extension: {}@{} capabilities=[{}] menu items={} ({})", entry.name,
                                      entry.sha256.substr(0, 8), capabilities, entry.items.size(), entry.id)};
        }
        return {false, std::format("Editor extension {} did not load: {}", Printable(entry.id, 120),
                                   Printable(entry.error))};
    }

    void ExtensionHost::Load()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ReloadBudget());
        std::vector<FoundFile> files = Discover();
        if (files.size() > MaxFiles)
        {
            ServerLog::Warn(std::format("The project's editor folder has {} extension files: only the first {} (in name "
                                     "order) are loaded",
                                     files.size(), MaxFiles));
            files.resize(MaxFiles);
        }

        std::vector<Entry> entries;
        entries.reserve(files.size());
        std::map<std::string, std::string> itemOwners; // item id -> the extension that has it
        const std::size_t totalBytes = TotalBytesBudget();
        std::size_t memoryUsed = 0; // what the states kept so far held at their most
        for (const FoundFile &file : files)
        {
            Entry entry = LoadOne(file, totalBytes - std::min(memoryUsed, totalBytes), deadline);
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
                    memoryUsed += entry.chargedBytes; // a state dropped above (a clash) gives its share back
                }
            }
            entries.push_back(std::move(entry));
        }

        // The log lines are made from the registry under the mutex and written after it is released: nothing reads
        // _entries without the lock
        std::vector<std::pair<bool, std::string>> lines;
        std::size_t loaded = 0;
        std::size_t total = 0;
        std::vector<Entry> previous;
        {
            const std::lock_guard lock(_mutex);
            previous.swap(_entries);
            _entries = std::move(entries);
            ++_uiRevision;
            lines.reserve(_entries.size());
            for (const Entry &entry : _entries)
            {
                lines.push_back(FormatLogLine(entry));
                loaded += entry.state == ExtensionState::Loaded ? 1 : 0;
            }
            total = _entries.size();
        }
        // The old states close here, outside the registry's mutex

        for (const auto &[isLoaded, text] : lines)
        {
            if (isLoaded)
            {
                ServerLog::Info(text);
            }
            else
            {
                ServerLog::Warn(text);
            }
        }
        ServerLog::Info(std::format("Editor extensions: {} of {} loaded", loaded, total));
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

    std::size_t ExtensionHost::ChargedBytesOf(const std::string_view id) const
    {
        const std::lock_guard lock(_mutex);
        for (const Entry &entry : _entries)
        {
            if (entry.id == id)
            {
                return entry.state == ExtensionState::Failed ? 0 : entry.chargedBytes; // faulted keeps its state
            }
        }
        return 0;
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
            ServerLog::Warn(std::format("Editor extension {} is faulted until reload ({} failures within {} minutes)",
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

    // ==================== Invocation: the host's side ====================

    bool ExtensionHost::IsUuidText(const std::string_view text)
    {
        if (text.size() != 36)
        {
            return false;
        }
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != '-')
                {
                    return false;
                }
            }
            else if (!std::isxdigit(static_cast<unsigned char>(c)))
            {
                return false;
            }
        }
        return true;
    }

    namespace
    {
        // A text the client will show: control characters other than line breaks and tabs replaced, cut to `maxBytes`
        std::string CleanShownText(const std::string_view text, const std::size_t maxBytes)
        {
            std::string result(text.substr(0, maxBytes));
            for (char &c : result)
            {
                const auto byte = static_cast<unsigned char>(c);
                if ((byte < 0x20 && c != '\n' && c != '\t') || byte == 0x7F)
                {
                    c = '?';
                }
            }
            return result;
        }

        // What a menu item returned (design 4.1: { message, select, warnings }) as the ExtensionResultInfo of the wire:
        // each part left out when empty. Throws ShapeError for anything else.
        json ShapeInvokeResult(const json &raw)
        {
            json result = json::object();
            if (raw.is_null())
            {
                return result; // returned nothing
            }
            if (!raw.is_object())
            {
                Bad("", std::format("the result must be a table such as {{ message = \"...\" }}, not {}", TypeOf(raw)));
            }
            CheckKeys(raw, {"message", "select", "warnings"}, "result");
            if (const json *message = Find(raw, "message"))
            {
                if (!message->is_string())
                {
                    Bad("result", std::format("'message' must be a string, not {}", TypeOf(*message)));
                }
                if (const std::string text = CleanShownText(message->get_ref<const std::string &>(),
                                                            ExtensionHost::MaxResultTextBytes);
                    !text.empty())
                {
                    result["message"] = text;
                }
            }
            if (const json *select = Find(raw, "select"))
            {
                if (!IsList(*select))
                {
                    Bad("result", std::format("'select' must be a list of entity ids, not {}", TypeOf(*select)));
                }
                if (select->size() > ExtensionHost::MaxResultSelect)
                {
                    Bad("result", std::format("'select' has more than {} entries", ExtensionHost::MaxResultSelect));
                }
                json ids = json::array();
                for (const json &id : *select)
                {
                    if (!id.is_string() || !ExtensionHost::IsUuidText(id.get_ref<const std::string &>()))
                    {
                        Bad("result", "'select' must hold entity ids (UUID strings)");
                    }
                    ids.push_back(id);
                }
                if (!ids.empty())
                {
                    result["select"] = std::move(ids);
                }
            }
            if (const json *warnings = Find(raw, "warnings"))
            {
                if (!IsList(*warnings))
                {
                    Bad("result", std::format("'warnings' must be a list of strings, not {}", TypeOf(*warnings)));
                }
                if (warnings->size() > ExtensionHost::MaxResultWarnings)
                {
                    Bad("result", std::format("'warnings' has more than {} entries", ExtensionHost::MaxResultWarnings));
                }
                json texts = json::array();
                for (const json &warning : *warnings)
                {
                    if (!warning.is_string())
                    {
                        Bad("result", "'warnings' must be strings");
                    }
                    texts.push_back(CleanShownText(warning.get_ref<const std::string &>(), ExtensionHost::MaxResultTextBytes));
                }
                if (!texts.empty())
                {
                    result["warnings"] = std::move(texts);
                }
            }
            return result;
        }
    }

    void ExtensionHost::SetInvocationBudgetForTest(const std::uint64_t instructions, const std::uint64_t maxMillis)
    {
        g_invokeInstructionsOverride.store(instructions);
        g_invokeMillisOverride.store(maxMillis);
    }

    void ExtensionHost::SetInvokeGraceMillisForTest(const std::int64_t millis)
    {
        g_invokeGraceOverride.store(millis);
    }

    void ExtensionHost::SetMaxNestedCommandsForTest(const std::uint32_t limit)
    {
        g_maxNestedOverride.store(limit);
    }

    std::uint32_t ExtensionHost::MaxNested()
    {
        const std::uint32_t value = g_maxNestedOverride.load();
        return value == 0 ? MaxNestedCommands : value;
    }

    void ExtensionHost::SetInvokeHookForTest(std::function<void(const std::string &)> hook)
    {
        const std::lock_guard lock(g_hookMutex);
        g_invokeHook = std::move(hook);
    }

    std::uint64_t ExtensionHost::InvokeAbandonMillis()
    {
        return CurrentInvocationBudget().maxMillis + CurrentInvokeGraceMillis();
    }

    std::optional<ExtensionHost::MenuItemRef> ExtensionHost::FindMenuItem(const std::string_view itemId) const
    {
        const std::lock_guard lock(_mutex);
        for (const Entry &entry : _entries)
        {
            if (entry.state == ExtensionState::Failed)
            {
                continue;
            }
            for (const json &item : entry.items)
            {
                if (item.at("id").get_ref<const std::string &>() != itemId)
                {
                    continue;
                }
                MenuItemRef ref;
                ref.extensionId = entry.id;
                ref.extensionName = entry.name;
                ref.sha8 = entry.sha256.substr(0, 8);
                ref.state = entry.state;
                ref.capabilities = entry.capabilities;
                ref.itemId = std::string(itemId);
                ref.title = item.at("title").get<std::string>();
                ref.minRole = item.at("minRole").get<std::string>();
                ref.form = item.contains("form") ? item.at("form") : json(nullptr);
                return ref;
            }
        }
        return std::nullopt;
    }

    ExtensionState ExtensionHost::FaultNow(const std::string_view id, const std::string_view reason)
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
                entry.state = ExtensionState::Faulted;
                entry.error = Printable(std::format("Disabled until reload: {}", reason));
                ++_uiRevision;
                becameFaulted = true;
                state = entry.state;
                changed = _changed;
                revision = _uiRevision;
                break;
            }
        }
        if (becameFaulted)
        {
            ServerLog::Warn(std::format("Editor extension {} is faulted until reload ({})", Printable(std::string(id), 120),
                                     Printable(std::string(reason), 200)));
            if (changed)
            {
                changed(revision);
            }
        }
        return state;
    }

    ExtensionHost::InvokeOutcome ExtensionHost::Invoke(const MenuItemRef &item, const json &selection, const json &params,
                                                       const NestedHandler &handler)
    {
        InvokeOutcome outcome;
        const auto refuse = [this, &outcome](std::string text)
        {
            outcome.failure = InvokeFailure::HostError;
            outcome.error = Printable(StripHostPaths(std::move(text), _root));
            return outcome;
        };

        // A worker slot first, as a load takes one: at most MaxWorkers threads of extension code exist, abandoned ones
        // included
        WorkerCount &workers = SharedWorkerCount();
        {
            const std::lock_guard lock(workers.mutex);
            if (workers.live >= MaxWorkers)
            {
                const int live = workers.live;
                return refuse(std::format("{} extension calls are still running (abandoned ones count): try again later",
                                          live));
            }
            ++workers.live;
        }
        {
            const std::lock_guard lock(_tracker->mutex);
            ++_tracker->live;
        }
        const std::shared_ptr<Tracker> tracker = _tracker;
        const auto releaseSlot = [&workers, tracker]
        {
            {
                const std::lock_guard lock(workers.mutex);
                --workers.live;
            }
            {
                const std::lock_guard lock(tracker->mutex);
                --tracker->live;
            }
            tracker->changed.notify_all();
        };

        // The state moves into the job for the call
        auto job = std::make_shared<ExtensionInvokeJob>();
        bool stopping = false;
        {
            const std::lock_guard lock(_mutex);
            stopping = _stopping;
            if (!stopping)
            {
                for (Entry &entry : _entries)
                {
                    if (entry.id == item.extensionId)
                    {
                        if (entry.state == ExtensionState::Loaded && entry.sandbox != nullptr)
                        {
                            job->sandbox = std::move(entry.sandbox);
                            job->invokeRef = entry.invokeRef;
                        }
                        break;
                    }
                }
            }
        }
        if (job->sandbox == nullptr)
        {
            releaseSlot();
            return refuse(stopping ? "The host is shutting down" : "The extension is not loaded");
        }

        // The state goes back into the registry (when the entry is still there and still wants it)
        const auto giveBack = [this, id = item.extensionId](std::unique_ptr<Scripting::LuaSandbox> sandbox)
        {
            std::unique_ptr<Scripting::LuaSandbox> dropped;
            {
                const std::lock_guard lock(_mutex);
                for (Entry &entry : _entries)
                {
                    if (entry.id == id)
                    {
                        if (entry.sandbox == nullptr && entry.state != ExtensionState::Failed)
                        {
                            entry.sandbox = std::move(sandbox);
                        }
                        break;
                    }
                }
                dropped = std::move(sandbox);
            }
            // Whatever the registry did not take closes here, outside its mutex
        };

        const Scripting::SandboxBudget budget = CurrentInvocationBudget();
        job->id = item.extensionId;
        job->itemId = item.itemId;
        job->context = json{{"itemId", item.itemId}, {"selection", selection}};
        job->params = params;
        job->budget = budget;
        job->hook = CurrentInvokeHook();

        try
        {
            std::thread([job, tracker]
            {
                RunInvocation(*job);
                // The call is over: the worker stops counting before anyone is told, as a load worker does
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
                job->changed.notify_all();
            }).detach();
        }
        catch (...)
        {
            releaseSlot();
            giveBack(std::move(job->sandbox));
            return refuse("A thread could not be started to run the menu item");
        }

        // The conversation: the worker asks, the command thread answers, until the worker is done. A worker that is
        // silent for the wall budget plus the grace is abandoned.
        const std::chrono::milliseconds silenceLimit(InvokeAbandonMillis());
        bool abandoned = false;
        std::uint32_t answered = 0;
        {
            std::unique_lock lock(job->mutex);
            auto silentSince = std::chrono::steady_clock::now();
            for (;;)
            {
                const bool woke = job->changed.wait_until(lock, silentSince + silenceLimit,
                                                          [&job] { return job->done || job->request.has_value(); });
                if (!woke)
                {
                    abandoned = true;
                    job->abandoned = true;
                    job->changed.notify_all();
                    break;
                }
                if (job->done)
                {
                    break;
                }
                ExtensionInvokeJob::Request request = std::move(*job->request);
                job->request.reset();
                lock.unlock();
                NestedAnswer answer;
                try
                {
                    answer = handler(request.op, request.args);
                }
                catch (const std::exception &e)
                {
                    answer = Refused(std::string("the host could not do it: ") + e.what());
                }
                catch (...)
                {
                    answer = Refused("the host could not do it");
                }
                if (request.op != "log")
                {
                    ++answered;
                }
                lock.lock();
                job->answer = std::move(answer);
                job->changed.notify_all();
                silentSince = std::chrono::steady_clock::now();
            }
        }
        outcome.nestedCommands = answered;

        // The state: back into the registry unless the worker still has it
        if (!abandoned)
        {
            giveBack(std::move(job->sandbox));
        }

        InvokeFailure failure = abandoned ? InvokeFailure::Abandoned : job->failure;
        std::string text = abandoned ? std::format("The menu item did not answer within {} ms and was abandoned",
                                                   silenceLimit.count())
                                     : job->error;
        if (failure == InvokeFailure::None)
        {
            try
            {
                outcome.result = ShapeInvokeResult(job->rawResult);
            }
            catch (const ShapeError &e)
            {
                failure = InvokeFailure::BadResult;
                text = std::string("The menu item returned something that is not a result: ") + e.what();
            }
        }

        outcome.failure = failure;
        if (failure == InvokeFailure::None)
        {
            return outcome;
        }
        outcome.result = json::object();
        outcome.error = Printable(StripHostPaths(std::move(text), _root));
        switch (failure)
        {
        case InvokeFailure::LuaError:
        case InvokeFailure::BadResult:
            outcome.faulted = NoteFailure(item.extensionId) == ExtensionState::Faulted;
            break;
        case InvokeFailure::BudgetExceeded:
            outcome.faulted = FaultNow(item.extensionId, "the menu item ran out of its budget") == ExtensionState::Faulted;
            break;
        case InvokeFailure::MemoryExhausted:
            outcome.faulted = FaultNow(item.extensionId, "the menu item used all the memory of its state") ==
                              ExtensionState::Faulted;
            break;
        case InvokeFailure::Abandoned:
            outcome.faulted = FaultNow(item.extensionId, "the menu item stopped answering and was abandoned") ==
                              ExtensionState::Faulted;
            break;
        case InvokeFailure::HostError:
        case InvokeFailure::None:
            break;
        }
        return outcome;
    }
}
