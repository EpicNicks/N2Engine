#pragma once
#include <memory>
#include <expected>
#include <string>
#include <nlohmann/json.hpp>

struct GLFWwindow;

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
        InvalidOptionalField // gamepadId, deadzone, invertX or invertY present with the wrong type
    };

    std::string BindingParseErrorToString(BindingParseError error);

    std::expected<std::unique_ptr<InputBinding>, BindingParseError> CreateBindingFromJson(
        GLFWwindow* window,
        const nlohmann::json& j
    );
}
