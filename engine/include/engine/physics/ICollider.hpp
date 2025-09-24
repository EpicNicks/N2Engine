#pragma once

namespace N2Engine
{
    namespace Physics
    {
        class ICollider
        {
        public:
            virtual void OnCollisionEnter() {}
            virtual void OnCollisionStay() {}
            virtual void OnCollisionExit() {}
        };
    }
}