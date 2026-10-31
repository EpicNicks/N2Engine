#include "engine/replay/GoldenReplay.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <string_view>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <math/DeterministicMath.hpp>
#include <math/DeterministicRandom.hpp>
#include <math/FloatEnvironment.hpp>
#include <math/Vector3.hpp>

#include "engine/scripting/LuaDeterministic.hpp"

namespace N2Engine::Replay
{
    namespace
    {
        constexpr float kStep = 1.0f / 60.0f;

        // 64-bit FNV-1a over bytes: no std::hash and no library call whose result may differ between platforms. A NaN is
        // hashed as one fixed pattern (the payload and sign of a NaN differ between x86 and arm).
        class Fnv
        {
        public:
            void Byte(const std::uint8_t value)
            {
                _hash ^= static_cast<std::uint64_t>(value);
                _hash *= 1099511628211ULL;
            }
            void U32(const std::uint32_t value)
            {
                for (int i = 0; i < 4; ++i)
                {
                    Byte(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
                }
            }
            void U64(const std::uint64_t value)
            {
                for (int i = 0; i < 8; ++i)
                {
                    Byte(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
                }
            }
            void Float(const float value)
            {
                U32(std::isnan(value) ? 0x7FC00000u : std::bit_cast<std::uint32_t>(value));
            }
            void Double(const double value)
            {
                U64(std::isnan(value) ? 0x7FF8000000000000ULL : std::bit_cast<std::uint64_t>(value));
            }
            void Text(const std::string &text)
            {
                for (const char c : text)
                {
                    Byte(static_cast<std::uint8_t>(c));
                }
                Byte(0xFFu);
            }
            [[nodiscard]] std::uint64_t Value() const { return _hash; }

        private:
            std::uint64_t _hash = 14695981039346656037ULL;
        };

        // Snapshot bytes: little-endian, written byte by byte (no memcpy of a struct, no host byte order)
        class Writer
        {
        public:
            void U8(const std::uint8_t value) { _bytes.push_back(value); }
            void U32(const std::uint32_t value)
            {
                for (int i = 0; i < 4; ++i)
                {
                    _bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
                }
            }
            void U64(const std::uint64_t value)
            {
                for (int i = 0; i < 8; ++i)
                {
                    _bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
                }
            }
            void Float(const float value) { U32(std::bit_cast<std::uint32_t>(value)); }
            void Double(const double value) { U64(std::bit_cast<std::uint64_t>(value)); }
            void Text(const std::string_view text)
            {
                U32(static_cast<std::uint32_t>(text.size()));
                _bytes.insert(_bytes.end(), text.begin(), text.end());
            }
            [[nodiscard]] std::vector<std::uint8_t> &Bytes() { return _bytes; }

        private:
            std::vector<std::uint8_t> _bytes;
        };

        /// Reads with every length checked against the bytes present; the first failure sticks and every later read
        /// returns zeros, so a caller checks Ok() once after a group of reads
        class Reader
        {
        public:
            explicit Reader(const std::span<const std::uint8_t> bytes) : _bytes(bytes) {}
            [[nodiscard]] bool Ok() const { return _ok; }
            [[nodiscard]] std::size_t Remaining() const { return _bytes.size() - _pos; }
            [[nodiscard]] bool AtEnd() const { return _ok && _pos == _bytes.size(); }
            std::uint8_t U8()
            {
                if (!Take(1))
                {
                    return 0;
                }
                return _bytes[_pos - 1];
            }
            std::uint32_t U32()
            {
                if (!Take(4))
                {
                    return 0;
                }
                std::uint32_t value = 0;
                for (std::size_t i = 0; i < 4; ++i)
                {
                    value |= static_cast<std::uint32_t>(_bytes[_pos - 4 + i]) << (8 * i);
                }
                return value;
            }
            std::uint64_t U64()
            {
                if (!Take(8))
                {
                    return 0;
                }
                std::uint64_t value = 0;
                for (std::size_t i = 0; i < 8; ++i)
                {
                    value |= static_cast<std::uint64_t>(_bytes[_pos - 8 + i]) << (8 * i);
                }
                return value;
            }
            float Float() { return std::bit_cast<float>(U32()); }
            double Double() { return std::bit_cast<double>(U64()); }
            std::string Text(const std::size_t maxBytes)
            {
                const std::uint32_t size = U32();
                if (!_ok || size > maxBytes || !Take(size))
                {
                    _ok = false;
                    return {};
                }
                return std::string(reinterpret_cast<const char *>(_bytes.data() + _pos - size), size);
            }

        private:
            bool Take(const std::size_t count)
            {
                if (!_ok || count > _bytes.size() - _pos)
                {
                    _ok = false;
                    return false;
                }
                _pos += count;
                return true;
            }

            std::span<const std::uint8_t> _bytes;
            std::size_t _pos = 0;
            bool _ok = true;
        };

        constexpr std::uint32_t kSnapshotFormat = 1;
        constexpr std::size_t kMaxSceneNameBytes = 64;
        constexpr std::size_t kMaxLuaDepth = 16;
        constexpr std::size_t kMaxLuaEntries = 1u << 20;
        constexpr std::size_t kMaxLuaStringBytes = 1u << 20;
        constexpr std::size_t kMaxGlobals = 256;

        std::uint64_t MakeIdentity(const std::string_view layout, const std::string_view script)
        {
            Fnv fnv;
            fnv.Text(std::string(layout));
            fnv.Text(std::string(script));
            return fnv.Value();
        }

        // A key of a saved Lua table. Keys sort in one fixed order (numbers by value, an integer before an equal float,
        // then strings bytewise): the order they are written, and the only order a blob may hold them in
        struct LuaKey
        {
            enum Kind : int
            {
                Integer = 0,
                Float = 1,
                String = 2,
            };
            Kind kind = Integer;
            long long integer = 0;
            double number = 0;
            std::string text;
        };

        bool KeyLess(const LuaKey &a, const LuaKey &b)
        {
            const bool aNumber = a.kind != LuaKey::String;
            const bool bNumber = b.kind != LuaKey::String;
            if (aNumber != bNumber)
            {
                return aNumber;
            }
            if (!aNumber)
            {
                return a.text < b.text; // std::string compares its bytes as unsigned char
            }
            const double av = a.kind == LuaKey::Integer ? static_cast<double>(a.integer) : a.number;
            const double bv = b.kind == LuaKey::Integer ? static_cast<double>(b.integer) : b.number;
            if (av != bv)
            {
                return av < bv;
            }
            if (a.kind == LuaKey::Integer && b.kind == LuaKey::Integer)
            {
                return a.integer < b.integer;
            }
            return a.kind < b.kind;
        }

        enum LuaTag : std::uint8_t
        {
            TagInteger = 1,
            TagFloat = 2,
            TagString = 3,
            TagTable = 4,
            TagFalse = 5,
            TagTrue = 6,
        };

        void WriteKey(Writer &w, const LuaKey &key)
        {
            switch (key.kind)
            {
            case LuaKey::Integer:
                w.U8(TagInteger);
                w.U64(static_cast<std::uint64_t>(key.integer));
                break;
            case LuaKey::Float:
                w.U8(TagFloat);
                w.Double(key.number);
                break;
            case LuaKey::String:
                w.U8(TagString);
                w.Text(key.text);
                break;
            }
        }

        bool ReadKey(Reader &r, LuaKey &key)
        {
            const std::uint8_t tag = r.U8();
            if (tag == TagInteger)
            {
                key.kind = LuaKey::Integer;
                key.integer = static_cast<long long>(r.U64());
            }
            else if (tag == TagFloat)
            {
                key.kind = LuaKey::Float;
                key.number = r.Double();
                lua_Integer asInteger = 0;
                // NaN is no key, and a float with an integer value is the same key as that integer (Lua normalises it)
                if (std::isnan(key.number) || lua_numbertointeger(key.number, &asInteger))
                {
                    return false;
                }
            }
            else if (tag == TagString)
            {
                key.kind = LuaKey::String;
                key.text = r.Text(kMaxLuaStringBytes);
            }
            else
            {
                return false;
            }
            return r.Ok();
        }

        void PushKey(lua_State *L, const LuaKey &key)
        {
            switch (key.kind)
            {
            case LuaKey::Integer:
                lua_pushinteger(L, static_cast<lua_Integer>(key.integer));
                break;
            case LuaKey::Float:
                lua_pushnumber(L, static_cast<lua_Number>(key.number));
                break;
            case LuaKey::String:
                lua_pushlstring(L, key.text.data(), key.text.size());
                break;
            }
        }

        bool WriteLuaValue(lua_State *L, int index, Writer &w, std::set<const void *> &seen, const std::size_t depth,
                           std::size_t &entries, std::string &error)
        {
            index = lua_absindex(L, index);
            switch (lua_type(L, index))
            {
            case LUA_TBOOLEAN:
                w.U8(lua_toboolean(L, index) != 0 ? TagTrue : TagFalse);
                return true;
            case LUA_TNUMBER:
                if (lua_isinteger(L, index) != 0)
                {
                    w.U8(TagInteger);
                    w.U64(static_cast<std::uint64_t>(lua_tointeger(L, index)));
                }
                else
                {
                    w.U8(TagFloat);
                    w.Double(static_cast<double>(lua_tonumber(L, index)));
                }
                return true;
            case LUA_TSTRING:
            {
                std::size_t size = 0;
                const char *text = lua_tolstring(L, index, &size);
                if (size > kMaxLuaStringBytes)
                {
                    error = "a Lua string is too long to save";
                    return false;
                }
                w.U8(TagString);
                w.Text(std::string_view(text, size));
                return true;
            }
            case LUA_TTABLE:
                break;
            default:
                error = std::string("the Lua state holds a ") + luaL_typename(L, index) + ", which a snapshot cannot hold";
                return false;
            }

            if (depth >= kMaxLuaDepth)
            {
                error = "a Lua table is nested too deeply to save";
                return false;
            }
            if (!seen.insert(lua_topointer(L, index)).second)
            {
                error = "a Lua table is reachable twice (shared or cyclic), which a snapshot cannot hold";
                return false;
            }
            if (lua_getmetatable(L, index) != 0)
            {
                lua_pop(L, 1);
                error = "a Lua table has a metatable, which a snapshot cannot hold";
                return false;
            }
            if (lua_checkstack(L, 4) == 0)
            {
                error = "the Lua stack is exhausted";
                return false;
            }
            // Collect the keys (in whatever order lua_next gives them), then write them in the canonical order
            std::vector<LuaKey> keys;
            lua_pushnil(L);
            while (lua_next(L, index) != 0)
            {
                lua_pop(L, 1); // the value; the key stays for lua_next
                LuaKey key;
                const int type = lua_type(L, -1);
                if (type == LUA_TNUMBER && lua_isinteger(L, -1) != 0)
                {
                    key.kind = LuaKey::Integer;
                    key.integer = static_cast<long long>(lua_tointeger(L, -1));
                }
                else if (type == LUA_TNUMBER)
                {
                    key.kind = LuaKey::Float;
                    key.number = static_cast<double>(lua_tonumber(L, -1));
                }
                else if (type == LUA_TSTRING)
                {
                    std::size_t size = 0;
                    const char *text = lua_tolstring(L, -1, &size);
                    key.kind = LuaKey::String;
                    key.text.assign(text, size);
                }
                else
                {
                    error = std::string("a Lua table has a ") + lua_typename(L, type) + " key, which a snapshot cannot hold";
                    lua_pop(L, 1);
                    return false;
                }
                keys.push_back(std::move(key));
                if (++entries > kMaxLuaEntries)
                {
                    error = "the Lua state is too large to save";
                    lua_pop(L, 1);
                    return false;
                }
            }
            std::sort(keys.begin(), keys.end(), KeyLess);
            w.U8(TagTable);
            w.U32(static_cast<std::uint32_t>(keys.size()));
            for (const LuaKey &key : keys)
            {
                WriteKey(w, key);
                PushKey(L, key);
                lua_rawget(L, index);
                const bool ok = WriteLuaValue(L, -1, w, seen, depth + 1, entries, error);
                lua_pop(L, 1);
                if (!ok)
                {
                    return false;
                }
            }
            return true;
        }

        /// Reads one value and leaves it on the Lua stack. False for a malformed or non-canonical value (the stack is then
        /// in an unspecified state: the caller throws the scene away).
        bool ReadLuaValue(lua_State *L, Reader &r, const std::size_t depth, std::size_t &entries)
        {
            const std::uint8_t tag = r.U8();
            if (!r.Ok() || lua_checkstack(L, 6) == 0)
            {
                return false;
            }
            switch (tag)
            {
            case TagInteger:
                lua_pushinteger(L, static_cast<lua_Integer>(r.U64()));
                return r.Ok();
            case TagFloat:
                lua_pushnumber(L, static_cast<lua_Number>(r.Double()));
                return r.Ok();
            case TagString:
            {
                const std::string text = r.Text(kMaxLuaStringBytes);
                if (!r.Ok())
                {
                    return false;
                }
                lua_pushlstring(L, text.data(), text.size());
                return true;
            }
            case TagFalse:
                lua_pushboolean(L, 0);
                return true;
            case TagTrue:
                lua_pushboolean(L, 1);
                return true;
            case TagTable:
                break;
            default:
                return false;
            }
            if (depth >= kMaxLuaDepth)
            {
                return false;
            }
            const std::uint32_t count = r.U32();
            // Every entry is at least two bytes: a count beyond that cannot be honest
            if (!r.Ok() || count > r.Remaining() / 2)
            {
                return false;
            }
            entries += count;
            if (entries > kMaxLuaEntries)
            {
                return false;
            }
            lua_createtable(L, 0, static_cast<int>(std::min<std::uint32_t>(count, 1024)));
            LuaKey previous;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                LuaKey key;
                if (!ReadKey(r, key) || (i > 0 && !KeyLess(previous, key)))
                {
                    return false; // malformed, or a duplicate or out-of-order key
                }
                PushKey(L, key);
                if (!ReadLuaValue(L, r, depth + 1, entries))
                {
                    return false;
                }
                lua_rawset(L, -3);
                previous = std::move(key);
            }
            return true;
        }

