#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/network/InputLayout.hpp"

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
     * stand-in for the networked InputFrame. A reader refuses a version it does not know.
     *
     * Version 2 (netcode C3, input mapping) carries the real networked input instead of the triple: the quantised
     * payload of an InputFrame under a declared Network::InputLayout. It adds a layout header and changes the row:
     *
     *   { "format": "n2replay", "version": 2, "scene": ..., "seed": ..., "ticks": ..., "hashInterval": ...,
     *     "layout": [ {"name": "jump", "kind": "button", "bits": 1, "range": 1, "signed": false}, ... ],
     *     "layoutHash": "0x<16 hex digits>",
     *     "inputs": [ [tick, slot, "<hex of the packed payload>"], ... ] }
     *
     * The layout array lists the controls in the layout's order (the byte order of the names), the hash is
     * InputLayout::Hash() and must match the layout (a file whose hash disagrees with its own header is refused), and a
     * payload is exactly PayloadBytes() bytes as two hex digits each and must be one the layout can produce. Rows keep
     * the hold semantics of version 1: from the row's tick on the slot holds that payload; a slot with no row holds the
     * layout's NeutralPayload(). Ordering, tick and slot ranges are the same.
     *
     * The built-in scenes read a (moveX, moveY, buttons) triple, so a version 2 row is mapped onto one, the same way for
     * a replay and for a live session (DeriveSceneInput): the triple of a row is always filled in by the reader and is
     * checked against the payload by ValidateReplayLog. Version 1 files stay valid and unchanged: they are read as they
     * were, with an empty layout and no payloads (they are the inputs of Network::LegacyMoveLayout()).
     */
    constexpr int kReplayFormatVersion = 1;
    constexpr int kReplayFormatVersionLayout = 2;
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
        /// Version 2 only: the packed InputFrame payload (empty in version 1). moveX, moveY and buttons are then derived
        /// from it by DeriveSceneInput.
        std::vector<std::uint8_t> payload;

        friend bool operator==(const ReplayInput &, const ReplayInput &) = default;
    };

    struct ReplayLog
    {
        int version = kReplayFormatVersion;
        std::string scene;
        std::uint32_t seed = 1;
        int ticks = 0;
        int hashInterval = 60;
        /// Version 2 only: the controls of the input layout (empty in version 1)
        std::vector<Network::InputControlSpec> layout;
        std::vector<ReplayInput> inputs;
    };

    /**
     * The scene input of a payload: what the built-in scenes (and so every replay and live session) read from an
     * InputFrame. The mapping is by control name and position, never by device:
     *   moveX, moveY: the Vector2/Vector3 control "move" (its x and y), or else the axes "moveX" and "moveY"; absent
     *                 means 0. The value is scaled to [-127, 127]: floor(value / range * 127 + 0.5), clamped.
     *   buttons:      bit i is set when the i-th Button control of the layout (in the layout's order) is on, for i < 32.
     * With Network::LegacyMoveLayout() this is exactly the version 1 triple. False when the payload is not one the layout
     * produces.
     */
    [[nodiscard]] bool DeriveSceneInput(const Network::InputLayout &layout, std::span<const std::uint8_t> payload, int &moveX,
                                        int &moveY, std::uint32_t &buttons);

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
