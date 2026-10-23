#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::Network
{
    /// The most controls a layout declares
    inline constexpr std::size_t kMaxInputControls = 32;
    /// The longest control name
    inline constexpr std::size_t kMaxInputControlNameLength = 32;
    /// The most bits of one component (an axis, or one component of a vector)
    inline constexpr std::uint8_t kMaxInputComponentBits = 16;
    /// The most bytes of one packed InputFrame payload (the sum of every control's bits, rounded up to bytes)
    inline constexpr std::size_t kMaxInputPayloadBytes = 32;
    /// The largest range of an axis or vector component
    inline constexpr float kMaxInputRange = 1.0e6f;

    enum class InputControlKind : std::uint8_t
    {
        /// One bit, on or off. One value.
        Button,
        /// A number in [-range, range] (signed) or [0, range] (unsigned). One value.
        Axis,
        /// Two axes with the same bits and range: x, y. Two values.
        Vector2,
        /// Three: x, y, z. Three values.
        Vector3
    };

    /// "button", "axis", "vector2" or "vector3"
    [[nodiscard]] std::string_view ToString(InputControlKind kind) noexcept;
    /// The names ToString gives (lower case, exact); nullopt for anything else
    [[nodiscard]] std::optional<InputControlKind> ParseInputControlKind(std::string_view text) noexcept;

    /// One declared control. `bits`, `range` and `isSigned` are per component and only mean something for an axis or a
    /// vector: a button is always one bit (Create normalises them).
    struct InputControlSpec
    {
        std::string name;
        InputControlKind kind = InputControlKind::Button;
        std::uint8_t bits = 8;
        float range = 1.0f;
        bool isSigned = true;
    };

    /**
     * The networked input layout (netcode stage B4): which named controls a player's input holds, and how each is
     * quantised on the wire. Declared once per game, in C++ (InputLayout::Create) or from a Lua script
     * (Network.DeclareInputLayout), and the same on the server and every client: its hash goes into the fingerprint, so
     * a client whose layout differs refuses to join.
     *
     * Controls are held in the byte order of their names (so the order they were declared in does not matter, and the
     * layout is the same wherever it was declared). An InputFrame's payload is the controls' values packed one after the
     * other, lowest bit first, each component in `bits` bits, padded with zero bits to whole bytes. A component is a whole
     * number of steps, so the value a frame decodes to is the same on every machine:
     *   signed:   steps = 2^(bits-1) - 1, the value is the step in [-steps, steps] times range / steps (zero is exact)
     *   unsigned: steps = 2^bits - 1, the value is the step in [0, steps] times range / steps
     *   button:   0 or 1 (a value of 0.5 or more packs as 1)
     * A value outside the range, an infinity or a NaN packs as the nearest end of the range (NaN as 0). Packing then
     * unpacking therefore returns the value rounded to the nearest step (a tie rounds up).
     *
     * A default-constructed layout is empty: no controls, a payload of 0 bytes. A server with an empty layout takes no
     * input and drops a client that sends any.
     *
     * Bounds: at most kMaxInputControls controls, names of 1 to kMaxInputControlNameLength characters (A-Z, a-z, 0-9 and
     * '_', not starting with a digit, no two the same), 1 to kMaxInputComponentBits bits per component (2 to 16 for a signed
     * one), a range in (0, kMaxInputRange], and at most kMaxInputPayloadBytes * 8 bits in all.
     */
    class InputLayout
    {
    public:
        InputLayout() = default;

        /// Validates `controls`, normalises and sorts them by name. The error names the first thing wrong.
        [[nodiscard]] static std::expected<InputLayout, std::string> Create(std::vector<InputControlSpec> controls);

        [[nodiscard]] bool Empty() const noexcept { return _controls.empty(); }
        [[nodiscard]] std::size_t ControlCount() const noexcept { return _controls.size(); }
        [[nodiscard]] const InputControlSpec &Control(const std::size_t index) const { return _controls[index]; }

        /// The number of values a frame holds: a button, an axis 1, a Vector2 2, a Vector3 3, summed
        [[nodiscard]] std::size_t ValueCount() const noexcept { return _valueCount; }
        /// Where the values of control `index` start in a values array (its components follow one another)
        [[nodiscard]] std::size_t ValueOffset(const std::size_t index) const { return _offsets[index]; }
        /// The values control `index` holds (1 to 3)
        [[nodiscard]] std::size_t ComponentsOf(std::size_t index) const;
        /// The index of the control with this name
        [[nodiscard]] std::optional<std::size_t> FindControl(std::string_view name) const;

        [[nodiscard]] std::size_t BitCount() const noexcept { return _bitCount; }
        [[nodiscard]] std::size_t PayloadBytes() const noexcept { return (_bitCount + 7) / 8; }

        /// FNV-1a over the controls (name, kind, bits, signedness and range) in order. Two layouts that pack
        /// differently never hash the same on purpose; it is what the fingerprint compares.
        [[nodiscard]] std::uint64_t Hash() const;

        /// The values of no input at all: every one 0. A button off, an axis centred (an unsigned axis at its lowest).
        [[nodiscard]] std::vector<float> NeutralValues() const { return std::vector<float>(_valueCount, 0.0f); }
        /// Its payload (Pack of NeutralValues: the zero of a signed axis is its middle step, not the zero bit pattern)
        [[nodiscard]] std::vector<std::uint8_t> NeutralPayload() const;

        /// Packs `values` (ValueCount() of them, in control order). nullopt when the count is wrong.
        [[nodiscard]] std::optional<std::vector<std::uint8_t>> Pack(std::span<const float> values) const;

        /// True when `payload` is exactly PayloadBytes() long, every component is a step the layout can produce and the
        /// padding bits are zero: the one byte string Pack gives for the values it decodes to
        [[nodiscard]] bool IsValidPayload(std::span<const std::uint8_t> payload) const;

        /// The values a valid payload holds; nullopt when IsValidPayload is false
        [[nodiscard]] std::optional<std::vector<float>> Unpack(std::span<const std::uint8_t> payload) const;

    private:
        std::vector<InputControlSpec> _controls;
        std::vector<std::size_t> _offsets;
        std::size_t _valueCount = 0;
        std::size_t _bitCount = 0;
    };

    /// The layout the game declared (Network.DeclareInputLayout from Lua, or SetDeclaredInputLayout from C++). Empty until
    /// one is declared. A host copies it into the options of its NetworkServer or NetworkClient.
    [[nodiscard]] const InputLayout &DeclaredInputLayout() noexcept;
    /// Replaces the declared layout (the last declaration wins)
    void SetDeclaredInputLayout(InputLayout layout);
}