        /// What a slot holds right now (the last input row at or before the tick)
        struct Held
        {
            int moveX = 0;
            int moveY = 0;
            std::uint32_t buttons = 0;
        };
        using HeldInputs = std::array<Held, kMaxSlots>;

        class Scene
        {
        public:
            virtual ~Scene() = default;
            /// Whether the scene was built (a Lua scene fails when its script does)
            [[nodiscard]] virtual bool Ready(std::string &error) const
            {
                (void)error;
                return true;
            }
            virtual bool Step(int tick, const HeldInputs &inputs, std::string &error) = 0;
            virtual void Hash(Fnv &fnv) = 0;
            /// What defines the scene: its layout tag and script text. A blob of another definition is refused.
            [[nodiscard]] virtual std::uint64_t Identity() const = 0;
            /// The scene's state after the ticks run (see ReplaySimulation::Save); false with `error` for state it cannot hold
            virtual bool Save(Writer &writer, std::string &error) = 0;
            /// Reads the state Save wrote into this (freshly built) scene; false for anything wrong with the bytes
            virtual bool Load(Reader &reader, std::string &error) = 0;
        };

        // ---------------------------------------------------------------------------------------------------------
        // "transforms": float code, engine Sin/Cos, DeterministicRandom
        // ---------------------------------------------------------------------------------------------------------
        class TransformsScene final : public Scene
        {
        public:
            explicit TransformsScene(const std::uint32_t seed) : _rng(seed)
            {
                for (int i = 0; i < kCount; ++i)
                {
                    // One draw per statement: the order in which a call's arguments are evaluated is unspecified
                    const float x = _rng.Range(-10.0f, 10.0f);
                    const float y = _rng.Range(0.0f, 5.0f);
                    const float z = _rng.Range(-10.0f, 10.0f);
                    _position[i] = Math::Vector3(x, y, z);
                    _yaw[i] = _rng.Range(0.0f, 6.0f);
                }
            }

