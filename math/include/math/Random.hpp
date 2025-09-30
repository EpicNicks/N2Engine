#pragma once
#include <cmath>
#include <random>

namespace N2Engine
{
    namespace Math
    {
        class Vector2;

        namespace Random
        {
            inline std::mt19937 &GetRandomEngine()
            {
                static std::random_device rd;
                static std::mt19937 gen(rd());
                return gen;
            }

            // Random float [0, 1]
            inline float RandomFloat()
            {
                static std::uniform_real_distribution<float> dis(0.0f, 1.0f);
                return dis(GetRandomEngine());
            }

            // Random float in range
            inline float RandomRange(float min, float max)
            {
                return min + RandomFloat() * (max - min);
            }

            // Random int in range [min, max] inclusive
            inline int RandomInt(int min, int max)
            {
                std::uniform_int_distribution<int> dis(min, max);
                return dis(GetRandomEngine());
            }

            Vector2 RandomInUnitCircle();
            Vector2 RandomOnUnitCircle();
        }
    }
}