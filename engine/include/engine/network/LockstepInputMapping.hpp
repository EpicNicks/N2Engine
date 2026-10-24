#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

#include "engine/network/InputLayout.hpp"
#include "engine/network/NetworkInput.hpp"
#include "engine/network/n2net/Lockstep.hpp"

namespace N2Engine::Network
{
    /**
     * The mapping between the networked InputFrame (B4) and the lockstep input (C3). It lives in the engine library, not
     * in n2net: n2net has no engine dependency, so the scheduler and the bundle codec know only a payload size, the
     * layout hash and the neutral bytes (N2Net::LockstepLayoutInfo), and this file is where an InputLayout is turned into
     * those and where the layout's quantisation is checked.
     *
     * An InputFrame is (tick, sequence, packed payload). Lockstep keys everything by (slot, tick), so the frame's tick
     * becomes the scheduler's tick and the payload becomes the LockstepInput as it is: the bytes are not re-packed or
     * re-quantised anywhere between the sender's InputLayout::Pack and the simulation, so every peer reads the same
     * values (the quantisation of the layout is the determinism guarantee: a value is a whole step of the layout, and
     * Unpack gives the same float for the same step everywhere). The sequence number is a transport detail of the
     * client-to-server channel (it names acknowledgements) and does not exist in lockstep or in a replay row;
     * ToInputFrame takes one from its caller.
     *
     * The simulation reads the inputs of N2Net::TickInputs (slot order, one per slot, identical on every peer), never a
     * live device, and takes the values out of the payload with InputLayout::Unpack.
     */

    /// What the scheduler needs of `layout`. nullopt when the payload is larger than a lockstep input.
    [[nodiscard]] std::optional<N2Net::LockstepLayoutInfo> MakeLockstepLayoutInfo(const InputLayout &layout);

    /// The lockstep input for a frame under `layout`. The error names the problem: a payload that is not exactly the
    /// layout's size, or not one the layout produces (an out-of-range step or non-zero padding), is refused.
    [[nodiscard]] std::expected<N2Net::LockstepInput, std::string> ToLockstepInput(const InputLayout &layout,
                                                                                  const InputFrame &frame);
    /// The same for a bare payload
    [[nodiscard]] std::expected<N2Net::LockstepInput, std::string> ToLockstepInput(const InputLayout &layout,
                                                                                  std::span<const std::uint8_t> payload);

    /// The frame for `tick` that carries `input`, with the caller's sequence
    [[nodiscard]] InputFrame ToInputFrame(std::uint32_t tick, std::uint32_t sequence, const N2Net::LockstepInput &input);

    /// N2Net::DecodeInputBundle for a session under `layout`, and every frame must also be a payload the layout produces
    /// (anything else is Malformed). A bundle packed under another layout is LayoutMismatch.
    [[nodiscard]] std::expected<N2Net::InputBundle, N2Net::BundleError> DecodeInputBundleFor(const InputLayout &layout,
                                                                                            std::span<const std::uint8_t> bytes);

    /**
     * The layout of the original (moveX, moveY, buttons) triple of the C2 replay format version 1: moveX and moveY are
     * signed 8-bit axes with range 127 (so a value is the integer in [-127, 127] exactly) and the buttons are the
     * controls b00 ... b29. A layout holds at most 32 controls, so this carries 30 of the 32 button bits of the
     * triple: bits 30 and 31 are dropped (no committed replay uses a button above bit 1). Six bytes. It is what the
     * tests that predate the mapping run under, and the layout a version 1 file converts to.
     */
    [[nodiscard]] const InputLayout &LegacyMoveLayout();
    /// The packed LegacyMoveLayout() payload of a triple (moveX and moveY in [-127, 127]; button bits 30 and 31 dropped)
    [[nodiscard]] N2Net::LockstepInput LegacyInput(int moveX, int moveY, std::uint32_t buttons);
}