            bool Step(const int tick, const HeldInputs &inputs, std::string &) override
            {
                const Held &held = inputs[0];
                const float dx = static_cast<float>(held.moveX) * (1.0f / 127.0f);
                const float dy = static_cast<float>(held.moveY) * (1.0f / 127.0f);
                const bool kick = (held.buttons & 1u) != 0;
                for (int i = 0; i < kCount; ++i)
                {
                    float turn = 0.01f * static_cast<float>(i + 1) + 0.05f * dx;
                    if (kick)
                    {
                        const float extra = _rng.Range(0.0f, 1.0f);
                        turn += extra;
                    }
                    _yaw[i] += turn;
                    const float s = Math::Sin(_yaw[i]);
                    const float c = Math::Cos(_yaw[i]);
                    const float speed = 3.0f + 2.0f * dy;
                    _position[i].x += c * speed * kStep;
                    _position[i].z += s * speed * kStep;
                    float lift = 0.0f;
                    if (tick % 7 == 0)
                    {
                        lift = _rng.Range(-0.01f, 0.01f);
                    }
                    _position[i].y += lift;
                }
                return true;
            }

            void Hash(Fnv &fnv) override
            {
                for (int i = 0; i < kCount; ++i)
                {
                    fnv.Float(_position[i].x);
                    fnv.Float(_position[i].y);
                    fnv.Float(_position[i].z);
                    fnv.Float(_yaw[i]);
                }
            }

