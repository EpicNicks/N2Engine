#pragma once

#include <type_traits>
#include "engine/Component.hpp"

namespace N2Engine
{
    template <typename T>
    concept DerivedFromComponent = std::is_base_of_v<Component, T>;
}