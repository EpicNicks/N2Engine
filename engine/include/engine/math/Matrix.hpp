#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <initializer_list>
#include <iostream>

namespace N2Engine
{
    namespace Math
    {
        template <typename T, std::size_t M, std::size_t N>
        class Matrix
        {
        public:
            using value_type = T;

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

            void print() const
            {
                for (std::size_t i = 0; i < M; ++i)
                {
                    for (std::size_t j = 0; j < N; ++j)
                        std::cout << (*this)(i, j) << " ";
                    std::cout << "\n";
                }
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