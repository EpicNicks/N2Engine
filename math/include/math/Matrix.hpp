#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <initializer_list>
#include <iostream>
#include <algorithm>
#include <string>
#include <sstream>

#include "math/Vector3.hpp"

namespace N2Engine
{
    namespace Math
    {
        template <typename T, std::size_t M, std::size_t N>
        class Matrix
        {
        public:
            std::array<T, M * N> data{};

            constexpr Matrix() = default;

            Matrix(std::initializer_list<T> init)
            {
                if (init.size() != M * N)
                    throw std::invalid_argument("Initializer list size does not match matrix dimensions");
                std::copy(init.begin(), init.end(), data.begin());
            }

            constexpr T &operator()(std::size_t row, std::size_t col)
            {
                return data[row * N + col];
            }

            constexpr const T &operator()(std::size_t row, std::size_t col) const
            {
                return data[row * N + col];
            }

            constexpr T &operator[](std::size_t row, std::size_t col)
            {
                return data[row * N + col];
            }

            constexpr const T &operator[](std::size_t row, std::size_t col) const
            {
                return data[row * N + col];
            }

            void fill(const T &value) { data.fill(value); }

            constexpr Matrix operator+(const Matrix &other) const
            {
                Matrix result;
                for (std::size_t i = 0; i < M * N; ++i)
                    result.data[i] = data[i] + other.data[i];
                return result;
            }

            constexpr Matrix operator-(const Matrix &other) const
            {
                Matrix result;
                for (std::size_t i = 0; i < M * N; ++i)
                    result.data[i] = data[i] - other.data[i];
                return result;
            }

            constexpr Matrix operator*(const T &scalar) const
            {
                Matrix result;
                for (std::size_t i = 0; i < M * N; ++i)
                    result.data[i] = data[i] * scalar;
                return result;
            }

            template <std::size_t P>
            constexpr Matrix<T, M, P> operator*(const Matrix<T, N, P> &rhs) const
            {
                Matrix<T, M, P> result;
                for (std::size_t i = 0; i < M; ++i)
                {
                    for (std::size_t j = 0; j < P; ++j)
                    {
                        T sum{};
                        for (std::size_t k = 0; k < N; ++k)
                        {
                            sum += (*this)(i, k) * rhs(k, j);
                        }
                        result(i, j) = sum;
                    }
                }
                return result;
            }

            constexpr bool operator==(const Matrix<T, M, N> &other) const
            {
                for (size_t i = 0; i < M * N; i++)
                {
                    if (data[i] != other.data[i])
                    {
                        return false;
                    }
                }
                return true;
            }

            constexpr bool operator!=(const Matrix<T, M, N> &other) const
            {
                return !(*this == other);
            }

            constexpr Matrix<T, N, M> transpose() const
            {
                Matrix<T, N, M> result;
                for (std::size_t i = 0; i < M; ++i)
                    for (std::size_t j = 0; j < N; ++j)
                        result(j, i) = (*this)(i, j);
                return result;
            }

            static constexpr Matrix<T, M, N> identity()
            {
                static_assert(M == N, "Identity matrix only defined for square matrices");
                Matrix<T, M, N> I;
                for (std::size_t i = 0; i < M; ++i)
                    for (std::size_t j = 0; j < N; ++j)
                        I(i, j) = (i == j) ? T{1} : T{0};
                return I;
            }

            constexpr T determinant() const
            {
                static_assert(M == N, "Determinant only defined for square matrices");
                if constexpr (M == 1)
                {
                    return data[0];
                }
                else if constexpr (M == 2)
                {
                    return (*this)(0, 0) * (*this)(1, 1) - (*this)(0, 1) * (*this)(1, 0);
                }
                else
                {
                    T det{};
                    for (std::size_t col = 0; col < N; ++col)
                    {
                        det += ((col % 2 == 0) ? 1 : -1) * (*this)(0, col) * minor(0, col).determinant();
                    }
                    return det;
                }
            }

            constexpr Matrix inverse() const
            {
                static_assert(M == N, "Inverse only defined for square matrices");
                T det = determinant();
                if (det == T{0})
                    throw std::runtime_error("Matrix is singular and cannot be inverted");

                Matrix adj;
                for (std::size_t i = 0; i < M; ++i)
                {
                    for (std::size_t j = 0; j < N; ++j)
                    {
                        T cofactor = (((i + j) % 2 == 0) ? 1 : -1) * minor(i, j).determinant();
                        adj(j, i) = cofactor; // transpose for adjugate
                    }
                }
                return adj * (1 / det);
            }

            std::string toString() const
            {
                std::ostringstream ss;
                ss << "[";
                for (std::size_t row = 0; row < M; ++row)
                {
                    if (row > 0)
                    {
                        ss << "\n";
                    }
                    for (std::size_t col = 0; col < N; ++col)
                    {
                        if (col > 0)
                            ss << " ";
                        ss << data[row * N + col];
                    }
                }
                ss << "]";
                return ss.str();
            }

            Vector3 TransformPoint(const Vector3 &point) const
                requires(M == 4 && N == 4)
            {

                T x = static_cast<T>(point.x) * (*this)(0, 0) + static_cast<T>(point.y) * (*this)(0, 1) + static_cast<T>(point.z) * (*this)(0, 2) + (*this)(0, 3);
                T y = static_cast<T>(point.x) * (*this)(1, 0) + static_cast<T>(point.y) * (*this)(1, 1) + static_cast<T>(point.z) * (*this)(1, 2) + (*this)(1, 3);
                T z = static_cast<T>(point.x) * (*this)(2, 0) + static_cast<T>(point.y) * (*this)(2, 1) + static_cast<T>(point.z) * (*this)(2, 2) + (*this)(2, 3);
                T w = static_cast<T>(point.x) * (*this)(3, 0) + static_cast<T>(point.y) * (*this)(3, 1) + static_cast<T>(point.z) * (*this)(3, 2) + (*this)(3, 3);

                if (w != T{0})
                    return Vector3{static_cast<float>(x / w), static_cast<float>(y / w), static_cast<float>(z / w)};
                else
                    return Vector3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
            }

        private:
            constexpr Matrix<T, M - 1, N - 1> minor(std::size_t row, std::size_t col) const
            {
                Matrix<T, M - 1, N - 1> result;
                std::size_t dst = 0;
                for (std::size_t i = 0; i < M; ++i)
                {
                    if (i == row)
                        continue;
                    for (std::size_t j = 0; j < N; ++j)
                    {
                        if (j == col)
                            continue;
                        result.data[dst++] = (*this)(i, j);
                    }
                }
                return result;
            }
        };
    }
}