            [[nodiscard]] std::uint64_t Identity() const override { return MakeIdentity("transforms/1", ""); }

            bool Save(Writer &writer, std::string &) override
            {
                writer.U32(_rng.State());
                for (int i = 0; i < kCount; ++i)
                {
                    writer.Float(_position[i].x);
                    writer.Float(_position[i].y);
                    writer.Float(_position[i].z);
                    writer.Float(_yaw[i]);
                }
                return true;
            }

            bool Load(Reader &reader, std::string &error) override
            {
                _rng.Seed(reader.U32());
                for (int i = 0; i < kCount; ++i)
                {
                    const float x = reader.Float();
                    const float y = reader.Float();
                    const float z = reader.Float();
                    _position[i] = Math::Vector3(x, y, z);
                    _yaw[i] = reader.Float();
                }
                if (!reader.Ok())
                {
                    error = "the transforms state is truncated";
                    return false;
                }
                return true;
            }

        private:
            static constexpr int kCount = 24;
            Math::DeterministicRandom _rng;
            std::array<Math::Vector3, kCount> _position;
            std::array<float, kCount> _yaw{};
        };

        // ---------------------------------------------------------------------------------------------------------
        // The Lua scenes: a state with only the base, math, string and table libraries, the engine math shims, Ordered.pairs
        // and a seeded engine random stream; math.random is removed
        // ---------------------------------------------------------------------------------------------------------
        class LuaScene : public Scene
        {
        public:
            LuaScene(const std::uint32_t seed, const char *script) : _rng(seed)
            {
                _lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
                Scripting::InstallDeterministicMath(_lua);
                Scripting::InstallOrderedPairs(_lua);
                _lua["math"]["random"] = sol::lua_nil;
                _lua["math"]["randomseed"] = sol::lua_nil;
                _lua.set_function("rng_float", [this]() { return static_cast<double>(_rng.NextFloat()); });
                _lua.set_function("rng_range",
                                  [this](const double lo, const double hi) {
                                      return static_cast<double>(_rng.Range(static_cast<float>(lo), static_cast<float>(hi)));
                                  });
                _lua.set_function("rng_int", [this](const int lo, const int hi) { return _rng.NextInt(lo, hi); });
                // Every global that exists before the script runs belongs to the sandbox, not to the simulation state
                for (std::string &name : GlobalNames(false))
                {
                    _baseline.insert(std::move(name));
                }
                const sol::protected_function_result result = _lua.safe_script(script, sol::script_pass_on_error);
                if (!result.valid())
                {
                    const sol::error err = result;
                    _error = std::string("script: ") + err.what();
                }
            }

            [[nodiscard]] bool Ready(std::string &error) const override
            {
                if (!_error.empty())
                {
                    error = _error;
                    return false;
                }
                return true;
            }

