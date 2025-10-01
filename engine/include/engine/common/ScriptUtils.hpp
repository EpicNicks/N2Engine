#pragma once

#include <string_view>
#include <string>

constexpr std::string_view stripNamespace(std::string_view name)
{
    auto pos = name.rfind("::");
    return pos == std::string_view::npos ? name : name.substr(pos + 2);
}

#define NAMEOF(X) std::string(stripNamespace(#X))