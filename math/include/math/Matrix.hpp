#pragma once

#include <array>
#include <span>
#include <immintrin.h>
#include <stdexcept>
#include <initializer_list>
#include <format>
#include <cmath>
#include <algorithm>
#include <string>
#include <iostream>

#ifdef _WIN32
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif

#include "math/CpuInfo.hpp"
#include "math/Vector3.hpp"

namespace N2Engine::Math
{
    // Helper for horizontal add in SSE2
    namespace detail
    {
        inline float horizontal_add_sse2(__m128 v)
        {
            __m128 shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 sums = _mm_add_ps(v, shuf);
            shuf = _mm_movehl_ps(shuf, sums);
            sums = _mm_add_ss(sums, shuf);
            return _mm_cvtss_f32(sums);
        }
    }

    // ===== BASE TEMPLATE - Scalar implementation =====
    template <typename T, std::size_t M, std::size_t N>
    class Matrix
    {
    public:
        std::array<T, M * N> data{};

        constexpr Matrix() = default;

        constexpr Matrix(std::initializer_list<T> init)
        {
            if (init.size() != M * N)
                throw std::invalid_argument("Initializer list size does not match matrix dimensions");
            std::copy(init.begin(), init.end(), data.begin());
        }

        constexpr T& operator()(std::size_t row, std::size_t col)
        {
            return data[row * N + col];
        }

        constexpr const T& operator()(std::size_t row, std::size_t col) const
        {
            return data[row * N + col];
        }

        constexpr std::span<T> operator[](std::size_t row)
        {
            return std::span<T>(data.data() + row * N, N);
        }

        constexpr std::span<const T> operator[](std::size_t row) const
        {
            return std::span<const T>(data.data() + row * N, N);
        }

        constexpr T& operator[](std::size_t row, std::size_t col)
        {
            return data(row, col);
        }

        constexpr const T& operator[](std::size_t row, std::size_t col) const
        {
            return data(row, col);
        }

        constexpr void fill(const T &value) { data.fill(value); }

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

        constexpr bool operator==(const Matrix &other) const
        {
            for (std::size_t i = 0; i < M * N; ++i)
            {
                if (data[i] != other.data[i])
                    return false;
            }
            return true;
        }

        constexpr bool operator!=(const Matrix &other) const
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
            requires(M == N)
        {
            Matrix<T, M, N> I;
            for (std::size_t i = 0; i < M; ++i)
                for (std::size_t j = 0; j < N; ++j)
                    I(i, j) = (i == j) ? T{1} : T{0};
            return I;
        }

        [[nodiscard]] std::string toString() const
        {
            std::string result = "[";
            for (std::size_t row = 0; row < M; ++row)
            {
                if (row > 0)
                    result += "\n ";
                for (std::size_t col = 0; col < N; ++col)
                {
                    if (col > 0)
                        result += " ";
                    result += std::format("{:8.3f}", static_cast<float>(data[row * N + col]));
                }
            }
            result += "]";
            return result;
        }
    };

    // ===== TEMPLATE SPECIALIZATION - 4x4 float with SIMD =====
    template <>
    class Matrix<float, 4, 4>
    {
    private:
        alignas(16) std::array<float, 16> data{};

        // Function pointer types
        using MulFunc = Matrix (*)(const Matrix &, const Matrix &);
        using AddFunc = Matrix (*)(const Matrix &, const Matrix &);
        using SubFunc = Matrix (*)(const Matrix &, const Matrix &);
        using ScalarMulFunc = Matrix (*)(const Matrix &, float);
        using TransformFunc = Vector3 (*)(const Matrix &, const Vector3 &);
        using TransposeFunc = Matrix (*)(const Matrix &);
        using InverseFunc = Matrix (*)(const Matrix &);
        using DeterminantFunc = float (*)(const Matrix &);

        // Static function pointers - set once at initialization
        inline static MulFunc multiply_func;
        inline static AddFunc add_func;
        inline static SubFunc sub_func;
        inline static ScalarMulFunc scalar_mul_func;
        inline static TransformFunc transform_func;
        inline static TransposeFunc transpose_func;
        inline static InverseFunc inverse_func;
        inline static DeterminantFunc determinant_func;
        inline static bool initialized;

    public:
        constexpr Matrix() = default;

        Matrix(std::initializer_list<float> init)
        {
            if (init.size() != 16)
                throw std::invalid_argument("Initializer list must have 16 elements");
            std::copy(init.begin(), init.end(), data.begin());
        }

