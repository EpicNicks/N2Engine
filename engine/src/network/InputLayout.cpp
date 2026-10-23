#include "engine/network/InputLayout.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <utility>

#include "engine/network/NetworkHash.hpp"

namespace N2Engine::Network
{
    namespace
    {
        std::uint8_t BitsOf(const InputControlSpec &control) noexcept
        {
            return control.kind == InputControlKind::Button ? static_cast<std::uint8_t>(1) : control.bits;
        }

        /// The number of steps from zero to the end of the range (signed: one side of it)
        double MaxStep(const InputControlSpec &control) noexcept
        {
            const unsigned bits = control.isSigned ? static_cast<unsigned>(control.bits) - 1u : static_cast<unsigned>(control.bits);
            return static_cast<double>((1u << bits) - 1u);
        }

        /// The largest value a component of this control holds on the wire
        std::uint32_t MaxStored(const InputControlSpec &control) noexcept
        {
            if (control.kind == InputControlKind::Button)
            {
                return 1u;
            }
            const auto steps = static_cast<std::uint32_t>(MaxStep(control));
            return control.isSigned ? steps * 2u : steps;
        }

        std::uint32_t Quantise(const InputControlSpec &control, const float value) noexcept
        {
            if (control.kind == InputControlKind::Button)
            {
                return value >= 0.5f ? 1u : 0u;
            }
            double x = static_cast<double>(value);
            if (x != x)
            {
                x = 0.0;
            }
            const double maxStep = MaxStep(control);
            double scaled = x / static_cast<double>(control.range) * maxStep;
            const double lowest = control.isSigned ? -maxStep : 0.0;
            if (scaled < lowest)
            {
                scaled = lowest;
            }
            else if (scaled > maxStep)
            {
                scaled = maxStep;
            }
            double rounded = std::floor(scaled + 0.5);
            if (rounded > maxStep)
            {
                rounded = maxStep;
            }
            const double stored = control.isSigned ? rounded + maxStep : rounded;
            return static_cast<std::uint32_t>(stored);
        }

        /// Plain IEEE operations on integers converted exactly (at most 17 bits): the same result everywhere
        float Dequantise(const InputControlSpec &control, const std::uint32_t stored) noexcept
        {
            if (control.kind == InputControlKind::Button)
            {
                return stored != 0 ? 1.0f : 0.0f;
            }
            const double maxStep = MaxStep(control);
            const double range = static_cast<double>(control.range);
            if (control.isSigned)
            {
                const double step = static_cast<double>(stored) - maxStep;
                return static_cast<float>(step / maxStep * range);
            }
            return static_cast<float>(static_cast<double>(stored) / maxStep * range);
        }

        void PutBits(std::vector<std::uint8_t> &bytes, const std::size_t position, const std::uint32_t value,
                     const std::uint8_t count)
        {
            for (std::uint8_t i = 0; i < count; ++i)
            {
                if (((value >> i) & 1u) != 0)
                {
                    const std::size_t bit = position + i;
                    bytes[bit / 8] = static_cast<std::uint8_t>(bytes[bit / 8] | (1u << (bit % 8)));
                }
            }
        }

        std::uint32_t GetBits(const std::span<const std::uint8_t> bytes, const std::size_t position, const std::uint8_t count)
        {
            std::uint32_t value = 0;
            for (std::uint8_t i = 0; i < count; ++i)
            {
                const std::size_t bit = position + i;
                if (((bytes[bit / 8] >> (bit % 8)) & 1u) != 0)
                {
                    value |= 1u << i;
                }
            }
            return value;
        }

        bool ValidName(const std::string &name)
        {
            if (name.empty() || name.size() > kMaxInputControlNameLength)
            {
                return false;
            }
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                const char c = name[i];
                const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
                const bool digit = c >= '0' && c <= '9';
                if (!letter && !(digit && i > 0))
                {
                    return false;
                }
            }
            return true;
        }

