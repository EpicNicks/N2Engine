#include "engine/network/NetworkPresets.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace N2Engine::Network
{
    namespace
    {
        using nlohmann::json;
        constexpr SettingClass L = SettingClass::Locked;
        constexpr SettingClass T = SettingClass::Tunable;
        constexpr SettingClass S = SettingClass::StartupChecked;

        PresetCell Cell(const std::uint8_t classes, std::string settingKey, json value, std::string description,
                        std::string lostGuarantee = {})
        {
            return {classes, std::move(settingKey), std::move(value), std::move(description),
                    std::move(lostGuarantee)};
        }

        PresetCell Cell(const SettingClass cls, std::string settingKey, json value, std::string description,
                        std::string lostGuarantee = {})
        {
            return Cell(static_cast<std::uint8_t>(cls), std::move(settingKey), std::move(value),
                        std::move(description), std::move(lostGuarantee));
        }

        /// A preset that does not govern the setting: any value, with the design's wording for it
        PresetCell Any(std::string description)
        {
            return Cell(static_cast<std::uint8_t>(0), {}, nullptr, std::move(description));
        }

        PresetRow Row(std::string key, std::string label, PresetCell none, PresetCell authoritative,
                      PresetCell deterministic)
        {
            return {std::move(key), std::move(label),
                    {std::move(none), std::move(authoritative), std::move(deterministic)}};
        }

        /// The default of a numeric setting (NumericSettingSpecs), so the table and the parser cannot drift apart
        json Default(const std::string_view specKey)
        {
            const NumericSettingSpec *spec = FindNumericSpec(specKey);
            if (spec == nullptr)
            {
                return nullptr;
            }
            return spec->integer ? json(static_cast<long long>(spec->defaultValue)) : json(spec->defaultValue);
        }

        std::string Key(const std::string_view specKey)
        {
            return "network." + std::string(specKey);
        }

        std::vector<PresetRow> BuildTable()
        {
            std::vector<PresetRow> rows;

            // ---- The preset bundles (physx-limitations-and-determinism 4.2), in the design's order ----
            rows.push_back(Row(
                "physics.backend", "Physics backend",
                Any("any (PhysX)"),
                Cell(T, {}, nullptr, "any; PhysX is fine"),
                Cell(L | S, {}, "CrossPlatform",
                     "the backend must declare DeterminismLevel CrossPlatform (IPhysicsBackend::GetDeterminism); "
                     "today none does",
                     "The physics backend is not bit-identical across platforms: peers desync")));
            rows.push_back(Row(
                "physics.gpu", "GPU physics",
                Any("any"),
                Cell(T, "physics.gpu", "off",
                     "off recommended: a server runs the CPU scene (a headless host has no GPU); clients may differ"),
                Cell(L | S, "physics.gpu", "off", "off",
                     "GPU dynamics is not deterministic: peers desync")));
            rows.push_back(Row(
                "physics.fixedTimestep", "Fixed timestep",
                Any("project value"),
                Cell(T, "physics.fixedTimestep", "1/tickRate", "on, rate = tick rate (30, 60 or 120 Hz)"),
                Cell(L | S, "physics.fixedTimestep", "1/tickRate",
                     "on, rate = tick rate; timeScale must not change the physics step",
                     "Physics dt follows timeScale or the frame rate: peers that pause or run differently desync")));
            rows.push_back(Row(
                "simulation.tickDriver", "Simulation tick driver",
                Any("frame-driven accumulator"),
                Cell(T, {}, nullptr, "tick counter owned by netcode; the catch-up clamp is tunable"),
                Cell(L | S, {}, nullptr,
                     "tick counter owned by netcode; no wall-clock clamp dropping ticks (not built yet)",
                     "A wall-clock catch-up clamp drops ticks: peers run different tick counts")));
            rows.push_back(Row(
                "math.simdTier", "Math SIMD tier",
                Any("detected"),
                Any("detected"),
                Cell(L | S, "math.simdTier", "scalar",
                     "one fixed tier for all peers: scalar, until a deterministic math path exists",
                     "SIMD tier Auto: x86 and ARM peers will desync")));
            rows.push_back(Row(
                "build.strictFp", "Floating-point build profile",
                Any("default"),
                Any("default"),
                Cell(L | S, {}, nullptr,
                     "the strict profile is compiled in (N2ENGINE_STRICT_FP defines N2_STRICT_FP): no FP "
                     "contraction, no fast-math, Release-only peers",
                     "Fused multiply-add or fast-math may differ between builds: peers desync")));
            rows.push_back(Row(
                "math.transcendentals", "Transcendental functions",
                Any("std"),
                Any("std"),
                Cell(L | S, {}, nullptr,
                     "engine-owned Sin/Cos/Atan2/Acos used by math, Lua and physics (not built yet)",
                     "libm differs between platforms in the last ulp: peers desync")));
            rows.push_back(Row(
                "physics.snapshot", "Simulation state snapshot/restore",
                Any("n/a"),
                Cell(S, {}, nullptr,
                     "per-entity set state for owned predicted entities (the backend reports SupportsSetState)"),
                Cell(L | S, {}, nullptr,
                     "full-world SaveState/RestoreState for rollback (the backend reports SupportsSnapshot); "
                     "lockstep mode waives it (network.deterministic.mode)",
                     "The world cannot be restored: rollback is impossible")));
            rows.push_back(Row(
                "deterministic.inputDelay", "Input delay",
                Any("n/a"),
                Cell(T, {}, 0, "0 (no setting in v1)"),
                Cell(T, Key("deterministic.inputDelay"), Default("deterministic.inputDelay"),
                     "2 ticks (0-6)")));
            rows.push_back(Row(
                "prediction.window", "Prediction / rollback window",
                Any("n/a"),
                Cell(T, Key("authoritative.predictionMaxMs"), Default("authoritative.predictionMaxMs"),
                     "client prediction window, RTT-bounded, at most 250 ms"),
                Cell(T, Key("deterministic.rollbackWindow"), Default("deterministic.rollbackWindow"),
                     "max rollback 8 ticks (0-15; 0 = pure lockstep)")));
            rows.push_back(Row(
                "state.sync", "State sync / snapshot rate",
                Any("n/a"),
                Cell(T, Key("authoritative.snapshotRate"), Default("authoritative.snapshotRate"),
                     "server snapshots 20-30 Hz, delta-compressed"),
                Cell(L | T, Key("deterministic.hashInterval"), Default("deterministic.hashInterval"),
                     "none (inputs only); a state hash every 30 ticks for desync detection (the rate is tunable, "
                     "the hash is always on)",
                     "Desyncs go undetected: the session cannot name the first divergent tick")));
            rows.push_back(Row(
                "interpolation", "Interpolation",
                Any("n/a"),
                Cell(T, Key("authoritative.interpolationDelayMs"), Default("authoritative.interpolationDelayMs"),
                     "remote entities rendered 2 snapshots behind (interpolation delay in ms)"),
                Cell(T, {}, nullptr, "render interpolation between ticks only")));
            rows.push_back(Row(
                "reconciliation", "Reconciliation / smoothing",
                Any("n/a"),
                Cell(T, Key("authoritative.smoothingMs"), Default("authoritative.smoothingMs"),
                     "rewind owned entities to the server state and replay inputs; visual smoothing of the error "
                     "over 100 ms; snap threshold 1 m (authoritative.snapThreshold)"),
                Any("none (bit-exact)")));
            rows.push_back(Row(
                "physics.authority", "Authority rules for physics bodies",
                Any("n/a"),
                Cell(T, {}, nullptr,
                     "server owns all dynamic bodies; a client predicts only its own avatar or kinematic "
                     "controller; remote and shared dynamic bodies are interpolated (per-body predict opt-in)"),
                Cell(L, {}, nullptr, "every peer simulates everything; no per-body authority",
                     "Per-body authority lets peers disagree about who simulates what: desync")));
            rows.push_back(Row(
                "random.policy", "RNG policy",
                Any("any"),
                Cell(T, {}, nullptr, "gameplay RNG server-side; clients may use cosmetic RNG freely"),
                Cell(L | S, {}, nullptr,
                     "only the seeded deterministic stream, seeded from the session; Random.* and Lua math.random "
                     "redirected or disabled (not startup-checked yet: needs the session)",
                     "Unseeded random values differ per peer: desync")));
            rows.push_back(Row(
                "ordering.rules", "Ordering rules",
                Any("none"),
                Any("none"),
                Cell(L | S, {}, nullptr,
                     "ordered containers in every simulation path, stable spawn order, coroutine order by creation "
                     "index, Lua pairs() banned in sim code or a fixed hash seed (determinism.luaSeed)",
                     "Hash- or pointer-ordered iteration differs per peer: desync")));
            rows.push_back(Row(
                "physics.threads", "Threads",
                Any("any"),
                Any("any"),
                Cell(L | S, {}, nullptr,
                     "physics dispatcher threads 0, or a backend that proves thread-count independence",
                     "Physics results may depend on thread timing: desync")));
            rows.push_back(Row(
                "physics.backendPicker", "Allowed backends shown in the picker",
                Any("all"),
                Any("all"),
                Cell(static_cast<std::uint8_t>(0), {}, nullptr,
                     "only backends with the declaration; the others are greyed with the reason")));

            // ---- The transport and runtime settings of the network block (netcode-runtime-design 5.1) ----
            rows.push_back(Row(
                "network.tickRate", "Tick rate",
                Any("n/a"),
                Cell(T, Key("tickRate"), Default("tickRate"), "30, 60 or 120 Hz recommended"),
                Cell(T, Key("tickRate"), Default("tickRate"), "30, 60 or 120 Hz recommended")));
            rows.push_back(Row(
                "network.transport.udp", "UDP transport",
                Any("n/a"),
                Cell(T, "network.transport.udp", true, "native UDP"),
                Cell(T, "network.transport.udp", true, "native UDP")));
            rows.push_back(Row(
                "network.transport.webSocket", "WebSocket transport",
                Any("n/a"),
                Cell(T, "network.transport.webSocket", true, "browser clients"),
                Cell(T, "network.transport.webSocket", true, "browser clients")));
            rows.push_back(Row(
                "network.transport.port", "Port",
                Any("n/a"),
                Cell(T, Key("transport.port"), Default("transport.port"), "listen port"),
                Cell(T, Key("transport.port"), Default("transport.port"), "listen port")));
            rows.push_back(Row(
                "network.authoritative.inputRedundancy", "Input redundancy",
                Any("n/a"),
                Cell(T, Key("authoritative.inputRedundancy"), Default("authoritative.inputRedundancy"),
                     "unacknowledged inputs resent in every packet"),
                Any("n/a")));
            rows.push_back(Row(
                "network.authoritative.snapThreshold", "Snap threshold",
                Any("n/a"),
                Cell(T, Key("authoritative.snapThreshold"), Default("authoritative.snapThreshold"),
                     "a correction larger than this many metres snaps instead of smoothing"),
                Any("n/a")));
            rows.push_back(Row(
                "network.authoritative.lagCompMaxMs", "Lag compensation limit",
                Any("n/a"),
                Cell(T, Key("authoritative.lagCompMaxMs"), Default("authoritative.lagCompMaxMs"),
                     "how far back the server rewinds a hit query, in ms"),
                Any("n/a")));
            rows.push_back(Row(
                "network.deterministic.mode", "Deterministic mode",
                Any("n/a"),
                Any("n/a"),
                Cell(T, "network.deterministic.mode", "lockstep", "lockstep or rollback (rollback needs a snapshot)")));
            return rows;
        }

        bool EqualsIgnoreCase(const std::string &a, const std::string &b)
        {
            return std::ranges::equal(a, b, [](const unsigned char x, const unsigned char y)
            {
                return std::tolower(x) == std::tolower(y);
            });
        }

        bool ValuesEqual(const json &a, const json &b)
        {
            if (a.is_string() && b.is_string())
            {
                return EqualsIgnoreCase(a.get<std::string>(), b.get<std::string>());
            }
            return a == b;
        }

        /// The key an override of this cell is stored under: its setting, or the row for a requirement
        const std::string &OverrideKey(const PresetRow &row, const PresetCell &cell)
        {
            return cell.settingKey.empty() ? row.key : cell.settingKey;
        }
    }

    const std::vector<PresetRow> &PresetTable()
    {
        static const std::vector<PresetRow> table = BuildTable();
        return table;
    }

    const PresetRow *FindPresetRow(const std::string_view key)
    {
        for (const PresetRow &row : PresetTable())
        {
            if (row.key == key)
            {
                return &row;
            }
        }
        return nullptr;
    }

    bool IsKnownSettingKey(const std::string_view key)
    {
        for (const PresetRow &row : PresetTable())
        {
            if (row.key == key)
            {
                return true;
            }
            for (const PresetCell &cell : row.cells)
            {
                if (!cell.settingKey.empty() && cell.settingKey == key)
                {
                    return true;
                }
            }
        }
        return false;
    }

    json EffectiveValues(const NetworkPreset preset, const json &overrides)
    {
        json values = json::object();
        for (const PresetRow &row : PresetTable())
        {
            const PresetCell &cell = row.Cell(preset);
            if (cell.classes != 0 && !cell.settingKey.empty() && !cell.value.is_null())
            {
                values[cell.settingKey] = cell.value;
            }
        }
        if (overrides.is_object())
        {
            for (auto it = overrides.begin(); it != overrides.end(); ++it)
            {
                if (values.contains(it.key()))
                {
                    values[it.key()] = it.value();
                }
            }
        }
        return values;
    }

    std::vector<LostGuarantee> LostGuarantees(const NetworkPreset preset, const json &overrides)
    {
        std::vector<LostGuarantee> lost;
        if (!overrides.is_object())
        {
            return lost;
        }
        for (const PresetRow &row : PresetTable())
        {
            const PresetCell &cell = row.Cell(preset);
            // A Tunable cell may be changed freely; a cell that is not Locked guarantees nothing
            if (!HasClass(cell.classes, SettingClass::Locked) || HasClass(cell.classes, SettingClass::Tunable) ||
                cell.lostGuarantee.empty())
            {
                continue;
            }
            const std::string &key = OverrideKey(row, cell);
            const auto found = overrides.find(key);
            if (found == overrides.end())
            {
                continue;
            }
            // A requirement with no value (the build profile, the backend) is waived by any override of its row
            if (cell.value.is_null() || !ValuesEqual(*found, cell.value))
            {
                lost.push_back({key, row.key, cell.lostGuarantee});
            }
        }
        return lost;
    }

    bool IsCustomised(const NetworkPreset preset, const json &overrides)
    {
        return !LostGuarantees(preset, overrides).empty();
    }
}