        constexpr float& operator()(std::size_t row, std::size_t col)
        {
            return data[row * 4 + col];
        }

        constexpr const float& operator()(std::size_t row, std::size_t col) const
        {
            return data[row * 4 + col];
        }

        constexpr void fill(const float &value) { data.fill(value); }

        // SIMD-optimized operations using function pointers
        Matrix operator+(const Matrix &other) const
        {
            return add_func(*this, other);
        }

        Matrix operator-(const Matrix &other) const
        {
            return sub_func(*this, other);
        }

        Matrix operator*(const Matrix &other) const
        {
            return multiply_func(*this, other);
        }

        Matrix operator*(float scalar) const
        {
            return scalar_mul_func(*this, scalar);
        }

        bool operator==(const Matrix &other) const
        {
            for (std::size_t i = 0; i < 16; ++i)
            {
                if (data[i] != other.data[i])
                    return false;
            }
            return true;
        }

        bool operator!=(const Matrix &other) const
        {
            return !(*this == other);
        }

        [[nodiscard]] Matrix transpose() const
        {
            return transpose_func(*this);
        }

        [[nodiscard]] Matrix inverse() const
        {
            return inverse_func(*this);
        }

        [[nodiscard]] float determinant() const
        {
            return determinant_func(*this);
        }

        // 4x4-specific methods
        [[nodiscard]] Vector3 TransformPoint(const Vector3 &point) const
        {
            return transform_func(*this, point);
        }

        static Matrix identity()
        {
            Matrix result;
            result.data = {
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f
            };
            return result;
        }

        static Matrix Translation(const Vector3 &translation)
        {
            Matrix result = identity();
            result(0, 3) = translation.x;
            result(1, 3) = translation.y;
            result(2, 3) = translation.z;
            return result;
        }

        static Matrix Scale(float sx, float sy, float sz)
        {
            Matrix result = identity();
            result(0, 0) = sx;
            result(1, 1) = sy;
            result(2, 2) = sz;
            return result;
        }

        static Matrix RotationX(float angle)
        {
            float c = std::cos(angle);
            float s = std::sin(angle);
            Matrix result = identity();
            result(1, 1) = c;
            result(1, 2) = -s;
            result(2, 1) = s;
            result(2, 2) = c;
            return result;
        }

        static Matrix RotationY(float angle)
        {
            float c = std::cos(angle);
            float s = std::sin(angle);
            Matrix result = identity();
            result(0, 0) = c;
            result(0, 2) = s;
            result(2, 0) = -s;
            result(2, 2) = c;
            return result;
        }

        static Matrix RotationZ(float angle)
        {
            float c = std::cos(angle);
            float s = std::sin(angle);
            Matrix result = identity();
            result(0, 0) = c;
            result(0, 1) = -s;
            result(1, 0) = s;
            result(1, 1) = c;
            return result;
        }

        [[nodiscard]] std::string toString() const
        {
            std::string result = "[";
            for (std::size_t row = 0; row < 4; ++row)
            {
                if (row > 0)
                    result += "\n ";
                for (std::size_t col = 0; col < 4; ++col)
                {
                    if (col > 0)
                        result += " ";
                    result += std::format("{:8.3f}", data[row * 4 + col]);
                }
            }
            result += "]";
            return result;
        }

        // Access to raw data
        [[nodiscard]] const float* Data() const { return data.data(); }
        float* Data() { return data.data(); }

        // SIMD initialization - call once at startup
        static void InitializeSIMD()
        {
            if (initialized)
                return;

            static CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();

            if (features.sse41)
            {
                std::cout << "Using SSE4.1 implementations for Matrix\n";

                multiply_func = &MultiplySSE2;
                add_func = &AddSSE2;
                sub_func = &SubSSE2;
                scalar_mul_func = &ScalarMulSSE2;
                transform_func = &TransformPointSSE41;
                transpose_func = &TransposeSSE2;
                inverse_func = &InverseSSE2;
                determinant_func = &DeterminantSSE2;
            }
            else if (features.sse2)
            {
                std::cout << "Using SSE2 implementations for Matrix\n";

                multiply_func = &MultiplySSE2;
                add_func = &AddSSE2;
                sub_func = &SubSSE2;
                scalar_mul_func = &ScalarMulSSE2;
                transform_func = &TransformPointSSE2;
                transpose_func = &TransposeSSE2;
                inverse_func = &InverseSSE2;
                determinant_func = &DeterminantSSE2;
            }
            else
            {
                multiply_func = &MultiplyScalar;
                add_func = &AddScalar;
                sub_func = &SubScalar;
                scalar_mul_func = &ScalarMulScalar;
                transform_func = &TransformPointScalar;
                transpose_func = &TransposeScalar;
                inverse_func = &InverseScalar;
                determinant_func = &DeterminantScalar;
            }

            initialized = true;
        }

