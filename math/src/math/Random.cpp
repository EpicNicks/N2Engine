#include "math/Random.hpp"
#include "math/Vector2.hpp"
#include "math/Constants.hpp"

namespace N2Engine
{
    namespace Math
    {
        namespace Random
        {
            Vector2 RandomOnUnitCircle()
            {
                float angle = RandomRange(0.0f, Constants::TWO_PI);
                return Vector2(std::cos(angle), std::sin(angle));
            }

            Vector2 RandomInUnitCircle()
            {
                float angle = RandomRange(0.0f, Constants::TWO_PI);
                float r = std::sqrt(RandomFloat());
                return Vector2(std::cos(angle) * r, std::sin(angle) * r);
            }
        }
    }
}