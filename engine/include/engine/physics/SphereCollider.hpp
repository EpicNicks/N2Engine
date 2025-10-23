#pragma once

#include "engine/physics/ICollider.hpp"

namespace N2Engine
{
    namespace Physics
    {
        class SphereCollider : public ICollider
        {
        public:
            float radius{0.5f};
        };
    }
}