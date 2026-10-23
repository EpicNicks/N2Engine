#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::Replay
{
    /**
     * The replay / input-log file (netcode C2). A replay names a built-in scene, the seed of its random stream, how many
     * fixed ticks to run, how often to hash, and the inputs of the players. Replaying it headless on any platform must
     * print the same hash at every checkpoint: the cross-leg golden-hash CI job (docs/testing.html, "The golden replay
     * job") compares them.
     *
     * The format is JSON, versioned, and kept minimal on purpose; C3 (lockstep, replay files, the hash tree) builds on it
     * and may add fields in a new version:
     *
     *   { "format": "n2replay", "version": 1, "scene": "<name>", "seed": <u32>, "ticks": <n>, "hashInterval": <n>,
     *     "inputs": [ [tick, slot, moveX, moveY, buttons], ... ] }
     *
     * An input row says: from this tick on, slot `slot` holds the state (moveX, moveY, buttons) until its next row (a slot
     * with no row yet holds all zeros). moveX and moveY are integers in [-127, 127], buttons is a u32 bit set, slot is in
     * [0, kMaxSlots). Rows are sorted by (tick, slot), strictly increasing, and tick is in [0, ticks). This is a
     * stand-in for the networked InputFrame, which does not exist in this file's lifetime yet: C3 maps InputFrame onto
     * these rows (or bumps the version). A reader refuses a version it does not know.
     */
    constexpr int kReplayFormatVersion = 1;
    constexpr int kMaxSlots = 4;
    constexpr int kMaxMove = 127;
    constexpr int kMaxTicks = 1'000'000;

    struct ReplayInput
    {
        int tick = 0;
        int slot = 0;
        int moveX = 0;
        int moveY = 0;
        std::uint32_t buttons = 0;

        friend bool operator==(const ReplayInput &, const ReplayInput &) = default;
    };

    struct ReplayLog
    {
        int version = kReplayFormatVersion;
        std::string scene;
        std::uint32_t seed = 1;
        int ticks = 0;
        int hashInterval = 60;
        std::vector<ReplayInput> inputs;
    };

    /// Empty when the log is well formed (ranges, ordering, a scene name), else the first problem
    [[nodiscard]] std::string ValidateReplayLog(const ReplayLog &log);

    /// The log, or nullopt with `error` set (not JSON, wrong "format", unknown version, a bad field or row, or a log
    /// ValidateReplayLog refuses)
    [[nodiscard]] std::optional<ReplayLog> ParseReplayLog(std::string_view text, std::string &error);

    /// Same as ParseReplayLog on the contents of a file
    [[nodiscard]] std::optional<ReplayLog> LoadReplayLog(const std::string &path, std::string &error);

    /// The canonical text of a log: one input row per line, ParseReplayLog(SerializeReplayLog(x)) == x
    [[nodiscard]] std::string SerializeReplayLog(const ReplayLog &log);
}