        protected:
            /// The names of the string-keyed globals in ascending byte order, functions left out when `stateOnly`
            /// (the script's own functions are rebuilt by running the script), the sandbox's names too
            std::vector<std::string> GlobalNames(const bool stateOnly)
            {
                lua_State *const L = _lua.lua_state();
                std::vector<std::string> names;
                lua_pushglobaltable(L);
                lua_pushnil(L);
                while (lua_next(L, -2) != 0)
                {
                    const bool isFunction = lua_type(L, -1) == LUA_TFUNCTION;
                    lua_pop(L, 1);
                    if (lua_type(L, -1) == LUA_TSTRING)
                    {
                        std::size_t size = 0;
                        const char *text = lua_tolstring(L, -1, &size);
                        std::string name(text, size);
                        if (!stateOnly || (!isFunction && !_baseline.contains(name)))
                        {
                            names.push_back(std::move(name));
                        }
                    }
                }
                lua_pop(L, 1);
                std::sort(names.begin(), names.end());
                return names;
            }

            /// The script's state: the DeterministicRandom position, then every non-function global the script defined
            bool SaveLuaState(Writer &writer, std::string &error)
            {
                lua_State *const L = _lua.lua_state();
                writer.U32(_rng.State());
                const std::vector<std::string> names = GlobalNames(true);
                writer.U32(static_cast<std::uint32_t>(names.size()));
                std::set<const void *> seen;
                std::size_t entries = 0;
                for (const std::string &name : names)
                {
                    writer.Text(name);
                    lua_getglobal(L, name.c_str());
                    const bool ok = WriteLuaValue(L, -1, writer, seen, 0, entries, error);
                    lua_pop(L, 1);
                    if (!ok)
                    {
                        error = "global '" + name + "': " + error;
                        return false;
                    }
                }
                return true;
            }

            /// Into a freshly built scene: the script's own state globals are replaced by the blob's, nothing else is touched
            bool LoadLuaState(Reader &reader, std::string &error)
            {
                lua_State *const L = _lua.lua_state();
                const int top = lua_gettop(L);
                _rng.Seed(reader.U32());
                const std::uint32_t count = reader.U32();
                if (!reader.Ok() || count > kMaxGlobals)
                {
                    error = "the Lua state is malformed";
                    return false;
                }
                for (const std::string &name : GlobalNames(true))
                {
                    lua_pushnil(L);
                    lua_setglobal(L, name.c_str());
                }
                std::string previous;
                std::size_t entries = 0;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const std::string name = reader.Text(256);
                    lua_getglobal(L, name.c_str());
                    const bool clash = lua_type(L, -1) == LUA_TFUNCTION;
                    lua_pop(L, 1);
                    // Ascending and unique: the one canonical order; no name of the sandbox, no function of the script
                    if (!reader.Ok() || (i > 0 && name <= previous) || _baseline.contains(name) || clash ||
                        !ReadLuaValue(L, reader, 0, entries))
                    {
                        lua_settop(L, top);
                        error = "the Lua state is malformed";
                        return false;
                    }
                    lua_setglobal(L, name.c_str());
                    previous = name;
                }
                lua_settop(L, top);
                return true;
            }

            sol::table MakeInputs(const HeldInputs &inputs)
            {
                sol::table table = _lua.create_table();
                for (int slot = 0; slot < kMaxSlots; ++slot)
                {
                    const Held &held = inputs[static_cast<std::size_t>(slot)];
                    table[slot + 1] = _lua.create_table_with("x", held.moveX, "y", held.moveY, "buttons",
                                                             static_cast<long long>(held.buttons));
                }
                return table;
            }

            sol::state _lua;
            Math::DeterministicRandom _rng;
            std::string _error;
            std::set<std::string> _baseline;
        };

        constexpr const char *kLuaSimScript = R"LUA(
entities = {}
counters = { spawned = 0, removed = 0, [1] = 0, [2] = 0, [3] = 0 }

function init()
    for i = 1, 12 do
        local x = rng_range(-50, 50)
        local y = rng_range(-50, 50)
        local hp = 100 + rng_int(0, 20)
        entities["e" .. i] = { x = x, y = y, vx = 0, vy = 0, hp = hp }
    end
end

function step(tick, inputs)
    local p = inputs[1]
    local tx = p.x / 127
    local ty = p.y / 127
    local boost = 1.0
    if (p.buttons & 1) ~= 0 then boost = 2.0 end
    local dead = {}
    for name, e in Ordered.pairs(entities) do
        local heading = math.atan(ty * 40 - e.y, tx * 40 - e.x)
        local jitter = rng_range(-0.2, 0.2)
        local speed = boost * (0.5 + math.exp(-e.hp / 60))
        e.vx = e.vx * 0.9 + math.cos(heading + jitter) * speed * 0.1
        e.vy = e.vy * 0.9 + math.sin(heading + jitter) * speed * 0.1
        e.x = e.x + e.vx
        e.y = e.y + e.vy
        local wear = rng_range(0, 1)
        e.hp = e.hp - wear * 0.3 * math.log(2 + math.abs(e.x))
        if e.hp <= 0 then dead[#dead + 1] = name end
    end
    for i = 1, #dead do
        entities[dead[i]] = nil
        counters.removed = counters.removed + 1
    end
    if tick % 25 == 0 then
        local x = rng_range(-10, 10)
        local y = rng_range(-10, 10)
        local hp = 40 + rng_int(0, 40)
        entities["s" .. tick] = { x = x, y = y, vx = 0, vy = 0, hp = hp }
        counters.spawned = counters.spawned + 1
    end
    local slot = 1 + tick % 3
    counters[slot] = counters[slot] + 1
end

function dump()
    local out = {}
    local n = 0
    local function push(v) n = n + 1; out[n] = v end
    for name, e in Ordered.pairs(entities) do
        for i = 1, #name do push(string.byte(name, i)) end
        push(e.x) push(e.y) push(e.vx) push(e.vy) push(e.hp)
    end
    for k, v in Ordered.pairs(counters) do
        if type(k) == "number" then
            push(k)
        else
            for i = 1, #k do push(string.byte(k, i)) end
        end
        push(v)
    end
    return out
end

init()
)LUA";

