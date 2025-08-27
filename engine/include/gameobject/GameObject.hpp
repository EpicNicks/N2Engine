#pragma once

#include <string>

#include "Asset.hpp"

namespace N2Engine
{
    class GameObject : public Asset
    {
    public:
        std::string name;
    };
}