#include "engine/input/InputBindingFactory.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>

#include "engine/input/InputBinding.hpp"
#include "engine/input/InputMapping.hpp"

namespace N2Engine::Input
{
    std::string BindingParseErrorToString(BindingParseError error)
    {
        switch (error)
        {
        case BindingParseError::MissingType: return "missing 'type' field";
        case BindingParseError::InvalidType: return "invalid or unknown binding type";
        case BindingParseError::MissingKey: return "missing 'key' field";
        case BindingParseError::MissingButton: return "missing 'button' field";
        case BindingParseError::MissingAxis: return "missing 'axis' field";
        case BindingParseError::MissingCompositeKeys: return "missing composite direction keys";
        case BindingParseError::InvalidValue: return "unknown key, button or axis name";
        case BindingParseError::InvalidOptionalField:
            return "optional field of the wrong type (gamepadId: non-negative integer, deadzone: number, "
                   "invertX/invertY: boolean)";
        }
        return "unknown error";
    }

    namespace
    {
        // nlohmann's enum mapping turns an unknown string into the enum's first value (so a typo like
        // "Escpae" silently became Key::Unknown). Accept a name only if it maps back to itself.
        template <typename E>
        bool IsKnownName(const nlohmann::json &value)
        {
            return value.is_string() && nlohmann::json(value.get<E>()) == value;
        }

        template <typename E>
        bool AllKnown(const nlohmann::json &j, std::initializer_list<const char *> fields)
        {
            return std::ranges::all_of(fields, [&j](const char *field) { return IsKnownName<E>(j[field]); });
        }

        // Optional fields are read with json::value, which throws nlohmann::json::type_error for a present
        // field of the wrong type (e.g. "deadzone": "0.2"). Checked first, so a bad one is a parse error.
        bool IsValidGamepadId(const nlohmann::json &j)
        {
            if (!j.contains("gamepadId"))
            {
                return true;
            }
            // Read as an int: a larger id would wrap
            constexpr auto MaxId = static_cast<std::int64_t>(std::numeric_limits<int>::max());
            const auto &id = j["gamepadId"];
            if (id.is_number_unsigned())
            {
                return id.get<std::uint64_t>() <= static_cast<std::uint64_t>(MaxId);
            }
            return id.is_number_integer() && id.get<std::int64_t>() >= 0 && id.get<std::int64_t>() <= MaxId;
        }

        bool IsOptionalNumber(const nlohmann::json &j, const char *field)
        {
            return !j.contains(field) || j[field].is_number();
        }

        bool IsOptionalBool(const nlohmann::json &j, const char *field)
        {
            return !j.contains(field) || j[field].is_boolean();
        }
    }

    std::expected<std::unique_ptr<InputBinding>, BindingParseError> CreateBindingFromJson(
        GLFWwindow *window,
        const nlohmann::json &j
    )
    {
        if (!j.contains("type") || !j["type"].is_string())
        {
            return std::unexpected(BindingParseError::MissingType);
        }

        if (!IsKnownName<BindingType>(j["type"]))
        {
            return std::unexpected(BindingParseError::InvalidType);
        }
        const BindingType type = j["type"].get<BindingType>();

        switch (type)
        {
        case BindingType::KeyboardButton:
            {
                if (!j.contains("key"))
                    return std::unexpected(BindingParseError::MissingKey);
                if (!AllKnown<Key>(j, {"key"}))
                    return std::unexpected(BindingParseError::InvalidValue);
                return std::make_unique<KeyboardButtonBinding>(
                    window,
                    j["key"].get<Key>()
                );
            }

        case BindingType::GamepadAxis:
            {
                if (!j.contains("axis"))
                    return std::unexpected(BindingParseError::MissingAxis);
                if (!AllKnown<GamepadAxis>(j, {"axis"}))
                    return std::unexpected(BindingParseError::InvalidValue);
                if (!IsValidGamepadId(j))
                    return std::unexpected(BindingParseError::InvalidOptionalField);
                return std::make_unique<AxisBinding>(
                    window,
                    j["axis"].get<GamepadAxis>(),
                    j.value("gamepadId", 0)
                );
            }

        case BindingType::GamepadStick:
            {
                if (!j.contains("xAxis") || !j.contains("yAxis"))
                    return std::unexpected(BindingParseError::MissingAxis);
                if (!AllKnown<GamepadAxis>(j, {"xAxis", "yAxis"}))
                    return std::unexpected(BindingParseError::InvalidValue);
                if (!IsValidGamepadId(j) || !IsOptionalNumber(j, "deadzone") ||
                    !IsOptionalBool(j, "invertX") || !IsOptionalBool(j, "invertY"))
                    return std::unexpected(BindingParseError::InvalidOptionalField);
                return std::make_unique<GamepadStickBinding>(
                    window,
                    j["xAxis"].get<GamepadAxis>(),
                    j["yAxis"].get<GamepadAxis>(),
                    j.value("gamepadId", 0),
                    j.value("deadzone", 0.15f),
                    j.value("invertX", false),
                    j.value("invertY", false)
                );
            }

        case BindingType::Vector2Composite:
            {
                if (!j.contains("up") || !j.contains("down") ||
                    !j.contains("left") || !j.contains("right"))
                    return std::unexpected(BindingParseError::MissingCompositeKeys);
                if (!AllKnown<Key>(j, {"up", "down", "left", "right"}))
                    return std::unexpected(BindingParseError::InvalidValue);
                return std::make_unique<Vector2CompositeBinding>(
                    window,
                    j["up"].get<Key>(),
                    j["down"].get<Key>(),
                    j["left"].get<Key>(),
                    j["right"].get<Key>()
                );
            }

        case BindingType::MouseButton:
            {
                if (!j.contains("button"))
                    return std::unexpected(BindingParseError::MissingButton);
                if (!AllKnown<MouseButton>(j, {"button"}))
                    return std::unexpected(BindingParseError::InvalidValue);
                return std::make_unique<MouseButtonBinding>(
                    window,
                    j["button"].get<MouseButton>()
                );
            }

        case BindingType::GamepadButton:
            {
                if (!j.contains("button"))
                    return std::unexpected(BindingParseError::MissingButton);
                if (!AllKnown<GamepadButton>(j, {"button"}))
                    return std::unexpected(BindingParseError::InvalidValue);
                if (!IsValidGamepadId(j))
                    return std::unexpected(BindingParseError::InvalidOptionalField);
                return std::make_unique<GamepadButtonBinding>(
                    window,
                    j["button"].get<GamepadButton>(),
                    j.value("gamepadId", 0)
                );
            }
        }

        return std::unexpected(BindingParseError::InvalidType);
    }
}
