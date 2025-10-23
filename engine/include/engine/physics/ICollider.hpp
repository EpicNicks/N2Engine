#pragma once

#include "engine/Component.hpp"

namespace N2Engine
{
    namespace Physics
    {
        class ICollider : public Component
        {
        public:
            bool isTrigger{false};

            virtual void OnCollisionEnter() {}
            virtual void OnCollisionStay() {}
            virtual void OnCollisionExit() {}
        };
    }
}