    private:
        // ===== SCALAR IMPLEMENTATIONS =====
        static Matrix MultiplyScalar(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            for (size_t i = 0; i < 4; ++i)
            {
                for (size_t j = 0; j < 4; ++j)
                {
                    float sum = 0.0f;
                    for (size_t k = 0; k < 4; ++k)
                    {
                        sum += a.data[i * 4 + k] * b.data[k * 4 + j];
                    }
                    result.data[i * 4 + j] = sum;
                }
            }
            return result;
        }

        static Matrix AddScalar(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            for (size_t i = 0; i < 16; ++i)
            {
                result.data[i] = a.data[i] + b.data[i];
            }
            return result;
        }

        static Matrix SubScalar(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            for (size_t i = 0; i < 16; ++i)
            {
                result.data[i] = a.data[i] - b.data[i];
            }
            return result;
        }

        static Matrix ScalarMulScalar(const Matrix &a, float scalar)
        {
            Matrix result;
            for (size_t i = 0; i < 16; ++i)
            {
                result.data[i] = a.data[i] * scalar;
            }
            return result;
        }

        static Vector3 TransformPointScalar(const Matrix &m, const Vector3 &point)
        {
            float x = point.x * m.data[0] + point.y * m.data[1] + point.z * m.data[2] + m.data[3];
            float y = point.x * m.data[4] + point.y * m.data[5] + point.z * m.data[6] + m.data[7];
            float z = point.x * m.data[8] + point.y * m.data[9] + point.z * m.data[10] + m.data[11];
            float w = point.x * m.data[12] + point.y * m.data[13] + point.z * m.data[14] + m.data[15];

            if (w != 0.0f)
            {
                return Vector3{x / w, y / w, z / w};
            }
            return Vector3{x, y, z};
        }

        static Matrix TransposeScalar(const Matrix &m)
        {
            Matrix result;
            for (size_t i = 0; i < 4; ++i)
            {
                for (size_t j = 0; j < 4; ++j)
                {
                    result.data[j * 4 + i] = m.data[i * 4 + j];
                }
            }
            return result;
        }

        static float DeterminantScalar(const Matrix &m)
        {
            float a00 = m.data[0], a01 = m.data[1], a02 = m.data[2], a03 = m.data[3];
            float a10 = m.data[4], a11 = m.data[5], a12 = m.data[6], a13 = m.data[7];
            float a20 = m.data[8], a21 = m.data[9], a22 = m.data[10], a23 = m.data[11];
            float a30 = m.data[12], a31 = m.data[13], a32 = m.data[14], a33 = m.data[15];

            return a00 * (a11 * (a22 * a33 - a23 * a32) - a12 * (a21 * a33 - a23 * a31) + a13 * (a21 * a32 - a22 * a31))
                -
                a01 * (a10 * (a22 * a33 - a23 * a32) - a12 * (a20 * a33 - a23 * a30) + a13 * (a20 * a32 - a22 * a30)) +
                a02 * (a10 * (a21 * a33 - a23 * a31) - a11 * (a20 * a33 - a23 * a30) + a13 * (a20 * a31 - a21 * a30)) -
                a03 * (a10 * (a21 * a32 - a22 * a31) - a11 * (a20 * a32 - a22 * a30) + a12 * (a20 * a31 - a21 * a30));
        }

