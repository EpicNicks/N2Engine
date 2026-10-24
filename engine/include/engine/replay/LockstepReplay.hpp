#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/network/InputLayout.hpp"
#include "engine/network/n2net/HashTree.hpp"
#include "engine/network/n2net/Lockstep.hpp"
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
     * Real input (C3, input mapping). Decision: the scheduler carries the InputFrame payload as opaque bytes sized by the
     * layout (N2Net::LockstepInput), and the replay format gets a version 2 with the layout header and a payload per row
     * (ReplayLog.hpp), while version 1 stays readable and unchanged. A recorder built with a layout writes version 2
     * (RecordInputs); the one without writes version 1 (RecordTick), which is what the committed C2 goldens are.
     *
     * The desync report is a JSON file, "n2desync" version 1, written by every peer that finds a mismatch.
     */
    class LockstepRecorder
    {
    public:
        /// A version 1 recorder: the inputs are the (moveX, moveY, buttons) triples of RecordTick
        LockstepRecorder(std::string scene, std::uint32_t seed, int hashInterval, int slotCount);
        /// A version 2 recorder: the inputs are the packed InputFrame payloads of RecordInputs, under `layout`
        LockstepRecorder(std::string scene, std::uint32_t seed, int hashInterval, int slotCount, const Network::InputLayout &layout);

        /// The confirmed state of every slot for the next tick, in tick order from tick 0 with no gap. False if the
        /// recorder is invalid (slotCount outside [1, kMaxSlots]) or full, or is a version 2 recorder.
        bool RecordTick(std::span<const SlotState> slots);

        /// Version 2: the confirmed input of every slot for the next tick (a slot beyond the span is neutral). False
        /// when the recorder is invalid, full or version 1, or when an input is not a payload of the layout (nothing is
        /// recorded then).
        bool RecordInputs(std::span<const N2Net::LockstepInput> inputs);

        [[nodiscard]] int TicksRecorded() const noexcept { return _log.ticks; }

        /// The log so far (ticks = the ticks recorded)
        [[nodiscard]] const ReplayLog &Log() const noexcept { return _log; }

    private:
        ReplayLog _log;
        int _slotCount;
        bool _layoutMode = false;
        Network::InputLayout _layout;
        std::array<SlotState, kMaxSlots> _held{};
        std::array<std::vector<std::uint8_t>, kMaxSlots> _heldPayload;
    };

    /// The scene input (moveX, moveY, buttons) of a live lockstep input: DeriveSceneInput, which is also what reading a
    /// version 2 replay row does, so a session and the replay of its log run the scene on the same values. False when the
    /// input is not a payload of the layout.
    [[nodiscard]] bool SceneStateFromInput(const Network::InputLayout &layout, const N2Net::LockstepInput &input, SlotState &out);

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
