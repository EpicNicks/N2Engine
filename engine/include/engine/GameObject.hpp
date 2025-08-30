#pragma once

#include <string>

#include "engine/base/Asset.hpp"

namespace N2Engine
{
    class GameObject : public Asset
    {
    public:
        std::string name;
    };
}