        static Matrix InverseScalar(const Matrix &m)
        {
            Matrix result;

            float a00 = m.data[0], a01 = m.data[1], a02 = m.data[2], a03 = m.data[3];
            float a10 = m.data[4], a11 = m.data[5], a12 = m.data[6], a13 = m.data[7];
            float a20 = m.data[8], a21 = m.data[9], a22 = m.data[10], a23 = m.data[11];
            float a30 = m.data[12], a31 = m.data[13], a32 = m.data[14], a33 = m.data[15];

            float c00 = a11 * (a22 * a33 - a23 * a32) - a12 * (a21 * a33 - a23 * a31) + a13 * (a21 * a32 - a22 * a31);
            float c01 = -(a10 * (a22 * a33 - a23 * a32) - a12 * (a20 * a33 - a23 * a30) + a13 * (a20 * a32 - a22 *
                a30));
            float c02 = a10 * (a21 * a33 - a23 * a31) - a11 * (a20 * a33 - a23 * a30) + a13 * (a20 * a31 - a21 * a30);
            float c03 = -(a10 * (a21 * a32 - a22 * a31) - a11 * (a20 * a32 - a22 * a30) + a12 * (a20 * a31 - a21 *
                a30));

            float c10 = -(a01 * (a22 * a33 - a23 * a32) - a02 * (a21 * a33 - a23 * a31) + a03 * (a21 * a32 - a22 *
                a31));
            float c11 = a00 * (a22 * a33 - a23 * a32) - a02 * (a20 * a33 - a23 * a30) + a03 * (a20 * a32 - a22 * a30);
            float c12 = -(a00 * (a21 * a33 - a23 * a31) - a01 * (a20 * a33 - a23 * a30) + a03 * (a20 * a31 - a21 *
                a30));
            float c13 = a00 * (a21 * a32 - a22 * a31) - a01 * (a20 * a32 - a22 * a30) + a02 * (a20 * a31 - a21 * a30);

            float c20 = a01 * (a12 * a33 - a13 * a32) - a02 * (a11 * a33 - a13 * a31) + a03 * (a11 * a32 - a12 * a31);
            float c21 = -(a00 * (a12 * a33 - a13 * a32) - a02 * (a10 * a33 - a13 * a30) + a03 * (a10 * a32 - a12 *
                a30));
            float c22 = a00 * (a11 * a33 - a13 * a31) - a01 * (a10 * a33 - a13 * a30) + a03 * (a10 * a31 - a11 * a30);
            float c23 = -(a00 * (a11 * a32 - a12 * a31) - a01 * (a10 * a32 - a12 * a30) + a02 * (a10 * a31 - a11 *
                a30));

            float c30 = -(a01 * (a12 * a23 - a13 * a22) - a02 * (a11 * a23 - a13 * a21) + a03 * (a11 * a22 - a12 *
                a21));
            float c31 = a00 * (a12 * a23 - a13 * a22) - a02 * (a10 * a23 - a13 * a20) + a03 * (a10 * a22 - a12 * a20);
            float c32 = -(a00 * (a11 * a23 - a13 * a21) - a01 * (a10 * a23 - a13 * a20) + a03 * (a10 * a21 - a11 *
                a20));
            float c33 = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);

            float det = a00 * c00 + a01 * c01 + a02 * c02 + a03 * c03;

            if (std::abs(det) < 1e-7f)
            {
                throw std::runtime_error("Matrix is singular and cannot be inverted");
            }

            float inv_det = 1.0f / det;

            result.data[0] = c00 * inv_det;
            result.data[1] = c10 * inv_det;
            result.data[2] = c20 * inv_det;
            result.data[3] = c30 * inv_det;
            result.data[4] = c01 * inv_det;
            result.data[5] = c11 * inv_det;
            result.data[6] = c21 * inv_det;
            result.data[7] = c31 * inv_det;
            result.data[8] = c02 * inv_det;
            result.data[9] = c12 * inv_det;
            result.data[10] = c22 * inv_det;
            result.data[11] = c32 * inv_det;
            result.data[12] = c03 * inv_det;
            result.data[13] = c13 * inv_det;
            result.data[14] = c23 * inv_det;
            result.data[15] = c33 * inv_det;

            return result;
        }

        // ===== SSE2 IMPLEMENTATIONS =====
        static Matrix MultiplySSE2(const Matrix &a, const Matrix &b)
        {
            Matrix result;

            for (size_t i = 0; i < 4; ++i)
            {
                __m128 a_i0_broadcast = _mm_set1_ps(a.data[i * 4 + 0]);
                __m128 a_i1_broadcast = _mm_set1_ps(a.data[i * 4 + 1]);
                __m128 a_i2_broadcast = _mm_set1_ps(a.data[i * 4 + 2]);
                __m128 a_i3_broadcast = _mm_set1_ps(a.data[i * 4 + 3]);

                __m128 bRow0 = _mm_load_ps(&b.data[0]);
                __m128 bRow1 = _mm_load_ps(&b.data[4]);
                __m128 bRow2 = _mm_load_ps(&b.data[8]);
                __m128 bRow3 = _mm_load_ps(&b.data[12]);

                __m128 result_row = _mm_add_ps(
                    _mm_add_ps(
                        _mm_mul_ps(a_i0_broadcast, bRow0),
                        _mm_mul_ps(a_i1_broadcast, bRow1)),
                    _mm_add_ps(
                        _mm_mul_ps(a_i2_broadcast, bRow2),
                        _mm_mul_ps(a_i3_broadcast, bRow3)));

                _mm_store_ps(&result.data[i * 4], result_row);
            }
            return result;
        }

