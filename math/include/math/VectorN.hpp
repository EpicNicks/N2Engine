#pragma once

#include <array>
#include <cmath>
#include <type_traits>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <sstream>

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

            VectorN()
            {
                vector.fill(T{0});
            }

            VectorN(const VectorN &other) : vector(other.vector) {}
            VectorN(VectorN &&other) noexcept : vector(std::move(other.vector)) {}
            VectorN &operator=(const VectorN &other)
            {
                if (this != &other)
                {
                    vector = other.vector;
                }
                return *this;
            }

            constexpr VectorN(std::initializer_list<T> items)
            {
                if (items.size() != DIMENSION)
                {
                    throw std::invalid_argument("Initializer list size must match vector dimension");
                }
                std::copy(items.begin(), items.end(), vector.begin());
            }

            explicit VectorN(const std::array<T, DIMENSION> &arr) : vector(arr) {}
            explicit VectorN(std::array<T, DIMENSION> &&arr) : vector(std::move(arr)) {}

            template <typename Iterator>
            VectorN(Iterator first, Iterator last)
            {
                if (std::distance(first, last) != DIMENSION)
                {
                    throw std::invalid_argument("Iterator range size must match vector dimension");
                }
                std::copy(first, last, vector.begin());
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

            /**
             * The Hadamard product (element-wise multiplication)
             */
            VectorN Scale(const VectorN &other) const
            {
                VectorN result;
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    result[i] = (*this)[i] * other[i];
                }
                return result;
            }

            static VectorN Lerp(const VectorN &a, const VectorN &b, T t)
            {
                VectorN result;
                for (std::size_t i = 0; i < DIMENSION; i++)
                {
                    result[i] = a[i] + t * (b[i] - a[i]);
                }
                return result;
            }

            static VectorN Slerp(const VectorN &a, const VectorN &b, T t)
            {
                T dot = a.Dot(b);
                dot = std::clamp(dot, T{-1}, T{1});

                if (std::abs(dot) > T{0.9995})
                {
                    return Lerp(a, b, t);
                }

                T theta = std::acos(dot) * t;
                VectorN relative = (b - a * dot).Normalized();
                return a * std::cos(theta) + relative * std::sin(theta);
            }

            VectorN Lerp(const VectorN &other, T t) const
            {
                return Lerp(*this, other, t);
            }

            VectorN Slerp(const VectorN &other, T t) const
            {
                return Slerp(*this, other, t);
            }

            std::string toString()
            {
                std::ostringstream ss;
                ss << "[";
                for (int i = 0; i < vector.size(); i++)
                {
                    ss << std::to_string(vector[i]);
                    if (i < vector.size() - 1)
                    {
                        ss << ",";
                    }
                }
                ss << "]";
                return ss.str();
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
                return CreateUniform(T{1});
            }
            static VectorN Zero()
            {
                return CreateUniform(T{0});
            }

            static VectorN CreateUniform(T t)
            {
                VectorN result;
                result.vector.fill(t);
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