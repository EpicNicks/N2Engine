#pragma once

#include "UUID.hpp"

namespace N2Engine
{
    class Asset
    {
    private:
        Math::UUID _uuid;

    public:
        Math::UUID GetUUID();
    };
}