#pragma once
#include <memory>
#include <expected>
#include <string>
#include <nlohmann/json.hpp>

namespace N2Engine::Input
{
    class InputDevice;
}

namespace N2Engine::Input
{
    class InputBinding;

    enum class BindingParseError
    {
        MissingType,
        InvalidType,
        MissingKey,
        MissingButton,
        MissingAxis,
        MissingCompositeKeys,
        InvalidValue, // a key/button/axis name that doesn't exist
        InvalidOptionalField, // gamepadId, deadzone, invertX, invertY, direction or pressPoint present but invalid
        MissingChordBinding, // a Chord without an object 'modifier' and 'binding'
        ChordTooDeep // Chords nested more than MaxChordDepth levels
    };

    /// How many Chords may nest inside each other (a chord of a chord is a two-modifier chord)
    inline constexpr int MaxChordDepth = 4;

    std::string BindingParseErrorToString(BindingParseError error);

    std::expected<std::unique_ptr<InputBinding>, BindingParseError> CreateBindingFromJson(
        InputDevice *window,
        const nlohmann::json& j
    );
}
