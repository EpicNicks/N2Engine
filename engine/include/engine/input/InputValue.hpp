#pragma once

#include <math/Vector2.hpp>
#include <variant>

namespace N2Engine::Input
{
    class InputValue
    {
        std::variant<bool, float, Math::Vector2> value;

    public:
        template <typename T>
        InputValue(T val) : value(val) {}

        // Automatic conversions
        Math::Vector2 asVector2() const
        {
            return std::visit([](auto &&val) -> Math::Vector2
            {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, Math::Vector2>) {
                    return val;
                } else if constexpr (std::is_same_v<T, bool>) {
                    return Math::Vector2(val ? 1.0f : 0.0f, 0.0f);
                } else if constexpr (std::is_same_v<T, float>) {
                    return Math::Vector2(val, 0.0f);
                } }, value);
        }

        bool asBool() const
        {
            return std::visit([](auto &&val) -> bool
            {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, bool>) {
                    return val;
                } else if constexpr (std::is_same_v<T, Math::Vector2>) {
                    return val.Length() > 0.0f;  // Deadzone
                } else if constexpr (std::is_same_v<T, float>) {
                    return std::abs(val) > 0.0f;
                } }, value);
        }

        float asFloat() const
        {
            return std::visit([](auto &&val) -> float
            {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, float>) {
                    return val;
                } else if constexpr (std::is_same_v<T, bool>) {
                    return val ? 1.0f : 0.0f;
                } else if constexpr (std::is_same_v<T, Math::Vector2>) {
                    return val.Length();
                } }, value);
        }
    };
}
