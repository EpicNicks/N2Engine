#pragma once

#include <array>
#include <cmath>
#include <type_traits>

namespace N2Engine
{
    namespace Math
    {
        template <typename T, std::size_t DIMENSION>
        struct VectorN
        {
            static_assert(std::is_arithmetic_v<T>, "VectorN requires arithmetic type (int, float, double, etc.)");
            static_assert(DIMENSION > 0, "VectorN dimension must be greater than 0");

        public:
            std::array<T, DIMENSION> vector;

            VectorN(std::initializer_list<T> items)
            {
                if (items.size() != DIMENSION)
                {
                    throw std::invalid_argument("Initializer list size must match vector dimension");
                }
                std::copy(items.begin(), items.end(), vector.begin());
            }

            T Magnitude() const
            {
                T sumOfSquares = T{0};
                for (const auto &item : vector)
                {
                    sumOfSquares += item * item;
                }
                return std::sqrt(sumOfSquares);
            }

            T Distance(const VectorN &other) const
            {
                return (*this - other).Magnitude();
            }
            VectorN Normalized() const
            {
                T mag = Magnitude();
                if (mag == T{0})
                {
                    return Zero();
                }
                return (*this) / mag;
            }

            VectorN &Normalize()
            {
                T mag = Magnitude();
                if (mag != T{0})
                {
                    (*this) /= mag;
                }
                return *this;
            }

            T Dot(const VectorN &other) const
            {
                T result = T{0};
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    result += (*this)[i] * other[i];
                }
                return result;
            }

            T &operator[](std::size_t index)
            {
                return vector[index];
            }

            const T &operator[](std::size_t index) const
            {
                return vector[index];
            }

            VectorN operator+(const VectorN &other) const
            {
                VectorN sum;
                for (size_t i = 0; i < DIMENSION; i++)
                {
                    sum[i] = (*this)[i] + other[i];
                }
                return sum;
            }

            VectorN operator-(const VectorN &other) const
            {
                VectorN difference;
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    difference[i] = (*this)[i] - other[i];
                }
                return difference;
            }
            VectorN operator*(T scalar) const
            {
                VectorN result;
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    result[i] = (*this)[i] * scalar;
                }
                return result;
            }

            VectorN operator/(T scalar) const
            {
                VectorN result;
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    result[i] = (*this)[i] / scalar;
                }
                return result;
            }
            VectorN &operator+=(const VectorN &other)
            {
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    (*this)[i] += other[i];
                }
                return *this;
            }

            VectorN &operator-=(const VectorN &other)
            {
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    (*this)[i] -= other[i];
                }
                return *this;
            }
            VectorN &operator*=(T other)
            {
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    (*this)[i] *= other;
                }
                return *this;
            }
            VectorN &operator/=(T other)
            {
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    (*this)[i] /= other;
                }
                return *this;
            }

            bool operator==(const VectorN &other) const
            {
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    if ((*this)[i] != other[i])
                    {
                        return false;
                    }
                }
                return true;
            }
            bool operator!=(const VectorN &other) const
            {
                return !(*this == other);
            }

            static VectorN One()
            {
                VectorN result;
                result.vector.fill(T{1});
                return result;
            }
            static VectorN Zero()
            {
                VectorN result;
                result.vector.fill(T{0});
                return result;
            }
        };
        template <typename T, std::size_t DIMENSION>
        VectorN<T, DIMENSION> operator*(T scalar, const VectorN<T, DIMENSION> &vec)
        {
            return vec * scalar;
        }
    }
}