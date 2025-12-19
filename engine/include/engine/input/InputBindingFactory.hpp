#pragma once
#include <memory>
#include <expected>
#include <string>
#include <nlohmann/json.hpp>

class GLFWwindow;

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
        MissingCompositeKeys
    };

    std::string BindingParseErrorToString(BindingParseError error);

    std::expected<std::unique_ptr<InputBinding>, BindingParseError> CreateBindingFromJson(
        GLFWwindow* window,
        const nlohmann::json& j
    );
}