        static Matrix AddSSE2(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            for (size_t i = 0; i < 16; i += 4)
            {
                __m128 va = _mm_load_ps(&a.data[i]);
                __m128 vb = _mm_load_ps(&b.data[i]);
                __m128 sum = _mm_add_ps(va, vb);
                _mm_store_ps(&result.data[i], sum);
            }
            return result;
        }

        static Matrix SubSSE2(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            for (size_t i = 0; i < 16; i += 4)
            {
                __m128 va = _mm_load_ps(&a.data[i]);
                __m128 vb = _mm_load_ps(&b.data[i]);
                __m128 diff = _mm_sub_ps(va, vb);
                _mm_store_ps(&result.data[i], diff);
            }
            return result;
        }

        static Matrix ScalarMulSSE2(const Matrix &a, float scalar)
        {
            Matrix result;
            __m128 scalar_vec = _mm_set1_ps(scalar);
            for (size_t i = 0; i < 16; i += 4)
            {
                __m128 va = _mm_load_ps(&a.data[i]);
                __m128 product = _mm_mul_ps(va, scalar_vec);
                _mm_store_ps(&result.data[i], product);
            }
            return result;
        }

        static Vector3 TransformPointSSE2(const Matrix &m, const Vector3 &point)
        {
            __m128 point_vec = _mm_set_ps(1.0f, point.z, point.y, point.x);

            __m128 row0 = _mm_load_ps(&m.data[0]);
            __m128 row1 = _mm_load_ps(&m.data[4]);
            __m128 row2 = _mm_load_ps(&m.data[8]);
            __m128 row3 = _mm_load_ps(&m.data[12]);

            __m128 mul0 = _mm_mul_ps(row0, point_vec);
            __m128 mul1 = _mm_mul_ps(row1, point_vec);
            __m128 mul2 = _mm_mul_ps(row2, point_vec);
            __m128 mul3 = _mm_mul_ps(row3, point_vec);

            float x = detail::horizontal_add_sse2(mul0);
            float y = detail::horizontal_add_sse2(mul1);
            float z = detail::horizontal_add_sse2(mul2);
            float w = detail::horizontal_add_sse2(mul3);

            if (w != 0.0f)
            {
                return Vector3{x / w, y / w, z / w};
            }
            return Vector3{x, y, z};
        }

        static Matrix TransposeSSE2(const Matrix &m)
        {
            Matrix result;

            __m128 row0 = _mm_load_ps(&m.data[0]);
            __m128 row1 = _mm_load_ps(&m.data[4]);
            __m128 row2 = _mm_load_ps(&m.data[8]);
            __m128 row3 = _mm_load_ps(&m.data[12]);

            __m128 tmp0 = _mm_unpacklo_ps(row0, row1);
            __m128 tmp1 = _mm_unpackhi_ps(row0, row1);
            __m128 tmp2 = _mm_unpacklo_ps(row2, row3);
            __m128 tmp3 = _mm_unpackhi_ps(row2, row3);

            __m128 result_row0 = _mm_movelh_ps(tmp0, tmp2);
            __m128 result_row1 = _mm_movehl_ps(tmp2, tmp0);
            __m128 result_row2 = _mm_movelh_ps(tmp1, tmp3);
            __m128 result_row3 = _mm_movehl_ps(tmp3, tmp1);

            _mm_store_ps(&result.data[0], result_row0);
            _mm_store_ps(&result.data[4], result_row1);
            _mm_store_ps(&result.data[8], result_row2);
            _mm_store_ps(&result.data[12], result_row3);

            return result;
        }

        static float DeterminantSSE2(const Matrix &m)
        {
            // For now, use scalar implementation (full SIMD determinant is complex)
            return DeterminantScalar(m);
        }

        static Matrix InverseSSE2(const Matrix &m)
        {
            // For now, use scalar implementation (full SIMD inverse is very complex)
            return InverseScalar(m);
        }

