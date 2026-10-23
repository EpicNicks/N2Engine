#include "engine/replay/GoldenReplay.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>

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
                    fnv.Double(values.get<double>(i));
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
        std::unique_ptr<Scene> scene;
        int ticks = 0;
    };

    ReplaySimulation::ReplaySimulation(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
    ReplaySimulation::~ReplaySimulation() = default;

    std::unique_ptr<ReplaySimulation> ReplaySimulation::Create(const std::string &scene, const std::uint32_t seed, std::string &error)
    {
        auto impl = std::make_unique<Impl>();
        impl->sceneName = scene;
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
