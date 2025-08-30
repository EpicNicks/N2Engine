#pragma once

#include "engine/math/UUID.hpp"

namespace N2Engine
{
    class Asset
    {
    private:
        Math::UUID _uuid;

    public:
        Asset();
        Math::UUID GetUUID();
    };
}