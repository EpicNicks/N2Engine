#pragma once

#include <math/UUID.hpp>

namespace N2Engine
{
    namespace Base
    {
        class Asset
        {
        protected:
            Math::UUID _uuid;

        public:
            virtual ~Asset() = default;
            Asset();
            explicit Asset(Math::UUID uuid);
            [[nodiscard]] Math::UUID GetUUID() const;
            void SetUUID(const Math::UUID &uuid);
        };
    }
}