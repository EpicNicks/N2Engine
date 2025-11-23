#pragma once

#include <string_view>
#include <string>

constexpr std::string_view stripNamespace(const std::string_view name)
{
    const size_t pos = name.rfind("::");
    return pos == std::string_view::npos ? name : name.substr(pos + 2);
}

constexpr std::string_view stripDotOperators(const std::string_view name)
{
    const size_t pos = name.rfind(".");
    return pos == std::string_view::npos ? "" : name.substr(pos);
}

#define NAMEOF(X) std::string(stripNamespace(#X))
#define QUALIFIED_NAMEOF(X) (std::string(#X))
#define NAMEOF_FIELD(X) std::string(stripDotOperators(stripNamespace(#X)))
