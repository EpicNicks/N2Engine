#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/network/n2net/HashTree.hpp"
#include "engine/replay/GoldenReplay.hpp"
#include "engine/replay/ReplayLog.hpp"

namespace N2Engine::Replay
{
    /**
     * Recording a lockstep session as a replay file, and the desync report (netcode C3, design section 4.5).
     *
     * The replay file of C2 (format "n2replay", version 1) already is the input log of a lockstep session: its rows say
     * "from this tick on slot S holds this state", which is exactly what the confirmed inputs of every tick give once
     * unchanged inputs are dropped. So the version stays 1 and the format does not change; the recorder only turns the
     * scheduler's stream of confirmed ticks into rows, and the headless RunReplay of the result must print the hashes the
     * live session printed (the test of this: tests/engine/replay/LockstepReplayTests.cpp). A session of more than
     * kMaxTicks ticks, or with more than kMaxSlots slots, cannot be recorded in this format (RecordTick refuses).
     *
     * The desync report is a JSON file, "n2desync" version 1, written by every peer that finds a mismatch.
     */
    class LockstepRecorder
    {
    public:
        LockstepRecorder(std::string scene, std::uint32_t seed, int hashInterval, int slotCount);

        /// The confirmed state of every slot for the next tick, in tick order from tick 0 with no gap. False if the
        /// recorder is invalid (slotCount outside [1, kMaxSlots]) or full.
        bool RecordTick(std::span<const SlotState> slots);

        [[nodiscard]] int TicksRecorded() const noexcept { return _log.ticks; }

        /// The log so far (ticks = the ticks recorded)
        [[nodiscard]] const ReplayLog &Log() const noexcept { return _log; }

    private:
        ReplayLog _log;
        int _slotCount;
        std::array<SlotState, kMaxSlots> _held{};
    };

    /// Most objects of one tree written into a report (the checkpoint trees are bounded by kMaxHistory, and a tree with
    /// more objects is cut and marked "truncated")
    inline constexpr std::size_t kMaxReportObjects = 4096;

    struct DesyncReport
    {
        std::string peerName;
        /// The checkpoint tick the roots differed at
        std::uint32_t tick = 0;
        std::uint64_t localRoot = 0;
        std::uint64_t remoteRoot = 0;
        /// BuildFingerprintJson() of this build
        std::string buildFingerprintJson;
        /// The determinism fingerprint of the session (engine version, scene, registry; whatever the host wants to
        /// record), a JSON object or empty
        std::string determinismFingerprintJson;
        /// The input log from tick 0 (the replay file)
        ReplayLog inputLog;
        /// The last K checkpoint trees, oldest first
        const std::vector<N2Net::HashTree> *trees = nullptr;
        /// What Bisect found
        N2Net::Divergence divergence;
    };

    /// The text of the report. A fingerprint that is not valid JSON is written as a string.
    [[nodiscard]] std::string DesyncReportToJson(const DesyncReport &report);

    /// Writes the report to `path`; false with `error` when the file cannot be written
    [[nodiscard]] bool WriteDesyncReport(const DesyncReport &report, const std::string &path, std::string &error);

    [[nodiscard]] const char *DivergenceKindName(N2Net::Divergence::Kind kind) noexcept;
}
