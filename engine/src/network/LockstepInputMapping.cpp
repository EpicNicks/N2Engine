#include "engine/network/LockstepInputMapping.hpp"

#include <vector>

namespace N2Engine::Network
{
    static_assert(N2Net::kMaxLockstepInputBytes == kMaxInputPayloadBytes,
                  "n2net repeats the payload cap because it has no engine dependency");

    std::optional<N2Net::LockstepLayoutInfo> MakeLockstepLayoutInfo(const InputLayout &layout)
    {
        const std::optional<N2Net::LockstepInput> neutral = N2Net::LockstepInput::FromBytes(layout.NeutralPayload());
        if (!neutral || layout.PayloadBytes() > N2Net::kMaxLockstepInputBytes)
        {
            return std::nullopt;
        }
        N2Net::LockstepLayoutInfo info;
        info.payloadBytes = static_cast<std::uint8_t>(layout.PayloadBytes());
        info.hash = layout.Hash();
        info.neutral = *neutral;
        return info;
    }

    std::expected<N2Net::LockstepInput, std::string> ToLockstepInput(const InputLayout &layout,
                                                                    const std::span<const std::uint8_t> payload)
    {
        // The length is checked before the bytes are looked at
        if (payload.size() != layout.PayloadBytes())
        {
            return std::unexpected("payload is " + std::to_string(payload.size()) + " bytes, the layout takes " +
                                   std::to_string(layout.PayloadBytes()));
        }
        const std::optional<N2Net::LockstepInput> input = N2Net::LockstepInput::FromBytes(payload);
        if (!input)
        {
            return std::unexpected("payload is larger than a lockstep input");
        }
        if (!layout.IsValidPayload(payload))
        {
            return std::unexpected("payload is not one the layout produces");
        }
        return *input;
    }

    std::expected<N2Net::LockstepInput, std::string> ToLockstepInput(const InputLayout &layout, const InputFrame &frame)
    {
        return ToLockstepInput(layout, std::span<const std::uint8_t>(frame.payload));
    }

    InputFrame ToInputFrame(const std::uint32_t tick, const std::uint32_t sequence, const N2Net::LockstepInput &input)
    {
        InputFrame frame;
        frame.tick = tick;
        frame.sequence = sequence;
        frame.payload.assign(input.Bytes().begin(), input.Bytes().end());
        return frame;
    }

    std::expected<N2Net::InputBundle, N2Net::BundleError> DecodeInputBundleFor(const InputLayout &layout,
                                                                              const std::span<const std::uint8_t> bytes)
    {
        const std::optional<N2Net::LockstepLayoutInfo> info = MakeLockstepLayoutInfo(layout);
        if (!info)
        {
            return std::unexpected(N2Net::BundleError::LayoutMismatch);
        }
        auto bundle = N2Net::DecodeInputBundle(bytes, *info);
        if (!bundle)
        {
            return bundle;
        }
        for (const N2Net::LockstepInput &frame : bundle->frames)
        {
            if (!layout.IsValidPayload(frame.Bytes()))
            {
                return std::unexpected(N2Net::BundleError::Malformed);
            }
        }
        return bundle;
    }

    const InputLayout &LegacyMoveLayout()
    {
        static const InputLayout layout = []
        {
            std::vector<InputControlSpec> controls;
            for (const char *axis : {"moveX", "moveY"})
            {
                controls.push_back(InputControlSpec{axis, InputControlKind::Axis, 8, 127.0f, true});
            }
            for (int i = 0; i < 30; ++i)
            {
                std::string name = "b";
                name += static_cast<char>('0' + i / 10);
                name += static_cast<char>('0' + i % 10);
                controls.push_back(InputControlSpec{std::move(name), InputControlKind::Button, 1, 1.0f, false});
            }
            auto created = InputLayout::Create(std::move(controls));
            return created.has_value() ? std::move(*created) : InputLayout{};
        }();
        return layout;
    }

    N2Net::LockstepInput LegacyInput(const int moveX, const int moveY, const std::uint32_t buttons)
    {
        const InputLayout &layout = LegacyMoveLayout();
        std::vector<float> values(layout.ValueCount(), 0.0f);
        // Controls are held in the byte order of their names, so look each one up by name
        for (std::size_t i = 0; i < layout.ControlCount(); ++i)
        {
            const InputControlSpec &control = layout.Control(i);
            float &value = values[layout.ValueOffset(i)];
            if (control.name == "moveX")
            {
                value = static_cast<float>(moveX);
            }
            else if (control.name == "moveY")
            {
                value = static_cast<float>(moveY);
            }
            else
            {
                const unsigned bit = static_cast<unsigned>((control.name[1] - '0') * 10 + (control.name[2] - '0'));
                value = ((buttons >> bit) & 1u) != 0 ? 1.0f : 0.0f;
            }
        }
        const auto packed = layout.Pack(values);
        return packed ? N2Net::LockstepInput::FromBytes(*packed).value_or(N2Net::LockstepInput{}) : N2Net::LockstepInput{};
    }
}
