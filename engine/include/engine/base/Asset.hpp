#pragma once

#include <math/UUID.hpp>

namespace N2Engine
{
    namespace Base
    {
        class Asset
        {
        private:
            Math::UUID _uuid;

        public:
            Asset();
            Asset(Math::UUID uuid);
            Math::UUID GetUUID();
        };
    }
}