        // ===== SSE4.1 IMPLEMENTATIONS =====
        static Vector3 TransformPointSSE41(const Matrix &m, const Vector3 &point)
        {
            __m128 point_vec = _mm_set_ps(1.0f, point.z, point.y, point.x);

            __m128 row0 = _mm_load_ps(&m.data[0]);
            __m128 row1 = _mm_load_ps(&m.data[4]);
            __m128 row2 = _mm_load_ps(&m.data[8]);
            __m128 row3 = _mm_load_ps(&m.data[12]);

            // Use SSE4.1 dot product instruction
            __m128 x = _mm_dp_ps(row0, point_vec, 0xF1);
            __m128 y = _mm_dp_ps(row1, point_vec, 0xF1);
            __m128 z = _mm_dp_ps(row2, point_vec, 0xF1);
            __m128 w = _mm_dp_ps(row3, point_vec, 0xF1);

            float x_val = _mm_cvtss_f32(x);
            float y_val = _mm_cvtss_f32(y);
            float z_val = _mm_cvtss_f32(z);
            float w_val = _mm_cvtss_f32(w);

            if (w_val != 0.0f)
            {
                return Vector3{x_val / w_val, y_val / w_val, z_val / w_val};
            }
            return Vector3{x_val, y_val, z_val};
        }
    };

    // ===== TEMPLATE SPECIALIZATION - 3x3 float with SIMD =====
    template <>
    class Matrix<float, 3, 3>
    {
    private:
        alignas(16) std::array<float, 12> data{}; // 9 used + 3 padding

        using MulFunc = Matrix (*)(const Matrix &, const Matrix &);
        using AddFunc = Matrix (*)(const Matrix &, const Matrix &);
        using SubFunc = Matrix (*)(const Matrix &, const Matrix &);
        using ScalarMulFunc = Matrix (*)(const Matrix &, float);
        using TransposeFunc = Matrix (*)(const Matrix &);

        inline static MulFunc multiply_func;
        inline static AddFunc add_func;
        inline static SubFunc sub_func;
        inline static ScalarMulFunc scalar_mul_func;
        inline static TransposeFunc transpose_func;
        inline static bool initialized;

    public:
        constexpr Matrix() = default;

        Matrix(std::initializer_list<float> init)
        {
            if (init.size() != 9)
                throw std::invalid_argument("Initializer list must have 9 elements");
            auto it = init.begin();
            for (size_t i = 0; i < 9; ++i)
            {
                data[i] = *it++;
            }
        }

        constexpr float& operator()(std::size_t row, std::size_t col)
        {
            return data[row * 3 + col];
        }

        constexpr const float& operator()(std::size_t row, std::size_t col) const
        {
            return data[row * 3 + col];
        }

        constexpr void fill(const float &value)
        {
            for (size_t i = 0; i < 9; ++i)
                data[i] = value;
        }

        Matrix operator+(const Matrix &other) const
        {
            return add_func(*this, other);
        }

        Matrix operator-(const Matrix &other) const
        {
            return sub_func(*this, other);
        }

        Matrix operator*(const Matrix &other) const
        {
            return multiply_func(*this, other);
        }

        Matrix operator*(float scalar) const
        {
            return scalar_mul_func(*this, scalar);
        }

        bool operator==(const Matrix &other) const
        {
            for (std::size_t i = 0; i < 9; ++i)
            {
                if (data[i] != other.data[i])
                    return false;
            }
            return true;
        }

        bool operator!=(const Matrix &other) const
        {
            return !(*this == other);
        }

        [[nodiscard]] Matrix transpose() const
        {
            return transpose_func(*this);
        }

        static Matrix identity()
        {
            Matrix result;
            result.data[0] = 1.0f;
            result.data[1] = 0.0f;
            result.data[2] = 0.0f;
            result.data[3] = 0.0f;
            result.data[4] = 1.0f;
            result.data[5] = 0.0f;
            result.data[6] = 0.0f;
            result.data[7] = 0.0f;
            result.data[8] = 1.0f;
            return result;
        }

        static Matrix RotationX(float angle)
        {
            float c = std::cos(angle);
            float s = std::sin(angle);
            Matrix result = identity();
            result(1, 1) = c;
            result(1, 2) = -s;
            result(2, 1) = s;
            result(2, 2) = c;
            return result;
        }

        static Matrix RotationY(float angle)
        {
            float c = std::cos(angle);
            float s = std::sin(angle);
            Matrix result = identity();
            result(0, 0) = c;
            result(0, 2) = s;
            result(2, 0) = -s;
            result(2, 2) = c;
            return result;
        }

        static Matrix RotationZ(float angle)
        {
            float c = std::cos(angle);
            float s = std::sin(angle);
            Matrix result = identity();
            result(0, 0) = c;
            result(0, 1) = -s;
            result(1, 0) = s;
            result(1, 1) = c;
            return result;
        }

        [[nodiscard]] std::string toString() const
        {
            std::string result = "[";
            for (std::size_t row = 0; row < 3; ++row)
            {
                if (row > 0)
                    result += "\n ";
                for (std::size_t col = 0; col < 3; ++col)
                {
                    if (col > 0)
                        result += " ";
                    result += std::format("{:8.3f}", data[row * 3 + col]);
                }
            }
            result += "]";
            return result;
        }

        static void InitializeSIMD()
        {
            if (initialized)
                return;

            CPUInfo::CPUFeatures features = CPUInfo::DetectCPUFeatures();

            if (features.sse2)
            {
                multiply_func = &MultiplySSE2;
                add_func = &AddSSE2;
                sub_func = &SubSSE2;
                scalar_mul_func = &ScalarMulSSE2;
                transpose_func = &TransposeScalar; // 3x3 transpose is simple enough for scalar
            }
            else
            {
                multiply_func = &MultiplyScalar;
                add_func = &AddScalar;
                sub_func = &SubScalar;
                scalar_mul_func = &ScalarMulScalar;
                transpose_func = &TransposeScalar;
            }

            initialized = true;
        }

    private:
        // Scalar implementations
        static Matrix MultiplyScalar(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            for (size_t i = 0; i < 3; ++i)
            {
                for (size_t j = 0; j < 3; ++j)
                {
                    float sum = 0.0f;
                    for (size_t k = 0; k < 3; ++k)
                    {
                        sum += a.data[i * 3 + k] * b.data[k * 3 + j];
                    }
                    result.data[i * 3 + j] = sum;
                }
            }
            return result;
        }

        static Matrix AddScalar(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            for (size_t i = 0; i < 9; ++i)
            {
                result.data[i] = a.data[i] + b.data[i];
            }
            return result;
        }

        static Matrix SubScalar(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            for (size_t i = 0; i < 9; ++i)
            {
                result.data[i] = a.data[i] - b.data[i];
            }
            return result;
        }

        static Matrix ScalarMulScalar(const Matrix &a, float scalar)
        {
            Matrix result;
            for (size_t i = 0; i < 9; ++i)
            {
                result.data[i] = a.data[i] * scalar;
            }
            return result;
        }

        static Matrix TransposeScalar(const Matrix &m)
        {
            Matrix result;
            for (size_t i = 0; i < 3; ++i)
            {
                for (size_t j = 0; j < 3; ++j)
                {
                    result.data[j * 3 + i] = m.data[i * 3 + j];
                }
            }
            return result;
        }

        // SSE2 implementations (partial SIMD for 3x3)
        static Matrix AddSSE2(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            // Process first 8 elements with SIMD (2 loads of 4 each)
            __m128 va1 = _mm_loadu_ps(&a.data[0]);
            __m128 vb1 = _mm_loadu_ps(&b.data[0]);
            __m128 sum1 = _mm_add_ps(va1, vb1);
            _mm_storeu_ps(&result.data[0], sum1);

            __m128 va2 = _mm_loadu_ps(&a.data[4]);
            __m128 vb2 = _mm_loadu_ps(&b.data[4]);
            __m128 sum2 = _mm_add_ps(va2, vb2);
            _mm_storeu_ps(&result.data[4], sum2);

            // Handle last element manually
            result.data[8] = a.data[8] + b.data[8];
            return result;
        }

        static Matrix SubSSE2(const Matrix &a, const Matrix &b)
        {
            Matrix result;
            __m128 va1 = _mm_loadu_ps(&a.data[0]);
            __m128 vb1 = _mm_loadu_ps(&b.data[0]);
            __m128 diff1 = _mm_sub_ps(va1, vb1);
            _mm_storeu_ps(&result.data[0], diff1);

            __m128 va2 = _mm_loadu_ps(&a.data[4]);
            __m128 vb2 = _mm_loadu_ps(&b.data[4]);
            __m128 diff2 = _mm_sub_ps(va2, vb2);
            _mm_storeu_ps(&result.data[4], diff2);

            result.data[8] = a.data[8] - b.data[8];
            return result;
        }

        static Matrix ScalarMulSSE2(const Matrix &a, float scalar)
        {
            Matrix result;
            __m128 scalar_vec = _mm_set1_ps(scalar);

            __m128 va1 = _mm_loadu_ps(&a.data[0]);
            __m128 product1 = _mm_mul_ps(va1, scalar_vec);
            _mm_storeu_ps(&result.data[0], product1);

            __m128 va2 = _mm_loadu_ps(&a.data[4]);
            __m128 product2 = _mm_mul_ps(va2, scalar_vec);
            _mm_storeu_ps(&result.data[4], product2);

            result.data[8] = a.data[8] * scalar;
            return result;
        }

        static Matrix MultiplySSE2(const Matrix &a, const Matrix &b)
        {
            // For 3x3, often scalar is competitive due to memory layout complexity
            return MultiplyScalar(a, b);
        }
    };

    // ===== TEMPLATE SPECIALIZATION - 2x2 float =====
    template <>
    class Matrix<float, 2, 2>
    {
    private:
        alignas(8) std::array<float, 4> data{};

    public:
        constexpr Matrix() = default;

        Matrix(std::initializer_list<float> init)
        {
            if (init.size() != 4)
                throw std::invalid_argument("Initializer list must have 4 elements");
            std::copy(init.begin(), init.end(), data.begin());
        }

        constexpr float& operator()(std::size_t row, std::size_t col)
        {
            return data[row * 2 + col];
        }

        constexpr const float& operator()(std::size_t row, std::size_t col) const
        {
            return data[row * 2 + col];
        }

        constexpr void fill(const float &value) { data.fill(value); }

        Matrix operator+(const Matrix &other) const
        {
            Matrix result;
            for (size_t i = 0; i < 4; ++i)
            {
                result.data[i] = data[i] + other.data[i];
            }
            return result;
        }

        Matrix operator-(const Matrix &other) const
        {
            Matrix result;
            for (size_t i = 0; i < 4; ++i)
            {
                result.data[i] = data[i] - other.data[i];
            }
            return result;
        }

        Matrix operator*(const Matrix &other) const
        {
            Matrix result;
            result.data[0] = data[0] * other.data[0] + data[1] * other.data[2];
            result.data[1] = data[0] * other.data[1] + data[1] * other.data[3];
            result.data[2] = data[2] * other.data[0] + data[3] * other.data[2];
            result.data[3] = data[2] * other.data[1] + data[3] * other.data[3];
            return result;
        }

        Matrix operator*(float scalar) const
        {
            Matrix result;
            for (size_t i = 0; i < 4; ++i)
            {
                result.data[i] = data[i] * scalar;
            }
            return result;
        }

        bool operator==(const Matrix &other) const
        {
            for (std::size_t i = 0; i < 4; ++i)
            {
                if (data[i] != other.data[i])
                    return false;
            }
            return true;
        }

        bool operator!=(const Matrix &other) const
        {
            return !(*this == other);
        }

        [[nodiscard]] Matrix transpose() const
        {
            Matrix result;
            result.data[0] = data[0];
            result.data[1] = data[2];
            result.data[2] = data[1];
            result.data[3] = data[3];
            return result;
        }

        static Matrix identity()
        {
            Matrix result;
            result.data = {1.0f, 0.0f, 0.0f, 1.0f};
            return result;
        }

        static Matrix Rotation(float angle)
        {
            float c = std::cos(angle);
            float s = std::sin(angle);
            Matrix result;
            result.data = {c, -s, s, c};
            return result;
        }

        static Matrix Scale(float sx, float sy)
        {
            Matrix result;
            result.data = {sx, 0.0f, 0.0f, sy};
            return result;
        }

        float determinant() const
        {
            return data[0] * data[3] - data[1] * data[2];
        }

        Matrix inverse() const
        {
            float det = determinant();
            if (std::abs(det) < 1e-7f)
            {
                throw std::runtime_error("Matrix is singular and cannot be inverted");
            }
            float inv_det = 1.0f / det;
            Matrix result;
            result.data[0] = data[3] * inv_det;
            result.data[1] = -data[1] * inv_det;
            result.data[2] = -data[2] * inv_det;
            result.data[3] = data[0] * inv_det;
            return result;
        }

        [[nodiscard]] constexpr std::string toString() const
        {
            std::string result = "[";
            for (std::size_t row = 0; row < 2; ++row)
            {
                if (row > 0)
                    result += "\n ";
                for (std::size_t col = 0; col < 2; ++col)
                {
                    if (col > 0)
                        result += " ";
                    result += std::format("{:8.3f}", data[row * 2 + col]);
                }
            }
            result += "]";
            return result;
        }
    };
}
