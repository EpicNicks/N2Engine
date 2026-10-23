#include "math/Random.hpp"
#include "math/Vector2.hpp"
#include "math/Constants.hpp"
#include "math/DeterministicMath.hpp"

namespace N2Engine
{
    namespace Math
    {
        namespace Random
        {
            Vector2 RandomOnUnitCircle()
            {
                if (auto &seeded = Detail::SeededStream())
                {
                    return seeded->OnUnitCircle();
                }
                float angle = RandomRange(0.0f, Constants::TWO_PI);
                return Vector2(Math::Cos(angle), Math::Sin(angle));
            }

            Vector2 RandomInUnitCircle()
            {
                if (auto &seeded = Detail::SeededStream())
                {
                    return seeded->InUnitCircle();
                }
                float angle = RandomRange(0.0f, Constants::TWO_PI);
                float r = std::sqrt(RandomFloat());
                return Vector2(Math::Cos(angle) * r, Math::Sin(angle) * r);
            }
        }
    }
}