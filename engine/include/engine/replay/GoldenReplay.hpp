#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/replay/ReplayLog.hpp"

namespace N2Engine::Replay
{
    /**
     * The headless replay runner and the fixed physics-free scenes of the golden determinism job (netcode C2).
     *
     * A replay (ReplayLog) names one of the scenes below. RunReplay builds it from the log's seed, steps it for the log's
     * number of fixed ticks (the inputs of the log applied as held state before each tick) and takes a 64-bit hash of the
     * whole simulation state after every `hashInterval` ticks. Nothing here touches the Application, the window, a
     * renderer, a physics backend, the clock or the file system: the result is a function of the log, so it must be the
     * same on every platform, compiler and architecture the engine is built for. Where it is not, that is a finding about
     * the toolchain (R2 of the netcode design), never something to tolerate.
     *
     * The scenes (their sources are in GoldenReplay.cpp):
     *   - "transforms": 24 transforms (Math::Vector3 positions, float yaws) moved by float code, the engine-owned
     *     Sin/Cos of math/DeterministicMath.hpp and a DeterministicRandom stream.
     *   - "lua_sim": the simulation lives in a sandboxed Lua state (engine math shims, Ordered.pairs, the seeded
     *     engine random stream; no pairs-order or math.random dependence): string-keyed entities that spawn and die, mixed
     *     number/string keyed counters.
     *   - "lua_driven": 16 C++ transforms steered by a Lua function every tick.
     *
     * Every tick runs under a FloatEnvironmentGuard, like Application::RunFixedTick in a deterministic mode.
     */
    struct ReplayHash
    {
        int tick = 0;
        std::uint64_t hash = 0;
    };

    struct ReplayResult
    {
        bool ok = false;
        std::string error;
        /// One entry per checkpoint: ticks hashInterval, 2 * hashInterval, ... up to and including log.ticks
        std::vector<ReplayHash> hashes;
    };

    /// The names the scenes go by, in the order of the golden file
    [[nodiscard]] std::vector<std::string> SceneNames();

    /// What a slot holds for one tick (the input of ReplayInput without the position in time)
    struct SlotState
    {
        int moveX = 0;
        int moveY = 0;
        std::uint32_t buttons = 0;
    };

    /**
     * One scene stepped a tick at a time: what RunReplay is built on, and what a live lockstep session runs so that its
     * checkpoint hashes are the very ones a replay of its recorded input log produces (netcode C3). Same guarantees as
     * RunReplay: no clock, no file system, every tick under the floating-point policy.
     */
    class ReplaySimulation
    {
    public:
        /// nullptr with `error` set for an unknown scene or a scene whose script failed
        [[nodiscard]] static std::unique_ptr<ReplaySimulation> Create(const std::string &scene, std::uint32_t seed, std::string &error);
        ~ReplaySimulation();
        ReplaySimulation(const ReplaySimulation &) = delete;
        ReplaySimulation &operator=(const ReplaySimulation &) = delete;

        /// Runs one tick with `held` as the slots' state (slots beyond the span hold zeros; at most kMaxSlots are read)
        [[nodiscard]] bool Step(std::span<const SlotState> held, std::string &error);
        /// Ticks run so far
        [[nodiscard]] int TicksRun() const noexcept;
        /// The 64-bit state hash after TicksRun() ticks: the value RunReplay records at a checkpoint
        [[nodiscard]] std::uint64_t Hash();

    private:
        struct Impl;
        explicit ReplaySimulation(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> _impl;
    };

    /// Replays the log. ok is false with an error for an invalid log, an unknown scene or a script error.
    [[nodiscard]] ReplayResult RunReplay(const ReplayLog &log);

    /// "0x" and 16 lower-case hex digits
    [[nodiscard]] std::string FormatHash(std::uint64_t hash);

    /// What the build is, for the aggregation job's summary (compiler, architecture, strict floating point, ...)
    [[nodiscard]] std::string BuildFingerprintJson();

    struct NamedResult
    {
        std::string name;
        ReplayResult result;
    };

    /// The text of replay-hashes.json (format 1, the same record shape as the physics corpus file, scene = replay name,
    /// kind = "state"): what a CI leg uploads for scripts/replay-determinism.py
    [[nodiscard]] std::string HashesToJson(const std::string &leg, const std::vector<NamedResult> &results, int ticks, int interval);
}