        class LuaSimScene final : public LuaScene
        {
        public:
            explicit LuaSimScene(const std::uint32_t seed) : LuaScene(seed, kLuaSimScript) {}

            [[nodiscard]] std::uint64_t Identity() const override { return MakeIdentity("lua_sim/1", kLuaSimScript); }
            bool Save(Writer &writer, std::string &error) override { return SaveLuaState(writer, error); }
            bool Load(Reader &reader, std::string &error) override { return LoadLuaState(reader, error); }

            bool Step(const int tick, const HeldInputs &inputs, std::string &error) override
            {
                sol::protected_function step = _lua["step"];
                const sol::protected_function_result result = step(tick, MakeInputs(inputs));
                if (!result.valid())
                {
                    const sol::error err = result;
                    error = std::string("step: ") + err.what();
                    return false;
                }
                return true;
            }

            void Hash(Fnv &fnv) override
            {
                sol::protected_function dump = _lua["dump"];
                const sol::protected_function_result result = dump();
                if (!result.valid())
                {
                    fnv.U32(0xDEADu);
                    return;
                }
                const sol::table values = result;
                const std::size_t count = values.size();
                fnv.U64(count);
                for (std::size_t i = 1; i <= count; ++i)
                {
                    // Only a state a snapshot of foreign bytes could build holds anything but numbers here: hash a fixed
                    // marker for it instead of raising a Lua error outside a protected call
                    const sol::object value = values.get<sol::object>(i);
                    if (value.get_type() == sol::type::number)
                    {
                        fnv.Double(value.as<double>());
                    }
                    else
                    {
                        fnv.U32(0xBAD0u);
                    }
                }
            }
        };

        constexpr const char *kLuaDrivenScript = R"LUA(
acc = 0

function think(i, tick, px, pz, ix, iy, buttons)
    local ta = tick * 0.02 + i * 0.4
    local tx = math.cos(ta) * 20 + ix / 127 * 10
    local tz = math.sin(ta) * 20 + iy / 127 * 10
    local heading = math.atan(tz - pz, tx - px)
    local dist = math.sqrt((tx - px) * (tx - px) + (tz - pz) * (tz - pz))
    local speed = 3 * (1 - math.exp(-dist * 0.1))
    if (buttons & 1) ~= 0 then speed = speed * 2 end
    local jitter = rng_range(-0.05, 0.05)
    acc = acc + math.log(1 + dist)
    return math.cos(heading + jitter) * speed, math.sin(heading + jitter) * speed
end
)LUA";

        class LuaDrivenScene final : public LuaScene
        {
        public:
            explicit LuaDrivenScene(const std::uint32_t seed) : LuaScene(seed, kLuaDrivenScript)
            {
                for (int i = 0; i < kCount; ++i)
                {
                    const float x = _rng.Range(-20.0f, 20.0f);
                    const float z = _rng.Range(-20.0f, 20.0f);
                    _position[i] = Math::Vector3(x, 0.0f, z);
                }
            }

            bool Step(const int tick, const HeldInputs &inputs, std::string &error) override
            {
                sol::protected_function think = _lua["think"];
                const Held &held = inputs[0];
                for (int i = 0; i < kCount; ++i)
                {
                    const sol::protected_function_result result =
                        think(i + 1, tick, static_cast<double>(_position[i].x), static_cast<double>(_position[i].z),
                              held.moveX, held.moveY, static_cast<long long>(held.buttons));
                    if (!result.valid())
                    {
                        const sol::error err = result;
                        error = std::string("think: ") + err.what();
                        return false;
                    }
                    const double vx = result.get<double>(0);
                    const double vz = result.get<double>(1);
                    _position[i].x += static_cast<float>(vx) * kStep;
                    _position[i].z += static_cast<float>(vz) * kStep;
                    _position[i].y = 0.5f * Math::Sin(0.1f * static_cast<float>(tick) + static_cast<float>(i));
                }
                return true;
            }

            [[nodiscard]] std::uint64_t Identity() const override { return MakeIdentity("lua_driven/1", kLuaDrivenScript); }

            bool Save(Writer &writer, std::string &error) override
            {
                for (int i = 0; i < kCount; ++i)
                {
                    writer.Float(_position[i].x);
                    writer.Float(_position[i].y);
                    writer.Float(_position[i].z);
                }
                return SaveLuaState(writer, error);
            }

            bool Load(Reader &reader, std::string &error) override
            {
                for (int i = 0; i < kCount; ++i)
                {
                    const float x = reader.Float();
                    const float y = reader.Float();
                    const float z = reader.Float();
                    _position[i] = Math::Vector3(x, y, z);
                }
                if (!reader.Ok())
                {
                    error = "the lua_driven state is truncated";
                    return false;
                }
                return LoadLuaState(reader, error);
            }