        InputLayout &DeclaredStorage()
        {
            static InputLayout declared;
            return declared;
        }
    }

    std::string_view ToString(const InputControlKind kind) noexcept
    {
        switch (kind)
        {
        case InputControlKind::Button:
            return "button";
        case InputControlKind::Axis:
            return "axis";
        case InputControlKind::Vector2:
            return "vector2";
        case InputControlKind::Vector3:
            return "vector3";
        }
        return "unknown";
    }

    std::optional<InputControlKind> ParseInputControlKind(const std::string_view text) noexcept
    {
        if (text == "button")
        {
            return InputControlKind::Button;
        }
        if (text == "axis")
        {
            return InputControlKind::Axis;
        }
        if (text == "vector2")
        {
            return InputControlKind::Vector2;
        }
        if (text == "vector3")
        {
            return InputControlKind::Vector3;
        }
        return std::nullopt;
    }

    std::expected<InputLayout, std::string> InputLayout::Create(std::vector<InputControlSpec> controls)
    {
        if (controls.size() > kMaxInputControls)
        {
            return std::unexpected(std::format("an input layout holds at most {} controls, {} were declared",
                                               kMaxInputControls, controls.size()));
        }

        std::size_t bitCount = 0;
        for (std::size_t i = 0; i < controls.size(); ++i)
        {
            InputControlSpec &control = controls[i];
            // The name is checked first: it is the only thing a later message may print, and only once it is clean
            if (!ValidName(control.name))
            {
                return std::unexpected(std::format("control {} has an invalid name: 1 to {} characters of A-Z, a-z, 0-9 "
                                                   "and '_', not starting with a digit",
                                                   i + 1, kMaxInputControlNameLength));
            }
            if (static_cast<std::uint8_t>(control.kind) > static_cast<std::uint8_t>(InputControlKind::Vector3))
            {
                return std::unexpected(std::format("control '{}' has an unknown kind", control.name));
            }
            if (control.kind == InputControlKind::Button)
            {
                control.bits = 1;
                control.range = 1.0f;
                control.isSigned = false;
            }
            else
            {
                if (!(control.range > 0.0f) || control.range > kMaxInputRange)
                {
                    return std::unexpected(std::format("control '{}' has a range outside (0, {}]", control.name,
                                                       kMaxInputRange));
                }
                const std::uint8_t lowestBits = control.isSigned ? 2 : 1;
                if (control.bits < lowestBits || control.bits > kMaxInputComponentBits)
                {
                    return std::unexpected(std::format("control '{}' has {} bits per component; {} to {} are allowed",
                                                       control.name, static_cast<unsigned>(control.bits),
                                                       static_cast<unsigned>(lowestBits),
                                                       static_cast<unsigned>(kMaxInputComponentBits)));
                }
            }
        }

        std::sort(controls.begin(), controls.end(),
                  [](const InputControlSpec &a, const InputControlSpec &b) { return a.name < b.name; });
        for (std::size_t i = 1; i < controls.size(); ++i)
        {
            if (controls[i].name == controls[i - 1].name)
            {
                return std::unexpected(std::format("two controls are named '{}'", controls[i].name));
            }
        }

        InputLayout layout;
        layout._offsets.reserve(controls.size());
        for (const InputControlSpec &control : controls)
        {
            layout._offsets.push_back(layout._valueCount);
            std::size_t components = 1;
            if (control.kind == InputControlKind::Vector2)
            {
                components = 2;
            }
            else if (control.kind == InputControlKind::Vector3)
            {
                components = 3;
            }
            layout._valueCount += components;
            bitCount += components * BitsOf(control);
        }
        if (bitCount > kMaxInputPayloadBytes * 8)
        {
            return std::unexpected(std::format("the layout needs {} bits; at most {} fit one input frame", bitCount,
                                               kMaxInputPayloadBytes * 8));
        }
        layout._bitCount = bitCount;
        layout._controls = std::move(controls);
        return layout;
    }

    std::size_t InputLayout::ComponentsOf(const std::size_t index) const
    {
        switch (_controls[index].kind)
        {
        case InputControlKind::Vector2:
            return 2;
        case InputControlKind::Vector3:
            return 3;
        default:
            return 1;
        }
    }

    std::optional<std::size_t> InputLayout::FindControl(const std::string_view name) const
    {
        for (std::size_t i = 0; i < _controls.size(); ++i)
        {
            if (_controls[i].name == name)
            {
                return i;
            }
        }
        return std::nullopt;
    }

    std::uint64_t InputLayout::Hash() const
    {
        Fnv1a64 hash;
        hash.String("n2net-input-1");
        hash.U32(static_cast<std::uint32_t>(_controls.size()));
        for (const InputControlSpec &control : _controls)
        {
            hash.String(control.name);
            hash.U8(static_cast<std::uint8_t>(control.kind));
            hash.U8(BitsOf(control));
            hash.U8(control.isSigned ? 1 : 0);
            hash.U32(std::bit_cast<std::uint32_t>(control.range));
        }
        return hash.Value();
    }

    std::vector<std::uint8_t> InputLayout::NeutralPayload() const
    {
        // A signed zero is the middle step, not the zero bit pattern, so the neutral payload is packed like any other
        const std::vector<float> neutral = NeutralValues();
        std::optional<std::vector<std::uint8_t>> payload = Pack(neutral);
        return payload.has_value() ? std::move(*payload) : std::vector<std::uint8_t>(PayloadBytes(), 0);
    }

    std::optional<std::vector<std::uint8_t>> InputLayout::Pack(const std::span<const float> values) const
    {
        if (values.size() != _valueCount)
        {
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes(PayloadBytes(), 0);
        std::size_t position = 0;
        std::size_t valueIndex = 0;
        for (std::size_t i = 0; i < _controls.size(); ++i)
        {
            const InputControlSpec &control = _controls[i];
            const std::uint8_t bits = BitsOf(control);
            const std::size_t components = ComponentsOf(i);
            for (std::size_t k = 0; k < components; ++k)
            {
                PutBits(bytes, position, Quantise(control, values[valueIndex]), bits);
                position += bits;
                ++valueIndex;
            }
        }
        return bytes;
    }

    bool InputLayout::IsValidPayload(const std::span<const std::uint8_t> payload) const
    {
        return Unpack(payload).has_value();
    }

    std::optional<std::vector<float>> InputLayout::Unpack(const std::span<const std::uint8_t> payload) const
    {
        if (payload.size() != PayloadBytes())
        {
            return std::nullopt;
        }
        std::vector<float> values;
        values.reserve(_valueCount);
        std::size_t position = 0;
        for (std::size_t i = 0; i < _controls.size(); ++i)
        {
            const InputControlSpec &control = _controls[i];
            const std::uint8_t bits = BitsOf(control);
            const std::uint32_t largest = MaxStored(control);
            const std::size_t components = ComponentsOf(i);
            for (std::size_t k = 0; k < components; ++k)
            {
                const std::uint32_t stored = GetBits(payload, position, bits);
                if (stored > largest)
                {
                    return std::nullopt;
                }
                values.push_back(Dequantise(control, stored));
                position += bits;
            }
        }
        // The padding is zero: one byte string per input
        for (std::size_t bit = position; bit < payload.size() * 8; ++bit)
        {
            if (((payload[bit / 8] >> (bit % 8)) & 1u) != 0)
            {
                return std::nullopt;
            }
        }
        return values;
    }

    const InputLayout &DeclaredInputLayout() noexcept
    {
        return DeclaredStorage();
    }

    void SetDeclaredInputLayout(InputLayout layout)
    {
        DeclaredStorage() = std::move(layout);
    }
}