            void Hash(Fnv &fnv) override
            {
                for (int i = 0; i < kCount; ++i)
                {
                    fnv.Float(_position[i].x);
                    fnv.Float(_position[i].y);
                    fnv.Float(_position[i].z);
                }
                const double acc = _lua["acc"].get_or(0.0);
                fnv.Double(acc);
            }

        private:
            static constexpr int kCount = 16;
            std::array<Math::Vector3, kCount> _position;
        };

        std::unique_ptr<Scene> MakeScene(const std::string &name, const std::uint32_t seed)
        {
            if (name == "transforms")
            {
                return std::make_unique<TransformsScene>(seed);
            }
            if (name == "lua_sim")
            {
                return std::make_unique<LuaSimScene>(seed);
            }
            if (name == "lua_driven")
            {
                return std::make_unique<LuaDrivenScene>(seed);
            }
            return nullptr;
        }
    }

    std::vector<std::string> SceneNames()
    {
        return {"transforms", "lua_sim", "lua_driven"};
    }

    std::string FormatHash(const std::uint64_t hash)
    {
        return std::format("0x{:016x}", hash);
    }

    struct ReplaySimulation::Impl
    {
        std::string sceneName;
        std::uint32_t seed = 0;
        std::unique_ptr<Scene> scene;
        int ticks = 0;
    };

    ReplaySimulation::ReplaySimulation(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
    ReplaySimulation::~ReplaySimulation() = default;

    std::unique_ptr<ReplaySimulation> ReplaySimulation::Create(const std::string &scene, const std::uint32_t seed, std::string &error)
    {
        auto impl = std::make_unique<Impl>();
        impl->sceneName = scene;
        impl->seed = seed;
        impl->scene = MakeScene(scene, seed);
        if (!impl->scene)
        {
            error = "unknown scene '" + scene + "'";
            return nullptr;
        }
        if (!impl->scene->Ready(error))
        {
            return nullptr;
        }
        return std::unique_ptr<ReplaySimulation>(new ReplaySimulation(std::move(impl)));
    }

    bool ReplaySimulation::Step(const std::span<const SlotState> held, std::string &error)
    {
        HeldInputs inputs{};
        for (std::size_t i = 0; i < held.size() && i < inputs.size(); ++i)
        {
            inputs[i] = Held{held[i].moveX, held[i].moveY, held[i].buttons};
        }
        // The tick runs under the deterministic floating-point policy whatever a library did to the thread before
        const Math::FloatEnvironmentGuard guard;
        if (!guard.Verified())
        {
            error = "the floating-point policy could not be set";
            return false;
        }
        if (!_impl->scene->Step(_impl->ticks, inputs, error))
        {
            return false;
        }
        ++_impl->ticks;
        return true;
    }

    int ReplaySimulation::TicksRun() const noexcept
    {
        return _impl->ticks;
    }

    std::uint64_t ReplaySimulation::Hash()
    {
        Fnv fnv;
        fnv.Text(_impl->sceneName);
        fnv.U32(static_cast<std::uint32_t>(_impl->ticks));
        _impl->scene->Hash(fnv);
        return fnv.Value();
    }

    std::vector<std::uint8_t> ReplaySimulation::Save(std::string &error)
    {
        Writer payload;
        if (!_impl->scene->Save(payload, error))
        {
            return {};
        }
        Writer out;
        for (const char c : std::string_view("N2RS"))
        {
            out.U8(static_cast<std::uint8_t>(c));
        }
        out.U32(kSnapshotFormat);
        out.Text(_impl->sceneName);
        out.U32(_impl->seed);
        out.U64(_impl->scene->Identity());
        out.U32(static_cast<std::uint32_t>(_impl->ticks));
        out.U64(Hash());
        out.U32(static_cast<std::uint32_t>(payload.Bytes().size()));
        out.Bytes().insert(out.Bytes().end(), payload.Bytes().begin(), payload.Bytes().end());
        Fnv checksum;
        for (const std::uint8_t byte : out.Bytes())
        {
            checksum.Byte(byte);
        }
        out.U64(checksum.Value());
        return std::move(out.Bytes());
    }

    bool ReplaySimulation::Restore(const std::span<const std::uint8_t> blob, std::string &error)
    {
        constexpr std::size_t kChecksumBytes = 8;
        if (blob.size() < 4 + kChecksumBytes)
        {
            error = "the snapshot is too short";
            return false;
        }
        const std::span<const std::uint8_t> body = blob.first(blob.size() - kChecksumBytes);
        Fnv checksum;
        for (const std::uint8_t byte : body)
        {
            checksum.Byte(byte);
        }
        Reader tail(blob.last(kChecksumBytes));
        if (tail.U64() != checksum.Value())
        {
            error = "the snapshot's checksum does not match (truncated or damaged)";
            return false;
        }
        Reader reader(body);
        const std::array<std::uint8_t, 4> magic{reader.U8(), reader.U8(), reader.U8(), reader.U8()};
        if (!reader.Ok() || magic != std::array<std::uint8_t, 4>{'N', '2', 'R', 'S'})
        {
            error = "not a replay-scene snapshot";
            return false;
        }
        if (reader.U32() != kSnapshotFormat)
        {
            error = "the snapshot's format version is not this build's";
            return false;
        }
        const std::string sceneName = reader.Text(kMaxSceneNameBytes);
        const std::uint32_t seed = reader.U32();
        const std::uint64_t identity = reader.U64();
        const std::uint32_t ticks = reader.U32();
        const std::uint64_t hash = reader.U64();
        const std::uint32_t payloadBytes = reader.U32();
        if (!reader.Ok() || payloadBytes != reader.Remaining())
        {
            error = "the snapshot's header is malformed";
            return false;
        }
        if (sceneName != _impl->sceneName)
        {
            error = "the snapshot is of scene '" + sceneName + "', this simulation runs '" + _impl->sceneName + "'";
            return false;
        }
        if (seed != _impl->seed)
        {
            error = std::format("the snapshot is of seed {}, this simulation runs seed {}", seed, _impl->seed);
            return false;
        }
        if (ticks > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        {
            error = "the snapshot's tick count is out of range";
            return false;
        }

        // Built into a fresh scene; this simulation changes only when everything has checked out
        std::unique_ptr<Scene> fresh = MakeScene(_impl->sceneName, _impl->seed);
        if (!fresh || !fresh->Ready(error))
        {
            if (error.empty())
            {
                error = "the scene could not be built";
            }
            return false;
        }
        if (fresh->Identity() != identity)
        {
            error = "the snapshot was made by another definition of the scene (its script or layout changed)";
            return false;
        }
        if (!fresh->Load(reader, error))
        {
            return false;
        }
        if (!reader.AtEnd())
        {
            error = "the snapshot has bytes left over after the scene's state";
            return false;
        }
        Fnv check;
        check.Text(_impl->sceneName);
        check.U32(ticks);
        fresh->Hash(check);
        if (check.Value() != hash)
        {
            error = "the restored state does not hash to the snapshot's hash";
            return false;
        }
        _impl->scene = std::move(fresh);
        _impl->ticks = static_cast<int>(ticks);
        return true;
    }

    ReplayResult RunReplay(const ReplayLog &log)
    {
        ReplayResult result;
        result.error = ValidateReplayLog(log);
        if (!result.error.empty())
        {
            return result;
        }
        const std::unique_ptr<ReplaySimulation> simulation = ReplaySimulation::Create(log.scene, log.seed, result.error);
        if (!simulation)
        {
            return result;
        }

        std::array<SlotState, kMaxSlots> held{};
        std::size_t next = 0;
        for (int tick = 0; tick < log.ticks; ++tick)
        {
            while (next < log.inputs.size() && log.inputs[next].tick == tick)
            {
                const ReplayInput &row = log.inputs[next];
                held[static_cast<std::size_t>(row.slot)] = SlotState{row.moveX, row.moveY, row.buttons};
                ++next;
            }
            if (!simulation->Step(held, result.error))
            {
                return result;
            }
            const int done = tick + 1;
            if (done % log.hashInterval == 0)
            {
                result.hashes.push_back(ReplayHash{done, simulation->Hash()});
            }
        }
        result.ok = true;
        return result;
    }

    std::string BuildFingerprintJson()
    {
        nlohmann::json fingerprint;
#if defined(__EMSCRIPTEN__)
        fingerprint["compiler"] = std::string("emscripten clang ") + __clang_version__;
        fingerprint["arch"] = "wasm32";
#elif defined(_MSC_VER)
        fingerprint["compiler"] = "msvc " + std::to_string(_MSC_FULL_VER);
#elif defined(__clang__)
        fingerprint["compiler"] = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
        fingerprint["compiler"] = std::string("gcc ") + __VERSION__;
#else
        fingerprint["compiler"] = "unknown";
#endif
#if !defined(__EMSCRIPTEN__)
#if defined(_M_X64) || defined(__x86_64__)
        fingerprint["arch"] = "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
        fingerprint["arch"] = "arm64";
#else
        fingerprint["arch"] = "unknown";
#endif
#endif
        nlohmann::json flags;
#ifdef N2_STRICT_FP
        flags["strict_fp"] = true;
#else
        flags["strict_fp"] = false;
#endif
#ifdef NDEBUG
        flags["ndebug"] = true;
#else
        flags["ndebug"] = false;
#endif
        flags["fp_control_state"] = Math::FloatEnvironment::kHasControlState;
        fingerprint["flags"] = flags;
        return fingerprint.dump();
    }

    std::string HashesToJson(const std::string &leg, const std::vector<NamedResult> &results, const int ticks, const int interval)
    {
        nlohmann::json records = nlohmann::json::array();
        for (const NamedResult &named : results)
        {
            for (const ReplayHash &entry : named.result.hashes)
            {
                records.push_back(nlohmann::json{
                    {"scene", named.name}, {"kind", "state"}, {"tick", entry.tick}, {"hash", FormatHash(entry.hash)}});
            }
        }
        nlohmann::json root;
        root["format"] = 1;
        root["leg"] = leg;
        root["ticks"] = ticks;
        root["interval"] = interval;
        root["fingerprint"] = nlohmann::json::parse(BuildFingerprintJson());
        root["records"] = records;
        return root.dump(1) + "\n";
    }
}
